# SPDX-License-Identifier: CC0-1.0
#
# Builds this project's first custom ARM7 core (see main_arm7.c's own
# comment for why: the prebuilt arm7_minimal.elf Makefile.game used before
# has no DSWiFi support, and only the ARM7 CPU can talk to the WiFi chip on
# this console). Invoked as a sub-make from ../Makefile.game, the same way
# BlocksDS's own project templates build their ARM7 and ARM9 halves as two
# separate sub-makes joined by ndstool rather than one Makefile compiling
# both (they target different CPUs, with different specs/crt0/linker
# scripts -- not something to fold into the ARM9-side Makefile.game).
#
# Uses the same $(BLOCKSDS)/$(WONDERFUL_TOOLCHAIN) exports and toolchain
# prefix Makefile.game already does. The ?= default below only matters if
# BLOCKSDS is not already exported by the environment -- the CI container
# actually exports BLOCKSDS=/opt/wonderful/thirdparty/blocksds/core itself
# (confirmed from a real build log), which wins over this default either
# way; it is only here for a shell that does not set it at all.
export BLOCKSDS			?= /opt/blocksds/core
export WONDERFUL_TOOLCHAIN	?= /opt/wonderful
ARM_NONE_EABI_PATH		?= $(WONDERFUL_TOOLCHAIN)/toolchain/gcc-arm-none-eabi/bin/

SOURCEDIRS	:=	.
# zlib, deflate-only: adler32.c/deflate.c/trees.c/zutil.c are the minimal
# set deflate() actually needs (confirmed against external/zlib/deflate.c's
# own #includes) -- this ARM7 build only ever compresses, never
# decompresses, so inflate*.c/infback.c/compress.c/gz*.c (the gzip
# wrapper/CLI helpers) are deliberately left out, both for ARM7 code-size
# headroom and because pulling in code this side of the link never calls
# is pure risk for no benefit. See arm7_chunk_compress.c's own comment for
# why windowBits/memLevel are also cut down from the ARM9 side's defaults.
#
# crc32.c is ALSO left out, but that needs -DNO_GZIP below to actually hold:
# deflate.c's gzip-header code path (state->wrap==2) is compiled in by
# default (deflate.h: "#ifndef NO_GZIP / #define GZIP") and calls crc32()
# from several functions (deflateResetKeep(), read_buf(), deflate() itself)
# -- the call is part of the compiled object code regardless of the RUNTIME
# wrap value this project actually uses (deflateInit2()'s windowBits=12 is
# positive, i.e. the zlib-wrapper format, never gzip), so without -DNO_GZIP
# the linker still demands a crc32 symbol that was never pulled in. Confirmed
# via a real CI link failure: "deflate.c:(.text.deflate+0x274): undefined
# reference to `crc32'". -DNO_GZIP (deflate.h's own documented, intended
# escape hatch for exactly this) compiles that whole code path out instead
# of pulling in crc32.c just to satisfy a reference nothing here ever
# exercises.
ZLIBDIR		:=	../../../external/zlib
ZLIB_SOURCES	:=	$(ZLIBDIR)/adler32.c $(ZLIBDIR)/deflate.c \
			$(ZLIBDIR)/trees.c $(ZLIBDIR)/zutil.c
INCLUDEDIRS	:=	$(ZLIBDIR) build/zconf
# ARM7 (not ARM9): nds.h itself #errors out without exactly one of these --
# the BlocksDS/libnds headers this file includes use it to pick the ARM7 vs
# ARM9 half of every dual-CPU register/function declaration they provide
# (REG_DMA_FILL, IRQ_RTC, installSoundFIFO(), ... all the symbols main_arm7.c
# needs), not just a cosmetic label.
DEFINES		:=	-D__NDS__ -DARM7 -DNO_GZIP
LIBS		:=	-lnds7 -ldswifi7 -lc
LIBDIRS		:=	$(BLOCKSDS)/libs/libnds $(BLOCKSDS)/libs/dswifi

# zconf.h is generated (never checked into the zlib submodule itself) --
# same two-line sed substitution as src/dsi/Makefile.game's own ARM9-side
# copy of this exact step; see that file's comment for why #cmakedefine
# needs this instead of zlib's own absent-a-CMake-run configure step, and
# why $(shell ...) (not a prerequisite rule) is what actually runs it before
# the rest of this file is parsed.
$(shell mkdir -p build/zconf && sed \
	-e 's/#cmakedefine Z_PREFIX/\/* #undef Z_PREFIX *\//' \
	-e 's/#cmakedefine Z_HAVE_UNISTD_H/#define Z_HAVE_UNISTD_H/' \
	$(ZLIBDIR)/zconf.h.cmakein > build/zconf/zconf.h)

NAME	:= dsi_arm7
BUILDDIR	:= build
ELF		:= $(BUILDDIR)/$(NAME).elf
MAP		:= $(BUILDDIR)/$(NAME).map

PREFIX	:= $(ARM_NONE_EABI_PATH)arm-none-eabi-
CC		:= $(PREFIX)gcc
LD		:= $(CC)

ARCH		:= -mcpu=arm7tdmi -mtune=arm7tdmi
WARNFLAGS	:= -Wall

C_FILES		:= $(foreach dir,$(SOURCEDIRS),$(wildcard $(dir)/*.c)) $(ZLIB_SOURCES)

INCLUDEFLAGS	:= $(foreach path,$(INCLUDEDIRS),-I$(path)) \
		   $(foreach path,$(LIBDIRS),-I$(path)/include)

LIBDIRSFLAGS	:= $(foreach path,$(LIBDIRS),-L$(path)/lib)

CFLAGS		:= -std=gnu11 $(WARNFLAGS) $(DEFINES) $(ARCH) \
		   -mthumb -mthumb-interwork $(INCLUDEFLAGS) -O2 \
		   -ffunction-sections -fdata-sections \
		   -fomit-frame-pointer

LDFLAGS		:= -mthumb -mthumb-interwork $(LIBDIRSFLAGS) \
		   -Wl,-Map,$(MAP) -Wl,--gc-sections -nostdlib \
		   -T$(BLOCKSDS)/sys/crts/ds_arm7.ld \
		   -Wl,--no-warn-rwx-segments \
		   -Wl,--start-group $(LIBS) -lgcc -Wl,--end-group

OBJS		:= $(addprefix $(BUILDDIR)/,$(notdir $(C_FILES:%.c=%.o)))
DEPS		:= $(OBJS:.o=.d)
export VPATH	:= $(SOURCEDIRS) $(ZLIBDIR)

.PHONY: all clean

all: $(BUILDDIR) $(ELF)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(ELF): $(OBJS)
	$(LD) -o $@ $(OBJS) $(BLOCKSDS)/sys/crts/ds_arm7_crt0.o $(LDFLAGS)

clean:
	rm -rf $(BUILDDIR)

$(BUILDDIR)/%.o : %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)
