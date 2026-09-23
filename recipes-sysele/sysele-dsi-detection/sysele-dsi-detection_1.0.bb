SUMMARY = "System Electronics DSI detection demo for Astrial H15"
DESCRIPTION = "Ready-to-run YOLOv8 detection, tracking, privacy effects, and direct DSI output for Astrial H15."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://meson.build \
    file://meson_options.txt \
    file://main.cpp \
    file://dsi_detection \
"

S = "${WORKDIR}"

inherit meson pkgconfig

DEPENDS = " \
    cxxopts \
    gstreamer1.0 \
    gstreamer1.0-plugins-base \
    hailo-analytics-api \
    hailo-postprocess-tools \
    libmedialib \
    opencv \
"

RDEPENDS:${PN} += " \
    gawk \
    gstreamer1.0-plugins-bad-debugutilsbad \
    gstreamer1.0-plugins-bad-kms \
    gstreamer1.0-plugins-base-app \
    gstreamer1.0-plugins-base-videoconvert \
    hailo-analytics-api \
    i2c-tools \
    libmedialib \
    sysele-tools \
    v4l-utils \
"

# apps/dsi/<name> holds what the dsi_<name> command uses and produces, and
# the command itself lives in bin with every other command.
SYSELE_APP_DIR = "/opt/sysele/apps/dsi/detection"
SYSELE_BIN_DIR = "/opt/sysele/bin"
EXTRA_OEMESON += "-Dinstall_dir=${SYSELE_APP_DIR}"

do_install:append() {
    # The application directory holds one executable, its logs, and a run link
    # to the command, so that starting it from here does the same thing as
    # typing the command.
    install -d ${D}${SYSELE_APP_DIR}/logs

    install -d ${D}${SYSELE_BIN_DIR}
    install -m 0755 ${WORKDIR}/dsi_detection ${D}${SYSELE_BIN_DIR}/dsi_detection
    ln -sf ${SYSELE_BIN_DIR}/dsi_detection ${D}${SYSELE_APP_DIR}/run

    install -d ${D}${bindir}
    ln -sf ${SYSELE_BIN_DIR}/dsi_detection ${D}${bindir}/dsi_detection
}

FILES:${PN} += " \
    ${SYSELE_APP_DIR} \
    ${SYSELE_BIN_DIR}/dsi_detection \
"

COMPATIBLE_MACHINE = "^astrial-h15$"
