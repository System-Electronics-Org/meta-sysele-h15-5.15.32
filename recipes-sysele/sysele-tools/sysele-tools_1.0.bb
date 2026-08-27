SUMMARY = "System Electronics tools for Astrial H15"
DESCRIPTION = "Skeleton package for System Electronics board tools. Installs \
under /opt/sysele and currently ships only the build banner, so that a future \
production image can drop the whole package with a single line instead of \
deleting files from the rootfs."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://sysele-motd.sh \
    file://sysele-info \
"

S = "${WORKDIR}"

# Deliberately not allarch: the banner bakes MACHINE and DISTRO_VERSION in, so
# the package is not identical across machines.

SYSELE_DIR = "/opt/sysele"

# DATETIME changes on every build. Without this the task signature changes too,
# the recipe rebuilds every time and it invalidates the sstate cache behind it.
do_install[vardepsexclude] += "DATETIME"

do_install() {
    # An empty directory is not packaged on its own; it has to be created here
    # and claimed in FILES below, otherwise the recipe builds and installs
    # nothing.
    install -d ${D}${SYSELE_DIR}
    install -d ${D}${SYSELE_DIR}/bin
    install -d ${D}${sysconfdir}/profile.d

    # DATETIME is YYYYMMDDhhmmss; make it readable. METADATA_REVISION is a full
    # SHA, 12 characters are enough to identify it.
    built=$(echo "${DATETIME}" | sed -E 's/(....)(..)(..)(..)(..)(..)/\1-\2-\3 \4:\5:\6/')
    poky_rev=$(echo "${METADATA_REVISION}" | cut -c1-12)

    cat > ${D}${SYSELE_DIR}/build-info <<SYSELE_EOF

  Astrial H15 - System Electronics

    Distro       ${DISTRO} ${DISTRO_VERSION}
    Machine      ${MACHINE}
    Built        $built
    Poky rev     $poky_rev

SYSELE_EOF

    install -m 0755 ${WORKDIR}/sysele-info ${D}${SYSELE_DIR}/bin/sysele-info
    install -m 0644 ${WORKDIR}/sysele-motd.sh ${D}${sysconfdir}/profile.d/sysele-motd.sh
}

FILES:${PN} += " \
    ${SYSELE_DIR} \
    ${sysconfdir}/profile.d/sysele-motd.sh \
"

# NOTE, open point for review: METADATA_REVISION is the revision of poky, not
# of this layer, so the banner above labels it as such rather than pretending
# otherwise. If the banner has to show the SHA of meta-sysele-bsp, the usual
# way is to compute it at parse time in conf/layer.conf, for example:
#
#   SYSELE_LAYER_REVISION := "${@os.popen("git -C ${LAYERDIR} rev-parse --short HEAD 2>/dev/null || echo unknown").read().strip()}"
#
# Trade-off: it runs git on every parse of the layer, it returns "unknown" when
# the layer is not a git checkout (release tarballs, vendored copies), and the
# value has to be excluded from task signatures like DATETIME is above or every
# commit to the layer rebuilds this recipe. Not adopted here: needs a decision.
