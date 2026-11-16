# brickio: USB audio, MIDI and sync for the sydilaxe TrimUI apps
#   make test     the host tests (ASan / UBSan)
#   make check    every module compiled for the host and, with zig, for the Brick (aarch64, glibc 2.28)
#   make version
# zig: ZIG=/path/to/zig, or `pip install ziglang==0.14.1` (used as `python3 -m ziglang`)

ZIG      ?= $(shell command -v zig 2>/dev/null || echo python3 -m ziglang)
HOST_CC  ?= cc
SDL_INC  := third_party/SDL2-2.30.9-headers
SRC      := $(wildcard src/*.c)
HDR      := $(wildcard src/*.h)
WARN     := -Wall -Wextra -Wno-missing-field-initializers
TFLAGS   := -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined
TESTS    := test_aring test_duplex test_link test_linkpos test_midi test_sync

.PHONY: test check cross version
test: $(addprefix build/,$(TESTS))
	@for t in $(TESTS); do echo "== $$t"; build/$$t || exit 1; done

build/test_aring: tests/test_aring.c src/aring.c $(HDR)
	@mkdir -p build
	$(HOST_CC) $(TFLAGS) -o $@ tests/test_aring.c src/aring.c -lm
build/test_duplex: tests/test_duplex.c src/duplex.c $(HDR)
	@mkdir -p build
	$(HOST_CC) $(TFLAGS) -o $@ tests/test_duplex.c src/duplex.c -ldl -lpthread
build/test_link: tests/test_link.c tests/golden_link.h src/link.c $(HDR)
	@mkdir -p build
	$(HOST_CC) $(TFLAGS) -o $@ tests/test_link.c src/link.c -lpthread
build/test_linkpos: tests/test_linkpos.c tests/golden_linkpos.h src/linkpos.c $(HDR)
	@mkdir -p build
	$(HOST_CC) $(TFLAGS) -o $@ tests/test_linkpos.c src/linkpos.c -lm
build/test_midi: tests/test_midi.c src/midi.c src/linkpos.c $(HDR)
	@mkdir -p build
	$(HOST_CC) $(TFLAGS) -o $@ tests/test_midi.c src/midi.c src/linkpos.c -lpthread -lm
build/test_sync: tests/test_sync.c src/sync.c src/aclock.c $(HDR)
	@mkdir -p build
	$(HOST_CC) $(TFLAGS) -o $@ tests/test_sync.c src/sync.c src/aclock.c -lm

# every module on its own, as an app compiles it
check:
	@for f in $(SRC); do $(HOST_CC) -std=gnu11 $(WARN) -Werror -I$(SDL_INC) -fsyntax-only $$f || exit 1; done; echo "host: ok"
	@$(MAKE) -s cross
cross:
	@mkdir -p build/aarch64
	@for f in $(SRC); do $(ZIG) cc -target aarch64-linux-gnu.2.28 -mcpu=cortex_a53 -std=gnu11 -O2 $(WARN) -Werror -I$(SDL_INC) \
		-c -o build/aarch64/$$(basename $$f .c).o $$f || exit 1; done; echo "aarch64 (glibc 2.28): ok"

version:
	@cat VERSION
