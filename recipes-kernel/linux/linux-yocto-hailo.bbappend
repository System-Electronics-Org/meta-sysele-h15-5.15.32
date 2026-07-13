FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

LINUX_YOCTO_HAILO_BOARD_VENDOR = "sysele"

# Add device tree files - just the files, no patches
SRC_URI += " \
    file://arch/arm64/boot/dts/sysele/astrial-h15.dts \
    file://arch/arm64/boot/dts/sysele/Makefile \
"
do_configure:prepend() {
    # Create vendor directory
    install -d ${S}/arch/arm64/boot/dts/sysele
    
    # Copy device tree files
    cp ${WORKDIR}/arch/arm64/boot/dts/sysele/astrial-h15.dts ${S}/arch/arm64/boot/dts/sysele/
    cp ${WORKDIR}/arch/arm64/boot/dts/sysele/Makefile ${S}/arch/arm64/boot/dts/sysele/
    
    # Instead of patching, directly modify the main Makefile
    # First check if our vendor is already included
    if ! grep -q "subdir-y += sysele" ${S}/arch/arm64/boot/dts/Makefile; then
        # Find the line with "subdir-y += hailo" and add our line after it
        sed -i '/subdir-y += hailo/a subdir-y += sysele' ${S}/arch/arm64/boot/dts/Makefile
        echo "Added sysele to DTS Makefile"
    else
        echo "sysele already in DTS Makefile, skipping"
    fi
}

# Raspberry Pi 7" display support for the Hailo 1.12 kernel and vbus enabled.
SRC_URI:append = " file://0001-rpi-touchscreen-retry-reg-id-read.patch \
    file://0002-hailo15-evb-cpld-display-config.patch \
    file://0003-cdns3-hailo-force-drive-vbus.patch"
