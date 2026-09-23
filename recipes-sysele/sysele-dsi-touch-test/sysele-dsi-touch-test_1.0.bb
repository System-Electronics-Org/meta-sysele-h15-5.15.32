SUMMARY = "System Electronics DSI touch test for Astrial H15"
DESCRIPTION = "Touch test for the DSI panel: the screen turns black and a red dot follows every finger, fading back to black."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://meson.build \
    file://meson_options.txt \
    file://main.cpp \
    file://dsi_touch_test \
"

S = "${WORKDIR}"

inherit meson pkgconfig

DEPENDS = " \
    gstreamer1.0 \
    gstreamer1.0-plugins-base \
    opencv \
"

RDEPENDS:${PN} += " \
    gstreamer1.0-plugins-bad-kms \
    gstreamer1.0-plugins-base-app \
    gstreamer1.0-plugins-base-videoconvert \
"

# apps/dsi/<name> holds what the dsi_<name> command uses and produces, and the
# command itself lives in bin with every other command.
SYSELE_APP_DIR = "/opt/sysele/apps/dsi/touch_test"
SYSELE_BIN_DIR = "/opt/sysele/bin"
EXTRA_OEMESON += "-Dinstall_dir=${SYSELE_APP_DIR}"

do_install:append() {
    # The application directory holds one executable, its logs, and a run link
    # to the command, so that starting it from here does the same thing as
    # typing the command.
    install -d ${D}${SYSELE_APP_DIR}/logs

    install -d ${D}${SYSELE_BIN_DIR}
    install -m 0755 ${WORKDIR}/dsi_touch_test ${D}${SYSELE_BIN_DIR}/dsi_touch_test
    ln -sf ${SYSELE_BIN_DIR}/dsi_touch_test ${D}${SYSELE_APP_DIR}/run

    install -d ${D}${bindir}
    ln -sf ${SYSELE_BIN_DIR}/dsi_touch_test ${D}${bindir}/dsi_touch_test
}

FILES:${PN} += " \
    ${SYSELE_APP_DIR} \
    ${SYSELE_BIN_DIR}/dsi_touch_test \
"

COMPATIBLE_MACHINE = "^astrial-h15$"
