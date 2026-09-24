DEVICE ?= tdeck
SUPPORTED_DEVICES := $(shell python3 tools/release_catalog.py devices)
STANDALONE_ENV ?= $(DEVICE)
RNODE_DIR ?= vendor/rnode_firmware
LAUNCHER_DIR ?= launcher
BUILD_DIR ?= build
DIST_DIR ?= dist

ifeq ($(filter $(DEVICE),$(SUPPORTED_DEVICES)),)
$(error Unsupported DEVICE '$(DEVICE)'; choose one of: $(SUPPORTED_DEVICES))
endif

# Catalog values are whitespace-free Make assignments. Partition geometry comes
# from the selected CSV, so packaging and validation cannot drift independently.
RELEASE_CONFIG := $(shell python3 tools/release_catalog.py make --device $(DEVICE))
ifeq ($(strip $(RELEASE_CONFIG)),)
$(error Failed to load release catalog for $(DEVICE))
endif
$(foreach setting,$(RELEASE_CONFIG),$(eval $(subst =, := ,$(setting))))
PARTITIONS_BIN := $(BUILD_DIR)/$(PARTITIONS_BASENAME)
FULL_NAME := $(BRAND)-full
STANDALONE_NAME := $(BRAND)-standalone
RNODE_ONLY_NAME := $(BRAND)-rnode

FULL_BIN := $(BUILD_DIR)/$(FULL_NAME).bin
STANDALONE_BIN := $(BUILD_DIR)/$(STANDALONE_NAME).bin
RNODE_ONLY_BIN := $(BUILD_DIR)/$(RNODE_ONLY_NAME).bin

LAUNCHER_BIN := $(LAUNCHER_DIR)/.pio/build/$(DEVICE)_launcher/firmware.bin
STANDALONE_APP_BIN := .pio/build/$(STANDALONE_ENV)/firmware.bin

RNODE_OUTPUT := $(RNODE_DIR)/build/$(RNODE_TARGET).esp32.esp32s3
RNODE_BIN := $(RNODE_OUTPUT)/RNode_Firmware.ino.bin
RNODE_BOOTLOADER_BIN := $(RNODE_OUTPUT)/RNode_Firmware.ino.bootloader.bin
RNODE_PARTITIONS_BIN := $(RNODE_OUTPUT)/RNode_Firmware.ino.partitions.bin

BOOTLOADER_BIN := .pio/build/$(STANDALONE_ENV)/bootloader.bin
PLATFORMIO_ARDUINO ?= $(HOME)/.platformio/packages/framework-arduinoespressif32
ARDUINO15_ESP32 ?= $(HOME)/Library/Arduino15/packages/esp32/hardware/esp32/2.0.17
GEN_ESPPART ?= $(if $(wildcard $(PLATFORMIO_ARDUINO)/tools/gen_esp32part.py),$(PLATFORMIO_ARDUINO)/tools/gen_esp32part.py,$(ARDUINO15_ESP32)/tools/gen_esp32part.py)
BOOT_APP0_BIN ?= $(if $(wildcard $(PLATFORMIO_ARDUINO)/tools/partitions/boot_app0.bin),$(PLATFORMIO_ARDUINO)/tools/partitions/boot_app0.bin,$(ARDUINO15_ESP32)/tools/partitions/boot_app0.bin)

UPLOAD_BAUD ?= $(if $(filter m9,$(DEVICE)),115200,460800)
PORT ?= $(port)
ifeq ($(PORT),)
PORT := /dev/ttyACM0
endif

.PHONY: all build setup doctor doctor-source source-check build-standalone build-launcher build-rnode check check-all bundle full-image standalone-image rnode-only-image package package-all protocol-check software-check release flash clean

all: bundle

build: package

setup:
ifeq ($(HAS_RNODE),1)
	$(MAKE) -C $(RNODE_DIR) $(RNODE_PREP_TARGET)
else
	@echo "$(DEVICE): Standalone uses PlatformIO; no RNode toolchain setup needed"
endif

doctor:
	python3 tools/doctor.py --device $(DEVICE)

doctor-source:
	python3 tools/doctor.py --device $(DEVICE) --source
	python3 tools/check_prebuilt.py --allow-dirty

source-check:
	python3 tools/release_catalog.py check
	python3 tools/check_source_release.py
	python3 tools/check_runtime_boundary.py
	python3 tools/collect_licenses.py --check

build-standalone:
	python3 -m platformio run -e $(STANDALONE_ENV)

ifeq ($(HAS_RNODE),1)
build-launcher:
	python3 -m platformio run -d $(LAUNCHER_DIR) -e $(DEVICE)_launcher

build-rnode:
	$(MAKE) -C $(RNODE_DIR) firmware-$(RNODE_TARGET)

$(PARTITIONS_BIN): $(PARTITION_CSV)
	mkdir -p $(BUILD_DIR)
	python3 $(GEN_ESPPART) $(PARTITION_CSV) $(PARTITIONS_BIN)

check: build-launcher build-standalone build-rnode
	python3 tools/check_image_fit.py --device $(DEVICE) --launcher $(LAUNCHER_BIN) \
		--standalone $(STANDALONE_APP_BIN) --rnode $(RNODE_BIN)

