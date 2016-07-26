CC = gcc
CXX = g++

CFLAGS = -W -Wall -Wextra -ansi -pedantic -lm -pthread -Wno-unused-function
CXXFLAGS = -W -Wall -Wextra -ansi -pedantic -std=gnu++11 -pthread

#It's recommended to double-compile zopfli by first adding -fprofile-generate, running it on
#some file with 5000 iterations and using master thread only (--t0). After initial run
#it should recompiled with -fprofile-use instead. This usually better optimizes the code
#speeding up zopfli by ~8% than one-time compile without mentioned tweak.
#
#Fine-tune switches listed below are found to be the best on ARMv7 CPU, other architectures may
#require other switches added or listed ones elminated/negated.

ZDEBUG  = -O0 -g
ZDEFOPT = -Ofast -D NDEBUG -fno-associative-math
ZARMOPT = -Ofast -D NDEBUG -fno-associative-math
ZADDOPT = -g0 -s -flto -fuse-linker-plugin -flto-partition=max -flto-compression-level=0 -ffat-lto-objects -fgraphite-identity -floop-nest-optimize
CAVXFLAGS = -mavx -mtune=corei7-avx -march=corei7-avx
CNEONFLAGS = -march=armv7-a -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb-interwork -mno-unaligned-access -mneon-for-64bits -mstructure-size-boundary=32 -fno-tree-slp-vectorize -fno-crossjumping -ftracer -ftree-loop-ivcanon -fno-tree-loop-distribution -fselective-scheduling2 -fsel-sched-pipelining -fira-region=all -free -fno-cx-limited-range -fno-defer-pop -fno-function-cse -fno-sched-interblock -fno-sched-last-insn-heuristic -fno-sel-sched-pipelining-outer-loops -fno-tree-fre -fno-tree-loop-im -fno-zero-initialized-in-bss -fno-ipa-reference -fno-ipa-cp -fbranch-target-load-optimize2 -ffunction-sections -fdata-sections

ZOPFLILIB_SRC = src/zopfli/blocksplitter.c src/zopfli/cache.c\
                src/zopfli/inthandler.c src/zopfli/deflate.c\
                src/zopfli/crc32.c src/zopfli/gzip_container.c\
                src/zopfli/zip_container.c src/zopfli/hash.c\
                src/zopfli/katajainen.c src/zopfli/lz77.c\
                src/zopfli/squeeze.c src/zopfli/tree.c\
                src/zopfli/util.c src/zopfli/adler.c\
                src/zopfli/zlib_container.c src/zopfli/zopfli_lib.c
ZOPFLILIB_OBJ := $(patsubst src/zopfli/%.c,%.o,$(ZOPFLILIB_SRC))
ZOPFLIBIN_SRC := src/zopfli/zopfli_bin.c
LODEPNG_SRC := src/zopflipng/lodepng/lodepng.cpp src/zopflipng/lodepng/lodepng_util.cpp
ZOPFLIPNGLIB_SRC := src/zopflipng/zopflipng_lib.cc
ZOPFLIPNGBIN_SRC := src/zopflipng/zopflipng_bin.cc

.PHONY: zopfli zopflipng

# Zopfli binary
zopfli:
	$(CC) -static -D NLIB $(ZOPFLILIB_SRC) $(ZOPFLIBIN_SRC) $(CFLAGS) $(ZDEFOPT) $(ZADDOPT) -o zopfli

zopfliavx:
	$(CC) -static -D NLIB $(ZOPFLILIB_SRC) $(ZOPFLIBIN_SRC) $(CFLAGS) $(ZDEFOPT) $(CAVXFLAGS) $(ZADDOPT) -o zopfli

zopflineon:
	$(CC) -static -D NLIB $(ZOPFLILIB_SRC) $(ZOPFLIBIN_SRC) $(CFLAGS) $(ZARMOPT) $(CNEONFLAGS) $(ZADDOPT) -o zopfli

zopflidebug:
	$(CC) -D NLIB $(ZOPFLILIB_SRC) $(ZOPFLIBIN_SRC) $(CFLAGS) $(ZDEBUG) -o zopfli

defdbparser:
	$(CC) -static $(DEFDBPARSER_SRC) $(ZARMOPT) -o defdbparser

