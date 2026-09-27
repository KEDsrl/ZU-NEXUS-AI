################################################################################
# ked-libyuv - libyuv recente (NV12Scale) per dx-stream
#
# Sostituisce il pacchetto libyuv in-tree di Buildroot, il cui snapshot e'
# antecedente all'introduzione di NV12Scale (meta' 2021). Mirror GitHub del
# repo chromium libyuv/libyuv.
################################################################################

KED_LIBYUV_VERSION = 5d03bf9bab5693ccf692f18b538d8d9c00387c73
KED_LIBYUV_SITE = https://github.com/lemenkov/libyuv.git
KED_LIBYUV_SITE_METHOD = git
KED_LIBYUV_LICENSE = BSD-3-Clause
KED_LIBYUV_LICENSE_FILES = LICENSE
KED_LIBYUV_INSTALL_STAGING = YES
KED_LIBYUV_DEPENDENCIES = jpeg

KED_LIBYUV_CONF_OPTS = -DUNIT_TEST=OFF

$(eval $(cmake-package))
