FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

LINUX_YOCTO_HAILO_BOARD_VENDOR = "sysele"

# Add device tree files - just the files, no patches
SRC_URI += " \
    file://arch/arm64/boot/dts/sysele/astrial-h15.dts \
    file://arch/arm64/boot/dts/sysele/astrial-h15-ws101.dts \
    file://arch/arm64/boot/dts/sysele/panel-ws101.dts \
    file://arch/arm64/boot/dts/sysele/Makefile \
"
do_configure:prepend() {
    # Create vendor directory
    install -d ${S}/arch/arm64/boot/dts/sysele
    
    # Copy device tree files
    cp ${WORKDIR}/arch/arm64/boot/dts/sysele/astrial-h15.dts ${S}/arch/arm64/boot/dts/sysele/
    cp ${WORKDIR}/arch/arm64/boot/dts/sysele/astrial-h15-ws101.dts ${S}/arch/arm64/boot/dts/sysele/
    cp ${WORKDIR}/arch/arm64/boot/dts/sysele/panel-ws101.dts ${S}/arch/arm64/boot/dts/sysele/
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

# Raspberry Pi 7" touchscreen panel support.
SRC_URI:append = " file://0001-rpi-touchscreen-retry-reg-id-read.patch"

# USB3: allow the board device tree to force DRIVE_VBUS high.
# Inseparable from the hailo,force-drive-vbus property in the Linux DTS: the
# patch without the property is inert, the property without the patch is
# silently ignored.
SRC_URI:append = " file://0003-cdns3-hailo-force-drive-vbus.patch"

# WM8960 audio codec on the Astrial H15 carrier.
SRC_URI:append = " file://0005-wm8960-auto-sysclk.patch \
    file://wm8960.cfg"

# i2c-gpio, needed only by the bitbanged audio bus. Goes away with it.
SRC_URI:append = " file://i2c-gpio.cfg"

# ISP: with one sensor the front-end stays disabled, and a sensor on CSI-RX1
# presents vdid 1, which the driver refuses in that mode. Experimental, the
# reasoning is in the patch header.
SRC_URI:append = " file://0006-isp-allow-vdid-nonzero-without-fe.patch"

# Waveshare DSI 10.1" 1280x800 panel. The driver is not upstream: it comes from
# the Raspberry Pi fork and has to be re-taken by hand on every kernel bump. The
# patch header lists the four points to re-check when that happens.
SRC_URI:append = " file://0007-panel-waveshare-dsi.patch \
    file://waveshare-dsi.cfg"

# Panel MCU: re-send the configuration registers on every prepare. Without this
# any DPMS off or console blank leaves the panel dark until the next boot,
# because the MCU does not come back on the power register alone.
SRC_URI:append = " file://0008-panel-waveshare-mcu-config-on-prepare.patch"

# cdns-dsi: the D-PHY was initialized and powered on and never de-initialized,
# so it was never rebuilt after a suspend. Upstream fix, plus a synchronous
# suspend so the clock and reset cycle is guaranteed before post_disable
# returns. Must come after 0008: it makes real disables happen, and without
# 0008 the panel does not come back from them.
SRC_URI:append = " file://0009-cdns-dsi-fix-phy-de-init.patch"
