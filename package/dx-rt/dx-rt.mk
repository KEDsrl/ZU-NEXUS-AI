################################################################################
# dx-rt - DeepX Runtime SDK v3.3.2
################################################################################

DX_RT_VERSION = a30624494506607b65a5d621e5cd37d3c2dbcb99
DX_RT_SITE = https://github.com/DEEPX-AI/dx_rt.git
DX_RT_SITE_METHOD = git
DX_RT_LICENSE = Proprietary
DX_RT_LICENSE_FILES = LICENSE
DX_RT_INSTALL_STAGING = YES
DX_RT_DEPENDENCIES = ncurses

# Allineato alla recipe Yocto meta-deepx-m1 (dx-rt_3.3.2.bb) ma senza
# ONNXRuntime (USE_ORT=OFF) e senza binding Python, per restare
# autosufficienti. Per riabilitare ORT: pacchetto onnxruntime + USE_ORT=ON.
DX_RT_CONF_OPTS = \
	-DUSE_ORT=OFF \
	-DUSE_SERVICE=ON \
	-DUSE_PYTHON=OFF \
	-DUSE_DXRT_TEST=OFF \
	-DUSE_SHARED_DXRT_LIB=ON \
	-DBUILD_SHARED_LIBS=ON \
	-DCMAKE_SKIP_RPATH=ON

# dx_rt installa ogni target una seconda volta con DESTINATION assoluta nel
# source tree (${CMAKE_SOURCE_DIR}/bin, comodita' per build native): con
# DESTDIR quelle regole finiscono in staging/<path assoluto> e Buildroot
# giustamente le rifiuta. Le rimuoviamo dopo il patching.
define DX_RT_DROP_SRCTREE_INSTALL
	grep -rlZ 'DESTINATION $${CMAKE_SOURCE_DIR}' \
		--include=CMakeLists.txt --include='*.cmake' $(@D) \
		| xargs -0 -r sed -i '/DESTINATION $${CMAKE_SOURCE_DIR}/d'
endef
DX_RT_POST_PATCH_HOOKS += DX_RT_DROP_SRCTREE_INSTALL

define DX_RT_INSTALL_SERVICE
	$(INSTALL) -D -m 0644 $(DX_RT_PKGDIR)/dxrtd.service \
		$(TARGET_DIR)/usr/lib/systemd/system/dxrtd.service
	mkdir -p $(TARGET_DIR)/etc/systemd/system/multi-user.target.wants
	ln -sf ../../../../usr/lib/systemd/system/dxrtd.service \
		$(TARGET_DIR)/etc/systemd/system/multi-user.target.wants/dxrtd.service
endef
ifeq ($(BR2_INIT_SYSTEMD),y)
DX_RT_POST_INSTALL_TARGET_HOOKS += DX_RT_INSTALL_SERVICE
endif

$(eval $(cmake-package))
