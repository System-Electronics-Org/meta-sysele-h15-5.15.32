FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Add your changes to SRC_URI
SRC_URI += " \
    file://arch/arm/mach-hailo/Kconfig.patch \
    file://arch/arm/dts/Makefile.patch \
    file://board/sysele/astrial-h15/Kconfig \
    file://board/sysele/astrial-h15/Makefile \
    file://include/configs/astrial-h15.h \
    file://arch/arm/dts/astrial-h15.dts \
    file://configs/astrial-h15_defconfig \
"

UBOOT_MACHINE = "astrial-h15_defconfig"

do_configure:prepend() {
    # Create directories
    install -d ${S}/board/sysele/astrial-h15
    
    # Copy board files
    cp ${WORKDIR}/board/sysele/astrial-h15/Kconfig ${S}/board/sysele/astrial-h15/
    cp ${WORKDIR}/board/sysele/astrial-h15/Makefile ${S}/board/sysele/astrial-h15/
    
    # Copy config header
    install -d ${S}/include/configs
    cp ${WORKDIR}/include/configs/astrial-h15.h ${S}/include/configs/
    
    # Copy devicetree
    cp ${WORKDIR}/arch/arm/dts/astrial-h15.dts ${S}/arch/arm/dts/
    
    # Copy defconfig
    cp ${WORKDIR}/configs/astrial-h15_defconfig ${S}/configs/
}
