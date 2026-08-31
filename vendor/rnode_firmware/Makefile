# Copyright (C) 2024, Mark Qvist
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

ARDUINO_ESP_CORE_VER := 2.0.17

CARDPUTER_ADV_FQBN := esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=8M,FlashMode=dio,PartitionScheme=default_8MB
CARDPUTER_ADV_SKETCH := build/cardputer_adv_sketch/RNode_Firmware
CARDPUTER_ADV_BUILD := build/cardputer_adv_build
CARDPUTER_ADV_OUTPUT := build/cardputer_adv.esp32.esp32s3
CARDPUTER_CONFIG_H ?= ../../src/boards/cardputer/config/BoardConfig.h
CARDPUTER_VERSION_MAJOR ?= $(shell awk '/\#define RSDECK_VERSION_MAJOR/ {print $$3; exit}' $(CARDPUTER_CONFIG_H))
CARDPUTER_VERSION_MINOR ?= $(shell awk '/\#define RSDECK_VERSION_MINOR/ {print $$3; exit}' $(CARDPUTER_CONFIG_H))
CARDPUTER_VERSION_PATCH ?= $(shell awk '/\#define RSDECK_VERSION_PATCH/ {print $$3; exit}' $(CARDPUTER_CONFIG_H))
CARDPUTER_ADV_DEFAULT_FREQ ?= 915000000UL
CARDPUTER_ADV_DEFAULT_BW ?= 250000UL
CARDPUTER_ADV_DEFAULT_SF ?= 11
CARDPUTER_ADV_DEFAULT_CR ?= 5
CARDPUTER_ADV_DEFAULT_TXP ?= 14
CARDPUTER_ADV_EXTRA_FLAGS := "-DBOARD_MODEL=0x43" "-DCARDPUTER_ADV_DEFAULT_FREQ=$(CARDPUTER_ADV_DEFAULT_FREQ)" "-DCARDPUTER_ADV_DEFAULT_BW=$(CARDPUTER_ADV_DEFAULT_BW)" "-DCARDPUTER_ADV_DEFAULT_SF=$(CARDPUTER_ADV_DEFAULT_SF)" "-DCARDPUTER_ADV_DEFAULT_CR=$(CARDPUTER_ADV_DEFAULT_CR)" "-DCARDPUTER_ADV_DEFAULT_TXP=$(CARDPUTER_ADV_DEFAULT_TXP)" "-DRSDECK_VERSION_MAJOR=$(CARDPUTER_VERSION_MAJOR)" "-DRSDECK_VERSION_MINOR=$(CARDPUTER_VERSION_MINOR)" "-DRSDECK_VERSION_PATCH=$(CARDPUTER_VERSION_PATCH)"

TDECK_FQBN := esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=16M,FlashMode=dio,PartitionScheme=no_ota,PSRAM=opi
TDECK_SKETCH := build/tdeck_sketch/RNode_Firmware
TDECK_BUILD := build/tdeck_build
TDECK_OUTPUT := build/tdeck.esp32.esp32s3
RSDECK_CONFIG_H ?= ../../src/boards/tdeck/config/BoardConfig.h
RSDECK_VERSION_MAJOR ?= $(shell awk '/\#define RSDECK_VERSION_MAJOR/ {print $$3; exit}' $(RSDECK_CONFIG_H))
RSDECK_VERSION_MINOR ?= $(shell awk '/\#define RSDECK_VERSION_MINOR/ {print $$3; exit}' $(RSDECK_CONFIG_H))
RSDECK_VERSION_PATCH ?= $(shell awk '/\#define RSDECK_VERSION_PATCH/ {print $$3; exit}' $(RSDECK_CONFIG_H))
TDECK_EXTRA_FLAGS := "-DBOARD_MODEL=0x3B" "-DRSDECK_VERSION_MAJOR=$(RSDECK_VERSION_MAJOR)" "-DRSDECK_VERSION_MINOR=$(RSDECK_VERSION_MINOR)" "-DRSDECK_VERSION_PATCH=$(RSDECK_VERSION_PATCH)"

