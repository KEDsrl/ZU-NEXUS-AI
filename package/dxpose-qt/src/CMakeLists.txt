cmake_minimum_required(VERSION 3.16)
project(dxpose-qt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

# GStreamer via pkg-config (Buildroot fornisce i .pc nello staging)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0)

set(SOURCES main.cpp isp.cpp npuinfo.cpp pipelineview.cpp fwupdate.cpp)

# I loghi NON sono compilati nel binario: sono file nel rootfs
# (/usr/share/dxpose-qt/logos, installati da dxpose-qt.mk) letti a runtime.

add_executable(dxpose-qt ${SOURCES})

target_link_libraries(dxpose-qt PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network
    PkgConfig::GST)

include(GNUInstallDirs)
install(TARGETS dxpose-qt RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
