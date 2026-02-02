###########################################################
# Netplay Tool - Multi-platform build
# Supported platforms: tg5040, tg5050
###########################################################

# Supported platforms
SUPPORTED_PLATFORMS = tg5040 tg5050
DEFAULT_PLATFORM = tg5040

# Set platform from environment or default
ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
PLATFORM=$(DEFAULT_PLATFORM)
endif

# Validate platform
ifeq (,$(filter $(PLATFORM),$(SUPPORTED_PLATFORMS)))
$(error Invalid PLATFORM '$(PLATFORM)'. Supported: $(SUPPORTED_PLATFORMS))
endif

# Validate cross-compiler
ifeq (,$(CROSS_COMPILE))
$(error Missing CROSS_COMPILE for this toolchain)
endif

###########################################################

TARGET = netplay

# Base paths (from workspace/nextui-netplay/src/)
WORKSPACE_PATH = ../..
COMMON_PATH = $(WORKSPACE_PATH)/all/common
PLATFORM_PATH = $(WORKSPACE_PATH)/$(PLATFORM)
MINARCH_PATH = $(WORKSPACE_PATH)/all/minarch

# Include paths
INCDIR = -I. -I$(COMMON_PATH) -I$(PLATFORM_PATH)/platform -I$(MINARCH_PATH)/libretro-common/include
INCDIR += -I./include

SOURCE = netplay.c netplay_config.c fileops.c ui.c selfupdate.c \
         include/parson/parson.c \
         $(COMMON_PATH)/utils.c $(COMMON_PATH)/api.c $(COMMON_PATH)/config.c $(COMMON_PATH)/scaler.c \
         $(PLATFORM_PATH)/platform/platform.c

CC = $(CROSS_COMPILE)gcc

# Compiler flags
MY_CFLAGS = -O2 -fomit-frame-pointer
MY_CFLAGS += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -std=gnu99
MY_CFLAGS += -DUSE_SDL2 -DUSE_GLES -DGL_GLEXT_PROTOTYPES
MY_CFLAGS += -I$(PREFIX)/include -I$(PREFIX_LOCAL)/include
MY_CFLAGS += -I$(PLATFORM_PATH)/libmsettings

# Linker flags
MY_LDFLAGS = -L$(PREFIX)/lib -L$(PREFIX_LOCAL)/lib
MY_LDFLAGS += -L$(PREFIX)/lib/$(CROSS_TRIPLE)
MY_LDFLAGS += -L$(PLATFORM_PATH)/libmsettings
MY_LDFLAGS += -L$(MINARCH_PATH)/build/$(PLATFORM)
MY_LDFLAGS += $(shell pkg-config --libs sdl2 glesv2 2>/dev/null || echo "-lSDL2 -lGLESv2")
MY_LDFLAGS += -lSDL2_image -lSDL2_ttf
MY_LDFLAGS += -lmsettings -lsamplerate -lzip -lm -lpthread -ldl -lz
MY_LDFLAGS += -lasound

# Platform-specific dependencies
ifeq ($(PLATFORM), tg5050)
MY_LDFLAGS += -ltinyalsa
endif

# Output binary (platform-specific)
PRODUCT = ../bin/$(PLATFORM)/$(TARGET).elf

all:
	@mkdir -p ../bin/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(MY_CFLAGS) $(MY_LDFLAGS)

clean:
	rm -f ../bin/tg5040/$(TARGET).elf ../bin/tg5050/$(TARGET).elf
