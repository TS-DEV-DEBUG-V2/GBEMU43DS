.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)

TARGET   := gbemu-3ds
BUILD    := build
CORE_DIR := ../../src
SOURCES  := source $(CORE_DIR)

APP_TITLE       := GBEMU
APP_DESCRIPTION := Game Boy emulator
APP_AUTHOR      := TS
APP_ICON        := $(TOPDIR)/icon.png

include $(DEVKITARM)/3ds_rules

export PATH := $(DEVKITPRO)/tools/bin:$(PATH)

BANNER_PNG   := $(TOPDIR)/banner.png
BANNER_AUDIO := $(TOPDIR)/silence.wav
RSF          := $(TOPDIR)/gbemu.rsf

CORE_CPPFILES := gb_cpu.cpp gb_memory.cpp gb_ppu.cpp gb_timer.cpp \
                  gb_input.cpp gb_apu.cpp gb_cartridge.cpp bios.cpp bios4gbc.cpp gb_log.cpp unzip.cpp gb_savestate.cpp gb_gamegenie.cpp gb_gameshark.cpp gb_printer.cpp

PORT_CPPFILES := main.cpp

CPPFILES := $(PORT_CPPFILES) $(CORE_CPPFILES)

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mthumb-interwork

CFLAGS := -g -Wall -O3 -flto -funroll-loops -ffast-math -mword-relocations -ffunction-sections -fomit-frame-pointer $(ARCH) -fcommon \
          -Wno-unused-parameter -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-function \
          $(foreach dir,$(SOURCES),-I$(TOPDIR)/$(dir)) \
          -I$(CTRULIB)/include -I$(PORTLIBS)/include

CXXFLAGS := $(CFLAGS) -std=gnu++17 -fno-rtti -fno-exceptions

LDFLAGS = -specs=3dsx.specs -g -O3 -flto $(ARCH) -Wl,-Map,$(TARGET).map

LIBS := -lcitro2d -lcitro3d -lctru -lm

LIBDIRS := $(CTRULIB) $(PORTLIBS)
LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

LD := $(CXX)

_3DSXFLAGS += --smdh=$(OUTPUT).smdh

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

export OFILES  := $(CPPFILES:.cpp=.o)

.PHONY: $(BUILD) clean all cia

all: $(BUILD)

cia: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile $(CURDIR)/$(TARGET).cia

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf $(TARGET).map $(TARGET).bnr $(TARGET).cia

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).3dsx : $(OUTPUT).elf $(OUTPUT).smdh
$(OUTPUT).elf  : $(OFILES)

$(OUTPUT).bnr : $(BANNER_PNG) $(BANNER_AUDIO)
	$(SILENTMSG) building $(notdir $@)
	$(SILENTCMD)bannertool makebanner -i $(BANNER_PNG) -a $(BANNER_AUDIO) -o $@

$(OUTPUT).cia : $(OUTPUT).elf $(OUTPUT).smdh $(OUTPUT).bnr $(RSF)
	$(SILENTMSG) building $(notdir $@)
	$(SILENTCMD)makerom -f cia -o $@ -rsf $(RSF) -target t \
		-elf $(OUTPUT).elf -icon $(OUTPUT).smdh -banner $(OUTPUT).bnr \
		-desc app:2 -exefslogo

-include $(DEPENDS)

endif
