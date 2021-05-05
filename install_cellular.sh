#!/usr/bin/env bash

sudo apt-get install usb-modeswitch ppp
mkdir -p /home/pi/4Gmodem
sudo cp /home/pi/ct-driver/cellular/umtskeeper.tar.gz /home/pi/4Gmodem
md5sum /home/pi/4Gmodem/umtskeeper.tar.gz
tar -xzvf /home/pi/4Gmodem/umtskeeper.tar.gz -C /home/pi/4Gmodem

