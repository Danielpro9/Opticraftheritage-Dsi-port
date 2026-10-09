# SPDX-License-Identifier: CC0-1.0
#
# BlocksDS bring-up Makefile for the DSi port, based on BlocksDS's
# templates/rom_arm9_only. Needs BlocksDS + Wonderful Toolchain, not
# devkitARM/devkitPro -- see the readme note below for why.
#
#   cd src/dsi && make
#
# Install (Linux/WSL): https://wonderful.asie.pl/wiki/doku.php?id=getting_started
# then:
#   export PATH=/opt/wonderful/bin:$PATH
#   wf-pacman -Syu wf-tools && wf-config repo enable blocksds && wf-pacman -Syu
#   wf-pacman -S blocksds-toolchain
#   ln -s /opt/wonderful/thirdparty/blocksds /opt/blocksds
#
# Produces DsiBringup.nds.

export BLOCKSDS			?= /opt/blocksds/core
export BLOCKSDSEXT		?= /opt/blocksds/external

export WONDERFUL_TOOLCHAIN	?= /opt/wonderful
ARM_NONE_EABI_PATH	?= $(WONDERFUL_TOOLCHAIN)/toolchain/gcc-arm-none-eabi/bin/

NAME		:= DsiBringup

# tools/ has the smoke-test main; system/ has the early video/memory modules it
# calls into (DsiEarlyVideo.cpp, unused by the smoke test itself, compiles here
# too so the "real game" init code is proven against the real toolchain).
SOURCEDIRS	:= tools system
# ".." is src/ -- both this port's own headers ("dsi/...") and the shared
# "platform/Log.h" this code logs through are rooted there.
#
# ../../external/zlib + the generated build/$(NAME)/zconf dir: system/
# is glob-included below (SOURCEDIRS), so system/DsiArm7Compress.cpp's own
# "#include <zlib.h>" (for compressBound() -- see that file's own comment)
# needs to resolve here too, same as Makefile.game's own copy of this exact
# include-path/zconf-generation pair.
INCLUDEDIRS	:= .. ../../external/zlib build/$(NAME)/zconf
# No graphics/audio/NitroFS asset pipeline yet -- this Makefile only has S/C/CPP
# compile rules below. Copy those sections from
# $(BLOCKSDS)/../templates/rom_arm9_only/Makefile (grit for GFXDIRS, mmutil for
# AUDIODIRS, bin2c for BINDIRS) once real assets exist.

DEFINES		:= -DDSI_PLATFORM=1

# See Makefile.game's own copy of this exact step for the full rationale
# (zlib's zconf.h is generated from zconf.h.cmakein, never checked in).
$(shell mkdir -p build/$(NAME)/zconf && sed \
	-e 's/#cmakedefine Z_PREFIX/\/* #undef Z_PREFIX *\//' \
	-e 's/#cmakedefine Z_HAVE_UNISTD_H/#define Z_HAVE_UNISTD_H/' \
	../../external/zlib/zconf.h.cmakein > build/$(NAME)/zconf/zconf.h)

# No custom ARM7 code and no DSWiFi/audio yet, so the prebuilt "libnds only"
# core is enough. Swap to arm7_dswifi_maxmod.elf once networking/audio land.
ARM7ELF		:= $(BLOCKSDS)/sys/arm7/main_core/arm7_minimal.elf

LIBS		:= -lnds9
LIBDIRS		:= $(BLOCKSDS)/libs/libnds

BUILDDIR	:= build/$(NAME)
ELF		:= build/$(NAME).elf
DUMP		:= build/$(NAME).dump
MAP		:= build/$(NAME).map
ROM		:= $(NAME).nds

PREFIX		:= $(ARM_NONE_EABI_PATH)arm-none-eabi-
CC		:= $(PREFIX)gcc
CXX		:= $(PREFIX)g++
LD		:= $(PREFIX)gcc
OBJDUMP		:= $(PREFIX)objdump
MKDIR		:= mkdir
RM		:= rm -rf

ifeq ($(VERBOSE),1)
V		:=
else
V		:= @
endif

