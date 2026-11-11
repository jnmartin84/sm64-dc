# Makefile to rebuild SM64 split image

### Default target ###

KOS_ROMDISK_DIR = romdisk

default: all

### Build Options ###

# These options can either be changed by modifying the makefile, or
# by building with 'make SETTING=value'. 'make clean' may be required.

# Version of the game to build
VERSION ?= us
# Graphics microcode used
GRUCODE ?= f3d_old
# If COMPARE is 1, check the output sha1sum when building 'all'
COMPARE ?= 1
# If NON_MATCHING is 1, define the NON_MATCHING and AVOID_UB macros when building (recommended)
NON_MATCHING ?= 0
# Build for the N64 (turn this off for ports)
TARGET_N64 ?= 0
# Build for Emscripten/WebGL
TARGET_WEB ?= 0
# Build for PSP
TARGET_PSP ?= 0
# Build for Dreamcast
TARGET_DC ?= 0
# Render at 240p (320x240) instead of 480i/VGA (640x480) on Dreamcast
LOWRES ?= 0
# Compiler to use (ido or gcc)
#COMPILER ?= ido

# Checks for local vs system hexdump
ifeq (, $(shell which hexdump))
$(warning "system hexdump is not available please provide a local binary")
  HEXD_2 := $(shell ./hexdump -h 2> /dev/null)
  ifndef HEXD_2
    $(error "local hexdump is not available please install systemwide or provide local a binary")
  endif
  HEXDUMP := ./hexdump
else
HEXDUMP := hexdump
endif

# Add a version tag to all builds
SRC_VER := $(shell git describe --always --abbrev=8 --dirty 2>/dev/null 2>/dev/null || date "+%Y%m%d-%H%M%S")

# Automatic settings only for ports
ifeq ($(TARGET_N64),0)

  NON_MATCHING := 1
  GRUCODE := f3dex2e
  TARGET_WINDOWS := 0
  ifeq ($(TARGET_PSP), 0)
  ifeq ($(TARGET_DC), 0)
  ifeq ($(TARGET_WEB),0)
    ifeq ($(OS),Windows_NT)
      TARGET_WINDOWS := 1
    else
      # TODO: Detect Mac OS X, BSD, etc. For now, assume Linux
      ifneq ($(TARGET_PSP), 1)
      ifneq ($(TARGET_DC), 1)
        TARGET_LINUX := 1
      endif
      endif
    endif
  endif
  endif

  ifeq ($(TARGET_WINDOWS),1)
    # On Windows, default to DirectX 11
    ifneq ($(ENABLE_OPENGL),1)
      ifneq ($(ENABLE_DX12),1)
        ENABLE_DX11 ?= 1
      endif
    endif
  else
    # On others, default to OpenGL
    ENABLE_OPENGL ?= 1
  endif

  # Sanity checks
  ifeq ($(ENABLE_DX11),1)
    ifneq ($(TARGET_WINDOWS),1)
      $(error The DirectX 11 backend is only supported on Windows)
    endif
    ifeq ($(ENABLE_OPENGL),1)
      $(error Cannot specify multiple graphics backends)
    endif
    ifeq ($(ENABLE_DX12),1)
      $(error Cannot specify multiple graphics backends)
    endif
  endif
  ifeq ($(ENABLE_DX12),1)
    ifneq ($(TARGET_WINDOWS),1)
      $(error The DirectX 12 backend is only supported on Windows)
    endif
    ifeq ($(ENABLE_OPENGL),1)
      $(error Cannot specify multiple graphics backends)
    endif
    ifeq ($(ENABLE_DX11),1)
      $(error Cannot specify multiple graphics backends)
    endif
  endif

  endif

#PSP DEFS
ifeq ($(TARGET_PSP), 1)
  ENABLE_OPENGL ?= 1
  NON_MATCHING := 1
endif
#Dreamcast DEFS
ifeq ($(TARGET_DC), 1)
  ENABLE_OPENGL ?= 1
  NON_MATCHING := 1
endif
endif

ifeq ($(COMPILER),gcc)
  NON_MATCHING := 1
endif

# Release

ifeq ($(VERSION),jp)
  VERSION_DEF := VERSION_JP
  GRUCODE_DEF := F3D_OLD
else
ifeq ($(VERSION),us)
  VERSION_DEF := VERSION_US
  GRUCODE_DEF := F3D_OLD
else
ifeq ($(VERSION),eu)
  VERSION_DEF := VERSION_EU
  GRUCODE_DEF := F3D_NEW
else
ifeq ($(VERSION),sh)
  $(warning Building SH is experimental and is prone to breaking. Try at your own risk.)
  VERSION_DEF := VERSION_SH
  GRUCODE_DEF := F3D_NEW
# TODO: GET RID OF THIS!!! We should mandate assets for Shindou like EU but we dont have the addresses extracted yet so we'll just pretend you have everything extracted for now.
  NOEXTRACT := 1 
else
  $(error unknown version "$(VERSION)")
endif
endif
endif
endif

TARGET := sm64.$(VERSION)
VERSION_CFLAGS := -D$(VERSION_DEF)
VERSION_ASFLAGS := --defsym $(VERSION_DEF)=1

# Microcode

ifeq ($(GRUCODE),f3dex) # Fast3DEX
  GRUCODE_DEF := F3DEX_GBI
  GRUCODE_ASFLAGS := --defsym F3DEX_GBI_SHARED=1
  TARGET := $(TARGET).f3dex
  COMPARE := 0
else
ifeq ($(GRUCODE), f3dex2) # Fast3DEX2
  GRUCODE_DEF := F3DEX_GBI_2
  GRUCODE_ASFLAGS := --defsym F3DEX_GBI_SHARED=1
  TARGET := $(TARGET).f3dex2
  COMPARE := 0
else
ifeq ($(GRUCODE), f3dex2e) # Fast3DEX2 Extended (for PC)
  GRUCODE_DEF := F3DEX_GBI_2E
  TARGET := $(TARGET).f3dex2e
  COMPARE := 0
else
ifeq ($(GRUCODE),f3d_new) # Fast3D 2.0H (Shindou)
  GRUCODE_DEF := F3D_NEW
  TARGET := $(TARGET).f3d_new
  COMPARE := 0
else
ifeq ($(GRUCODE),f3dzex) # Fast3DZEX (2.0J / Animal Forest - Dōbutsu no Mori)
  $(warning Fast3DZEX is experimental. Try at your own risk.)
  GRUCODE_DEF := F3DEX_GBI_2
  GRUCODE_ASFLAGS := --defsym F3DEX_GBI_SHARED=1
  TARGET := $(TARGET).f3dzex
  COMPARE := 0
endif
endif
endif
endif
endif

GRUCODE_CFLAGS := -D$(GRUCODE_DEF)
GRUCODE_ASFLAGS := $(GRUCODE_ASFLAGS) --defsym $(GRUCODE_DEF)=1

ifeq ($(NON_MATCHING),1)
  MATCH_CFLAGS := -DNON_MATCHING -DAVOID_UB
  MATCH_ASFLAGS := --defsym AVOID_UB=1
  COMPARE := 0
endif

################### Universal Dependencies ###################

# (This is a bit hacky, but a lot of rules implicitly depend
# on tools and assets, and we use directory globs further down
# in the makefile that we want should cover assets.)

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(MAKECMDGOALS),distclean)

# Make sure assets exist
NOEXTRACT ?= 0
ifeq ($(NOEXTRACT),0)
DUMMY != $(PYTHON) ./extract_assets.py $(VERSION) >&2 || echo FAIL
ifeq ($(DUMMY),FAIL)
  $(error Failed to extract assets)
endif
endif

# Make tools if out of date
DUMMY != $(MAKE) -s -C tools >&2 || echo FAIL
ifeq ($(DUMMY),FAIL)
  $(error Failed to build tools)
endif

endif
endif

################ Target Executable and Sources ###############

# BUILD_DIR is location where all build artifacts are placed
BUILD_DIR_BASE := build
ifeq ($(TARGET_N64),1)
  BUILD_DIR := $(BUILD_DIR_BASE)/$(VERSION)
else ifeq ($(TARGET_WEB),1)
  BUILD_DIR := $(BUILD_DIR_BASE)/$(VERSION)_web
else ifeq ($(TARGET_PSP),1)
  BUILD_DIR := $(BUILD_DIR_BASE)/$(VERSION)_psp
else ifeq ($(TARGET_DC),1)
  BUILD_DIR := $(BUILD_DIR_BASE)/$(VERSION)_dc
else
  BUILD_DIR := $(BUILD_DIR_BASE)/$(VERSION)_pc
endif

