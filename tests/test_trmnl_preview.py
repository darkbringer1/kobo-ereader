"""Exercise the prepared upstream shell with fake network, display and power tools."""
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
LOOP = ROOT / 'output/trmnl-install/TRMNL/trmnlloop.sh'


@unittest.skipUnless(LOOP.exists(), 'Run prepare_trmnl.py with the pinned upstream sources first')
class PreviewTests(unittest.TestCase):
    def run_preview(self, failure=''):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            for folder in ['tools', 'scripts', 'bin/fbink']:
                (base / folder).mkdir(parents=True)
            def script(name, body):
                target = base / name
                target.write_text('#!/bin/sh\n' + body + '\n')
                target.chmod(0o755)
            # All hardware paths and temporary files are confined to the test.
            loop = LOOP.read_text().replace('/sys/', str(base / 'sys') + '/')
            loop = loop.replace('/tmp/', str(base) + '/')
            (base / 'loop.sh').write_text(loop)
            script('scripts/log.sh', ':')
            script('scripts/getrssi.sh', 'echo -65')
            for name in ['ledToggle.sh', 'enable-wifi.sh', 'restore-wifi-async.sh', 'disable-wifi.sh']:
                script('scripts/' + name, 'echo POWER >>"$CALLS"; exit 97')
            script('bin/busybox_kobo', 'echo SUSPEND >>"$CALLS"; exit 97')
            script('bin/fbink/fbdepth', 'echo ROTATE >>"$CALLS"; exit 97')
            script('tools/sleep', ':')
            script('tools/pidof', 'echo "$PREVIEW_PID"')
            script('tools/jq', '''case "$1" in
tojson) echo '{}' ;;
-r) case "$2" in .image_url) echo http://test/image.png ;; *) echo 900 ;; esac ;;
esac''')
            script('tools/curl', '''echo CURL >>"$CALLS"
[ "$FAILURE" = curl ] && exit 22
while [ "$#" -gt 0 ]; do
  if [ "$1" = -o ]; then shift; echo '{}' >"$1"; fi
  shift
done
exit 0''')
            script('bin/fbink/fbink', '''echo RENDER >>"$CALLS"
ps -o stat= -p "$PREVIEW_PID" >>"$CALLS"
[ "$FAILURE" = render ] && exit 4
exit 0''')
            sleeper = subprocess.Popen(['/bin/sleep', '60'])
            try:
                env = dict(os.environ, PATH=str(base / 'tools') + ':' + os.environ['PATH'],
                           TRMNL_TEST_ONCE='1', trmnl_loop_ignore_curl_errors='true',
                           trmnl_image_format='png', trmnl_apiurl='http://test',
                           CALLS=str(base / 'calls'), FAILURE=failure, PREVIEW_PID=str(sleeper.pid))
                result = subprocess.run(['/bin/bash', 'loop.sh'], cwd=base, env=env,
                                        capture_output=True, text=True, timeout=5)
                calls = (base / 'calls').read_text()
                state = subprocess.check_output(['ps', '-o', 'stat=', '-p', str(sleeper.pid)], text=True)
                self.assertNotIn('T', state, 'Preview failed to resume the stock UI')
                for forbidden in ['POWER', 'SUSPEND', 'ROTATE']:
                    self.assertNotIn(forbidden, calls)
                return result, calls
            finally:
                sleeper.send_signal(signal.SIGCONT)
                sleeper.terminate()
                sleeper.wait()

    def test_success_pauses_only_for_render_then_resumes(self):
        result, calls = self.run_preview()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls.count('CURL'), 2)
        self.assertIn('RENDER\nT', calls)

    def test_network_failure_is_nonzero_and_never_renders(self):
        result, calls = self.run_preview('curl')
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('RENDER', calls)

    def test_renderer_failure_is_nonzero_and_resumes_ui(self):
        result, calls = self.run_preview('render')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('RENDER', calls)
