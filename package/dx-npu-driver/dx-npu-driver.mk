################################################################################
# dx-npu-driver - DeepX DX-M1 NPU kernel driver (v2.4.1)
################################################################################

DX_NPU_DRIVER_VERSION = c05be168ea0c28757737035ea58aab6e59f03256
DX_NPU_DRIVER_SITE = https://github.com/DEEPX-AI/dx_rt_npu_linux_driver.git
DX_NPU_DRIVER_SITE_METHOD = git
DX_NPU_DRIVER_LICENSE = Proprietary
DX_NPU_DRIVER_LICENSE_FILES = LICENSE

# Il Kbuild seleziona i moduli con variabili CONFIG_DX_* (vedi modules/device.mk):
# passate sulla riga di comando arrivano al kbuild del kernel.
DX_NPU_DRIVER_MODULE_SUBDIRS = modules
# Le define di versione sono iniettate dal Makefile esterno del driver via
# CCFLAGS (i Kbuild fanno "ccflags-y += $(CCFLAGS)"): valori da release.ver
# del commit pinnato sopra (RT v2.4.1, PCIe v2.2.0). Aggiornarle insieme a
# DX_NPU_DRIVER_VERSION.
DX_NPU_DRIVER_MODULE_MAKE_OPTS = \
	CONFIG_DX_AI_ACCEL_RT=m \
	CONFIG_DX_AI_ACCEL_M1=y \
	CONFIG_DX_AI_ACCEL_PCIE_DEEPX=m \
	CONFIG_DX_AI_ACCEL_PCIE_XILINX=n \
	CCFLAGS="-DRT_VERSION_MAJOR=2 -DRT_VERSION_MINOR=4 -DRT_VERSION_PATCH=1 \
		-DPCIE_VERSION_MAJOR=2 -DPCIE_VERSION_MINOR=2 -DPCIE_VERSION_PATCH=0"

define DX_NPU_DRIVER_INSTALL_CONF
	$(INSTALL) -D -m 0644 $(@D)/modules/dx_dma.conf \
		$(TARGET_DIR)/etc/modprobe.d/dx_dma.conf
	mkdir -p $(TARGET_DIR)/etc/udev/rules.d
	echo 'KERNEL=="dxrt*", MODE="0666"' > \
		$(TARGET_DIR)/etc/udev/rules.d/99-dx-dma.rules
endef
DX_NPU_DRIVER_POST_INSTALL_TARGET_HOOKS += DX_NPU_DRIVER_INSTALL_CONF

$(eval $(kernel-module))
$(eval $(generic-package))
