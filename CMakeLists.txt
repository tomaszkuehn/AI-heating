# Top-level CMakeLists for the ESP32 heating controller firmware.
# Builds the `heating_ctrl` application from the main/ component and
# embeds the web UI assets so they are available before LittleFS is populated.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(heating_ctrl)