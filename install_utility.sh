#!/usr/bin/env bash

sudo apt-get -y --no-install-recommends install libqt*5-dev qt*5-dev && 
sudo apt-get -y install qml-module-qtquick-* && 
sudo apt-get -y install matchbox-keyboard && 
sudo apt-get -y install xfonts-thai &&
sudo apt -y install libasound2-dev &&
sudo cp -n /home/pi/ct-driver/utility/keyboard/Executable/matchbox-keyboard /usr/local/bin && 
sudo chmod +x /usr/local/bin/matchbox-keyboard &&
sudo cp -r /home/pi/ct-driver/utility/keyboard/Configuration/matchbox-keyboard /usr/local/share &&
sudo cp -n /home/pi/ct-driver/utility/fonts/browalia.ttc /usr/share/fonts/truetype/ &&
sudo cp -n /home/pi/ct-driver/utility/fonts/DB\ Helvethaica\ X\ v3.2.ttf /usr/share/fonts/truetype/ &&
sudo cp .env home/pi/
echo "\n end of install and reboot" 
sudo reboot now