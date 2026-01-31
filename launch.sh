#!/bin/sh
DIR="$(dirname "$0")"
cd "$DIR"

# Use system PLATFORM variable, fallback to tg5040 if not set
[ -z "$PLATFORM" ] && PLATFORM="tg5040"

export LD_LIBRARY_PATH="$DIR:$DIR/bin:$DIR/bin/$PLATFORM:$LD_LIBRARY_PATH"
export HOME="/mnt/SDCARD/.userdata/$PLATFORM"

# Run the platform-specific binary
"$DIR/bin/$PLATFORM/netplay.elf" &> "$DIR/log.txt"
