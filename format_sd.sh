#!/bin/bash
set -e

DEVICE="/dev/mmcblk0"

echo "=== Formatting SD Card for LILYGO T-Beam ESP32 ($DEVICE) ==="

if [ ! -b "$DEVICE" ]; then
    echo "Error: Device $DEVICE not found!"
    exit 1
fi

# Ensure all partitions on the device are unmounted
echo "Unmounting any active partitions..."
sudo umount ${DEVICE}* 2>/dev/null || true

echo "Wiping existing partition table and signatures on $DEVICE..."
sudo wipefs --all --force "$DEVICE"

echo "Creating standard MBR (MS-DOS) partition table..."
sudo parted -s "$DEVICE" mklabel msdos
sudo parted -s "$DEVICE" mkpart primary fat32 1MiB 100%
sudo parted -s "$DEVICE" set 1 boot on

# Wait for kernel to register partition
sleep 1

PARTITION="${DEVICE}p1"
if [ ! -b "$PARTITION" ]; then
    PARTITION="${DEVICE}1"
fi

echo "Formatting $PARTITION as FAT32 (Label: TACOMA_CAN)..."
sudo mkfs.vfat -F 32 -n "TACOMA_CAN" "$PARTITION"

echo ""
echo "=== SUCCESS: SD Card ($PARTITION) formatted as FAT32 ==="
echo "You can now eject the card and insert it into the T-Beam Supreme MicroSD slot."
