# ============================================================
#  便捷 Makefile：包装 ESP-IDF 的 idf.py，方便使用 make 命令
#
#  本 Makefile 会自动 source ESP-IDF 的 export.sh，
#  因此不需要先手动执行 `. $HOME/esp/esp-idf/export.sh`。
#
#    make build          # 编译整个工程
#    make flash          # 编译并烧录
#    make monitor        # 打开串口监视器
#    make flash-monitor  # 烧录后直接打开监视器
#    make clean          # 清理构建中间产物
#    make fullclean      # 彻底清理（含 sdkconfig）
#
#  指定串口（默认由 idf.py 自动探测）：
#    make flash PORT=/dev/ttyACM0
#
#  如果 ESP-IDF 不在默认位置，可覆盖：
#    make flash IDF_PATH=/your/path/esp-idf
# ============================================================

# 用 bash 执行 recipe（export.sh 需要 bash 语法）
SHELL := /bin/bash

# ESP-IDF 根目录（默认 $HOME/esp/esp-idf）
IDF_PATH ?= $(HOME)/esp/esp-idf
IDF_ENV = . $(IDF_PATH)/export.sh >/dev/null 2>&1

PORT ?=
BAUD ?= 921600

IDF_FLAGS = $(if $(PORT),-p $(PORT),) -b $(BAUD)

.PHONY: build flash monitor flash-monitor clean fullclean menuconfig erase-flash size set-target

build:
	$(IDF_ENV) && idf.py build

flash:
	$(IDF_ENV) && idf.py $(IDF_FLAGS) flash

monitor:
	$(IDF_ENV) && idf.py $(if $(PORT),-p $(PORT),) monitor

flash-monitor:
	$(IDF_ENV) && idf.py $(IDF_FLAGS) flash monitor

clean:
	$(IDF_ENV) && idf.py clean

fullclean:
	$(IDF_ENV) && idf.py fullclean

menuconfig:
	$(IDF_ENV) && idf.py menuconfig

erase-flash:
	$(IDF_ENV) && idf.py $(if $(PORT),-p $(PORT),) erase-flash

size:
	$(IDF_ENV) && idf.py size

set-target:
	$(IDF_ENV) && idf.py set-target esp32s3
