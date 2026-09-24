SUMMARY = "System Electronics AI-ISP DSI demo for Astrial H15"
DESCRIPTION = "Touch A/B comparison of the standard ISP and AI-ISP Gen3 with photo capture on the Astrial H15 DSI display."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://meson.build \
    file://meson_options.txt \
    file://main.cpp \
    file://dsi_ai_isp \
    file://ai_profile.template.json \
    file://application_settings.json \
    file://encoder_sink0.json \
    file://iq_ai.json \
    file://iq_standard.json \
    file://medialib_config.template.json \
    file://sensor_config.json \
    file://standard_profile.template.json \
"

S = "${WORKDIR}"

inherit meson pkgconfig

DEPENDS = " \
    cxxopts \
    gstreamer1.0 \
    gstreamer1.0-plugins-base \
    libmedialib \
    opencv \
"

RDEPENDS:${PN} += " \
    gstreamer1.0-plugins-bad-debugutilsbad \
    gstreamer1.0-plugins-bad-kms \
    gstreamer1.0-plugins-base-app \
    gstreamer1.0-plugins-base-videoconvert \
    libmedialib \
"

SYSELE_APP_DIR = "/opt/sysele/apps/dsi/ai_isp"
SYSELE_BIN_DIR = "/opt/sysele/bin"
EXTRA_OEMESON += "-Dinstall_dir=${SYSELE_APP_DIR}"

do_install:append() {
    install -d ${D}${SYSELE_APP_DIR}/config ${D}${SYSELE_APP_DIR}/logs
    install -m 0644 ${WORKDIR}/ai_profile.template.json ${D}${SYSELE_APP_DIR}/config/ai_profile.template.json
    install -m 0644 ${WORKDIR}/application_settings.json ${D}${SYSELE_APP_DIR}/config/application_settings.json
    install -m 0644 ${WORKDIR}/encoder_sink0.json ${D}${SYSELE_APP_DIR}/config/encoder_sink0.json
    install -m 0644 ${WORKDIR}/iq_ai.json ${D}${SYSELE_APP_DIR}/config/iq_ai.json
    install -m 0644 ${WORKDIR}/iq_standard.json ${D}${SYSELE_APP_DIR}/config/iq_standard.json
    install -m 0644 ${WORKDIR}/medialib_config.template.json ${D}${SYSELE_APP_DIR}/config/medialib_config.template.json
    install -m 0644 ${WORKDIR}/sensor_config.json ${D}${SYSELE_APP_DIR}/config/sensor_config.json
    install -m 0644 ${WORKDIR}/standard_profile.template.json ${D}${SYSELE_APP_DIR}/config/standard_profile.template.json

    install -d ${D}${SYSELE_BIN_DIR}
    install -m 0755 ${WORKDIR}/dsi_ai_isp ${D}${SYSELE_BIN_DIR}/dsi_ai_isp
    ln -sf ${SYSELE_BIN_DIR}/dsi_ai_isp ${D}${SYSELE_APP_DIR}/run

    install -d ${D}${bindir}
    ln -sf ${SYSELE_BIN_DIR}/dsi_ai_isp ${D}${bindir}/dsi_ai_isp
}

FILES:${PN} += " \
    ${SYSELE_APP_DIR} \
    ${SYSELE_BIN_DIR}/dsi_ai_isp \
"

COMPATIBLE_MACHINE = "^astrial-h15$"