testlib:
	$(CC) src/libtest/libtest.c -ldl -lpsapi $(CFLAGS) $(ZDEFOPT) $(ZADDOPT) -o zopflitest

# Zopfli shared library
libzopfli:
	$(CC) $(ZOPFLILIB_SRC) $(CFLAGS) $(ZDEFOPT) $(ZADDOPT) -fPIC -c
	$(CC) $(ZOPFLILIB_OBJ) $(CFLAGS) $(ZDEFOPT) $(ZADDOPT) -fPIC -shared -Wl,-soname,libzopfli.so.1 -o libzopfli.so.1.0.1

libzopfliavx:
	$(CC) $(ZOPFLILIB_SRC) $(CFLAGS) $(ZDEFOPT) $(CAVXFLAGS) $(ZADDOPT)  -c
	$(CC) $(ZOPFLILIB_OBJ) $(CFLAGS) $(ZDEFOPT) $(CAVXFLAGS) $(ZADDOPT) -shared -Wl,-soname,libzopfli.so.1 -o libzopfli.so.1.0.1

libzopflineon:
	$(CC) $(ZOPFLILIB_SRC) $(CFLAGS) $(ZARMOPT) $(CNEONFLAGS) $(ZADDOPT)  -c
	$(CC) $(ZOPFLILIB_OBJ) $(CFLAGS) $(ZARMOPT) $(CNEONFLAGS) $(ZADDOPT) -shared -Wl,-soname,libzopfli.so.1 -o libzopfli.so.1.0.1

# ZopfliPNG binary
zopflipng:
	$(CC) -D NLIB $(ZOPFLILIB_SRC) $(CFLAGS) $(ZDEFOPT) $(ZADDOPT) -c
	$(CXX) -static -static-libgcc -D NLIB $(ZOPFLILIB_OBJ) $(LODEPNG_SRC) $(ZOPFLIPNGLIB_SRC) $(ZOPFLIPNGBIN_SRC) $(CXXFLAGS) $(ZDEFOPT) $(ZADDOPT) -o zopflipng
	
zopflipngavx:
	$(CC) -D NLIB $(ZOPFLILIB_SRC) $(CFLAGS) $(ZDEFOPT) $(CAVXFLAGS) $(ZADDOPT) -c
	$(CXX) -static -static-libgcc -D NLIB $(ZOPFLILIB_OBJ) $(LODEPNG_SRC) $(ZOPFLIPNGLIB_SRC) $(ZOPFLIPNGBIN_SRC) $(CXXFLAGS) $(ZDEFOPT) $(CAVXFLAGS) $(ZADDOPT) -o zopflipng

zopflipngneon:
	$(CC) -D NLIB $(ZOPFLILIB_SRC) $(CFLAGS) $(ZARMOPT) $(CNEONFLAGS) $(ZADDOPT) -c
	$(CXX) -static -static-libgcc -D NLIB $(ZOPFLILIB_OBJ) $(LODEPNG_SRC) $(ZOPFLIPNGLIB_SRC) $(ZOPFLIPNGBIN_SRC) $(CXXFLAGS) $(ZARMOPT) $(CNEONFLAGS) $(ZADDOPT) -o zopflipng

zopflipngdebug:
	$(CC) -D NLIB $(ZOPFLILIB_SRC) $(CFLAGS) $(ZDEBUG) -c
	$(CXX) -D NLIB $(ZOPFLILIB_OBJ) $(LODEPNG_SRC) $(ZOPFLIPNGLIB_SRC) $(ZOPFLIPNGBIN_SRC) $(CXXFLAGS) $(ZDEBUG) -o zopflipng

# ZopfliPNG shared library
libzopflipng:
	$(CC) $(ZOPFLILIB_SRC) $(CFLAGS) $(ZDEFOPT) $(ZADDOPT) -fPIC -c
	$(CXX) $(ZOPFLILIB_OBJ) $(LODEPNG_SRC) $(ZOPFLIPNGLIB_SRC) $(CXXLAGS) $(ZDEFOPT) $(ZADDOPT) -fPIC --shared -Wl,-soname,libzopflipng.so.1 -o libzopflipng.so.1.0.0

# Remove all libraries and binaries
clean:
	rm -f zopflipng zopfli $(ZOPFLILIB_OBJ) libzopfli*
