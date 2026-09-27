################################################################################
# librdkafka - client Apache Kafka (dipendenza di dx-stream)
################################################################################

LIBRDKAFKA_VERSION = 2.3.0
LIBRDKAFKA_SITE = $(call github,confluentinc,librdkafka,v$(LIBRDKAFKA_VERSION))
LIBRDKAFKA_LICENSE = BSD-2-Clause
LIBRDKAFKA_LICENSE_FILES = LICENSE
LIBRDKAFKA_INSTALL_STAGING = YES
LIBRDKAFKA_DEPENDENCIES = zlib openssl

LIBRDKAFKA_CONF_OPTS = \
	-DRDKAFKA_BUILD_EXAMPLES=OFF \
	-DRDKAFKA_BUILD_TESTS=OFF \
	-DRDKAFKA_BUILD_STATIC=OFF \
	-DWITH_SSL=ON \
	-DWITH_ZLIB=ON \
	-DWITH_SASL=OFF \
	-DWITH_CURL=OFF \
	-DWITH_ZSTD=OFF \
	-DENABLE_LZ4_EXT=OFF

# La build CMake non genera i .pc, ma il meson di dx-stream risolve la
# dipendenza via pkg-config: li generiamo noi nello staging.
define LIBRDKAFKA_INSTALL_PC
	mkdir -p $(STAGING_DIR)/usr/lib/pkgconfig
	printf 'prefix=/usr\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: rdkafka\nDescription: Apache Kafka C client library\nVersion: $(LIBRDKAFKA_VERSION)\nLibs: -L$${libdir} -lrdkafka\nLibs.private: -lssl -lcrypto -lz -lpthread -lm\nCflags: -I$${includedir}\n' \
		> $(STAGING_DIR)/usr/lib/pkgconfig/rdkafka.pc
	printf 'prefix=/usr\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: rdkafka++\nDescription: Apache Kafka C++ client library\nVersion: $(LIBRDKAFKA_VERSION)\nRequires: rdkafka\nLibs: -L$${libdir} -lrdkafka++\nCflags: -I$${includedir}\n' \
		> $(STAGING_DIR)/usr/lib/pkgconfig/rdkafka++.pc
endef
LIBRDKAFKA_POST_INSTALL_STAGING_HOOKS += LIBRDKAFKA_INSTALL_PC

$(eval $(cmake-package))
