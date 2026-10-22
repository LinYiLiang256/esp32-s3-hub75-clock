# ============================================================
#  HUB75 全彩 LED 单元板 (64x64) demo1 工程
#  目标芯片：ESP32-S3 (N16R8)
#  构建方式：ESP-IDF v5.x 的 CMake 构建系统
# ============================================================
cmake_minimum_required(VERSION 3.16)

# ESP-IDF 的 CMake 扩展脚本（由 export.sh 设置 IDF_PATH）
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(demo1)
