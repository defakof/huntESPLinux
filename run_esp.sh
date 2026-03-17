#!/bin/bash
# Run Hunt ESP overlay
# Game must be in borderless windowed (not exclusive fullscreen) for overlay to render on top.

# Load kernel module if not loaded
if [ ! -e /dev/hunt_read ]; then
    echo "Loading kernel module..."
    sudo insmod /var/home/defakof/Desktop/hunt/driver/hunt_reader.ko
fi

# Run overlay
cd /var/home/defakof/Desktop/hunt
./overlay/build/hunt_esp
