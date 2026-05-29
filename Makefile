#---------------------------------------------------------------------------------
# Makefile for switch-pctltcp-sysmodule (boot2 sysmodule)
# Install: sd:/atmosphere/contents/<TID>/exefs.nsp + flags/boot2.flag
# Build:   make -> pctltcp-sysmodule.nsp
# Entry point: int main(int argc, char **argv)  (switch.specs CRT0)
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

# ---- Toolchain paths (devkitPro convention) ----
DEVKITA64   := $(DEVKITPRO)/devkitA64
LIBNX      := $(DEVKITPRO)/libnx
TOOLS      := $(DEVKITPRO)/tools

CC          := $(DEVKITA64)/bin/aarch64-none-elf-gcc
CXX         := $(DEVKITA64)/bin/aarch64-none-elf-g++
AS          := $(DEVKITA64)/bin/aarch64-none-elf-as
LD          := $(DEVKITA64)/bin/aarch64-none-elf-gcc
OBJCOPY     := $(DEVKITA64)/bin/aarch64-none-elf-objcopy
NOFDEFAULTS := $(TOOLS)/bin/nofdefaultS

# ---- Flags ----
ARCH    := -march=armv8-a -mtune=cortex-a57 -mtp=soft -fpie
CFLAGS  := -g -Wall -O2 -ffunction-sections $(ARCH) \
            -I$(LIBNX)/include -D__SWITCH__ -DVERSION_S=\"1.0.0\"
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11
ASFLAGS  := -g $(ARCH)
# Force switch.specs (application CRT0, int main).
# switch_sysmodule.specs does NOT exist in devkitPro libnx 4.12.0+.
LDFLAGS := -specs=$(LIBNX)/switch.specs -g $(ARCH) \
            -Wl,-Map,$(notdir $*).map
LIBS    := -L$(LIBNX)/lib -lnx

# ---- Targets ----
TARGET := pctltcp-sysmodule
BUILD  := build
OBJS   := $(BUILD)/main.o $(BUILD)/http_server.o $(BUILD)/pctl_handler.o

.PHONY: all clean

all: $(TARGET).nsp

# ---- NSP from ELF (boot2 sysmodule) ----
$(TARGET).nsp: $(TARGET).elf
	@echo built ... $(notdir $@)
	$(NOFDEFAULTS) $< $@

# ---- ELF link ----
$(TARGET).elf: $(OBJS)
	$(LD) -o $@ $^ $(LDFLAGS) $(LIBS)

# ---- Compile rules ----
$(BUILD)/%.o: source/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/%.o: source/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/%.o: source/%.s | $(BUILD)
	$(AS) $(ASFLAGS) -c -o $@ $<

$(BUILD):
	@mkdir -p $@

# ---- Clean ----
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).nro $(TARGET).nsp

# ---- Dependencies ----
-include $(BUILD)/*.d