TPAGER_FQBN := esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=16M,FlashMode=dio,PartitionScheme=no_ota,PSRAM=enabled
TPAGER_SKETCH := build/tpager_sketch/RNode_Firmware
TPAGER_BUILD := build/tpager_build
TPAGER_OUTPUT := build/tpager.esp32.esp32s3
TPAGER_CONFIG_H ?= ../../src/boards/tpager/config/BoardConfig.h
TPAGER_VERSION_MAJOR ?= $(shell awk '/\#define RSDECK_VERSION_MAJOR/ {print $$3; exit}' $(TPAGER_CONFIG_H))
TPAGER_VERSION_MINOR ?= $(shell awk '/\#define RSDECK_VERSION_MINOR/ {print $$3; exit}' $(TPAGER_CONFIG_H))
TPAGER_VERSION_PATCH ?= $(shell awk '/\#define RSDECK_VERSION_PATCH/ {print $$3; exit}' $(TPAGER_CONFIG_H))
TPAGER_EXTRA_FLAGS := "-DBOARD_MODEL=0x45" "-DRSDECK_VERSION_MAJOR=$(TPAGER_VERSION_MAJOR)" "-DRSDECK_VERSION_MINOR=$(TPAGER_VERSION_MINOR)" "-DRSDECK_VERSION_PATCH=$(TPAGER_VERSION_PATCH)"

.PHONY: all clean prep-esp32 prep-m5stack prep-cardputer_adv prepare-cardputer-adv prepare-tdeck prepare-tpager firmware-cardputer_adv firmware-tdeck firmware-tpager

all: firmware-tdeck firmware-tpager firmware-cardputer_adv

clean:
	rm -rf build

prep-esp32:
	arduino-cli core update-index --config-file arduino-cli.yaml
	arduino-cli core install esp32:esp32@$(ARDUINO_ESP_CORE_VER) --config-file arduino-cli.yaml
	arduino-cli lib install "Adafruit BusIO@1.17.4"
	arduino-cli lib install "Adafruit GFX Library@1.12.5"
	arduino-cli lib install "Adafruit seesaw Library@1.7.9"
	arduino-cli lib install "Adafruit SSD1306@2.5.16"
	arduino-cli lib install "Adafruit SH110X@2.1.14"
	arduino-cli lib install "Adafruit ST7735 and ST7789 Library@1.11.0"
	arduino-cli lib install "Adafruit NeoPixel@1.15.4"
	arduino-cli lib install "XPowersLib@0.3.3"
	arduino-cli lib install "Crypto@0.4.0"
	arduino-cli lib install "LovyanGFX@1.1.16"

prep-m5stack:
	arduino-cli lib install "IRremote@4.6.1"
	arduino-cli lib install "LibSSH-ESP32@5.8.0"
	arduino-cli lib install "M5GFX@0.2.19"
	arduino-cli lib install "M5Unified@0.2.13"
	arduino-cli lib install "M5Cardputer@1.1.1"

prep-cardputer_adv: prep-esp32 prep-m5stack

prepare-cardputer-adv:
	rm -rf $(CARDPUTER_ADV_SKETCH)
	mkdir -p $(CARDPUTER_ADV_SKETCH)
	cp *.h *.cpp RNode_Firmware.ino $(CARDPUTER_ADV_SKETCH)/
	cp -R Fonts $(CARDPUTER_ADV_SKETCH)/

prepare-tdeck:
	rm -rf $(TDECK_SKETCH)
	mkdir -p $(TDECK_SKETCH)
	cp *.h *.cpp RNode_Firmware.ino $(TDECK_SKETCH)/
	cp -R Fonts $(TDECK_SKETCH)/

prepare-tpager:
	rm -rf $(TPAGER_SKETCH)
	mkdir -p $(TPAGER_SKETCH)
	cp *.h *.cpp RNode_Firmware.ino $(TPAGER_SKETCH)/
	cp -R Fonts $(TPAGER_SKETCH)/

firmware-cardputer_adv: prepare-cardputer-adv
	arduino-cli compile --log --fqbn "$(CARDPUTER_ADV_FQBN)" --build-path $(CARDPUTER_ADV_BUILD) --output-dir $(CARDPUTER_ADV_OUTPUT) --build-property "compiler.cpp.extra_flags=$(CARDPUTER_ADV_EXTRA_FLAGS)" $(CARDPUTER_ADV_SKETCH)

firmware-tdeck: prepare-tdeck
	arduino-cli compile --log --fqbn "$(TDECK_FQBN)" --build-path $(TDECK_BUILD) --output-dir $(TDECK_OUTPUT) --build-property "compiler.cpp.extra_flags=$(TDECK_EXTRA_FLAGS)" $(TDECK_SKETCH)

firmware-tpager: prepare-tpager
	arduino-cli compile --log --fqbn "$(TPAGER_FQBN)" --build-path $(TPAGER_BUILD) --output-dir $(TPAGER_OUTPUT) --build-property "compiler.cpp.extra_flags=$(TPAGER_EXTRA_FLAGS)" $(TPAGER_SKETCH)
