# ============================================================
#  HUB75 全彩 LED 单元板 (64x64) demo2：WiFi 配网 + 网络时钟
#  目标芯片：ESP32-S3 (N16R8)
#  构建方式：ESP-IDF v5.x 的 CMake 构建系统
# ============================================================
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(demo2_wifi_time_clock)
