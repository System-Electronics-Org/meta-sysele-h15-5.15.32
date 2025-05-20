# Flashing SPI Flash

```
/home/sysele/.local/bin/uart_boot_fw_loader --serial-device-name /dev/ttyUSB0  --firmware ./build/tmp/deploy/images/hailo15-sbc/hailo15_uart_recovery_fw.bin && hailo15_spi_flash_program  --scu-bootloader ./build/tmp/deploy/images/hailo15-sbc/hailo15_scu_bl.bin  --scu-bootloader-config ./build/tmp/deploy/images/hailo15-sbc/scu_bl_cfg_a.bin  --scu-firmware ./build/tmp/deploy/images/hailo15-sbc/hailo15_scu_fw.bin  --uboot-device-tree ./build/tmp/deploy/images/hailo15-sbc/u-boot.dtb.signed  --bootloader ./build/tmp/deploy/images/hailo15-sbc/u-boot-spl.bin  --bootloader-env ./build/tmp/deploy/images/hailo15-sbc/u-boot-initial-env  --customer-certificate ./build/tmp/deploy/images/hailo15-sbc/customer_certificate.bin  --uart-load --serial-device-name /dev/ttyUSB0 --uboot-tfa ./build/tmp/deploy/images/hailo15-sbc/u-boot-tfa.itb
```
