# EdgeNOS Buildroot external tree — x86_64 base system.
# No custom packages yet; this hook exists so package/<name>/<name>.mk can be added.
include $(sort $(wildcard $(BR2_EXTERNAL_EDGENOS_PATH)/package/*/*.mk))
