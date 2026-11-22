#!/usr/bin/env python3
"""Compile Princeton WordNet 3.1 (EN) and FreeDict dictd archives (DE->EN, EN->DE)
into KoboDash's compact, mmap-friendly UTF-8 sorted binary dictionary format (KDDICT01).
"""
import argparse
import gzip
import re
import struct
import tarfile
from collections import defaultdict
from pathlib import Path

POS_FILES = [
    ("dict/data.noun", "n."),
    ("dict/data.verb", "v."),
    ("dict/data.adj", "adj."),
    ("dict/data.adv", "adv."),
]

B64_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
B64_MAP = {ch: i for i, ch in enumerate(B64_CHARS)}

MAX_SENSES_PER_WORD = 4
MAX_DEF_BYTES = 340


def b64dec(s: str) -> int:
    v = 0
    for ch in s:
        v = (v << 6) + B64_MAP[ch]
    return v


def is_valid_headword(word: str) -> bool:
    if not word or " " in word or len(word) > 42:
        return False
    # Reject prefix fragments like "haus..." or "haus…"
    if "." in word or "…" in word or "/" in word:
        return False
    # Allow Latin/German letters + hyphen + apostrophe
    for ch in word:
        o = ord(ch)
        if ch in "-'":
            continue
        if ch.isalpha() and o <= 0x017F:
            continue
        return False
    return any(ch.isalpha() for ch in word)


def parse_wordnet_tar(tar_path: Path) -> dict[str, bytes]:
    entries: dict[str, list[tuple[str, str]]] = defaultdict(list)
    with tarfile.open(tar_path, "r:gz") as tf:
        members = {m.name.lstrip("./"): m for m in tf.getmembers()}
        for rel_name, pos_tag in POS_FILES:
            m = members.get(rel_name)
            if not m:
                raise RuntimeError(f"Missing {rel_name} in {tar_path}")
            f = tf.extractfile(m)
            assert f is not None
            for raw_line in f:
                line = raw_line.decode("utf-8", errors="replace")
                if not line or line[0] == " ":
                    continue
                pipe = line.find("|")
                if pipe < 0:
                    continue
                head = line[:pipe].strip().split()
                gloss = re.sub(r"\s+", " ", line[pipe + 1 :].strip())
                if len(head) < 5 or not gloss:
                    continue
                try:
                    w_cnt = int(head[3], 16)
                except ValueError:
                    continue
                idx = 4
                for _ in range(w_cnt):
                    if idx >= len(head):
                        break
                    raw_word = head[idx]
                    idx += 2
                    paren = raw_word.find("(")
                    if paren > 0:
                        raw_word = raw_word[:paren]
                    word = raw_word.replace("_", " ").strip().lower()
                    if not is_valid_headword(word):
                        continue
                    senses = entries[word]
                    if len(senses) < MAX_SENSES_PER_WORD:
                        if not any(g == gloss for _, g in senses):
                            senses.append((pos_tag, gloss))

    out: dict[str, bytes] = {}
    for w, senses in entries.items():
        parts = []
        for pos, gloss in senses:
            if len(gloss) > 150:
                cut = gloss.rfind(";", 0, 148)
                gloss = gloss[:cut] if cut > 40 else (gloss[:147].rstrip() + "...")
            parts.append(f"({pos}) {gloss}")
        out[w] = truncate_utf8("\n".join(parts), MAX_DEF_BYTES)
    return out


def truncate_utf8(text: str, max_bytes: int) -> bytes:
    raw = text.encode("utf-8")
    if len(raw) <= max_bytes:
        return raw
    raw = raw[: max_bytes - 3]
    while raw and (raw[-1] & 0xC0) == 0x80:
        raw = raw[:-1]
    return raw.rstrip() + b"..."


_IPA_RE = re.compile(r"/[^/\n]{1,40}/")
_TAG_RE = re.compile(r"<([^>]{1,24})>")
_DOMAIN_RE = re.compile(r"^\s*\[[^\]]{1,24}\]\s*")


def clean_freedict_trans(line: str) -> tuple[bool, str]:
    s = line.strip()
    if not s:
        return False, ""
    is_specialized = bool(_DOMAIN_RE.match(s))
    # Strip IPA blocks & inline abbreviations
    s = _IPA_RE.sub("", s)
    # Compact POS tags like <n>, <v>, <adj>, <fem, n, sg>
    s = _TAG_RE.sub(lambda m: f"({m.group(1)})", s)
    s = re.sub(r"\s+", " ", s).strip(" ,;")
    return is_specialized, s


def extract_head_pos(head_line: str) -> str:
    m = _TAG_RE.search(head_line)
    if not m:
        return ""
    tag = m.group(1).replace(", sg", "").strip()
    return f"({tag})" if tag else ""


