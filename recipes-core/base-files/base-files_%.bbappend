# Set custom hostname for Astrial H15 board
# System Electronics customization

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

# Force hostname to be set correctly
do_install:append() {
    echo "astrialh15" > ${D}${sysconfdir}/hostname
}