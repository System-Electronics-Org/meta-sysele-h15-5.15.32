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

# Panel MCU: check and retry every i2c write and log the failure. Diagnostics:
# it is how the MCU state that NACKs every write while still ACKing reads was
# found. The configuration registers stay in probe.
SRC_URI:append = " file://0008-panel-waveshare-check-and-retry-mcu-writes.patch"

# cdns-dsi: the D-PHY was initialized and powered on and never de-initialized,
# so it was never rebuilt after a suspend. Upstream fix, plus a synchronous
# suspend so the clock and reset cycle is guaranteed before post_disable
# returns.
SRC_URI:append = " file://0009-cdns-dsi-fix-phy-de-init.patch"

# cdns-dsi: video was enabled without waiting for the clock and data lanes to
# leave LP. Upstream fix, already in stable, plus a measurement of how long the
# wait actually takes, which is the diagnosis we still owe the Hailo ticket.
SRC_URI:append = " file://0010-cdns-dsi-wait-for-lanes-ready.patch"

# Panel: the MCU has no readable ID, so a wrong device tree drives the wrong
# panel silently. Say at probe what the device tree asked for.
SRC_URI:append = " file://0011-panel-waveshare-log-selected-panel.patch"

# D-PHY: power_off never stopped the TX state machine that power_on starts.
# Aligned with mainline.
SRC_URI:append = " file://0012-cdns-dphy-stop-tx-state-machine-in-power-off.patch"

# D-PHY: is_configured / is_powered state and the power_on guard, as mainline.
SRC_URI:append = " file://0013-cdns-dphy-guard-power-on.patch"

# D-PHY: pm_runtime calls with no hardware effect, absent in mainline.
# esc_clk handling stays, it is a Hailo addition.
SRC_URI:append = " file://0014-cdns-dphy-drop-pm-runtime-calls.patch"

# cdns-dsi: clear the lane ready flags together with PLL_LOCKED, so the wait
# measured by 0010 is real on every cycle and not only on the first one.
SRC_URI:append = " file://0015-cdns-dsi-clear-stale-lane-ready-flags.patch"

# cdns-dsi: log a failed phy_power_on instead of ignoring it.
SRC_URI:append = " file://0016-cdns-dsi-report-failed-phy-power-on.patch"

# D-PHY: keep the calibration wait time field when starting the state machine,
# as mainline, instead of overwriting the whole register.
SRC_URI:append = " file://0017-cdns-dphy-preserve-ssm-calibration-wait-time.patch"

# D-PHY: PSM divider set up in power_on, as mainline. First patch to drop if
# this series makes things worse: it changes register programming order.
SRC_URI:append = " file://0018-cdns-dphy-set-up-psm-in-power-on.patch"

# Diagnostic, inert by default: cdns_dphy.cal_wait_time=N forces the PHY
# calibration wait at power on. To be removed once the boot threshold is known.
SRC_URI:append = " file://0019-cdns-dphy-diagnostic-cal-wait-time.patch"
