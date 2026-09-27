################################################################################
# dx-fw - Firmware DeepX DX-M1/DX-M1M v2.5.6
################################################################################

DX_FW_VERSION = 3998ea708ba1bdadcc8e349fe87a082d300d3ec5
DX_FW_SITE = https://github.com/DEEPX-AI/dx_fw.git
DX_FW_SITE_METHOD = git
DX_FW_LICENSE = Proprietary
DX_FW_LICENSE_FILES = LICENSE

# Installa tutte le varianti (m1 mdot2/h1, m1m mdot2): stessa immagine
# per entrambe le versioni di modulo.
define DX_FW_INSTALL_TARGET_CMDS
	cd $(@D) && for f in $$(find m1* -name fw.bin); do \
		$(INSTALL) -D -m 0644 $$f \
			$(TARGET_DIR)/lib/firmware/deepx/$$(dirname $$f)/fw.bin; \
	done
endef

$(eval $(generic-package))
