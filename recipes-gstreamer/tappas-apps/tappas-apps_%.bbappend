# Fix missing clean_symlinks_config_isp.sh script installation
# The script exists in the source but was not being installed correctly

do_install:append() {
    if [ -f "${WORKDIR}/git/apps/h15/gstreamer/clean_symlinks_config_isp.sh" ]; then
        install -m 0755 ${WORKDIR}/git/apps/h15/gstreamer/clean_symlinks_config_isp.sh ${D}/home/root/apps/clean_symlinks_config_isp.sh
    fi
}