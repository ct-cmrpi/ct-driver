## Driver for the ct-cmrpi 
Modified drivers and device tree sources on the open source Raspberry Pi Compute Module 3+
based on Raspberry Pi OS and kernel 5.10.33 

```
$ uname -a
Linux raspberrypi 5.10.33-v7+ #1415 SMP Fri Apr 30 15:49:00 BST 2021 armv7l GNU/Linux
```

## First Install 
we used [rpi-source](https://github.com/RPi-Distro/rpi-source) to obtain kernel source 
```
sudo apt-get update
sudo apt install raspberrypi-kernel-headers
sudo apt-get install git bc bison flex libssl-dev
sudo apt-get install libncurses5-dev
git clone https://github.com/ct-cmrpi/ct-driver.git
sudo wget https://raw.githubusercontent.com/notro/rpi-source/master/rpi-source -O /usr/local/bin/rpi-source && sudo chmod +x /usr/local/bin/rpi-source && /usr/local/bin/rpi-source -q --tag-update
rpi-source
sudo reboot
```
 
## MIPI DSI Display, SDIO WIFI, CTP Touch
LCD display it has 7.0Inch 800X1280 IPS MIPI AML070WXBI4002
WIFI module it has RTL8723BS SDIO

First copy file to kernel source directory
```
sudo cp ct-driver/display/drivers/gpu/drm/panel/panel-jd9366.c ~/linux/drivers/gpu/drm/panel/
sudo cp ct-driver/display/drivers/gpu/drm/panel/Kconfig ~/linux/drivers/gpu/drm/panel/
sudo cp ct-driver/display/drivers/gpu/drm/panel/Makefile ~/linux/drivers/gpu/drm/panel/
```

Run `make menuconfig` to configure and build the modules

```
cd /home/pi/linux
make menuconfig
```

Kernel configuration as follows
```
#----------- Display ------------
#Device Drivers  --->
#	Graphics support  --->
#		Display Panels  --->  
#			<M> JD9366 panel
#			
#---------- WiFi module ----------
#Device Drivers  --->
#	Staging drivers  --->
#		Realtek RTL8723BS SDIO Wireless LAN NIC driver	
```
Build, Install and Load the modules
```
zcat /proc/config.gz > .config.running
scripts/diffconfig .config.running .config
make prepare

make -C /lib/modules/$(uname -r)/build M=drivers/gpu/drm/panel/ modules
make -C /lib/modules/$(uname -r)/build M=drivers/staging/rtl8723bs/ modules
sudo make -C /lib/modules/$(uname -r)/build M=drivers/gpu/drm/panel/ modules_install
sudo make -C /lib/modules/$(uname -r)/build M=drivers/staging/rtl8723bs/ modules_install
sudo depmod
sudo modprobe panel-jd9366
sudo modprobe panel-jdi-lt070me05000
sudo modprobe r8723bs
```

compiled device tree overlay and copied to `/boot/overlays` 

```
cd /home/pi/ct-driver/display && dtc -I dts -O dtb -o panel-jd9366.dtbo panel-jd9366.dts && sudo cp panel-jd9366.dtbo /boot/overlays/ 
cd /home/pi/ct-driver/touch && sudo dtc -I dts -O dtb -o ct_touch.dtbo ct_touch.dts && sudo cp ct_touch.dtbo /boot/overlays/
```
and configure boot file in `sudo nano /boot/config.txt` as follows

```
#==================== CT-200 Rev3.2 gpio config deteil===================
#	6=RST_LCD, 7=DCDC_EN, 8=PWR_USB2, 9=PWR_USB1, 10=FAN, 11=Relay, 12=LCD_EN, 
#	13=BT_dis, 22=PWR_SFM, 23=PWR_EC25, 24=MUTE_AMP, 25=WL_DS, 26=WL_H_WAKE, 
#	27=RST_EC25,30=BT_wake, 31=BT_H_wake, 42=RST_TP, 43=TP_INT_B
gpio=7=op,dh 
gpio=8=op,dh
gpio=9=op,dh
gpio=10=op,dl
gpio=22=op,dl
gpio=23=op,dl
gpio=24=op,dl

#===== Set the audio lines on GPIO40 and GPIO41 pins
dtoverlay=pwm-2chan,pin=40,func=4,pin2=41,func2=4

#	MIPI DSI Display
dtoverlay=vc4-kms-v3d
#dtoverlay=vc4-kms-dsi-lt070me05000-v2
dtoverlay=panel-jd9366
gpio=12=op,dh

#===== WiFi
dtoverlay=sdio,sdio_overclock=25,gpios_34_39,poll_once=on

#===== Touch
dtparam=i2c0=on
dtoverlay=ct_touch
```

## GPU, drivers, and screen setup
custom DSI panel that starts in 800x1280 mode
rotated display to landscape (1280x800) with 

```
xrandr --output DSI-1 --rotate left
```

touchscreen rotation
```
sudo nano /usr/share/X11/xorg.conf.d/40-libinput.conf
```
add option :
```
Section "InputClass"
        Identifier "libinput touchscreen catchall"
        MatchIsTouchscreen "on"
        MatchDevicePath "/dev/input/event*"
        Driver "libinput"
        Option "TransformationMatrix" "0 -1 1 1 0 0 0 0 1"
EndSection
```

boot display rotation `sudo nano /boot/cmdline.txt`
```
video=DSI-1:800x1280@60,rotate=90
```