full-image: check $(PARTITIONS_BIN)
	python3 tools/make_dual_image.py --device $(DEVICE) \
		--bootloader $(BOOTLOADER_BIN) \
		--partitions $(PARTITIONS_BIN) \
		--boot-app0 $(BOOT_APP0_BIN) \
		--launcher $(LAUNCHER_BIN) \
		--standalone $(STANDALONE_APP_BIN) \
		--rnode $(RNODE_BIN) \
		--output $(FULL_BIN)
else
build-launcher build-rnode full-image rnode-only-image:
	@echo "$(DEVICE): Full and RNode are not supported yet" >&2
	@exit 1

check: build-standalone
	python3 tools/check_image_fit.py --device $(DEVICE) --standalone $(STANDALONE_APP_BIN)
endif

check-all:
	@for device in $(SUPPORTED_DEVICES); do \
		$(MAKE) check DEVICE=$$device || exit $$?; \
	done

standalone-image: build-standalone
	mkdir -p $(BUILD_DIR)
	python3 -m esptool --chip esp32s3 merge-bin --flash-mode dio --flash-size $(FLASH_SIZE) \
		$(if $(STANDALONE_FLASH_FREQ),--flash-freq $(STANDALONE_FLASH_FREQ),) --output $(STANDALONE_BIN) \
		0x0000 $(BOOTLOADER_BIN) 0x8000 .pio/build/$(STANDALONE_ENV)/partitions.bin \
		0xe000 $(BOOT_APP0_BIN) $(STANDALONE_FACTORY_OFFSET) $(STANDALONE_APP_BIN)

ifeq ($(HAS_RNODE),1)
rnode-only-image: build-rnode
	mkdir -p $(BUILD_DIR)
	python3 -m esptool --chip esp32s3 merge-bin \
		--flash-mode dio --flash-size $(FLASH_SIZE) \
		--output $(RNODE_ONLY_BIN) \
		0x0000 $(RNODE_BOOTLOADER_BIN) \
		0x8000 $(RNODE_PARTITIONS_BIN) \
		0xe000 $(BOOT_APP0_BIN) \
		$(RNODE_FACTORY_OFFSET) $(RNODE_BIN)

bundle: full-image
FACTORY_BIN := $(FULL_BIN)

package: full-image standalone-image rnode-only-image
else
bundle: check standalone-image
FACTORY_BIN := $(STANDALONE_BIN)

package: check standalone-image
endif
	mkdir -p $(DIST_DIR)
	rm -f $(DIST_DIR)/$(FULL_NAME).zip \
	      $(DIST_DIR)/$(STANDALONE_NAME).zip \
	      $(DIST_DIR)/$(RNODE_ONLY_NAME).zip \
	      $(DIST_DIR)/$(APP_STANDALONE_NAME).bin \
	      $(DIST_DIR)/$(RNODE_ONLY_NAME).bin
ifeq ($(HAS_RNODE),1)
	python3 tools/package_merged_zip.py --image $(FULL_BIN) --name $(FULL_NAME) --device $(DEVICE) --package full --flash-size $(FLASH_SIZE) --output $(DIST_DIR)/$(FULL_NAME).zip
	python3 tools/package_merged_zip.py --image $(RNODE_ONLY_BIN) --name $(RNODE_ONLY_NAME) --device $(DEVICE) --package rnode --flash-size $(FLASH_SIZE) --output $(DIST_DIR)/$(RNODE_ONLY_NAME).zip
	cp $(RNODE_BIN) $(DIST_DIR)/$(APP_RNODE_NAME).bin
endif
	python3 tools/package_merged_zip.py --image $(STANDALONE_BIN) --name $(STANDALONE_NAME) --device $(DEVICE) --package standalone --flash-size $(FLASH_SIZE) --output $(DIST_DIR)/$(STANDALONE_NAME).zip
	cp $(STANDALONE_APP_BIN) $(DIST_DIR)/$(APP_STANDALONE_NAME).bin

package-all:
	@for device in $(SUPPORTED_DEVICES); do \
		$(MAKE) package DEVICE=$$device || exit $$?; \
	done

protocol-check: source-check
	cd ../rsReticulumLite && ./scripts/test-matrix.sh
	cd ../rsLXMFLite && ./scripts/test-matrix.sh
	cd protocol && cargo fmt --all -- --check
	cd protocol && cargo test --workspace --locked
	cd protocol && cargo clippy --workspace --no-default-features --features 'std profile-small' --all-targets --locked -- -D warnings
	cd protocol && cargo clippy --workspace --no-default-features --features 'std profile-micro' --all-targets --locked -- -D warnings
	cd protocol && cargo check --workspace --no-default-features --features profile-small --target thumbv7em-none-eabihf --locked
	cd protocol && cargo check --workspace --no-default-features --features profile-small --target riscv32imc-unknown-none-elf --locked
	cd protocol && cargo check --workspace --no-default-features --features profile-micro --target thumbv7em-none-eabihf --locked
	cd protocol && cargo check --workspace --no-default-features --features profile-micro --target riscv32imc-unknown-none-elf --locked
	python3 tools/check_prebuilt.py --allow-dirty

software-check: protocol-check check-all

release: package

flash: bundle
	python3 -m esptool --chip esp32s3 --port $(PORT) --baud $(UPLOAD_BAUD) --before default_reset --after hard_reset write-flash 0x0 $(FACTORY_BIN)

clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)
