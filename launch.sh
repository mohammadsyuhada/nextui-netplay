#!/bin/sh
DIR="$(dirname "$0")"
cd "$DIR"
[ -z "$PLATFORM" ] && PLATFORM="tg5040"
export LD_LIBRARY_PATH="$DIR:$DIR/bin:$LD_LIBRARY_PATH"
export HOME="/mnt/SDCARD/.userdata/$PLATFORM"
"$DIR/bin/netplay.elf" &> "$DIR/log.txt"
