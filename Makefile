#---------------------------------------------------------------------------------
# switch-pctltcp-sysmodule Makefile
# Builds both .nro (for testing) and exefs.nsp (for sysmodule deployment)
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET := pctltcp-sysmodule

# Sysmodule (background service)
APPLET_TYPE := 4
NOSHAREDFW := true

BUILD   := build
SOURCES := source
DATA    := data
INCLUDES := include

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------

ARCH    := -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS  := -g -Wall -O2 -ffunction-sections \
           $(ARCH) $(DEFINES)

CFLAGS  += $(INCLUDE) -D__SWITCH__ -DVERSION_S=\"1.0.0\"

CXXFLAGS:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS := -g $(ARCH)

LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS    := -lnx

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------

LIBDIRS := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)

export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES        := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES        := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
        export LD       := $(CC)
else
        export LD       := $(CXX)
endif

export OFILES_BIN       := $(addsuffix .o,$(BINFILES))
export OFILES_SRC       := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES   := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN       := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).nro exefs.nsp

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

.PHONY: all

DEPENDS := $(OFILES_SRC:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all : $(OUTPUT).nro $(OUTPUT).elf

$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp $(OUTPUT).icon

$(OUTPUT).elf : $(OFILES)

$(OFILES_SRC) : $(HFILES_BIN)

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
# Build exefs.nsp from .elf + .npdm
#---------------------------------------------------------------------------------
$(OUTPUT).nsp : $(OUTPUT).elf exefs/main.npdm
	@echo "  NPDMTOOL  exefs/main.npdm"
	@npdmtool exefs/main.npdm build/$(TARGET).npdm
	@echo "  ELF2NSP   $@"
	@elf2nsp $(OUTPUT).elf build/$(TARGET).npdm $@

exefs.nsp : $(OUTPUT).nsp
	@cp $< $@

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