LIBULTRA := $(BUILD_DIR)/libultra.a
ifeq ($(TARGET_WEB),1)
EXE := $(BUILD_DIR)/$(TARGET).html
else
ifeq ($(TARGET_WINDOWS),1)
EXE := $(BUILD_DIR)/$(TARGET).exe
else
EXE := $(BUILD_DIR)/$(TARGET)
endif
endif
ROM := $(BUILD_DIR)/$(TARGET).z64
ELF := $(BUILD_DIR)/$(TARGET).elf
LD_SCRIPT := sm64.ld
MIO0_DIR := $(BUILD_DIR)/bin
SOUND_BIN_DIR := $(BUILD_DIR)/sound
TEXTURE_DIR := textures
ACTOR_DIR := actors
LEVEL_DIRS := $(patsubst levels/%,%,$(dir $(wildcard levels/*/header.h)))

# Directories containing source files
SRC_DIRS := src src/engine src/game src/audio src/menu src/buffers actors levels bin bin/$(VERSION) data assets
ASM_DIRS := lib
ifeq ($(TARGET_N64),1)
  ASM_DIRS := asm $(ASM_DIRS)
else
  SRC_DIRS := $(SRC_DIRS) src/pc src/pc/gfx src/pc/audio src/pc/controller
  ASM_DIRS :=
endif
BIN_DIRS := bin bin/$(VERSION)

ULTRA_SRC_DIRS := lib/src lib/src/math
ULTRA_ASM_DIRS := lib/asm lib/data
ULTRA_BIN_DIRS := lib/bin

GODDARD_SRC_DIRS := src/goddard src/goddard/dynlists

MIPSISET := -mips2
MIPSBIT := -32

ifeq ($(COMPILER),gcc)
  MIPSISET := -mips3
endif

ifeq ($(TARGET_N64),1)

ifeq ($(VERSION),eu)
  OPT_FLAGS := -O2
else
ifeq ($(VERSION),sh)
  OPT_FLAGS := -O2
else
  OPT_FLAGS := -g
endif
endif

  # Use a default opt flag for gcc
  ifeq ($(COMPILER),gcc)
    OPT_FLAGS := -Os -flto=auto -fipa-pta -fno-pic -fno-pie 
  endif

else
ifeq ($(TARGET_WEB),1)
  OPT_FLAGS := -O2 -g4 --source-map-base http://localhost:8080/
else
  OPT_FLAGS := -O2
endif
endif

# File dependencies and variables for specific files
include Makefile.split

# Source code files
LEVEL_C_FILES := $(wildcard levels/*/leveldata.c) $(wildcard levels/*/script.c) $(wildcard levels/*/geo.c)
C_FILES := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c)) $(LEVEL_C_FILES)
CXX_FILES := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.cpp))
S_FILES := $(foreach dir,$(ASM_DIRS),$(wildcard $(dir)/*.s))
ULTRA_C_FILES := $(foreach dir,$(ULTRA_SRC_DIRS),$(wildcard $(dir)/*.c))
GODDARD_C_FILES := $(foreach dir,$(GODDARD_SRC_DIRS),$(wildcard $(dir)/*.c))
ifeq ($(TARGET_N64),1)
  ULTRA_S_FILES := $(foreach dir,$(ULTRA_ASM_DIRS),$(wildcard $(dir)/*.s))
endif
GENERATED_C_FILES := $(BUILD_DIR)/assets/mario_anim_data.c $(BUILD_DIR)/assets/demo_data.c \
  $(addprefix $(BUILD_DIR)/bin/,$(addsuffix _skybox.c,$(notdir $(basename $(wildcard textures/skyboxes/*.png)))))

ifeq ($(TARGET_WINDOWS),0)
  CXX_FILES :=
endif

ifneq ($(TARGET_N64),1)
  ULTRA_C_FILES := \
    alBnkfNew.c \
    guLookAtRef.c \
    guMtxF2L.c \
    guNormalize.c \
    guOrthoF.c \
    guPerspectiveF.c \
    guRotateF.c \
    guScaleF.c \
    guTranslateF.c

  C_FILES := $(filter-out src/game/main.c,$(C_FILES))
  ULTRA_C_FILES := $(addprefix lib/src/,$(ULTRA_C_FILES))
endif

ifeq ($(VERSION),sh)
SOUND_BANK_FILES := $(wildcard sound/sound_banks/*.json)
SOUND_SEQUENCE_FILES := $(wildcard sound/sequences/jp/*.m64) \
    $(wildcard sound/sequences/*.m64) \
    $(foreach file,$(wildcard sound/sequences/jp/*.s),$(BUILD_DIR)/$(file:.s=.m64)) \
    $(foreach file,$(wildcard sound/sequences/*.s),$(BUILD_DIR)/$(file:.s=.m64))
else
SOUND_BANK_FILES := $(wildcard sound/sound_banks/*.json)
SOUND_SEQUENCE_FILES := $(wildcard sound/sequences/$(VERSION)/*.m64) \
    $(wildcard sound/sequences/*.m64) \
    $(foreach file,$(wildcard sound/sequences/$(VERSION)/*.s),$(BUILD_DIR)/$(file:.s=.m64)) \
    $(foreach file,$(wildcard sound/sequences/*.s),$(BUILD_DIR)/$(file:.s=.m64))
endif

SOUND_SAMPLE_DIRS := $(wildcard sound/samples/*)
SOUND_SAMPLE_AIFFS := $(foreach dir,$(SOUND_SAMPLE_DIRS),$(wildcard $(dir)/*.aiff))
SOUND_SAMPLE_TABLES := $(foreach file,$(SOUND_SAMPLE_AIFFS),$(BUILD_DIR)/$(file:.aiff=.table))
SOUND_SAMPLE_AIFCS := $(foreach file,$(SOUND_SAMPLE_AIFFS),$(BUILD_DIR)/$(file:.aiff=.aifc))
SOUND_OBJ_FILES := $(SOUND_BIN_DIR)/sound_data.o


# Object files
O_FILES := $(foreach file,$(C_FILES),$(BUILD_DIR)/$(file:.c=.o)) \
           $(foreach file,$(CXX_FILES),$(BUILD_DIR)/$(file:.cpp=.o)) \
           $(foreach file,$(S_FILES),$(BUILD_DIR)/$(file:.s=.o)) \
           $(foreach file,$(GENERATED_C_FILES),$(file:.c=.o))

ULTRA_O_FILES := $(foreach file,$(ULTRA_S_FILES),$(BUILD_DIR)/$(file:.s=.o)) \
                 $(foreach file,$(ULTRA_C_FILES),$(BUILD_DIR)/$(file:.c=.o))

GODDARD_O_FILES := $(foreach file,$(GODDARD_C_FILES),$(BUILD_DIR)/$(file:.c=.o))

# Automatic dependency files
DEP_FILES := $(O_FILES:.o=.d) $(ULTRA_O_FILES:.o=.d) $(GODDARD_O_FILES:.o=.d) $(BUILD_DIR)/$(LD_SCRIPT).d

# Files with GLOBAL_ASM blocks
ifeq ($(NON_MATCHING),0)
GLOBAL_ASM_C_FILES != grep -rl 'GLOBAL_ASM(' $(wildcard src/**/*.c)
GLOBAL_ASM_O_FILES = $(foreach file,$(GLOBAL_ASM_C_FILES),$(BUILD_DIR)/$(file:.c=.o))
GLOBAL_ASM_DEP = $(BUILD_DIR)/src/audio/non_matching_dep
endif

# Segment elf files
SEG_FILES := $(SEGMENT_ELF_FILES) $(ACTOR_ELF_FILES) $(LEVEL_ELF_FILES)

##################### Compiler Options #######################
INCLUDE_CFLAGS := -I include -I $(BUILD_DIR) -I $(BUILD_DIR)/include -I src -I .
ENDIAN_BITWIDTH := $(BUILD_DIR)/endian-and-bitwidth

ifeq ($(TARGET_N64),1)
IRIX_ROOT := tools/ido5.3_compiler

ifeq ($(shell type mips-linux-gnu-ld >/dev/null 2>/dev/null; echo $$?), 0)
  CROSS := mips-linux-gnu-
else ifeq ($(shell type mips64-linux-gnu-ld >/dev/null 2>/dev/null; echo $$?), 0)
  CROSS := mips64-linux-gnu-
else
  CROSS := mips64-elf-
endif

# check that either QEMU_IRIX is set or qemu-irix package installed
ifeq ($(COMPILER),ido)
  ifndef QEMU_IRIX
    QEMU_IRIX := $(shell which qemu-irix 2>/dev/null)
    ifeq (, $(QEMU_IRIX))
      $(error Please install qemu-irix package or set QEMU_IRIX env var to the full qemu-irix binary path)
    endif
  endif
endif

AS        := $(CROSS)as
CC        := $(QEMU_IRIX) -silent -L $(IRIX_ROOT) $(IRIX_ROOT)/usr/bin/cc
CPP       := cpp -P -Wno-trigraphs
LD        := $(CROSS)ld
AR        := $(CROSS)ar
OBJDUMP   := $(CROSS)objdump
OBJCOPY   := $(CROSS)objcopy
PYTHON    := python3

# change the compiler to gcc, to use the default, install the gcc-mips-linux-gnu package
ifeq ($(COMPILER),gcc)
  CC        := $(CROSS)gcc
endif

ifeq ($(TARGET_N64),1)
  TARGET_CFLAGS := -nostdinc -I include/libc -DTARGET_N64 -D_LANGUAGE_C
  CC_CFLAGS := -fno-builtin
endif

INCLUDE_CFLAGS := -I include -I $(BUILD_DIR) -I $(BUILD_DIR)/include -I src -I .

ifeq ($(TARGET_PSP),1)
  CC := psp-gcc
  CPP := cpp -P -Wno-trigraphs
  AS := psp-as
  CXX := psp-g++
  OPT_FLAGS += -march=mips32
  CC_CFLAGS += -DTARGET_PSP
endif

ifeq ($(TARGET_DC),1)
  CC := kos-cc
  CPP := cpp -P -Wno-trigraphs
  AS := kos-as
  CXX := kos-g++
endif

# Check code syntax with host compiler
CC_CHECK := gcc
CC_CHECK_CFLAGS := -fsyntax-only -fsigned-char $(CC_CFLAGS) $(TARGET_CFLAGS) $(INCLUDE_CFLAGS) -std=gnu90 -Wall -Wextra -Wno-format-security -Wno-main -DNON_MATCHING -DAVOID_UB $(VERSION_CFLAGS) $(GRUCODE_CFLAGS)

COMMON_CFLAGS = $(OPT_FLAGS) $(TARGET_CFLAGS) $(INCLUDE_CFLAGS) $(VERSION_CFLAGS) $(MATCH_CFLAGS) $(MIPSISET) $(GRUCODE_CFLAGS)

ASFLAGS := -march=vr4300 -mabi=32 -I include -I $(BUILD_DIR) $(VERSION_ASFLAGS) $(MATCH_ASFLAGS) $(GRUCODE_ASFLAGS)
CFLAGS = -Wab,-r4300_mul -non_shared -G 0 -Xcpluscomm -Xfullwarn -signed $(COMMON_CFLAGS) $(MIPSBIT)
OBJCOPYFLAGS := --pad-to=0x800000 --gap-fill=0xFF
SYMBOL_LINKING_FLAGS := $(addprefix -R ,$(SEG_FILES))
LDFLAGS := -T undefined_syms.txt -T $(BUILD_DIR)/$(LD_SCRIPT) -Map $(BUILD_DIR)/sm64.$(VERSION).map --no-check-sections $(SYMBOL_LINKING_FLAGS)
ENDIAN_BITWIDTH := $(BUILD_DIR)/endian-and-bitwidth

ifeq ($(COMPILER),gcc)
  CFLAGS := -march=vr4300 -mfix4300 -mabi=32 -mno-shared -G 0 -mhard-float -fno-stack-protector -fno-common -fno-zero-initialized-in-bss -I include -I src/ -I $(BUILD_DIR)/include -fno-PIC -mno-abicalls -fno-strict-aliasing -fno-inline-functions -ffreestanding -fwrapv -Wall -Wextra $(COMMON_CFLAGS)
endif

ifeq ($(shell getconf LONG_BIT), 32)
  # Work around memory allocation bug in QEMU
  export QEMU_GUEST_BASE := 1
else
  # Ensure that gcc treats the code as 32-bit
  CC_CHECK_CFLAGS += -m32
endif

# Prevent a crash with -sopt
export LANG := C

else # TARGET_N64

AS := as
# HOST_ tools are for building sound/sequences/00_sound_player.s on PSP
# as psp-as errors out due to relocation issues with the 'assembly'
ifeq ($(TARGET_DC),1)
HOST_AS      := kos-as
HOST_OBJCOPY := sh-elf-objcopy
else
HOST_AS      := as
HOST_OBJCOPY := objcopy
endif

ifneq ($(TARGET_WEB),1)
  CC := gcc
  CXX := g++
else
  CC := emcc
endif
ifeq ($(TARGET_WINDOWS),1)
  LD := $(CXX)
else
  LD := $(CC)
endif
CPP := cpp -P
OBJDUMP := objdump
OBJCOPY := objcopy
PYTHON := python3
ifeq ($(TARGET_PSP),1)
  CC  := psp-gcc
  CPP := psp-cpp -P
  CXX := psp-g++
  AS  := psp-as
  LD  := $(CC)
  OBJDUMP := psp-objdump
  OBJCOPY := psp-objcopy
endif
ifeq ($(TARGET_DC),1)
  CC  := kos-cc
  CPP := sh-elf-cpp -P
  CXX := kos-c++
  AS  := kos-as
  LD  := $(CC)
  OBJDUMP := sh-elf-objdump
  OBJCOPY := sh-elf-objcopy
  #CC_CHECK_CFLAGS += -DPSP_LEGACY_TYPES_DEFINED -DPSP_LEGACY_VOLATILE_TYPES_DEFINED
endif

# Platform-specific compiler and linker flags
ifeq ($(TARGET_WINDOWS),1)
  PLATFORM_CFLAGS  := -DTARGET_WINDOWS
  PLATFORM_LDFLAGS := -lm -lxinput9_1_0 -lole32 -no-pie -mwindows
endif
ifeq ($(TARGET_LINUX),1)
  PLATFORM_CFLAGS  := -DTARGET_LINUX `pkg-config --cflags libusb-1.0`
  PLATFORM_LDFLAGS := -lm -lpthread `pkg-config --libs libusb-1.0` -lasound -lpulse -no-pie
endif
ifeq ($(TARGET_PSP),1)
  PSPSDK_PREFIX = $(shell psp-config -p)
  PSP_PREFIX    = $(shell psp-config -P)
  # Notes from neo
  #-gdwarf-2 -gstrict-dwarf -g3 --ffunction-sections -fdata-sections -Wl,-gc-sections
  PLATFORM_CFLAGS  := -DTARGET_PSP -DPSP -D__PSP__ -DSRC_VER=\"$(SRC_VER)\" -DPSP_LEGACY_TYPES_DEFINED -DPSP_LEGACY_VOLATILE_TYPES_DEFINED -I$(PSPSDK_PREFIX)/include -G0 -D_PSP_FW_VERSION=500 -DNDEBUG -O3 -falign-functions=64 -flimit-function-alignment -g3 -fno-rounding-math -ffp-contract=off -Wfatal-errors -fsigned-char
  PLATFORM_LDFLAGS := -I$(PSPSDK_PREFIX)/lib -specs=$(PSPSDK_PREFIX)/lib/prxspecs -Wl,-q,-T$(PSPSDK_PREFIX)/lib/linkfile.prx $(PSPSDK_PREFIX)/lib/prxexports.o
endif
ifeq ($(TARGET_DC),1)
  #Notes from neo
  #-gdwarf-2 -gstrict-dwarf -g3 --ffunction-sections -fdata-sections -Wl,-gc-sections
  PLATFORM_CFLAGS  := $(KOS_CFLAGS) -Os -flto=auto -DTARGET_DC -Wall -Wextra -DNDEBUG -Werror -Wno-missing-braces -Wno-old-style-declaration
# -Wno-unused-variable -Wno-unused-parameter -fno-strict-aliasing

ifeq ($(LOWRES),1)
  PLATFORM_CFLAGS += -DLOWRES
endif

ifeq ($(FOR_DCLOAD),1)
  PLATFORM_CFLAGS += -DFOR_DCLOAD=1
endif

# The game runs at 60 fps (each 30 Hz logic step drawn interpolated, then exact) with KOS's
# horizontal 2x FSAA where it fits, both changeable in the debug menu (src/pc/dc_video_options.h):
# hold both triggers and press Up+X+Y. The frame loop, the PVR render lists, the menu and its
# frame-rate overlay come from enDjinn (third_party/endjinn); music and sounds play on AICAflow
# (third_party/aicaflow, src/pc/audio/sm64_aicaflow.c).
ENDJINN_DIR ?= third_party/endjinn
PLATFORM_CFLAGS += -I$(ENDJINN_DIR)/include -DENJ_CBASEPATH=\"/cd/\" -DENJ_FSAA=1 -DENJ_DEBUG_MENU=1
# SH4ZAM 0.10 under __FAST_MATH__ (our -ffast-math) computes sincos as two __builtin_sinf/cosf
# calls on float radians instead of one FSCA on the 16-bit angle, which differs by one unit on
# ~17% of angles and moves the game's sins/coss. include/sh4zam_port.h keeps FSCA, as
# SH4ZAM 0.9 had it, by reading SH4ZAM's inline routines without __FAST_MATH__.
# GCC flags stock SM64 code (mario_actions_moving.c) as maybe-uninitialized.
PLATFORM_CFLAGS += -Wno-error=maybe-uninitialized

# KOS dropped genwait_wait()'s callback argument in Nov 2025 without a version bump,
# so detect the signature from the installed header. The same KOS deprecates
# pvr_dr_init (now a no-op), which -Werror would otherwise reject.
ifeq ($(shell grep -c callback $(KOS_BASE)/include/kos/genwait.h 2>/dev/null),0)
  PLATFORM_CFLAGS += -DKOS_GENWAIT_3ARG -Wno-error=deprecated-declarations
endif

# GFX_BACKEND - selects the renderer backend (DC):
#   pvr  - raw KOS PVR direct-render (gfx_pvr.c)
  PLATFORM_CFLAGS += -DGFX_BACKEND_PVR=1

# need to finish working through these
#  PLATFORM_CFLAGS += -Wdouble-promotion

  PLATFORM_LDFLAGS := -Wl,--gc-sections  -Wl,-Map=output.map -lsh4zam
  # SM_WARM (gfx_retro_dc.c): hot code that does not fit in .sub0, placed on I-cache sets
  # DC_SM_WARM_SET.. by an ld ordering file. .sub1 starts one I-cache size after .sub0, so the offset
  # into .sub1 is the I-cache index. Pick the set from samples: lines .sub0 leaves cold.
  # 232: the sets after .sub0's 7.4 KB, which holds the rest of the
  # native renderer's hot code (2026-11-04).
  DC_SM_WARM_SET ?= 232
  # tri_state_setup in .sub2 at I-cache set DC_SM_WARM2_SET (gfx_retro_dc.c SM_WARM2). It is ~4.1 KB
  # and must end within .sub2's 8 KB: set 126 left 24 bytes, too few once it grew to 0x1048.
  DC_SM_WARM2_SET ?= 122
  # The renderer's hot data (.data.sm64_hot: rsp, rdp, sTri, the vertex bake buffers) at a fixed
  # offset from .sub0 mod the 16 KB operand cache, so it keeps its O-cache sets relative to the hot
  # code and its literal pools when code or .rodata grows (.sub0 moves in 8 KB steps). It goes into
  # .sub3, which starts 24 KB after .sub0 (8 KB steps, .sub1/.sub2 in use), i.e. at .sub0 + 8 KB
  # mod 16 KB; the Dreamcast has no MMU, so data there is fine. The offset must stay within
  # 0x2000..0x3fff minus the block's ~5.3 KB. 0x22c0 is the offset in the build whose title-screen
  # D-cache stalls were lowest (5.9% of display-list cycles). .bss starts on a 16 KB boundary.
  DC_HOT_DATA_RESIDUE ?= 0x22c0
  PLATFORM_LDFLAGS += -Wl,--section-ordering-file=$(BUILD_DIR)/sm_warm.ld
  # src/audio keeps the N64 sequencer path besides AICAflow's: tools/aica/recorder builds it to
  # record the songs and sounds from the ROM.
  AICAFLOW_DIR ?= third_party/aicaflow
  PLATFORM_CFLAGS += -DDC_AICAFLOW -I$(AICAFLOW_DIR)/driver/include -I$(AICAFLOW_DIR)/driver/format/include \
    -I$(AICAFLOW_DIR)/driver/sh4/include
  # The songs and sounds (build/us_dc/afx_stage/afx, tools/aica/sm64_audio_assets.sh) are read from
  # the disc's /afx, or with FOR_DCLOAD=1 through dcload from this machine's build folder.
  ifeq ($(FOR_DCLOAD),1)
    DC_AICAFLOW_ROOT ?= /pc$(abspath $(BUILD_DIR))/afx_stage/afx
  else
    DC_AICAFLOW_ROOT ?= /cd/afx
  endif
  PLATFORM_CFLAGS += -DDC_AICAFLOW_ROOT=\"$(DC_AICAFLOW_ROOT)\"
  # The debug menu's TAS replay and level select read tas.m64 and tas_slices.bin (tas/) from there.
  PLATFORM_CFLAGS += -DDC_TAS_ROOT=\"$(DC_AICAFLOW_ROOT)\"
endif
ifeq ($(TARGET_WEB),1)
  PLATFORM_CFLAGS  := -DTARGET_WEB
  PLATFORM_LDFLAGS := -lm -no-pie -s TOTAL_MEMORY=20MB -g4 --source-map-base http://localhost:8080/ -s "EXTRA_EXPORTED_RUNTIME_METHODS=['callMain']"
endif

PLATFORM_CFLAGS += -DNO_SEGMENTED_MEMORY

# Compiler and linker flags for graphics backend
ifeq ($(ENABLE_OPENGL),1)
  GFX_CFLAGS  := -DENABLE_OPENGL
  GFX_LDFLAGS :=
  ifeq ($(TARGET_WINDOWS),1)
    GFX_CFLAGS  += $(shell sdl2-config --cflags) -DGLEW_STATIC
    GFX_LDFLAGS += $(shell sdl2-config --libs) -lglew32 -lopengl32 -lwinmm -limm32 -lversion -loleaut32 -lsetupapi
  endif
  ifeq ($(TARGET_LINUX),1)
    GFX_CFLAGS  += $(shell sdl2-config --cflags)
    GFX_LDFLAGS += -lGL $(shell sdl2-config --libs) -lX11 -lXrandr
  endif
  ifeq ($(TARGET_WEB),1)
    GFX_CFLAGS  += -s USE_SDL=2
    GFX_LDFLAGS += -lGL -lSDL2
  endif
  ifeq ($(TARGET_PSP),1)
    GFX_LDFLAGS += -L$(PSPSDK_PREFIX)/lib src/pc/libME.a src/pc/gfx/libpspmath.a -lpspdebug  -lpspgu -lpspvfpu -lpspctrl -lpspge -lpspdisplay -lm -lpspsdk -lpsprtc -lpspaudio -lpsputility -lpspnet_inet -lpsppower -lc -lpspuser -lpspvram  
  endif
  ifeq ($(TARGET_DC),1)
    GFX_CFLAGS  += 
    GFX_LDFLAGS +=
  endif
endif
ifeq ($(ENABLE_DX11),1)
  GFX_CFLAGS := -DENABLE_DX11
  PLATFORM_LDFLAGS += -lgdi32 -static
endif
ifeq ($(ENABLE_DX12),1)
  GFX_CFLAGS := -DENABLE_DX12
  PLATFORM_LDFLAGS += -lgdi32 -static
endif

# -lAL


#ifneq ($(TARGET_DC),1)
#GFX_CFLAGS += -DWIDESCREEN
#endif

CC_CHECK := $(CC) -fsyntax-only -fsigned-char $(INCLUDE_CFLAGS) -Wall -Wextra -Wno-format-security -D_LANGUAGE_C $(VERSION_CFLAGS) $(MATCH_CFLAGS) $(PLATFORM_CFLAGS) $(GFX_CFLAGS) $(GRUCODE_CFLAGS)
CFLAGS := $(OPT_FLAGS) $(INCLUDE_CFLAGS) -D_LANGUAGE_C $(VERSION_CFLAGS) $(MATCH_CFLAGS) $(PLATFORM_CFLAGS) $(GFX_CFLAGS) $(GRUCODE_CFLAGS) -fno-strict-aliasing -fwrapv -fipa-pta -fno-pic -fno-pie 

ASFLAGS := -I include -I $(BUILD_DIR) $(VERSION_ASFLAGS)

LDFLAGS := $(PLATFORM_LDFLAGS) $(GFX_LDFLAGS)

ifneq ($(TARGET_PSP), 1)
ifneq ($(TARGET_DC), 1)
CFLAGS+=-march=native
endif
endif
endif

####################### Other Tools #########################

# N64 tools
TOOLS_DIR = tools
MIO0TOOL = $(TOOLS_DIR)/mio0
N64CKSUM = $(TOOLS_DIR)/n64cksum
N64GRAPHICS = $(TOOLS_DIR)/n64graphics
N64GRAPHICS_CI = $(TOOLS_DIR)/n64graphics_ci
TEXTCONV = $(TOOLS_DIR)/textconv
AIFF_EXTRACT_CODEBOOK = $(TOOLS_DIR)/aiff_extract_codebook
VADPCM_ENC = $(TOOLS_DIR)/vadpcm_enc
EXTRACT_DATA_FOR_MIO = $(TOOLS_DIR)/extract_data_for_mio
SKYCONV = $(TOOLS_DIR)/skyconv
EMULATOR = mupen64plus
EMU_FLAGS = --noosd
LOADER = loader64
LOADER_FLAGS = -vwf
SHA1SUM = sha1sum

ifeq (, $(shell which armips 2>/dev/null))
  RSPASM := $(TOOLS_DIR)/armips
else
  RSPASM = armips
endif

# Use Objcopy instead of extract_data_for_mio
ifeq ($(COMPILER),gcc)
EXTRACT_DATA_FOR_MIO := $(OBJCOPY) -O binary --only-section=.data
endif

######################## Targets #############################

ifeq ($(TARGET_N64),1)
all: $(ROM)
ifeq ($(COMPARE),1)
	@$(SHA1SUM) -c $(TARGET).sha1 || (echo 'The build succeeded, but did not match the official ROM. This is expected if you are making changes to the game.\nTo silence this message, use "make COMPARE=0"'. && false)
endif
else ifeq ($(TARGET_DC),1)
all: $(EXE) $(BUILD_DIR)/afx_stage/afx/tas_slices.bin
else
all: $(EXE)
endif

clean:
	$(RM) -r $(BUILD_DIR_BASE)

distclean:
	$(RM) -r $(BUILD_DIR_BASE)
	$(PYTHON) ./extract_assets.py --clean

test: $(ROM)
	$(EMULATOR) $(EMU_FLAGS) $<

load: $(ROM)
	$(LOADER) $(LOADER_FLAGS) $<

libultra: $(BUILD_DIR)/libultra.a

$(BUILD_DIR)/asm/boot.o: $(IPL3_RAW_FILES)
$(BUILD_DIR)/src/game/crash_screen.o: $(CRASH_TEXTURE_C_FILES)

$(BUILD_DIR)/lib/rsp.o: $(BUILD_DIR)/rsp/rspboot.bin $(BUILD_DIR)/rsp/fast3d.bin $(BUILD_DIR)/rsp/audio.bin

$(BUILD_DIR)/include/text_strings.h: include/text_strings.h.in
	$(TEXTCONV) charmap.txt $< $@

$(BUILD_DIR)/include/text_menu_strings.h: include/text_menu_strings.h.in
	$(TEXTCONV) charmap_menu.txt $< $@

ifeq ($(COMPILER),gcc)
$(BUILD_DIR)/lib/src/math/%.o: CFLAGS += -fno-builtin
endif

ifeq ($(VERSION),eu)
TEXT_DIRS := text/de text/us text/fr

# EU encoded text inserted into individual segment 0x19 files,
# and course data also duplicated in leveldata.c
$(BUILD_DIR)/bin/eu/translation_en.o: $(BUILD_DIR)/text/us/define_text.inc.c
$(BUILD_DIR)/bin/eu/translation_de.o: $(BUILD_DIR)/text/de/define_text.inc.c
$(BUILD_DIR)/bin/eu/translation_fr.o: $(BUILD_DIR)/text/fr/define_text.inc.c
$(BUILD_DIR)/levels/menu/leveldata.o: $(BUILD_DIR)/text/us/define_courses.inc.c
$(BUILD_DIR)/levels/menu/leveldata.o: $(BUILD_DIR)/text/de/define_courses.inc.c
$(BUILD_DIR)/levels/menu/leveldata.o: $(BUILD_DIR)/text/fr/define_courses.inc.c

else
ifeq ($(VERSION),sh)
TEXT_DIRS := text/jp
$(BUILD_DIR)/bin/segment2.o: $(BUILD_DIR)/text/jp/define_text.inc.c

else
TEXT_DIRS := text/$(VERSION)

# non-EU encoded text inserted into segment 0x02
$(BUILD_DIR)/bin/segment2.o: $(BUILD_DIR)/text/$(VERSION)/define_text.inc.c
endif
endif

$(BUILD_DIR)/text/%/define_courses.inc.c: text/define_courses.inc.c text/%/courses.h
	$(CPP) $(VERSION_CFLAGS) $< -o - -I text/$*/ | $(TEXTCONV) charmap.txt - $@

$(BUILD_DIR)/text/%/define_text.inc.c: text/define_text.inc.c text/%/courses.h text/%/dialogs.h
	$(CPP) $(VERSION_CFLAGS) $< -o - -I text/$*/ | $(TEXTCONV) charmap.txt - $@

RSP_DIRS := $(BUILD_DIR)/rsp
ALL_DIRS := $(BUILD_DIR) $(addprefix $(BUILD_DIR)/,$(SRC_DIRS) $(ASM_DIRS) $(GODDARD_SRC_DIRS) $(ULTRA_SRC_DIRS) $(ULTRA_ASM_DIRS) $(ULTRA_BIN_DIRS) $(BIN_DIRS) $(TEXTURE_DIRS) $(TEXT_DIRS) $(SOUND_SAMPLE_DIRS) $(addprefix levels/,$(LEVEL_DIRS)) include) $(MIO0_DIR) $(addprefix $(MIO0_DIR)/,$(VERSION)) $(SOUND_BIN_DIR) $(SOUND_BIN_DIR)/sequences/$(VERSION) $(RSP_DIRS)

# Make sure build directory exists before compiling anything
DUMMY != mkdir -p $(ALL_DIRS)

$(BUILD_DIR)/include/text_strings.h: $(BUILD_DIR)/include/text_menu_strings.h
$(BUILD_DIR)/src/menu/file_select.o: $(BUILD_DIR)/include/text_strings.h
$(BUILD_DIR)/src/menu/star_select.o: $(BUILD_DIR)/include/text_strings.h
$(BUILD_DIR)/src/game/ingame_menu.o: $(BUILD_DIR)/include/text_strings.h

################################################################
# TEXTURE GENERATION                                           #
################################################################

# RGBA32, RGBA16, IA16, IA8, IA4, IA1, I8, I4
$(BUILD_DIR)/%: %.png
	$(N64GRAPHICS) -i $@ -g $< -f $(lastword $(subst ., ,$@))

$(BUILD_DIR)/%.inc.c: $(BUILD_DIR)/% %.png
	$(HEXDUMP) -v -e '1/1 "0x%X,"' $< > $@
	echo >> $@

# Color Index CI8
$(BUILD_DIR)/%.ci8: %.ci8.png
	$(N64GRAPHICS_CI) -i $@ -g $< -f ci8

# Color Index CI4
$(BUILD_DIR)/%.ci4: %.ci4.png
	$(N64GRAPHICS_CI) -i $@ -g $< -f ci4

################################################################

# compressed segment generation

ifeq ($(TARGET_N64),1)
# TODO: ideally this would be `-Trodata-segment=0x07000000` but that doesn't set the address

$(BUILD_DIR)/bin/%.elf: $(BUILD_DIR)/bin/%.o
	$(LD) -e 0 -Ttext=$(SEGMENT_ADDRESS) -Map $@.map -o $@ $<
$(BUILD_DIR)/actors/%.elf: $(BUILD_DIR)/actors/%.o
	$(LD) -e 0 -Ttext=$(SEGMENT_ADDRESS) -Map $@.map -o $@ $<

# Override for level.elf, which otherwise matches the above pattern
.SECONDEXPANSION:
$(BUILD_DIR)/levels/%/leveldata.elf: $(BUILD_DIR)/levels/%/leveldata.o $(BUILD_DIR)/bin/$$(TEXTURE_BIN).elf
	$(LD) -e 0 -Ttext=$(SEGMENT_ADDRESS) -Map $@.map --just-symbols=$(BUILD_DIR)/bin/$(TEXTURE_BIN).elf -o $@ $<

$(BUILD_DIR)/bin/%.bin: $(BUILD_DIR)/bin/%.elf
	$(EXTRACT_DATA_FOR_MIO) $< $@

$(BUILD_DIR)/actors/%.bin: $(BUILD_DIR)/actors/%.elf
	$(EXTRACT_DATA_FOR_MIO) $< $@

$(BUILD_DIR)/levels/%/leveldata.bin: $(BUILD_DIR)/levels/%/leveldata.elf
	$(EXTRACT_DATA_FOR_MIO) $< $@

$(BUILD_DIR)/%.mio0: $(BUILD_DIR)/%.bin
	$(MIO0TOOL) $< $@

$(BUILD_DIR)/%.mio0.o: $(BUILD_DIR)/%.mio0.s
	$(AS) $(ASFLAGS) -o $@ $<

$(BUILD_DIR)/%.mio0.s: $(BUILD_DIR)/%.mio0
	printf ".section .data\n\n.incbin \"$<\"\n" > $@
endif

$(BUILD_DIR)/%.table: %.aiff
	$(AIFF_EXTRACT_CODEBOOK) $< >$@

$(BUILD_DIR)/%.aifc: $(BUILD_DIR)/%.table %.aiff
	$(VADPCM_ENC) -c $^ $@

$(BUILD_DIR)/rsp/%.bin $(BUILD_DIR)/rsp/%_data.bin: rsp/%.s
	$(RSPASM) -sym $@.sym -definelabel $(VERSION_DEF) 1 -definelabel $(GRUCODE_DEF) 1 -strequ CODE_FILE $(BUILD_DIR)/rsp/$*.bin -strequ DATA_FILE $(BUILD_DIR)/rsp/$*_data.bin $<

$(ENDIAN_BITWIDTH): tools/determine-endian-bitwidth.c
	echo "--endian little --bitwidth 32" > $@
#	$(CC) -c $(CFLAGS) -o $@.dummy2 $< 2>$@.dummy1; true
#	grep -o 'msgbegin --endian .* --bitwidth .* msgend' $@.dummy1 > $@.dummy2
#	head -n1 <$@.dummy2 | cut -d' ' -f2-5 > $@
#	@rm $@.dummy1
#	@rm $@.dummy2

$(SOUND_BIN_DIR)/sound_data.ctl: sound/sound_banks/ $(SOUND_BANK_FILES) $(SOUND_SAMPLE_AIFCS) $(ENDIAN_BITWIDTH)
	$(PYTHON) tools/assemble_sound.py $(BUILD_DIR)/sound/samples/ sound/sound_banks/ $(SOUND_BIN_DIR)/sound_data.ctl $(SOUND_BIN_DIR)/sound_data.tbl $(VERSION_CFLAGS) $$(cat $(ENDIAN_BITWIDTH))

$(SOUND_BIN_DIR)/sound_data.tbl: $(SOUND_BIN_DIR)/sound_data.ctl
	@true

ifeq ($(TARGET_DC),1)
# AICA hardware mixing replaces the software render path, so the raw VADPCM
# sample bytes are never dereferenced at runtime. Link only the .tbl ALSeqFile
# header (0x140 bytes) -- sampleAddr keying needs the base symbol + seqArray
# offsets, not the ~2.2 MB of samples -- so gSoundDataRaw doesn't bloat RAM.
$(SOUND_BIN_DIR)/sound_data.tbl.inc.c: $(SOUND_BIN_DIR)/sound_data.tbl
	head -c 320 $< | hexdump -v -e '1/1 "0x%X,"' > $@
	echo >> $@

# AICA voice driver: regenerate the Yamaha-ADPCM pool + sample table from the
# built sound_data banks (timestamp-gated; only re-runs when the banks change).
# scipy-optional -- a clean checkout builds with stock python3.
AICA_TOOLS := $(filter-out tools/aica/compare_encoders.py tools/aica/adpcm_quality.py,$(wildcard tools/aica/*.py)) tools/aica/ya2beam.c
# The AICAflow host library, rebuilt when its sources change, and its ARM7 firmware as a C array.
AICAFLOW_LIB := $(AICAFLOW_DIR)/driver/sh4/libaicaflow_host.a
SOUND_OBJ_FILES += $(BUILD_DIR)/aicaflow_firmware.o $(AICAFLOW_LIB)
$(AICAFLOW_LIB): $(wildcard $(AICAFLOW_DIR)/driver/sh4/src/*.c $(AICAFLOW_DIR)/driver/sh4/include/aicaflow/*.h \
                   $(AICAFLOW_DIR)/driver/include/aicaflow/*.h $(AICAFLOW_DIR)/driver/format/src/*.c \
                   $(AICAFLOW_DIR)/driver/format/include/aicaflow/*.h)
	$(MAKE) -C $(AICAFLOW_DIR)/driver/sh4
$(BUILD_DIR)/aicaflow_firmware.c: $(AICAFLOW_DIR)/firmware/aicaflow.drv
	$(PYTHON) -c 'import sys; d = open(sys.argv[1], "rb").read(); print("const unsigned char gAicaflowFirmware[] __attribute__((aligned(32))) = {" + ",".join(str(b) for b in d) + "};\nconst unsigned int gAicaflowFirmwareSize = sizeof(gAicaflowFirmware);")' $< > $@
$(BUILD_DIR)/aicaflow_firmware.o: $(BUILD_DIR)/aicaflow_firmware.c
	$(CC) -c -o $@ $<
# Each sound's script reverb (sound scripts do not run under AICAflow), for the reverb send.
$(BUILD_DIR)/sm64_sfx_reverb.h: sound/sequences/00_sound_player.s tools/aica/sm64_sfx_reverb.py
	$(PYTHON) tools/aica/sm64_sfx_reverb.py $< > $@
$(BUILD_DIR)/src/pc/audio/sm64_aicaflow.o: $(BUILD_DIR)/sm64_sfx_reverb.h

# The enDjinn sources the frame loop, the render lists, the debug menu and the overlay's quick
# font need, built with its own C dialect.
ENDJINN_SRCS := enj_qfont.c enj_font.c enj_draw.c enj_bitmap.c enj_render.c enj_state.c enj_mode.c \
                enj_ctrl.c enj_rumble.c enj_pacing.c enj_debug_menu.c enj_debug_menu_mode.c
ENDJINN_O_FILES := $(addprefix $(BUILD_DIR)/endjinn/,$(ENDJINN_SRCS:.c=.o))
SOUND_OBJ_FILES += $(ENDJINN_O_FILES)
# The overlay loads the quick font itself (dc_overlay.c, for the debug menu too), so enDjinn's
# startup must not load it too.
ENJ_QFONT_DEFINE := -DENJ_INJECT_QFONT=1
$(BUILD_DIR)/endjinn/enj_state.o: ENJ_QFONT_DEFINE :=
$(BUILD_DIR)/endjinn/%.o: $(ENDJINN_DIR)/code/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu23 -O2 -c -I$(ENDJINN_DIR)/include -I$(KOS_BASE)/utils -DENJ_CBASEPATH=\"/cd/\" -DENJ_FSAA=1 $(ENJ_QFONT_DEFINE) -DENJ_DEBUG_MENU=1 -o $@ $<

$(SOUND_BIN_DIR)/aica_pool.c: $(SOUND_BIN_DIR)/sound_data.tbl $(SOUND_BIN_DIR)/sound_data.ctl $(AICA_TOOLS)
	PYTHONPATH=tools/aica $(PYTHON) tools/aica/sm64_emit.py \
		--ctl $(SOUND_BIN_DIR)/sound_data.ctl --tbl $(SOUND_BIN_DIR)/sound_data.tbl \
		--outdir $(SOUND_BIN_DIR) --incdir $(BUILD_DIR)

# emitted together with aica_pool.c by the one recipe above
$(SOUND_BIN_DIR)/aica_sample_table.c $(BUILD_DIR)/aica_sample_table.h: $(SOUND_BIN_DIR)/aica_pool.c
	@true

# aica_synth.c #includes the generated header
$(BUILD_DIR)/src/audio/aica_synth.o: $(BUILD_DIR)/aica_sample_table.h
endif

ifeq ($(VERSION),sh)
$(SOUND_BIN_DIR)/sequences.bin: $(SOUND_BANK_FILES) sound/sequences.json sound/sequences/ sound/sequences/jp/ $(SOUND_SEQUENCE_FILES) $(ENDIAN_BITWIDTH)
	$(PYTHON) tools/assemble_sound.py --sequences $@ $(SOUND_BIN_DIR)/bank_sets sound/sound_banks/ sound/sequences.json $(SOUND_SEQUENCE_FILES) $(VERSION_CFLAGS) $$(cat $(ENDIAN_BITWIDTH))
else
$(SOUND_BIN_DIR)/sequences.bin: $(SOUND_BANK_FILES) sound/sequences.json sound/sequences/ sound/sequences/$(VERSION)/ $(SOUND_SEQUENCE_FILES) $(ENDIAN_BITWIDTH)
	$(PYTHON) tools/assemble_sound.py --sequences $@ $(SOUND_BIN_DIR)/bank_sets sound/sound_banks/ sound/sequences.json $(SOUND_SEQUENCE_FILES) $(VERSION_CFLAGS) $$(cat $(ENDIAN_BITWIDTH))
endif

$(SOUND_BIN_DIR)/bank_sets: $(SOUND_BIN_DIR)/sequences.bin
	@true

$(SOUND_BIN_DIR)/%.m64: $(SOUND_BIN_DIR)/%.o
	$(HOST_OBJCOPY) -j .rodata $< -O binary $@

$(SOUND_BIN_DIR)/%.o: $(SOUND_BIN_DIR)/%.s
	$(HOST_AS) $(ASFLAGS) -o $@ $< -Z  2> /dev/null

$(SOUND_BIN_DIR)/%.inc.c: $(SOUND_BIN_DIR)/%
	$(HEXDUMP) -v -e '1/1 "0x%X,"' $< > $@
	echo >> $@

$(SOUND_BIN_DIR)/sound_data.o: $(SOUND_BIN_DIR)/sound_data.ctl.inc.c $(SOUND_BIN_DIR)/sound_data.tbl.inc.c $(SOUND_BIN_DIR)/sequences.bin.inc.c $(SOUND_BIN_DIR)/bank_sets.inc.c

$(BUILD_DIR)/levels/scripts.o: $(BUILD_DIR)/include/level_headers.h

$(BUILD_DIR)/include/level_headers.h: levels/level_headers.h.in
	$(CPP) -I . levels/level_headers.h.in | $(PYTHON) tools/output_level_headers.py > $(BUILD_DIR)/include/level_headers.h

$(BUILD_DIR)/assets/mario_anim_data.c: $(wildcard assets/anims/*.inc.c)
	$(PYTHON) tools/mario_anims_converter.py > $@

$(BUILD_DIR)/assets/demo_data.c: assets/demo_data.json $(wildcard assets/demos/*.bin)
	$(PYTHON) tools/demo_data_converter.py assets/demo_data.json $(VERSION_CFLAGS) > $@

ifeq ($(COMPILER),ido)
# Source code
$(BUILD_DIR)/levels/%/leveldata.o: OPT_FLAGS := -g
$(BUILD_DIR)/actors/%.o: OPT_FLAGS := -g
$(BUILD_DIR)/bin/%.o: OPT_FLAGS := -g
$(BUILD_DIR)/src/goddard/%.o: OPT_FLAGS := -g
$(BUILD_DIR)/src/goddard/%.o: MIPSISET := -mips1
$(BUILD_DIR)/src/audio/%.o: OPT_FLAGS := -O2 -Wo,-loopunroll,0
$(BUILD_DIR)/src/audio/load.o: OPT_FLAGS := -O2 -framepointer -Wo,-loopunroll,0
$(BUILD_DIR)/lib/src/%.o: OPT_FLAGS :=
$(BUILD_DIR)/lib/src/math/ll%.o: MIPSISET := -mips3 -32
$(BUILD_DIR)/lib/src/math/%.o: OPT_FLAGS := -O2
$(BUILD_DIR)/lib/src/math/ll%.o: OPT_FLAGS :=
$(BUILD_DIR)/lib/src/ldiv.o: OPT_FLAGS := -O2
$(BUILD_DIR)/lib/src/string.o: OPT_FLAGS := -O2
$(BUILD_DIR)/lib/src/gu%.o: OPT_FLAGS := -O3
$(BUILD_DIR)/lib/src/al%.o: OPT_FLAGS := -O3

ifeq ($(VERSION),eu)
$(BUILD_DIR)/lib/src/_Litob.o: OPT_FLAGS := -O3
$(BUILD_DIR)/lib/src/_Ldtob.o: OPT_FLAGS := -O3
$(BUILD_DIR)/lib/src/_Printf.o: OPT_FLAGS := -O3
$(BUILD_DIR)/lib/src/sprintf.o: OPT_FLAGS := -O3

# enable loop unrolling except for external.c (external.c might also have used
# unrolling, but it makes one loop harder to match)
$(BUILD_DIR)/src/audio/%.o: OPT_FLAGS := -O2
$(BUILD_DIR)/src/audio/load.o: OPT_FLAGS := -O2
$(BUILD_DIR)/src/audio/external.o: OPT_FLAGS := -O2 -Wo,-loopunroll,0
else

# The source-to-source optimizer copt is enabled for audio. This makes it use
# acpp, which needs -Wp,-+ to handle C++-style comments.
$(BUILD_DIR)/src/audio/effects.o: OPT_FLAGS := -O2 -Wo,-loopunroll,0 -sopt,-inline=sequence_channel_process_sound,-scalaroptimize=1 -Wp,-+
$(BUILD_DIR)/src/audio/synthesis.o: OPT_FLAGS := -O2 -sopt,-scalaroptimize=1 -Wp,-+
#$(BUILD_DIR)/src/audio/seqplayer.o: OPT_FLAGS := -O2 -sopt,-inline_manual,-scalaroptimize=1 -Wp,-+ #-Wo,-v,-bb,-l,seqplayer_list.txt

# Add a target for build/eu/src/audio/*.copt to make it easier to see debug
$(BUILD_DIR)/src/audio/%.acpp: src/audio/%.c
	$(QEMU_IRIX) -silent -L $(IRIX_ROOT) $(IRIX_ROOT)/usr/lib/acpp $(TARGET_CFLAGS) $(INCLUDE_CFLAGS) $(VERSION_CFLAGS) $(MATCH_CFLAGS) $(GRUCODE_CFLAGS) -D__sgi -+ $< > $@

$(BUILD_DIR)/src/audio/seqplayer.copt: $(BUILD_DIR)/src/audio/seqplayer.acpp
	$(QEMU_IRIX) -silent -L $(IRIX_ROOT) $(IRIX_ROOT)/usr/lib/copt -signed -I=$< -CMP=$@ -cp=i -scalaroptimize=1 -inline_manual

$(BUILD_DIR)/src/audio/%.copt: $(BUILD_DIR)/src/audio/%.acpp
	$(QEMU_IRIX) -silent -L $(IRIX_ROOT) $(IRIX_ROOT)/usr/lib/copt -signed -I=$< -CMP=$@ -cp=i -scalaroptimize=1

endif
endif

ifeq ($(NON_MATCHING),0)
$(GLOBAL_ASM_O_FILES): CC := $(PYTHON) tools/asm_processor/build.py $(CC) -- $(AS) $(ASFLAGS) --
endif

# Rebuild files with 'GLOBAL_ASM' if the NON_MATCHING flag changes.
$(GLOBAL_ASM_O_FILES): $(GLOBAL_ASM_DEP).$(NON_MATCHING)
$(GLOBAL_ASM_DEP).$(NON_MATCHING):
	@rm -f $(GLOBAL_ASM_DEP).*
	touch $@

$(BUILD_DIR)/%.o: %.cpp
	@$(CXX) -fsyntax-only $(CFLAGS) -MMD -MP -MT $@ -MF $(BUILD_DIR)/$*.d $<
	$(CXX) -c $(CFLAGS) -o $@ $<

$(BUILD_DIR)/%.o: %.c
	@$(CC_CHECK) $(CC_CHECK_CFLAGS) -MMD -MP -MT $@ -MF $(BUILD_DIR)/$*.d $<
	@echo $@
	$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD_DIR)/%.o: $(BUILD_DIR)/%.c
	@$(CC_CHECK) $(CC_CHECK_CFLAGS) -MMD -MP -MT $@ -MF $(BUILD_DIR)/$*.d $<
	$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD_DIR)/%.o: %.s
	$(HOST_AS) $(ASFLAGS) -MD $(BUILD_DIR)/$*.d -o $@ $<

ifeq ($(TARGET_N64),1)
$(BUILD_DIR)/$(LD_SCRIPT): $(LD_SCRIPT)
	$(CPP) $(VERSION_CFLAGS) -MMD -MP -MT $@ -MF $@.d -I include/ -I . -DBUILD_DIR=$(BUILD_DIR) -o $@ $<

$(BUILD_DIR)/libultra.a: $(ULTRA_O_FILES)
	$(AR) rcs -o $@ $(ULTRA_O_FILES)
	tools/patch_libultra_math $@

$(BUILD_DIR)/libgoddard.a: $(GODDARD_O_FILES)
	$(AR) rcs -o $@ $(GODDARD_O_FILES)

$(ELF): $(O_FILES) $(MIO0_OBJ_FILES) $(SOUND_OBJ_FILES) $(SEG_FILES) $(BUILD_DIR)/$(LD_SCRIPT) undefined_syms.txt $(BUILD_DIR)/libultra.a $(BUILD_DIR)/libgoddard.a
	$(LD) -L $(BUILD_DIR) $(LDFLAGS) -o $@ $(O_FILES)$(LIBS) -lultra -lgoddard

$(ROM): $(ELF)
	$(OBJCOPY) $(OBJCOPYFLAGS) $< $(@:.z64=.bin) -O binary
	$(N64CKSUM) $(@:.z64=.bin) $@

$(BUILD_DIR)/$(TARGET).objdump: $(ELF)
	$(OBJDUMP) -D $< > $@

else

$(BUILD_DIR)/src/audio/seqplayer.o: CFLAGS += -O3
$(BUILD_DIR)/src/audio/synthesis.o: CFLAGS += -O3
$(BUILD_DIR)/src/engine/math_util.o: CFLAGS += -O3
$(BUILD_DIR)/src/pc/mixer.o: CFLAGS += -O3
$(BUILD_DIR)/src/pc/gfx/gfx_retro_dc.o: CFLAGS += -O3
$(BUILD_DIR)/src/pc/gfx/gfx_pvr.o: CFLAGS += -O3
# sh-elf GCC 15.2 hits an internal compiler error (reload, gen_reg_rtx) in
# bhv_haunted_chair_loop above -Os, so keep this file at -Os and out of LTO, which would otherwise
# recompile it at the link-time level.
$(BUILD_DIR)/src/game/obj_behaviors_2.o: CFLAGS += -Os -fno-lto
# The game logic (src/game, src/engine) without the float shortcuts the rest of the build takes
# (-ffast-math reorders, -ffp-contract=fast fuses multiply-add into FMAC, rounding once where the N64
# rounds twice; -fsingle-precision-constant makes the double constants float), so it computes what
# the N64 does and the TAS replay (tas/) stays in sync. -fno-math-errno keeps sqrtf the FPU's fsqrt,
# correctly rounded as the N64's sqrt.s is. DC_EXACT_MATH makes sh4zam's approximations exact
# there too (include/sh4zam_port.h).
$(BUILD_DIR)/src/game/%.o $(BUILD_DIR)/src/engine/%.o: CFLAGS += -fno-fast-math -fno-math-errno -ffp-contract=off -fno-single-precision-constant -DDC_EXACT_MATH=1

ifeq ($(TARGET_DC),1)
# Unused for now, might be later
include $(KOS_BASE)/Makefile.rules
  $(EXE): $(O_FILES) $(MIO0_FILES:.mio0=.o) $(SOUND_OBJ_FILES) $(ULTRA_O_FILES) $(GODDARD_O_FILES) romdisk.o $(BUILD_DIR)/sm_warm.ld
else
  $(EXE): $(O_FILES) $(MIO0_FILES:.mio0=.o) $(SOUND_OBJ_FILES) $(ULTRA_O_FILES) $(GODDARD_O_FILES)
endif
	@echo "Linking $@"
	@$(LD) -L $(BUILD_DIR) -o $@ $(O_FILES) $(SOUND_OBJ_FILES) $(ULTRA_O_FILES) $(GODDARD_O_FILES) $(LDFLAGS) -flto
ifeq ($(TARGET_DC),1)
# gfx_retro_dc.c's SM_WARM code must sit on I-cache sets DC_SM_WARM_SET.. inside .sub1, or it is
# back on whatever lines the link gives it. LTO may rename a static symbol to name.lto_priv.N.
	@sh-elf-nm $@ | awk -v r=$$(( $(DC_HOT_DATA_RESIDUE) )) 'function hex(s,  i, n) { n = 0; for (i = 1; i <= length(s); i++) n = n * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1; return n } \
	  $$3 == "__sub0_start" { s0 = hex($$1) } $$3 == "__sub2_start" { s2 = hex($$1) } $$3 ~ /^_tri_state_setup(\.lto_priv\.[0-9]+)?$$/ { ts = hex($$1) } \
	  $$3 == "__sub1_start" { s = hex($$1) } $$3 ~ /^_cc_eval_float/ { f = hex($$1) } $$3 ~ /^_(sTriStateEpoch|clip_rej|rsp)(\.lto_priv\.[0-9]+)?$$/ { h = (!h || hex($$1) < h) ? hex($$1) : h } \
	  END { if (!s || s % 8192 || f != s + $(DC_SM_WARM_SET) * 32) { print "SM_WARM placement broken: cc_eval_float is not at .sub1 + $(DC_SM_WARM_SET) * 32"; exit 1 } \
	        if (!s2 || ts != s2 + $(DC_SM_WARM2_SET) * 32) { print "SM_WARM2 placement broken: tri_state_setup is not at .sub2 + $(DC_SM_WARM2_SET) * 32"; exit 1 } \
	        d = (h - s0) % 16384; if (d < r || d >= r + 8192) { printf "hot data at .sub0 + %d mod 16384, not near $(DC_HOT_DATA_RESIDUE)\n", d; exit 1 } \
	        printf "SM_WARM: cc_eval_float on I-cache sets $(DC_SM_WARM_SET).., tri_state_setup on $(DC_SM_WARM2_SET)..; hot data at .sub0 + 0x%x mod 16 KB\n", d }' || { rm -f $@; exit 1; }
endif
ifeq ($(TARGET_PSP),1)
	psp-fixup-imports $@
	psp-prxgen $@ $@.prx

TITLE := "sm64-port $(SRC_VER)"

pbp: $(EXE)
	@mkdir -p $(BUILD_DIR)/mario64
	cp psp/snd_eng.prx $(BUILD_DIR)/mario64/
	mksfoex -d MEMSIZE=1 $(TITLE) $(BUILD_DIR)/PARAM.SFO
	pack-pbp $(BUILD_DIR)/mario64/EBOOT.PBP $(BUILD_DIR)/PARAM.SFO psp/EBOOT/ICON0.png NULL NULL psp/EBOOT/PIC0.png psp/EBOOT/SND0.at3 $(EXE).prx NULL

.PHONY: pbp
endif
ifeq ($(TARGET_DC),1)
$(BUILD_DIR)/sm_warm.ld: Makefile
	@printf '/* SM_WARM code (gfx_retro_dc.c) on I-cache sets $(DC_SM_WARM_SET).. */\n.sub1 : {\n  . = MAX(., %d); *(.sub1.sm64_warm)\n}\n' $$(( $(DC_SM_WARM_SET) * 32 )) > $@
	@printf '/* SM_WARM2 code on I-cache sets $(DC_SM_WARM2_SET).. */\n.sub2 : {\n  . = MAX(., %d); *(.sub2.sm64_warm)\n}\n' $$(( $(DC_SM_WARM2_SET) * 32 )) >> $@
	@printf '/* The hot data at .sub0 + $(DC_HOT_DATA_RESIDUE) mod 16 KB, .bss from a 16 KB boundary (O-cache sets). */\n.sub3 : {\n  . = MAX(., %d); *(.data.sm64_hot)\n}\n.bss : {\n  . = ALIGN(0x4000);\n}\n' $$(( ($(DC_HOT_DATA_RESIDUE) + 0x2000) & 0x3fff )) >> $@

elf: $(EXE)
	sh-elf-objcopy -R .stack -O binary $< $<.bin

scramble: elf
	$(KOS_BASE)/utils/scramble/scramble $(EXE).bin $(BUILD_DIR)/1ST_READ.bin

# The songs and sounds, recorded from the ROM's audio driver on this machine and turned into AICAflow
# assets (tools/aica/sm64_audio_assets.sh): build/us_dc/afx_stage/afx, the disc's /afx.
AICAFORGE_DIR ?= third_party/aicaforge
$(BUILD_DIR)/afx_sm64: tools/aica/afx_sm64.c $(AICAFORGE_DIR)/src/afx_compile_c.c $(AICAFORGE_DIR)/src/afx_compile_c.h
	cc -std=c11 -O2 -I$(AICAFLOW_DIR)/driver/format/include -I$(AICAFORGE_DIR)/src -o $@ $< \
	  $(AICAFORGE_DIR)/src/afx_compile_c.c $(AICAFLOW_DIR)/driver/format/src/codec.c -lm
$(BUILD_DIR)/afx_stage/afx/scene_core.smp: $(BUILD_DIR)/afx_sm64 $(SOUND_BIN_DIR)/aica_sample_table.c \
    $(SOUND_BIN_DIR)/sequences.bin $(wildcard tools/aica/*.py tools/aica/recorder/*) \
    tools/aica/sm64_audio_assets.sh tools/aica/sm64_sound_choices.json
	rm -rf $(BUILD_DIR)/afx_stage
	DC_GEN=$(BUILD_DIR) AFX_SM64=$(BUILD_DIR)/afx_sm64 MAKE=$(MAKE) PYTHON=$(PYTHON) \
	  tools/aica/sm64_audio_assets.sh $(BUILD_DIR)/afx_stage
# The TAS movie and its slices, for the debug menu (src/game/tas_slice.c).
$(BUILD_DIR)/afx_stage/afx/tas_slices.bin: $(BUILD_DIR)/afx_stage/afx/scene_core.smp tas/tas.m64 tas/tas_slices.bin
	cp tas/tas.m64 tas/tas_slices.bin $(BUILD_DIR)/afx_stage/afx/

cdi: $(EXE) $(BUILD_DIR)/afx_stage/afx/tas_slices.bin
	sh-elf-objcopy -R .stack -O binary $< sm64.bin
	kos-cc loader.c -o loader.elf
	mkdcdisc -n "Super Mario 64" -N -f sm64.bin -e loader.elf -d $(BUILD_DIR)/afx_stage/afx -o mario64.cdi

.PHONY: scramble
.PHONY: elf
endif
endif



.PHONY: all clean distclean default diff test load libultra
# with no prerequisites, .SECONDARY causes no intermediate target to be removed
.SECONDARY:

# Remove built-in rules, to improve performance
MAKEFLAGS += --no-builtin-rules

-include $(DEP_FILES)

print-% : ; $(info $* is a $(flavor $*) variable set to [$($*)]) @true
