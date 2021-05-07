#!/usr/bin/env bash

sudo apt-get install git bc bison flex libssl-dev &&
sudo apt-get install libncurses5-dev &&
sudo wget https://raw.githubusercontent.com/notro/rpi-source/master/rpi-source -O /usr/local/bin/rpi-source && sudo chmod +x /usr/local/bin/rpi-source && /usr/local/bin/rpi-source -q --tag-update &&
rpi-source &&
sudo cp ct-driver/display/drivers/gpu/drm/panel/panel-jd9366.c ~/linux/drivers/gpu/drm/panel/ &&
sudo cp ct-driver/display/drivers/gpu/drm/panel/Kconfig ~/linux/drivers/gpu/drm/panel/ &&
sudo cp ct-driver/display/drivers/gpu/drm/panel/Makefile ~/linux/drivers/gpu/drm/panel/ &&
cd /home/pi/linux &&
make menuconfig &&
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
cd /home/pi/ct-driver/display && dtc -I dts -O dtb -o panel-jd9366.dtbo panel-jd9366.dts && sudo cp panel-jd9366.dtbo /boot/overlays/ &&
cd /home/pi/ct-driver/touch && sudo dtc -I dts -O dtb -o ct_touch.dtbo ct_touch.dts && sudo cp ct_touch.dtbo /boot/overlays/ &&

echo "\n Please edit configfile : sudo nano /boot/config.txt and reboot" 