def parse_freedict_dictd(tar_path: Path) -> dict[str, bytes]:
    with tarfile.open(tar_path, "r:xz") as tf:
        idx_member = next(m for m in tf.getmembers() if m.name.endswith(".index"))
        dz_member = next(m for m in tf.getmembers() if m.name.endswith(".dict.dz"))
        idx_text = tf.extractfile(idx_member).read().decode("utf-8", errors="replace")
        dict_bytes = gzip.GzipFile(fileobj=tf.extractfile(dz_member)).read()

    # word -> list of (priority_score, pos_tag, translation, optional_example)
    grouped: dict[str, list[tuple[int, str, str]]] = defaultdict(list)

    for line in idx_text.splitlines():
        parts = line.split("\t")
        if len(parts) != 3:
            continue
        raw_hw, off_s, len_s = parts
        word = raw_hw.strip().lower()
        if not is_valid_headword(word):
            continue
        # Cap total senses collected per word to stay fast
        if len(grouped[word]) >= 16:
            continue
        off = b64dec(off_s)
        ln = b64dec(len_s)
        body = dict_bytes[off : off + ln].decode("utf-8", errors="replace")
        blines = [bl for bl in body.splitlines() if bl.strip()]
        if len(blines) < 2:
            continue
        head_line = blines[0]
        # Skip prefix-only entries like "Haus…"
        if "…" in head_line.split()[0]:
            continue
        pos = extract_head_pos(head_line)
        is_spec, trans = clean_freedict_trans(blines[1])
        if not trans:
            continue
        # Rank general translations before domain-tagged ones, shorter common ones first
        prio = (100 if is_spec else 0) + min(len(trans), 80)
        grouped[word].append((prio, pos, trans))

    out: dict[str, bytes] = {}
    for w, items in grouped.items():
        items.sort(key=lambda x: x[0])
        seen: set[str] = set()
        lines_out: list[str] = []
        by_pos: dict[str, list[str]] = {}
        pos_order: list[str] = []
        for _, pos, trans in items:
            key = trans.lower()
            if key in seen:
                continue
            seen.add(key)
            if pos not in by_pos:
                by_pos[pos] = []
                pos_order.append(pos)
            if len(by_pos[pos]) < 4:
                by_pos[pos].append(trans)
            if len(seen) >= 7:
                break
        for pos in pos_order[:4]:
            joined = "; ".join(by_pos[pos])
            lines_out.append(f"{pos} {joined}".strip() if pos else joined)
        if lines_out:
            out[w] = truncate_utf8("\n".join(lines_out), MAX_DEF_BYTES)
    return out


def write_kdict(entries: dict[str, bytes], out_path: Path) -> None:
    # CRITICAL: sort strictly by UTF-8 bytes to match C memcmp() binary search!
    items = sorted(((w.encode("utf-8"), db) for w, db in entries.items()), key=lambda x: x[0])
    items = [(wb, db) for wb, db in items if 0 < len(wb) <= 255 and 0 < len(db) <= 65535]

    n = len(items)
    header_size = 16
    data_start = header_size + n * 4

    offsets: list[int] = []
    chunks: list[bytes] = []
    cur_off = data_start
    for wb, db in items:
        offsets.append(cur_off)
        rec = struct.pack("<BH", len(wb), len(db)) + wb + db
        chunks.append(rec)
        cur_off += len(rec)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("wb") as f:
        f.write(b"KDDICT01")
        f.write(struct.pack("<II", n, header_size))
        f.write(struct.pack(f"<{n}I", *offsets))
        for ch in chunks:
            f.write(ch)
    print(f"Built {out_path}: {n:,} entries, {out_path.stat().st_size / (1024*1024):.2f} MB")


def main() -> int:
    ap = argparse.ArgumentParser(description="Build KoboDash binary dictionaries (EN, DE->EN, EN->DE)")
    ap.add_argument("--only", choices=["all", "en", "de-en", "en-de"], default="all")
    args = ap.parse_args()

    wn = Path("artifacts/downloads/wn3.1.dict.tar.gz")
    de_en = Path("artifacts/downloads/freedict-deu-eng.dictd.tar.xz")
    en_de = Path("artifacts/downloads/freedict-eng-deu.dictd.tar.xz")

    if args.only in ("all", "en") and wn.is_file():
        write_kdict(parse_wordnet_tar(wn), Path("output/en.kdict"))
    if args.only in ("all", "de-en") and de_en.is_file():
        write_kdict(parse_freedict_dictd(de_en), Path("output/de-en.kdict"))
    if args.only in ("all", "en-de") and en_de.is_file():
        write_kdict(parse_freedict_dictd(en_de), Path("output/en-de.kdict"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
