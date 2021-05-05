#!/usr/bin/env bash

sudo apt-get install usb-modeswitch ppp
cd /home/pi/ && mkidr 4Gmodem
sudo cp /home/pi/ct-driver/cellular/umtskeeper.tar.gz /home/pi/4Gmodem/
md5sum /home/pi/4Gmodem/umtskeeper.tar.gz
tar -xzvf /home/pi/4Gmodem/umtskeeper.tar.gz

