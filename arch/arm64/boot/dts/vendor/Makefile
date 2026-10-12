vendor := $(srctree)/$(src)

ifeq ($(CONFIG_SEC_A9Y18QLTE_PROJECT),y)
	subdir-y += samsung
else ifneq "$(wildcard $(vendor)/qcom)" ""
	subdir-y += qcom
endif
