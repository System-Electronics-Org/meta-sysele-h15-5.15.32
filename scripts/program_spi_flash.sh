#!/bin/bash

# Astrial H15 SPI Flash Programming Script
# System Electronics - Hailo-15 Development Platform
# 
# This script programs the SPI flash memory with the necessary bootloader components
# for the Astrial H15 board. Run this script after building the Yocto image.
#
# Prerequisites:
# - Hailo-15 board tools installed in virtual environment
# - Board connected via USB-Serial (typically /dev/ttyUSB0)
# - DIP switches set to programming mode (1=ON, 2=OFF)
# - Built Yocto image artifacts available
# - Root privileges (sudo) for serial port access
#
# Usage: sudo ./program_spi_flash.sh [serial_device]
# Example: sudo ./program_spi_flash.sh /dev/ttyUSB0

set -e

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Error: This script requires root privileges for serial port access."
    echo "Please run with sudo:"
    echo "  sudo ./program_spi_flash.sh [serial_device]"
    exit 1
fi

# Default serial device
SERIAL_DEVICE=${1:-/dev/ttyUSB0}

# Try to find build directory - check common locations
if [ -d "../build/tmp/deploy/images/astrial-h15" ]; then
    BUILD_DIR="../build/tmp/deploy/images/astrial-h15"
elif [ -d "build/tmp/deploy/images/astrial-h15" ]; then
    BUILD_DIR="build/tmp/deploy/images/astrial-h15"
elif [ -d "../../build/tmp/deploy/images/astrial-h15" ]; then
    BUILD_DIR="../../build/tmp/deploy/images/astrial-h15"
else
    BUILD_DIR="build/tmp/deploy/images/astrial-h15"  # Default fallback
fi

echo "========================================"
echo "Astrial H15 SPI Flash Programming"
echo "========================================"
echo "Serial Device: $SERIAL_DEVICE"
echo "Build Directory: $BUILD_DIR"
echo ""

# Check if build artifacts exist
if [ ! -d "$BUILD_DIR" ]; then
    echo "Error: Build directory not found: $BUILD_DIR"
    echo ""
    echo "Searched in the following locations:"
    echo "  - ../build/tmp/deploy/images/astrial-h15"
    echo "  - build/tmp/deploy/images/astrial-h15"  
    echo "  - ../../build/tmp/deploy/images/astrial-h15"
    echo ""
    echo "Please ensure you have:"
    echo "1. Set up the Yocto build environment"
    echo "2. Run 'bitbake core-image-hailo-dev' successfully"
    echo "3. Execute this script from the correct directory"
    exit 1
fi

# Check if serial device exists
if [ ! -e "$SERIAL_DEVICE" ]; then
    echo "Error: Serial device not found: $SERIAL_DEVICE"
    echo "Please check your USB-Serial connection"
    exit 1
fi

echo "Starting UART boot firmware loader..."
./uart_boot_fw_loader \
    --serial-device-name "$SERIAL_DEVICE" \
    --firmware "$BUILD_DIR/hailo15_uart_recovery_fw.bin"

if [ $? -ne 0 ]; then
    echo "Error: UART boot firmware loader failed"
    exit 1
fi

echo "Programming SPI flash memory..."
./hailo15_spi_flash_program \
    --scu-bootloader "$BUILD_DIR/hailo15_scu_bl.bin" \
    --scu-bootloader-config "$BUILD_DIR/scu_bl_cfg_a.bin" \
    --scu-firmware "$BUILD_DIR/hailo15_scu_fw.bin" \
    --uboot-device-tree "$BUILD_DIR/u-boot.dtb.signed" \
    --bootloader "$BUILD_DIR/u-boot-spl.bin" \
    --bootloader-env "$BUILD_DIR/u-boot-initial-env" \
    --customer-certificate "$BUILD_DIR/customer_certificate.bin" \
    --uart-load \
    --serial-device-name "$SERIAL_DEVICE" \
    --uboot-tfa "$BUILD_DIR/u-boot-tfa.itb"

if [ $? -eq 0 ]; then
    echo ""
    echo "========================================"
    echo "SPI Flash Programming Completed Successfully!"
    echo "========================================"
    echo ""
    echo "Next steps:"
    echo "1. Power down the board"
    echo "2. Set DIP switches to normal boot mode (1=OFF, 2=OFF)"
    echo "3. Power on the board to verify U-Boot menu appears"
    echo ""
else
    echo "Error: SPI flash programming failed"
    echo "Please check connections and try again"
    exit 1
fi