SOURCES_S	:= $(shell find -L $(SOURCEDIRS) -name "*.s")
SOURCES_C	:= $(shell find -L $(SOURCEDIRS) -name "*.c")
# compress.c alone: the one zlib symbol system/DsiArm7Compress.cpp's
# compressBound() call needs (plain arithmetic, no deps on deflate/inflate)
# -- not the full zlib source list Makefile.game links for the real game,
# since nothing else in this smoke-test build calls into zlib.
SOURCES_C	+= ../../external/zlib/compress.c
SOURCES_CPP	:= $(shell find -L $(SOURCEDIRS) -name "*.cpp")
# These specific src/platform files are safe to pull in on their own, unlike
# the rest of that directory: most of it assumes PC/PS2/Wii headers (SDL2,
# glad, gccore.h, ...) this Makefile does not provide. Their own #if
# PLATFORM_WII/PS2_PLATFORM branches are inert here (DSI_PLATFORM is defined,
# not those). The literal ".." in each path is intentional -- see BUILDDIR
# note below.
#   Log.cpp              -- the shared logger DsiEarly*.cpp call through
#                            (MC_LOG_INFO).
#   storage/PathUtils.cpp -- plain string helpers (join/parent/...), no OS
#                            calls, shared by every platform already.
#   storage/PosixFileSystem.cpp -- generic fopen/stat/opendir-based file I/O,
#                            now DSi's too -- see docs/guides/filesystem.md in
#                            the BlocksDS repo for why that POSIX surface is
#                            expected to be there.
#   RenderAPI_DSI.cpp, Diagnostics_DSI.cpp, Resources_DSI.cpp,
#   TextureResidencyPolicy_DSI.cpp, PlatformUserSettings_DSI.cpp,
#   ScreenshotBackend_DSI.cpp, Profiler_DSI.cpp, ClientProfilerBackend_DSI.cpp,
#   RenderLightingProfile_DSI.cpp, RenderTerrainAPI_DSI.cpp,
#   audio/SoundManager_DSI.cpp, InputBackend_DSI.cpp,
#   LegacyControlPromptBackend_DSI.cpp, GameSettingsBackend_DSI.cpp,
#   ClientPlatformPolicy_DSI.cpp
#                        -- small platform backends, unused by the bring-up
#                           smoke test itself but compiled here so they are
#                           proven against the real toolchain, same reasoning
#                           as DsiEarlyVideo.cpp.
SOURCES_CPP	+= ../platform/Log.cpp \
		   ../platform/storage/PathUtils.cpp \
		   ../platform/storage/PosixFileSystem.cpp \
		   ../platform/storage/StorageBackend_DSI.cpp \
		   ../platform/RenderAPI_DSI.cpp \
		   ../platform/Diagnostics_DSI.cpp \
		   ../platform/Resources_DSI.cpp \
		   ../platform/TextureResidencyPolicy_DSI.cpp \
		   ../platform/PlatformUserSettings_DSI.cpp \
		   ../platform/ScreenshotBackend_DSI.cpp \
		   ../platform/Profiler_DSI.cpp \
		   ../platform/ClientProfilerBackend_DSI.cpp \
		   ../platform/RenderLightingProfile_DSI.cpp \
		   ../platform/RenderTerrainAPI_DSI.cpp \
		   ../platform/audio/SoundManager_DSI.cpp \
		   ../platform/InputBackend_DSI.cpp \
		   ../platform/LegacyControlPromptBackend_DSI.cpp \
		   ../platform/GameSettingsBackend_DSI.cpp \
		   ../platform/ClientPlatformPolicy_DSI.cpp

DEFINES		+= -D__NDS__ -D__BLOCKSDS__ -DARM9 -DNO_SOUND

ARCH		:= -mthumb -mcpu=arm946e-s+nofp

# The one line that actually unlocks the DSi's extended RAM: ds_arm9.specs
# targets the classic ~4 MB NDS-compatible layout, dsi_arm9.specs targets the
# 16 MB DSi layout (see docs/guides/usage_notes.md "ARM9 options" and
# docs/internal/memory_map.md section 4.1 in the BlocksDS repo). This is what
# DsiEarlyMemory.cpp's isDSiMode()/getHeapLimit() readout at boot is there to
# confirm actually landed, since it depends on how the .nds is launched too
# (DSi Menu / a loader that respects the DSi-enhanced header), not just on how
# it was compiled.
SPECS		:= $(BLOCKSDS)/sys/crts/dsi_arm9.specs

