TARGET	:= pctltcp-sysmodule
SOURCES	:= source
DATA	:= data

# Sysmodule (not application!)
APPLET_TYPE	:= 4
NOSHAREDFW	:= true

ARCH	:= -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS	+= -O2 -ffunction-sections -fdata-sections -DVERSION_S=\"1.0.0\"
CXXFLAGS	+= $(CFLAGS)
LDFLAGS	+= -Wl,--gc-sections

LIBS	:= -lnx -lpthread

NACP_TITLE	:= "PCTL Sysmodule"
NACP_AUTHOR	:= "gmaitxqqq"
NACP_VERSION	:= "1.0.0"

include $(DEVKITPRO)/libnx/switch.build
