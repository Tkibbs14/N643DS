#---------------------------------------------------------------------------------
# N643DS for the 3DS (devkitARM). Run in WSL:  bash core/host3ds/build.sh   ->  core/host3ds/n643ds.3dsx
# (from gc3ds's core/host3ds/Makefile)
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Set DEVKITARM: export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET      := n643ds
BUILD       := build
SOURCES     := . ../src/cpu ../src/sys ../src/jit
DATA        :=
INCLUDES    := .
ROMFS       := romfs

APP_TITLE       := N643DS
APP_DESCRIPTION := N64 emulator (development)
APP_AUTHOR      := N643DS contributors

ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS   := -g -Wall -Wextra -Wno-unused-parameter -O2 -mword-relocations -ffunction-sections $(ARCH)
# The FPU interpreter follows the guest's rounding mode through fesetround (src/cpu/vr4300_fpu.c).
CFLAGS   += $(INCLUDE) -D__3DS__ -D_GNU_SOURCE -std=gnu11 -frounding-math -ffp-contract=off
CFLAGS   += $(EXTRA_CFLAGS)
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS     := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lctru -lm
LIBDIRS  := $(CTRULIB) $(PORTLIBS)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
export LD   := $(CC)

export OFILES_SOURCES := $(CFILES:.c=.o) $(SFILES:.S=.o)
export OFILES         := $(OFILES_SOURCES)

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON := $(CTRULIB)/default_icon.png
export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)

.PHONY: all clean

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

$(OUTPUT).3dsx: $(OUTPUT).elf $(OUTPUT).smdh
$(OUTPUT).elf: $(OFILES)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
