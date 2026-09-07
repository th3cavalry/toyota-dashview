#!/bin/bash
set -e

DEVICE="$1"

if [ -z "$DEVICE" ]; then
    echo "Usage: sudo $0 /dev/sdX   (check 'lsblk' first — this DESTROYS all data on that device)"
    exit 1
fi

if [ ! -b "$DEVICE" ]; then
    echo "Error: $DEVICE is not a block device (check lsblk)!"
    exit 1
fi

echo "=== THIS WILL ERASE EVERYTHING ON $DEVICE ==="
lsblk -o NAME,SIZE,MOUNTPOINT "$DEVICE" 2>/dev/null || true
read -r -p "Type the exact device path to confirm: " CONFIRM
if [ "$CONFIRM" != "$DEVICE" ]; then
    echo "Confirmation did not match. Aborted."
    exit 1
fi

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
echo "=== SUCCESS: MicroSD ($PARTITION) formatted as FAT32 ==="
echo "You can now eject the card and insert it into the MicroSD slot."
echo ""
echo "Optional: seed vehicle profiles (creates /profiles/ on the card)."
echo "  cp profiles/*.json /media/<you>/TACOMA_CAN/profiles/"
echo "Then pick the profile on the device: Settings > Vehicle Profile."
