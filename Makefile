#---------------------------------------------------------------------------------
# Makefile for switch-pctltcp-sysmodule (sysmodule)
# Install: sd:/atmosphere/sysmodules/pctltcp-sysmodule.sts
# Build:   make -> pctltcp-sysmodule.sts
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD  is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA    is a list of directories containing data files
# INCLUDES is a list of directories containing header files
#---------------------------------------------------------------------------------
TARGET		:= pctltcp-sysmodule
BUILD		:= build
SOURCES	:= source
DATA		:= data
INCLUDES	:= include

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH		:= -march=armv8-a -mtune=cortex-a57 -mtp=soft -fpie

CFLAGS		:= -g -Wall -O2 -ffunction-sections \
		   $(ARCH) $(DEFINES)

CFLAGS		+= $(INCLUDE) -D__SWITCH__ -DVERSION_S=\"1.0.0\"

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS		:= -g $(ARCH)

LDFLAGS	= -specs=$(DEVKITPRO)/libnx/switch_sysmodule.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS		:= -lnx

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBNX)

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

export DEPSDIR	:= $(CURDIR)/$(BUILD)

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

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).nro $(TARGET).sts

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:= $(OBJFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets — build .sts (sysmodule) instead of .nro
#---------------------------------------------------------------------------------
all	:	$(OUTPUT).sts

# Sysmodule target (self-contained executable)
$(OUTPUT).sts	:	$(OUTPUT).elf
	@echo built ... $(notdir $@)
	@$(NOFDEFAULTS) $< $@

$(OUTPUT).elf	:	$(OBJFILES)

$(OBJFILES_SRC)	: $(HFILES_BIN)

%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
