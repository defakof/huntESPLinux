#!/bin/bash
# Run Hunt ESP overlay
#
# The game must be running in borderless windowed at a resolution
# slightly below native (e.g., 2556x1436 instead of 2560x1440)
# so KWin doesn't treat it as exclusive fullscreen.
#
# Alternative: set in Hunt's graphics settings:
#   Display Mode: Windowed
#   Resolution: 2560x1440 (same as native)
#   Then maximize the window with Alt+F11 or drag to fill screen

# Load kernel module if not loaded
if [ ! -e /dev/hunt_read ]; then
    echo "Loading kernel module..."
    sudo insmod /var/home/defakof/Desktop/hunt/driver/hunt_reader.ko
fi

# Run overlay
cd /var/home/defakof/Desktop/hunt
./overlay/build/hunt_esp
