ZIG ?= zig
HOST_CC ?= cc
PYTHON ?= python3

CFLAGS_COMMON := -std=c11 -Wall -Wextra -Werror -O2 -Iinclude -Ithird_party/mbedtls/include -DMBEDTLS_CONFIG_FILE='"kd_mbedtls_config.h"' -ffunction-sections -fdata-sections
KOBO_TARGET := --target=arm-linux-gnueabihf.2.19 -mcpu=cortex_a8
# -z norelro -z lazy fixes Zig/LLD #18773 pre-main SIGSEGV under Kobo's glibc 2.19 ld-linux-armhf.so.3
KOBO_LDFLAGS := -Wl,-z,norelro -Wl,-z,lazy -Wl,--gc-sections

CORE_SRCS := \
	src/config.c \
	src/canvas.c \
	src/gallery.c \
	src/sysinfo.c \
	src/input.c \
	src/tls.c \
	src/dashboard.c \
	src/net.c \
	src/wifi.c \
	src/keyboard.c \
	src/dict.c \
	src/font.c \
	src/book.c \
	src/reader.c \
	src/ui.c

KOBO_SRCS := $(CORE_SRCS) src/display_kobo.c src/main.c
HOST_TEST_SRCS := $(CORE_SRCS) src/display_host.c tests/test_kobodash.c

FBINK_SO := ../artifacts/inspect/libfbink.so.1.0.0
ZLIB_SO  := ../artifacts/inspect/libz.so.1
MBEDTLS_KOBO := build/mbedtls-kobo/libmbedtls.a
MBEDTLS_HOST := build/mbedtls-host/libmbedtls.a

.PHONY: all kobo test preview deploy clean

all: kobo test

build/libfbink.so: $(FBINK_SO)
	@mkdir -p build
	cp "$(FBINK_SO)" build/libfbink.so

build/libz.so: $(ZLIB_SO)
	@mkdir -p build
	cp "$(ZLIB_SO)" build/libz.so

$(MBEDTLS_KOBO): include/kd_mbedtls_config.h
	$(PYTHON) ../scripts/build_mbedtls.py kobo

$(MBEDTLS_HOST): include/kd_mbedtls_config.h
	$(PYTHON) ../scripts/build_mbedtls.py host

kobo: build/kobodash

build/kobodash: $(KOBO_SRCS) include/kobodash.h include/kd_tls.h include/fbink.h include/kd_zlib.h include/stb_truetype.h build/libfbink.so build/libz.so $(MBEDTLS_KOBO)
	@mkdir -p build
	$(ZIG) cc $(KOBO_TARGET) $(CFLAGS_COMMON) $(KOBO_LDFLAGS) -s \
		-Lbuild -Wl,-rpath,/mnt/onboard/.adds/kobodash/lib:/mnt/onboard/.adds/dashboard/lib \
		$(KOBO_SRCS) $(MBEDTLS_KOBO) -lfbink -lz -lm -o build/kobodash
	@ls -lh build/kobodash

build/test_kobodash: $(HOST_TEST_SRCS) include/kobodash.h include/kd_tls.h include/kd_zlib.h include/stb_truetype.h $(MBEDTLS_HOST)
	@mkdir -p build
	$(HOST_CC) $(CFLAGS_COMMON) $(HOST_TEST_SRCS) $(MBEDTLS_HOST) -lz -lm -o build/test_kobodash

test: build/test_kobodash
	./build/test_kobodash

preview: build/test_kobodash
	@mkdir -p ../output/firmware-preview
	./build/test_kobodash --dump-dir ../output/firmware-preview
	$(PYTHON) ../scripts/preview_firmware.py ../output/firmware-preview

deploy: kobo
	$(PYTHON) ../scripts/deploy_firmware.py

clean:
	rm -rf build
