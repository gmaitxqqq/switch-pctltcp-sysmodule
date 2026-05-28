#---------------------------------------------------------------------------------
# switch-pctltcp-sysmodule Makefile
# Builds exefs.nsp for Atmosphere sysmodule deployment
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET := pctltcp-sysmodule
APPLET_TYPE := 4
NOSHAREDFW := true
BUILD   := build
SOURCES := source
INCLUDES := include

#---------------------------------------------------------------------------------
ARCH    := -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS  := -g -Wall -O2 -ffunction-sections $(ARCH) -D__SWITCH__ -DVERSION_S=\"1.0.0\"
CXXFLAGS:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11
ASFLAGS := -g $(ARCH)
LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH)
LIBS    := -lnx
LIBDIRS := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES  := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES:= $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))

ifeq ($(strip $(CPPFILES)),)
        export LD := $(CC)
else
        export LD := $(CXX)
endif

export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export OFILES    := $(OFILES_SRC)
export INCLUDE   := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                    -I$(CURDIR)/$(BUILD)
export LIBPATHS  := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo "Cleaning..."
	@rm -fr $(BUILD) $(TARGET).elf exefs.nsp

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS := $(OFILES_SRC:.o=.d)

.PHONY: all

all: exefs.nsp

$(OUTPUT).elf : $(OFILES)
	@echo "  LINK     $@"
	@$(LD) -o $@ $(OFILES) $(LDFLAGS) $(LIBPATHS) $(LIBS)

exefs.nsp : $(OUTPUT).elf exefs/main.npdm
	@echo "  NPDMTOOL exefs/main.npdm"
	@npdmtool exefs/main.npdm $(OUTPUT).npdm
	@echo "  ELF2NSP   $@"
	@elf2nsp $(OUTPUT).elf $(OUTPUT).npdm $@

-include $(DEPENDS)

%.o : %.c
	@echo "  CC       $<"
	@$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d $(CFLAGS) $(INCLUDE) -c $< -o $@

%.o : %.cpp
	@echo "  CXX      $<"
	@$(CXX) -MMD -MP -MF $(DEPSDIR)/$*.d $(CXXFLAGS) $(INCLUDE) -c $< -o $@

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
