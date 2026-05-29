#---------------------------------------------------------------------------------
# Makefile for switch-pctltcp-sysmodule (boot2 sysmodule)
# Installs to: sd:/atmosphere/contents/0100000000000023/exefs.nsp
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
#---------------------------------------------------------------------------------
TARGET		:= pctltcp-sysmodule

# boot2 sysmodule: use switch_sysmodule.specs + userAppMain()
APPLET_TYPE	:= 4
NODEFAULTFW	:= 1

BUILD		:= build
SOURCES		:= source
DATA		:= data
INCLUDES	:= include

# APP_JSON for npdmtool - defines NPDM/permissions
CONFIG_JSON	:= pctltcp-sysmodule.json

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH		:= -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS		:= -g -Wall -O2 -ffunction-sections \
		   $(ARCH) $(DEFINES)

CFLAGS		+= $(INCLUDE) -D__SWITCH__ -DVERSION=\"1.0.0\"

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS		:= -g $(ARCH)

LDFLAGS	= -specs=$(DEVKITPRO)/libnx/switch_sysmodule.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS		:= -lnx

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS		:= $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:= $(CURDIR)/$(TARGET)
export TOPDIR	:= $(CURDIR)

export VPATH	:= $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
		   $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSSDIR	:= $(CURDIR)/$(BUILD)

CFILES		:= $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:= $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:= $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:= $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for C projects
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:= $(CC)
else
	export LD	:= $(CXX)
endif

export OBJFILES_BIN	:= $(addsuffix .o,$(BINFILES))
export OBJFILES_SRC	:= $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OBJFILES		:= $(OBJFILES_BIN) $(OBJFILES_SRC)
export HFILES_BIN	:= $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:= $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
		   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
		   -I$(CURDIR)/$(BUILD)

export LIBPATHS	:= $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(CONFIG_JSON)),)
	jsons := $(wildcard *.json)
	ifneq (,$(findstring $(TARGET).json,$(jsons)))
		export APP_JSON := $(TOPDIR)/$(TARGET).json
	else
		ifneq (,$(findstring config.json,$(jsons)))
			export APP_JSON := $(TOPDIR)/config.json
		endif
	endif
else
	export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).nsp $(TARGET).sts

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPS		:= $(OBJFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets - build .nsp for boot2 sysmodule
#---------------------------------------------------------------------------------
all	:	$(OUTPUT).nsp

# .nsp target: ELF -> NSP (for boot2 / atmosphere/contents/)
$(OUTPUT).nsp	:	$(OUTPUT).elf
	@echo building ... $(notdir $@)
	@$(NOFDEFAULTS) $< $@

$(OUTPUT).elf	:	$(OBJFILES)

$(OBJFILES_SRC)	: $(HFILES_BIN)

%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
