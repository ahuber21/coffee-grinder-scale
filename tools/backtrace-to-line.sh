#!/bin/bash
# Resolves the addresses in an ESP32 backtrace to source lines, e.g.:
#   tools/backtrace-to-line.sh 0x400d1234:0x3ffb1f60 0x400d5678:0x3ffb1f80
# Uses the firmware.elf of the esp_wroom_02 build.

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
elf="$repo_root/.pio/build/esp_wroom_02/firmware.elf"
export PATH="$HOME/.platformio/packages/toolchain-xtensa-esp32/bin:$PATH"

for word in "$@"; do
  pc=$(echo "$word" | grep -oE "^[0-9a-fA-Zx]+")
  [ -z "$pc" ] && continue
  xtensa-esp32-elf-addr2line -pfiaC -e "$elf" "$pc"
done
