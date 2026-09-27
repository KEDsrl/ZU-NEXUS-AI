include $(sort $(wildcard $(BR2_EXTERNAL_KED_PATH)/package/*/*.mk))

# ----------------------------------------------------------------------------
# Device tree della board, copiati negli alberi kernel/u-boot.
# Kernel: i dts includono "zynqmp.dtsi" ecc. -> vanno in dts/xilinx/.
# U-Boot: dts in arch/arm/dts/ + entry nel Makefile (dts non in-tree).
#
# NB: gli hook sono agganciati a PRE_BUILD, non a POST_PATCH. Il passo di
# patch avviene una sola volta (.stamp_patched, alla prima estrazione dei
# sorgenti): con POST_PATCH un "make linux-rebuild" dopo aver modificato un
# .dts/.dtsi ricompilava la copia VECCHIA gia' presente nell'albero kernel,
# silenziosamente. Con PRE_BUILD la copia (cp -f, idempotente e istantanea)
# viene rifatta ad ogni build, quindi linux-rebuild vede sempre i sorgenti
# aggiornati dell'external.
# ----------------------------------------------------------------------------
ifeq ($(BR2_LINUX_KERNEL),y)
define KED_LINUX_COPY_DTS
	cp -f $(BR2_EXTERNAL_KED_PATH)/board/ked/zu-nexus/dts/linux/*.dts* \
		$(LINUX_DIR)/arch/arm64/boot/dts/xilinx/
	cp -f $(BR2_EXTERNAL_KED_PATH)/board/ked/zu-nexus/logo/logo_linux_clut224.ppm \
		$(LINUX_DIR)/drivers/video/logo/logo_linux_clut224.ppm
endef
LINUX_PRE_BUILD_HOOKS += KED_LINUX_COPY_DTS
endif

ifeq ($(BR2_TARGET_UBOOT),y)
define KED_UBOOT_COPY_DTS
	cp -f $(BR2_EXTERNAL_KED_PATH)/board/ked/zu-nexus/dts/uboot/*.dts \
		$(UBOOT_DIR)/arch/arm/dts/
	grep -q "zynqmp-ked-zcu-revA.dtb" $(UBOOT_DIR)/arch/arm/dts/Makefile || \
		sed -i '1i dtb-$$(CONFIG_ARCH_ZYNQMP) += zynqmp-ked-zcu-revA.dtb' \
			$(UBOOT_DIR)/arch/arm/dts/Makefile
endef
UBOOT_PRE_BUILD_HOOKS += KED_UBOOT_COPY_DTS
endif

# ----------------------------------------------------------------------------
# Mesa: aggiunge il target kmsro "xlnx" (pairing GPU lima <-> KMS Xilinx DP).
# Equivalente della patch meta-xilinx 0001-DRI_Add_xlnx_dri.patch, applicata
# via sed idempotente per essere tollerante alle versioni di Mesa.
# Senza: MESA-LOADER "failed to open xlnx" e Weston ripiega su pixman.
# ----------------------------------------------------------------------------
ifeq ($(BR2_PACKAGE_MESA3D),y)
define KED_MESA3D_ADD_XLNX_KMSRO
	grep -q "xlnx_dri.so" $(@D)/src/gallium/targets/dri/meson.build || \
		sed -i "s/'sun4i-drm_dri.so',/'sun4i-drm_dri.so',\n               'xlnx_dri.so',/" \
			$(@D)/src/gallium/targets/dri/meson.build
	grep -q "DEFINE_LOADER_DRM_ENTRYPOINT(xlnx)" $(@D)/src/gallium/targets/dri/target.c || \
		sed -i "s/DEFINE_LOADER_DRM_ENTRYPOINT(sun4i_drm)/DEFINE_LOADER_DRM_ENTRYPOINT(sun4i_drm)\nDEFINE_LOADER_DRM_ENTRYPOINT(xlnx)/" \
			$(@D)/src/gallium/targets/dri/target.c
endef
MESA3D_POST_PATCH_HOOKS += KED_MESA3D_ADD_XLNX_KMSRO
endif
