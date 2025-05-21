# How to boot Hailo-15 from a system with an empty eMMC

This guide will show how to setup a Hailo-15 to boot from SPI flash, program the eMMC and then boot the complete OS.

## Setup software

### Install Hailo-15 Board Tools

Unpack hailo_vision_processor_sw_package_2025-04.tar.gz


```bash
cd hailo_vision_processor_sw_package_<version>
cd tools
```

### Create a new virtual environment

```bash
python3 -m virtualenv hailo15_env
source hailo15_env/bin/activate
```
### Install board tools
```bash
pip install hailo15_board_tools-<VERSION>.whl
sudo apt-get install u-boot-tools
```
```bash
cd ..
vim /etc/udev/rules.d/11-ftdi.rules 
```
Add the following lines if not done so already
```txt
# FT4232/FT4232H 
SUBSYSTEM=="usb", ATTR{idVendor}=="0403", ATTR{idProduct}=="6011", GROUP="plugdev", MODE="0664"
```

## Add u-boot-tfa.itb file to QSPI flash

The current version of the Hailo-15 EVB Quick Start Guide misses one file we need to add to the SPI flash memory. Without the u-boot-tfa.itb file the bootmenu is not available. Below is an example bash script that adds the u-boot-tfa.itb file to the flash programming step.

Create a file called program_spi_flash.sh and add the text below.

```bash
#!/bin/bash

uart_boot_fw_loader --serial-device-name /dev/ttyUSB0  --firmware ./build/tmp/deploy/images/astrial-h15/hailo15_uart_recovery_fw.bin && hailo15_spi_flash_program  --scu-bootloader ./build/tmp/deploy/images/astrial-h15/hailo15_scu_bl.bin  --scu-bootloader-config ./build/tmp/deploy/images/astrial-h15/scu_bl_cfg_a.bin  --scu-firmware ./build/tmp/deploy/images/astrial-h15/hailo15_scu_fw.bin  --uboot-device-tree ./build/tmp/deploy/images/astrial-h15/u-boot.dtb.signed  --bootloader ./build/tmp/deploy/images/astrial-h15/u-boot-spl.bin  --bootloader-env ./build/tmp/deploy/images/astrial-h15/u-boot-initial-env  --customer-certificate ./build/tmp/deploy/images/astrial-h15/customer_certificate.bin  --uart-load --serial-device-name /dev/ttyUSB0 --uboot-tfa ./build/tmp/deploy/images/astrial-h15/u-boot-tfa.itb
```

## Program QSPI flash

To program the QSPI flash we need to set some DIP switches on the EVB.

#### DIP Switches ASTRIAL H15

1=ON 2=OFF

Power on the board and run the script. You may need to reset the board or power cycle it if you encounter errors.

```bash
./program_spi_flash.sh 
exit
```
Power down the board, Restore switches to original configuration to enable normal boot sequence.

1=OFF 2=OFF

## Programming the eMMC memory

This step requires the EVB to be connected via USB-Serial and Ethernet.
Assumes standard IP address: Hailo-15: 10.0.0.1, PC: 10.0.0.2
We will setup a TFTP server that provides the Yocto image in form of a wic file. The prebuilt file is called: **core-image-hailo-dev-astrial-h15.wic**

### Setting up a TFTP server
Alias **t15**

#### Source environment we created earlier
```bash
source hailo15_env/bin/activate
```

#### Install TFTP server
```bash
pip install tftpy
```

#### Create python script tftp_server.py

Create a new file and add the following text. Adapt the path to the location of the wic file.

```python
# tftp_server.py
import tftpy

server = tftpy.TftpServer('prebuilt/evb')
server.listen('0.0.0.0', 69)
```

#### Run tftp server script
Note: Ports below 1024 require root privileges
```bash
sudo tools/hailo15_env/bin/python tftp_server.py
```
### Updating eMMC

#### Open serial terminal
Alias **s15**
```bash
picocom --baud 115200 /dev/a53_serial
```
Boot the EVB.

#### Select Update eMMC (wic) from TFTP
```bash
  *** U-Boot Boot Menu ***

     Autodetect
     Boot from SD Card
     Boot from eMMC
     Update SD (wic) from TFTP
     Update eMMC (wic) from TFTP
     Update SD (partitions) from TFTP
     Update eMMC (partitions) from TFTP
     Boot from NFS
     SWUpdate
     SWUpdate AB board init
     U-Boot console


  Press UP/DOWN to move, ENTER to select, ESC/CTRL+C to quit
```

From the U-Boot console run

```
setenv boot_mmc 'run bootargs_base bootargs_mmc && run load_fitimage_from_mmc && bootm ${far_ram_addr}#conf-sysele_${machine_name}.dtb${dtb_overlays}'
setenv machine_name astrial-h15
setenv core_image_name core-image-hailo-dev
saveenv
```

In case of timeout

```bash
switch to partitions #0, OK
mmc1(part 0) is current device
ethernet@1b5000: PHY present at 0
ethernet@1b5000: Starting autonegotiation...
ethernet@1b5000: Autonegotiation complete
ethernet@1b5000: link up, 1000Mbps full-duplex (lpa: 0x2800)
Using ethernet@1b5000 device
TFTP from server 10.0.0.2; our IP address is 10.0.0.1
Filename 'core-image-minimal-hailo15-evb-security-camera.wic'.
Load address: 0x85000000
Loading: T T T T T T T T T T 
Retry count exceeded; starting again
hailo15#
```

retry by calling bootmenu

```bash
hailo15# bootmenu
```
Output should look like the following. This will take a little while.

```bash
switch to partitions #0, OK
mmc1(part 0) is current device
ethernet@1b5000: PHY present at 0
ethernet@1b5000: Starting autonegotiation...
ethernet@1b5000: Autonegotiation complete
ethernet@1b5000: link up, 1000Mbps full-duplex (lpa: 0x7800)
Using ethernet@1b5000 device
TFTP from server 10.0.0.2; our IP address is 10.0.0.1
Filename 'core-image-minimal-hailo15-evb-security-camera.wic'.
Load address: 0x85000000
Loading: #################################################################
	 #################################################################
...
	 ######################################################
	 10.7 MiB/s
done
Bytes transferred = 2322353152 (8a6c4c00 hex)
Device: sdio1@78001000
Manufacturer ID: 45
OEM: 100
Name: DG403 
Bus Speed: 52000000
Mode: MMC High Speed (52MHz)
Rd Block Len: 512
MMC version 5.1
High Capacity: Yes
Capacity: 29.1 GiB
Bus Width: 4-bit
Erase Group Size: 512 KiB
HC WP Group Size: 8 MiB
User Capacity: 29.1 GiB WRREL
Boot Capacity: 4 MiB ENH
RPMB Capacity: 4 MiB ENH
Boot area 0 is not write protected
Boot area 1 is not write protected

MMC write: dev # 1, block # 0, count 4535847 ... 4535847 blocks written: OK

```

#### Reboot and select Boot from eMMC


