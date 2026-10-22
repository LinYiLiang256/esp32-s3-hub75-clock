# ============================================================
#  便捷 Makefile：包装 ESP-IDF 的 idf.py，自动加载环境
#
#    make build / make flash / make monitor / make flash-monitor
#    make clean / make fullclean / make menuconfig
#
#  指定串口（默认由 idf.py 自动探测）：
#    make flash PORT=/dev/ttyACM0
#
#  如果 ESP-IDF 不在默认位置，可覆盖：
#    make flash IDF_PATH=/your/path/esp-idf
# ============================================================

SHELL := /bin/bash

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
