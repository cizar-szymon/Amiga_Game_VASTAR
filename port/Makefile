# Vastar port - PC build (MSYS2 UCRT64: gcc, SDL2, pkg-config)
#
#   make            build ../build/port/vastar.exe and z80_test.exe
#   make test       run the Z80 instruction tests (needs ZEX=dir with zexdoc.cim, zexall.cim)

OUT      := ../build/port

CC       := gcc
CFLAGS   := -std=c99 -O2 -Wall -Wextra -Isrc
SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2) -mconsole

CORE     := src/z80.c src/ay8910.c src/vastar.c src/video.c src/romload.c
HEADERS  := $(wildcard src/*.h)

all: $(OUT)/vastar.exe $(OUT)/z80_test.exe $(OUT)/SDL2.dll

# so that vastar.exe also starts outside the MSYS2 shell
$(OUT)/SDL2.dll: | $(OUT)
	cp "$$(dirname "$$(command -v sdl2-config)")/SDL2.dll" $@

$(OUT):
	mkdir -p $@

$(OUT)/vastar.exe: $(CORE) src/main_sdl.c $(HEADERS) | $(OUT)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(filter %.c,$^) $(SDL_LIBS) -o $@

$(OUT)/z80_test.exe: src/z80.c tests/z80_test.c src/z80.h | $(OUT)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

ZEX ?= tests
test: $(OUT)/z80_test.exe
	$(OUT)/z80_test.exe $(ZEX)/zexdoc.cim
	$(OUT)/z80_test.exe $(ZEX)/zexall.cim

.PHONY: all test
