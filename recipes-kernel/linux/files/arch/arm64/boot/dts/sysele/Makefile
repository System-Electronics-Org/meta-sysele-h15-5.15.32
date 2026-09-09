dtb-$(CONFIG_ARCH_HAILO15) += astrial-h15.dtb
dtb-$(CONFIG_ARCH_HAILO15) += astrial-h15-ws101.dtb
dtb-$(CONFIG_ARCH_HAILO15) += panel-ws101.dtbo

# Overlays can only attach to labels the base tree exports, and dtc emits the
# symbol table only when asked. base-dtb-y would do it automatically, but that
# covers composite DTBs assembled at build time, which this is not.
DTC_FLAGS_astrial-h15 += -@
