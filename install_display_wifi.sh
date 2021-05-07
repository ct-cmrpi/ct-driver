#!/usr/bin/env bash

cd /home/pi/linux/
zcat /proc/config.gz > .config.running &&
scripts/diffconfig .config.running .config &&
make prepare &&
make -C /lib/modules/$(uname -r)/build M=drivers/gpu/drm/panel/ modules &&
make -C /lib/modules/$(uname -r)/build M=drivers/staging/rtl8723bs/ modules &&
sudo make -C /lib/modules/$(uname -r)/build M=drivers/gpu/drm/panel/ modules_install &&
sudo make -C /lib/modules/$(uname -r)/build M=drivers/staging/rtl8723bs/ modules_install && 
sudo depmod &&
sudo modprobe panel-jd9366 &&
sudo modprobe panel-jdi-lt070me05000 &&
sudo modprobe r8723bs &&
