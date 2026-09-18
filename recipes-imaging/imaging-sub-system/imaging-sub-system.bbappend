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
