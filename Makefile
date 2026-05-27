TARGET := pctltcp-sysmodule
BUILD  := build
SOURCES := source
DATA   := data

# Sysmodule (not application!)
APPLET_TYPE := 4
NOSHAREDFW := true

# Compiler flags
ARCH   := -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -g -Wall -O2 -ffunction-sections -fdata-sections -D__SWITCH__ -DVERSION_S=\"1.0.0\"
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11
ASFLAGS  := -g $(ARCH)
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*).map
LIBS   := -lnx -lpthread

# Include switch_rules (NOT switch.build!)
include $(DEVKITPRO)/libnx/switch_rules
