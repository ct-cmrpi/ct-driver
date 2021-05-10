#!/usr/bin/env bash

DISPLAY=:0 pcmanfm --set-wallpaper /home/pi/ct-driver/utility/splash/ffx_white_ct200.png && sudo ifconfig wlan0 up && wpa_cli -i wlan0 reconnect && sudo systemctl stop fingerscan.service && sleep 10 && sudo systemctl start fingerscan.service && sleep 30 && pkill -o chromium && DISPLAY=:0 pcmanfm --set-wallpaper /home/pi/ct-driver/utility/splash/splash.png

