# Copyright (C) 2024, Mark Qvist
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

ARDUINO_ESP_CORE_VER := 2.0.17
RATSPEAK_BUILD_FLAGS = -std=gnu++17 $(shell python3 ../../tools/release_identity.py build-flags)

CARDPUTER_ADV_FQBN := esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=8M,FlashMode=dio,PartitionScheme=default_8MB
CARDPUTER_ADV_SKETCH := build/cardputer_adv_sketch/RNode_Firmware
CARDPUTER_ADV_BUILD := build/cardputer_adv_build
CARDPUTER_ADV_OUTPUT := build/cardputer_adv.esp32.esp32s3
CARDPUTER_ADV_DEFAULT_FREQ ?= 915000000UL
CARDPUTER_ADV_DEFAULT_BW ?= 250000UL
CARDPUTER_ADV_DEFAULT_SF ?= 11
CARDPUTER_ADV_DEFAULT_CR ?= 5
CARDPUTER_ADV_DEFAULT_TXP ?= 14
CARDPUTER_ADV_EXTRA_FLAGS := "-DBOARD_MODEL=0x43" "-DCARDPUTER_ADV_DEFAULT_FREQ=$(CARDPUTER_ADV_DEFAULT_FREQ)" "-DCARDPUTER_ADV_DEFAULT_BW=$(CARDPUTER_ADV_DEFAULT_BW)" "-DCARDPUTER_ADV_DEFAULT_SF=$(CARDPUTER_ADV_DEFAULT_SF)" "-DCARDPUTER_ADV_DEFAULT_CR=$(CARDPUTER_ADV_DEFAULT_CR)" "-DCARDPUTER_ADV_DEFAULT_TXP=$(CARDPUTER_ADV_DEFAULT_TXP)"

TDECK_FQBN := esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=16M,FlashMode=dio,PartitionScheme=no_ota,PSRAM=opi
TDECK_SKETCH := build/tdeck_sketch/RNode_Firmware
TDECK_BUILD := build/tdeck_build
TDECK_OUTPUT := build/tdeck.esp32.esp32s3
TDECK_EXTRA_FLAGS := "-DBOARD_MODEL=0x3B"

TPAGER_FQBN := esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=16M,FlashMode=dio,PartitionScheme=no_ota,PSRAM=enabled
TPAGER_SKETCH := build/tpager_sketch/RNode_Firmware
TPAGER_BUILD := build/tpager_build
TPAGER_OUTPUT := build/tpager.esp32.esp32s3
TPAGER_EXTRA_FLAGS := "-DBOARD_MODEL=0x45"

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
	cp ../../src/core/radio/SX1262Timing.h ../../src/core/radio/RadioTimingPolicy.h $(CARDPUTER_ADV_SKETCH)/
	cp ../esp_idf_compat/dhcpserver.c $(CARDPUTER_ADV_SKETCH)/DhcpServer.c
	cp ../esp_idf_compat/SdkVersion.h $(CARDPUTER_ADV_SKETCH)/
	cp ../esp_idf_compat/rtc_init.c $(CARDPUTER_ADV_SKETCH)/RtcInit.c
	cp ../esp_idf_compat/rtc_sleep.c $(CARDPUTER_ADV_SKETCH)/RtcSleep.c
	python3 ../../tools/patch_wifi_sockets.py --fqbn "$(CARDPUTER_ADV_FQBN)" --sketch $(CARDPUTER_ADV_SKETCH) --build-path $(CARDPUTER_ADV_BUILD) --output build/cardputer_adv_wifi/WiFi
	python3 ../../tools/patch_webserver.py --fqbn "$(CARDPUTER_ADV_FQBN)" --sketch $(CARDPUTER_ADV_SKETCH) --build-path $(CARDPUTER_ADV_BUILD) --output build/cardputer_adv_web/WebServer

prepare-tdeck:
	rm -rf $(TDECK_SKETCH)
	mkdir -p $(TDECK_SKETCH)
	cp *.h *.cpp RNode_Firmware.ino $(TDECK_SKETCH)/
	cp -R Fonts $(TDECK_SKETCH)/
	cp ../../src/core/radio/SX1262Timing.h ../../src/core/radio/RadioTimingPolicy.h $(TDECK_SKETCH)/
	cp ../esp_idf_compat/dhcpserver.c $(TDECK_SKETCH)/DhcpServer.c
	cp ../esp_idf_compat/SdkVersion.h $(TDECK_SKETCH)/
	cp ../esp_idf_compat/rtc_init.c $(TDECK_SKETCH)/RtcInit.c
	cp ../esp_idf_compat/rtc_sleep.c $(TDECK_SKETCH)/RtcSleep.c
	python3 ../../tools/patch_wifi_sockets.py --fqbn "$(TDECK_FQBN)" --sketch $(TDECK_SKETCH) --build-path $(TDECK_BUILD) --output build/tdeck_wifi/WiFi

