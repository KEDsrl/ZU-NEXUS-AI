################################################################################
#
# dxpose-qt - Visualizzatore Qt6/GStreamer IMX219 + DeepX DX-M1
#
# Sorgenti locali nell'external: SITE_METHOD=local -> Buildroot li sincronizza
# in output/build a ogni build, quindi basta 'make dxpose-qt-rebuild' dopo una
# modifica al codice (nessun dirclean necessario).
#
################################################################################

DXPOSE_QT_VERSION = 1.0
DXPOSE_QT_SITE = $(BR2_EXTERNAL_KED_PATH)/package/dxpose-qt/src
DXPOSE_QT_SITE_METHOD = local
DXPOSE_QT_LICENSE = Proprietary

# dx-stream serve solo a RUNTIME (elementi GStreamer caricati dal registry),
# ma lo teniamo come dipendenza di build cosi' l'immagine non puo' contenere
# l'app senza i plugin che le servono.
DXPOSE_QT_DEPENDENCIES = qt6base gstreamer1 gst1-plugins-base host-pkgconf

ifeq ($(BR2_PACKAGE_DX_STREAM),y)
DXPOSE_QT_DEPENDENCIES += dx-stream
endif

# ----------------------------------------------------------------------------
# Video di fallback: usato quando la camera non viene rilevata o la sua
# pipeline va in errore. Si installa SOLO se presente nell'external:
#     <ked-external>/media/dron_720p.y4m  ->  /usr/share/dxpose-qt/media/
# Nessun errore se manca: l'app ripiega sul primo video in /home/ked/videos.
#
# ATTENZIONE alle dimensioni: y4m e' video NON compresso (720p I420 ~1,4 MB
# per frame, ~41 MB al secondo a 30 fps). Il rootfs ext4 ha dimensione fissa
# (BR2_TARGET_ROOTFS_EXT2_SIZE): se il video non ci sta, la generazione di
# rootfs.ext4 fallisce piu' avanti con un errore poco chiaro. Qui si avvisa
# subito.
# NB: se cambia solo il video, "make" non se ne accorge: usare
#     make dxpose-qt-rebuild   (rifa' anche l'installazione nel target).
# ----------------------------------------------------------------------------
DXPOSE_QT_FALLBACK_VIDEO = $(BR2_EXTERNAL_KED_PATH)/media/dron_720p.y4m

define DXPOSE_QT_INSTALL_FALLBACK_VIDEO
	if [ -f $(DXPOSE_QT_FALLBACK_VIDEO) ]; then \
		SZ=$$(du -m $(DXPOSE_QT_FALLBACK_VIDEO) | cut -f1); \
		echo ">>> dxpose-qt: video di fallback $(DXPOSE_QT_FALLBACK_VIDEO) ($${SZ} MB)"; \
		if [ $$SZ -gt 400 ]; then \
			echo "*** ATTENZIONE: il video pesa $${SZ} MB e il rootfs e' $(BR2_TARGET_ROOTFS_EXT2_SIZE)."; \
			echo "*** Se la creazione di rootfs.ext4 fallisce, aumentare BR2_TARGET_ROOTFS_EXT2_SIZE"; \
			echo "*** oppure accorciare il video (y4m non e' compresso: ~41 MB/s a 720p30)."; \
		fi; \
		$(INSTALL) -D -m 0644 $(DXPOSE_QT_FALLBACK_VIDEO) \
			$(TARGET_DIR)/usr/share/dxpose-qt/media/dron_720p.y4m; \
	else \
		echo ">>> dxpose-qt: nessun $(DXPOSE_QT_FALLBACK_VIDEO): fallback su /home/ked/videos"; \
		rm -f $(TARGET_DIR)/usr/share/dxpose-qt/media/dron_720p.y4m; \
	fi
endef
DXPOSE_QT_POST_INSTALL_TARGET_HOOKS += DXPOSE_QT_INSTALL_FALLBACK_VIDEO

# ----------------------------------------------------------------------------
# Loghi del pannello: tutti i .png di <ked-external>/media/logos vanno in
#     /usr/share/dxpose-qt/logos/
# La cartella nel target viene SVUOTATA prima della copia: un logo tolto
# dall'external sparisce anche dall'immagine. L'app mostra solo quelli presenti.
# Ordine alfabetico del nome file; altezza adattata al numero di loghi.
# Dopo aver aggiunto/tolto loghi:  make dxpose-qt-rebuild
# ----------------------------------------------------------------------------
DXPOSE_QT_LOGOS_DIR = $(BR2_EXTERNAL_KED_PATH)/media/logos

define DXPOSE_QT_INSTALL_LOGOS
	rm -rf $(TARGET_DIR)/usr/share/dxpose-qt/logos
	mkdir -p $(TARGET_DIR)/usr/share/dxpose-qt/logos
	for f in $(DXPOSE_QT_LOGOS_DIR)/*.png; do \
		[ -f "$$f" ] || continue; \
		echo ">>> dxpose-qt: logo $$(basename $$f)"; \
		$(INSTALL) -m 0644 "$$f" $(TARGET_DIR)/usr/share/dxpose-qt/logos/; \
	done
endef
DXPOSE_QT_POST_INSTALL_TARGET_HOOKS += DXPOSE_QT_INSTALL_LOGOS

$(eval $(cmake-package))
