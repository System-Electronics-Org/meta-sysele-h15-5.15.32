# Fix missing clean_symlinks_config_isp.sh script installation
# The script exists in the source but was not being installed correctly
# Add custom detection_dsi.sh script for DSI display support

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://detection_dsi.sh"

do_install:append() {
    if [ -f "${WORKDIR}/git/apps/h15/gstreamer/clean_symlinks_config_isp.sh" ]; then
        install -m 0755 ${WORKDIR}/git/apps/h15/gstreamer/clean_symlinks_config_isp.sh ${D}/home/root/apps/clean_symlinks_config_isp.sh
    fi
    
    # Install custom detection_dsi.sh script
    install -d ${D}/home/root/apps/detection
    install -m 0755 ${WORKDIR}/detection_dsi.sh ${D}/home/root/apps/detection/detection_dsi.sh
}