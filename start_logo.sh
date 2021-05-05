#!/usr/bin/env bash

sudo ifconfig wlan0 up && wpa_cli -i wlan0 reconnect && sudo systemctl stop fingerscan.service && sleep 10 && sudo systemctl start fingerscan.service && sleep 30 && pkill -o chromium

