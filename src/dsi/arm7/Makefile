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
# prefix Makefile.game already does, not ClassiCube's reference copy of this
# same file (which assumes the Wonderful Toolchain package manager's nested
# install path) -- this project's Docker image installs BlocksDS straight to
# /opt/blocksds/core, already relied on throughout Makefile.game.
export BLOCKSDS			?= /opt/blocksds/core
export WONDERFUL_TOOLCHAIN	?= /opt/wonderful
ARM_NONE_EABI_PATH		?= $(WONDERFUL_TOOLCHAIN)/toolchain/gcc-arm-none-eabi/bin/

SOURCEDIRS	:=	.
INCLUDEDIRS	:=
LIBS		:=	-lnds7 -ldswifi7 -lc
LIBDIRS		:=	$(BLOCKSDS)/libs/libnds $(BLOCKSDS)/libs/dswifi

NAME	:= dsi_arm7
BUILDDIR	:= build
ELF		:= $(BUILDDIR)/$(NAME).elf
MAP		:= $(BUILDDIR)/$(NAME).map

PREFIX	:= $(ARM_NONE_EABI_PATH)arm-none-eabi-
CC		:= $(PREFIX)gcc
LD		:= $(CC)

ARCH		:= -mcpu=arm7tdmi -mtune=arm7tdmi
WARNFLAGS	:= -Wall

C_FILES		:= $(foreach dir,$(SOURCEDIRS),$(wildcard $(dir)/*.c))

INCLUDEFLAGS	:= $(foreach path,$(INCLUDEDIRS),-I$(path)) \
		   $(foreach path,$(LIBDIRS),-I$(path)/include)

LIBDIRSFLAGS	:= $(foreach path,$(LIBDIRS),-L$(path)/lib)

CFLAGS		:= -std=gnu11 $(WARNFLAGS) $(ARCH) \
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
export VPATH	:= $(SOURCEDIRS)

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
