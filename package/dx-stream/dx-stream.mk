################################################################################
# dx-stream - DeepX GStreamer plugin suite v3.0.1
#
# Oltre al plugin (libgstdxstream.so) questo pacchetto compila e installa:
#  - le librerie di postprocess custom (dx_stream/custom_library/
#    postprocess_library/*: PPU, YoloV5S, SCRFD500M, ...), ognuna un
#    progetto meson autonomo che dipende da gstdxstream via pkg-config
#    -> per questo serve INSTALL_STAGING = YES
#  - i file di configurazione (dx_stream/configs) in
#    /usr/share/gstdxstream/configs, con i path /usr/local/share
#    riscritti in /usr/share (prefix Buildroot)
#
# NON installa i modelli .dxnn: vanno scaricati da sdk.deepx.ai
# (scripts/get_resource.sh / setup.sh del repo) e copiati sul target.
################################################################################

DX_STREAM_VERSION = 0877d37e1aa85d2387e42c0646f53ec7f18b99c8
DX_STREAM_SITE = https://github.com/DEEPX-AI/dx_stream.git
DX_STREAM_SITE_METHOD = git
DX_STREAM_LICENSE = Proprietary
DX_STREAM_LICENSE_FILES = LICENSE
DX_STREAM_SUBDIR = gst-dxstream-plugin
DX_STREAM_INSTALL_STAGING = YES
DX_STREAM_DEPENDENCIES = \
	dx-rt gstreamer1 gst1-plugins-base eigen json-glib zlib \
	opencv4 ked-libyuv mosquitto librdkafka

DX_STREAM_CONF_OPTS = -Dv3_flag=false

# ----------------------------------------------------------------------------
# Librerie di postprocess: un progetto meson per directory. Riusiamo il
# cross-file generato dall'infra meson di Buildroot per questo pacchetto
# ($(@D)/$(DX_STREAM_SUBDIR)/build/cross-compilation.conf). La dipendenza
# meson 'gstdxstream' viene risolta via pkg-config nello staging, quindi
# questo hook gira come POST_INSTALL_TARGET (dopo l'install staging).
# ----------------------------------------------------------------------------
define DX_STREAM_BUILD_INSTALL_POSTPROCESS
	for d in $(@D)/dx_stream/custom_library/postprocess_library/*/ ; do \
		[ -f $$d/meson.build ] || continue ; \
		echo "== dx-stream postprocess: $$d" ; \
		rm -rf $$d/br-build ; \
		PATH=$(BR_PATH) $(TARGET_MAKE_ENV) $(HOST_DIR)/bin/meson setup \
			--prefix=/usr --libdir=lib --buildtype=release \
			--cross-file=$(@D)/$(DX_STREAM_SUBDIR)/build/cross-compilation.conf \
			$$d/br-build $$d || exit 1 ; \
		PATH=$(BR_PATH) $(HOST_DIR)/bin/ninja -C $$d/br-build || exit 1 ; \
		PATH=$(BR_PATH) DESTDIR=$(TARGET_DIR) \
			$(HOST_DIR)/bin/ninja -C $$d/br-build install || exit 1 ; \
	done
endef
DX_STREAM_POST_INSTALL_TARGET_HOOKS += DX_STREAM_BUILD_INSTALL_POSTPROCESS

# ----------------------------------------------------------------------------
# Config JSON dei modelli (preprocess/inference/postprocess), tracker e
# broker. I JSON upstream puntano a /usr/local/share/gstdxstream (prefix
# del build.sh vendor): li riscriviamo sul prefix /usr di Buildroot.
# ----------------------------------------------------------------------------
define DX_STREAM_INSTALL_CONFIGS
	mkdir -p $(TARGET_DIR)/usr/share/gstdxstream
	cp -a $(@D)/dx_stream/configs $(TARGET_DIR)/usr/share/gstdxstream/
	find $(TARGET_DIR)/usr/share/gstdxstream/configs -name '*.json' -exec \
		$(SED) 's|/usr/local/share/gstdxstream|/usr/share/gstdxstream|g' {} +
endef
DX_STREAM_POST_INSTALL_TARGET_HOOKS += DX_STREAM_INSTALL_CONFIGS

$(eval $(meson-package))
