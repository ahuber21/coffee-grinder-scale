#!/bin/bash

PATH=$HOME/.platformio/packages/toolchain-xtensa-esp32/bin:$PATH
ELF=/Users/ahuber/Nextcloud/basteln/2023-12-10-espresso-scale-eureka/.pio/build/esp_wroom_02/firmware.elf

for word in $@; do
    PC=$(echo $word | grep -oE "^[0-9a-fA-Zx]+")
    test -z $PC && continue
    xtensa-esp32-elf-addr2line -pfiaC -e $ELF $PC
done
