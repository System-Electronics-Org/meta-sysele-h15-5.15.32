# Sensor<N>_Entry.cfg ships with the wiring of Hailo's reference carrier, where
# the sensor of CSI-RX1 sits on I2C bus 2. On Astrial the buses are shifted:
# CSI-RX1 is on bus 1 and CSI-RX0 on bus 2.
#
# Only the bus line is rewritten. Shipping our own copies of the files would go
# stale in silence the first time a release changes their format.

do_install:append() {
    for n in 0 1; do
        case $n in
            0) bus=2 ;;
            1) bus=1 ;;
        esac
        f=${D}${bindir}/Sensor${n}_Entry.cfg
        if [ ! -f $f ]; then
            bbfatal "sysele: $f is not installed, the I2C bus fixup has nothing to correct"
        fi
        sed -i "s|^sensor_i2c_bus *=.*|sensor_i2c_bus = ${bus}|" $f
        if ! grep -q "^sensor_i2c_bus = ${bus}\$" $f; then
            bbfatal "sysele: failed to set the I2C bus in $f"
        fi
    done
}

# Hailo's setup_imx*.sh scripts switch the ISP to another sensor by copying
# <sensor>_Sensor0_Entry.cfg and 3aconfig_<sensor>.json over the live files,
# but the recipe installs neither, so every one of them stops at its first cp.
# Install the per-sensor templates from the source tree, under the names the
# scripts expect. The live Sensor<N>_Entry.cfg files above are not touched.
do_install:append() {
    for f in ${S}/units/isi/drv/*_Sensor*_Entry.cfg ${S}/units/3av2_src/3aconfig_*.json; do
        install -m 0644 $f ${D}${bindir}/
    done
    for f in imx678_Sensor0_Entry.cfg imx334_Sensor0_Entry.cfg 3aconfig_imx678.json 3aconfig_imx334.json; do
        [ -f ${D}${bindir}/$f ] || bbfatal "sysele: $f is not in the imaging source tree any more"
    done
}
