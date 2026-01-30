###########################################################
# Netplay Tool - Single binary build
# Uses tg5040 platform (handles both Brick and Smart Pro at runtime)
###########################################################

# Build platform (tg5040 handles runtime detection via is_brick)
PLATFORM = tg5040

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

SOURCE = netplay.c netplay_config.c fileops.c ui.c selfupdate.c \
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
MY_LDFLAGS += -lmsettings -lsamplerate -lzip -lm -ldl -lpthread

# Output binary (single binary for all platforms)
PRODUCT = ../bin/$(TARGET).elf

all:
	@mkdir -p ../bin
	$(CC) $(SOURCE) -o $(PRODUCT) $(MY_CFLAGS) $(MY_LDFLAGS)

clean:
	rm -f ../bin/$(TARGET).elf