prepare-tpager:
	rm -rf $(TPAGER_SKETCH)
	mkdir -p $(TPAGER_SKETCH)
	cp *.h *.cpp RNode_Firmware.ino $(TPAGER_SKETCH)/
	cp -R Fonts $(TPAGER_SKETCH)/
	cp ../../src/boards/tpager/hal/PagerBattery.h $(TPAGER_SKETCH)/
	cp ../../src/core/radio/SX1262Timing.h ../../src/core/radio/RadioTimingPolicy.h $(TPAGER_SKETCH)/
	cp ../esp_idf_compat/dhcpserver.c $(TPAGER_SKETCH)/DhcpServer.c
	cp ../esp_idf_compat/SdkVersion.h $(TPAGER_SKETCH)/
	cp ../esp_idf_compat/rtc_init.c $(TPAGER_SKETCH)/RtcInit.c
	cp ../esp_idf_compat/rtc_sleep.c $(TPAGER_SKETCH)/RtcSleep.c
	python3 ../../tools/patch_wifi_sockets.py --fqbn "$(TPAGER_FQBN)" --sketch $(TPAGER_SKETCH) --build-path $(TPAGER_BUILD) --output build/tpager_wifi/WiFi

firmware-cardputer_adv: check-build-identity prepare-cardputer-adv
	arduino-cli compile --log --fqbn "$(CARDPUTER_ADV_FQBN)" --build-path $(CARDPUTER_ADV_BUILD) --output-dir $(CARDPUTER_ADV_OUTPUT) --library build/cardputer_adv_wifi/WiFi --library build/cardputer_adv_web/WebServer --build-property "compiler.cpp.extra_flags=$(RATSPEAK_BUILD_FLAGS) $(CARDPUTER_ADV_EXTRA_FLAGS)" $(CARDPUTER_ADV_SKETCH)
	python3 ../../tools/patch_wifi_sockets.py --verify-database $(CARDPUTER_ADV_BUILD)/compile_commands.json --output build/cardputer_adv_wifi/WiFi
	python3 ../../tools/patch_webserver.py --verify-database $(CARDPUTER_ADV_BUILD)/compile_commands.json --output build/cardputer_adv_web/WebServer
	python3 ../../tools/check_sdk_backports.py --map $(CARDPUTER_ADV_BUILD)/RNode_Firmware.ino.map

firmware-tdeck: check-build-identity prepare-tdeck
	arduino-cli compile --log --fqbn "$(TDECK_FQBN)" --build-path $(TDECK_BUILD) --output-dir $(TDECK_OUTPUT) --library build/tdeck_wifi/WiFi --build-property "compiler.cpp.extra_flags=$(RATSPEAK_BUILD_FLAGS) $(TDECK_EXTRA_FLAGS)" $(TDECK_SKETCH)
	python3 ../../tools/patch_wifi_sockets.py --verify-database $(TDECK_BUILD)/compile_commands.json --output build/tdeck_wifi/WiFi
	python3 ../../tools/check_sdk_backports.py --map $(TDECK_BUILD)/RNode_Firmware.ino.map

firmware-tpager: check-build-identity prepare-tpager
	arduino-cli compile --log --fqbn "$(TPAGER_FQBN)" --build-path $(TPAGER_BUILD) --output-dir $(TPAGER_OUTPUT) --library build/tpager_wifi/WiFi --build-property "compiler.cpp.extra_flags=$(RATSPEAK_BUILD_FLAGS) $(TPAGER_EXTRA_FLAGS)" $(TPAGER_SKETCH)
	python3 ../../tools/patch_wifi_sockets.py --verify-database $(TPAGER_BUILD)/compile_commands.json --output build/tpager_wifi/WiFi
	python3 ../../tools/check_sdk_backports.py --map $(TPAGER_BUILD)/RNode_Firmware.ino.map

.PHONY: check-build-identity
check-build-identity:
	python3 ../../tools/release_identity.py build-flags
