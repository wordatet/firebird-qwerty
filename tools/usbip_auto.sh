#!/bin/bash
# USBIP Automation Script for Firebird
# Usage: ./usbip_auto.sh [attach|detach]

ACTION=$1
BUSID="1-1"
HWID="0451:e022"

if [ "$ACTION" == "attach" ]; then
    # Check if device is already attached to avoid redundant pkexec prompts
    if usbip port | grep -q "$HWID"; then
        echo "Device already attached."
        exit 0
    fi

    # Ensure kernel module is loaded
    if [ ! -d /sys/module/vhci_hcd ]; then
        pkexec modprobe vhci-hcd
    fi
    
    # Wait for vhci to initialize and server to be ready
    sleep 0.5
    
    # Attach device
    pkexec usbip attach --remote localhost --busid $BUSID
    
elif [ "$ACTION" == "detach" ]; then
    # Find the port(s) where our device is attached
    PORTS=$(usbip port | grep "$HWID" | awk '{print $2}' | sed 's/://')
    
    for PORT in $PORTS; do
        pkexec usbip detach --port "$PORT"
    done
else
    echo "Usage: $0 [attach|detach]"
    exit 1
fi
