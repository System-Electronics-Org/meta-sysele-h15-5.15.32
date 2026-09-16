SUMMARY = "System Electronics tools for Astrial H15"
DESCRIPTION = "Board tools and bring-up instruments for Astrial H15. Everything \
lives under /opt/sysele so that a production image can drop the whole package \
with a single line instead of deleting files from the rootfs, and the commands \
reach the shell through the PATH snippet and the symlinks in ${bindir}."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://sysele-motd.sh \
    file://sysele-path.sh \
    file://sysele-info \
    file://sysele-config \
    file://dsi_status \
    file://dsi_stream \
    file://dsi_config \
    file://reg_dump \
    file://dsi_trace.c \
    file://vsg_period.c \
    file://vsg_watchdog.c \
    file://clr_keeper.c \
    file://dpi_stopper.c \
"

# Two families, and the split is the point of the layout.
#
# Commands are what somebody picking up the board is meant to type: they end up
# in the PATH and in the sysele-info listing.
#
# Instruments answer one question each about the display bring-up. They read and
# write DSI and DPI registers through /dev/mem at full speed and can stop the
# panel, so they stay in their own directory, out of the PATH, where they have to
# be asked for by name.
SYSELE_CMD_SH = "sysele-config dsi_status dsi_stream dsi_config reg_dump"
SYSELE_CMD_C = "dsi_trace"
SYSELE_DIAG_C = "vsg_period vsg_watchdog clr_keeper dpi_stopper"

S = "${WORKDIR}"

# Deliberately not allarch: the banner bakes MACHINE and DISTRO_VERSION in, so
# the package is not identical across machines.

SYSELE_DIR = "/opt/sysele"
SYSELE_ROOT_HOME = "/home/root"

# DATETIME changes on every build. Without this the task signature changes too,
# the recipe rebuilds every time and it invalidates the sstate cache behind it.
do_install[vardepsexclude] += "DATETIME"

do_compile() {
    for t in ${SYSELE_CMD_C} ${SYSELE_DIAG_C}; do
        ${CC} ${CFLAGS} ${LDFLAGS} -o ${B}/$t ${WORKDIR}/$t.c -lm
    done
}

do_install() {
    # An empty directory is not packaged on its own; it has to be created here
    # and claimed in FILES below, otherwise the recipe builds and installs
    # nothing.
    install -d ${D}${SYSELE_DIR}/bin
    install -d ${D}${SYSELE_DIR}/diag
    install -d ${D}${SYSELE_DIR}/src

    # Generated files land here: the pipeline JSONs are built on the board from
    # Hailo's vision config by dsi_config, so they cannot ship with the package.
    install -d ${D}${SYSELE_DIR}/share

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

    # What sysele-config is allowed to select, derived from the same variable
    # that puts the trees in the FIT: a hand written list would drift from the
    # image the first time a variant is added. Names here are FIT configuration
    # names, and a name that does not exist is a board that does not boot, so
    # the tool refuses anything that is not in this file.
    list=${D}${SYSELE_DIR}/share/boot-config.list
    : > $list
    for e in ${KERNEL_DEVICETREE}; do
        file=$(basename $e)
        name=$(echo $file | sed 's/\.[^.]*$//')
        case $e in
            */*) vendor=$(dirname $e)_ ;;
            *)   vendor= ;;
        esac
        case $file in
            *.dtbo) type=overlay; conf="#conf-${vendor}$file" ;;
            *.dtb)  conf="conf-${vendor}$file"
                    if [ "$name" = "${MACHINE}" ]; then type=base-default; else type=base; fi ;;
            *) continue ;;
        esac
        label=$(echo "${SYSELE_DT_LABELS}" | tr '|' '\n' | sed -n "s|^$name=||p")
        [ -n "$label" ] || label=$name
        printf '%s\t%s\t%s\t%s\n' "$type" "$name" "$conf" "$label" >> $list
    done
    chmod 0644 $list
    for t in ${SYSELE_CMD_SH}; do
        install -m 0755 ${WORKDIR}/$t ${D}${SYSELE_DIR}/bin/$t
    done

    # The sources of the compiled tools ship too: on this board they are read
    # more often than they are rebuilt, and sysele-info takes the one line
    # description of a compiled tool from the first line of its source.
    for t in ${SYSELE_CMD_C}; do
        install -m 0755 ${B}/$t ${D}${SYSELE_DIR}/bin/$t
        install -m 0644 ${WORKDIR}/$t.c ${D}${SYSELE_DIR}/src/$t.c
    done
    for t in ${SYSELE_DIAG_C}; do
        install -m 0755 ${B}/$t ${D}${SYSELE_DIR}/diag/$t
        install -m 0644 ${WORKDIR}/$t.c ${D}${SYSELE_DIR}/src/$t.c
    done

    # Two ways in, on purpose. The profile.d snippet serves the login shells,
    # the symlinks serve everything else: a non interactive ssh command, a
    # systemd unit and a script never read profile.d, and typing the full path
    # to /opt every time is how a tool stops being used.
    install -d ${D}${sysconfdir}/profile.d
    install -m 0644 ${WORKDIR}/sysele-motd.sh ${D}${sysconfdir}/profile.d/sysele-motd.sh
    install -m 0644 ${WORKDIR}/sysele-path.sh ${D}${sysconfdir}/profile.d/sysele-path.sh

    install -d ${D}${bindir}
    for t in sysele-info ${SYSELE_CMD_SH} ${SYSELE_CMD_C}; do
        ln -sf ${SYSELE_DIR}/bin/$t ${D}${bindir}/$t
    done

    # Next to Hailo's apps/ in root's home: whoever opens the board sees that
    # this is a System Electronics product before they see anything else.
    install -d ${D}${SYSELE_ROOT_HOME}
    ln -sf ${SYSELE_DIR} ${D}${SYSELE_ROOT_HOME}/sysele
}

FILES:${PN} += " \
    ${SYSELE_DIR} \
    ${sysconfdir}/profile.d/sysele-motd.sh \
    ${sysconfdir}/profile.d/sysele-path.sh \
    ${SYSELE_ROOT_HOME}/sysele \
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