WARNFLAGS	:= -Wall

ifeq ($(SOURCES_CPP),)
	LIBS	+= -lc
else
	LIBS	+= -lstdc++ -lc
endif

INCLUDEFLAGS	:= $(foreach path,$(INCLUDEDIRS),-I$(path)) \
		   $(foreach path,$(LIBDIRS),-I$(path)/include)

LIBDIRSFLAGS	:= $(foreach path,$(LIBDIRS),-L$(path)/lib)

ASFLAGS		+= -x assembler-with-cpp $(INCLUDEFLAGS) $(DEFINES) \
		   $(ARCH) -ffunction-sections -fdata-sections \
		   -specs=$(SPECS)

CFLAGS		+= $(WARNFLAGS) $(INCLUDEFLAGS) $(DEFINES) \
		   $(ARCH) -O2 -ffunction-sections -fdata-sections \
		   -specs=$(SPECS)

CXXFLAGS	+= $(WARNFLAGS) $(INCLUDEFLAGS) $(DEFINES) \
		   $(ARCH) -O2 -ffunction-sections -fdata-sections \
		   -std=gnu++17 \
		   -specs=$(SPECS)

# --gc-sections: needed now that compress.c (added above, for
# compressBound()) is linked straight in rather than from an archive -- a
# plain .o on the command line pulls in ALL of its functions regardless of
# whether anything calls them, so without this, compress.c's own unused
# compress()/compress2() would demand deflate()/deflateInit2()/deflateEnd()
# (confirmed via a real CI link failure) even though nothing in this
# smoke-test build ever calls them. $(CFLAGS)'s existing -ffunction-sections
# already puts each function in its own section; this is the other half
# that actually discards the ones nothing references.
LDFLAGS		+= $(ARCH) $(LIBDIRSFLAGS) -Wl,-Map,$(MAP) $(DEFINES) \
		   -Wl,--gc-sections \
		   -Wl,--start-group $(LIBS) -Wl,--end-group -specs=$(SPECS)

OBJS_SOURCES	:= $(addsuffix .o,$(addprefix $(BUILDDIR)/,$(SOURCES_S))) \
		   $(addsuffix .o,$(addprefix $(BUILDDIR)/,$(SOURCES_C))) \
		   $(addsuffix .o,$(addprefix $(BUILDDIR)/,$(SOURCES_CPP)))

OBJS		:= $(OBJS_SOURCES)

DEPS		:= $(OBJS:.o=.d)

.PHONY: all clean dump

all: $(ROM)

# No -b/icon/title-banner: there is no artwork yet and the icon is optional
# (BlocksDS docs/internal/build_process.md section 5). Add GAME_TITLE/
# GAME_SUBTITLE/GAME_AUTHOR/GAME_ICON and pass
# "-b $(GAME_ICON) \"$(GAME_TITLE);$(GAME_SUBTITLE);$(GAME_AUTHOR)\""
# once real art exists.
$(ROM): $(ELF)
	@echo "  NDSTOOL $@"
	$(V)$(BLOCKSDS)/tools/ndstool/ndstool -c $@ \
		-7 $(ARM7ELF) -9 $(ELF)

$(ELF): $(OBJS)
	@echo "  LD      $@"
	$(V)$(LD) -o $@ $(OBJS) $(LDFLAGS)

$(DUMP): $(ELF)
	@echo "  OBJDUMP   $@"
	$(V)$(OBJDUMP) -h -C -S $< > $@

dump: $(DUMP)

clean:
	@echo "  CLEAN"
	$(V)$(RM) $(ROM) $(DUMP) build compile_commands.json

$(BUILDDIR)/%.s.o : %.s
	@echo "  AS      $<"
	@$(MKDIR) -p $(@D)
	$(V)$(CC) $(ASFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR)/%.c.o : %.c
	@echo "  CC      $<"
	@$(MKDIR) -p $(@D)
	$(V)$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR)/%.cpp.o : %.cpp
	@echo "  CXX     $<"
	@$(MKDIR) -p $(@D)
	$(V)$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)
