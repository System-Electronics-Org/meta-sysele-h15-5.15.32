FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += " \
    file://0001-resolve-dual-camera-sensors-from-media-graph.patch \
    file://0002-carry-sensor-id-into-the-gst-mode-profile.patch \
"

# libmedialib now uses libmediactl directly to resolve the sensor connected
# to each CSI receiver from the runtime media graph.
DEPENDS:append = " v4l-utils"
