#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := MicroPython

# ##########################################################################
# Variables for creating/flashing spiffs image file
ifeq ($(OS),Windows_NT)
	MKSPIFFS_BIN="mkspiffs.exe"
	MKFATFS_BIN="mkfatfs.exe"
else
	MKSPIFFS_BIN="mkspiffs"
	MKFATFS_BIN="mkfatfs"
endif
FILESYS_SIZE = $(shell echo $$(( $(CONFIG_MICROPY_INTERNALFS_SIZE) * 1024 )))
SPIFFS_IMAGE_COMPONENT_PATH := $(PWD)/components/spiffs_image
FATFS_IMAGE_COMPONENT_PATH := $(PWD)/components/fatfs_image
# ##########################################################################


ESPTOOLPY_SERIAL = $(IDF_PATH)/components/esptool_py/esptool/esptool.py --chip esp32 --port $(CONFIG_ESPTOOLPY_PORT) --baud $(CONFIG_ESPTOOLPY_BAUD) --before $(CONFIG_ESPTOOLPY_BEFORE) --after $(CONFIG_ESPTOOLPY_AFTER)

print_flash_cmd:
	echo $(ESPTOOLPY_SERIAL) write_flash $(if $(CONFIG_ESPTOOLPY_COMPRESSED),-z,-u) $(ESPTOOL_WRITE_FLASH_OPTIONS) $(ESPTOOL_ALL_FLASH_ARGS) | sed -e 's:'$(PWD)/build/'::g'

makefs:
	@echo "Making spiffs image; Flash address: $(CONFIG_MICROPY_INTERNALFS_START), Size: $(CONFIG_MICROPY_INTERNALFS_SIZE) KB ..."
	$(PROJECT_PATH)/components/mkspiffs/src/$(MKSPIFFS_BIN) -c $(SPIFFS_IMAGE_COMPONENT_PATH)/image -b 8192 -p 256 -s $(FILESYS_SIZE) $(BUILD_DIR_BASE)/spiffs_image.img
	@echo "--------------------------"
	@echo "To flash to ESP32 execute:"
	@echo "--------------------------"
	@echo "$(ESPTOOLPY_WRITE_FLASH) $(CONFIG_MICROPY_INTERNALFS_START) $(BUILD_DIR_BASE)/spiffs_image.img)"
	@echo "-----------------------------"
	@echo "or execute ./BUILD.sh flashfs"
	@echo "-----------------------------"

flashfs:
	@echo "Making spiffs image; Flash address: $(CONFIG_MICROPY_INTERNALFS_START), Size: $(CONFIG_MICROPY_INTERNALFS_SIZE) KB ..."
	$(PROJECT_PATH)/components/mkspiffs/src/$(MKSPIFFS_BIN) -c $(SPIFFS_IMAGE_COMPONENT_PATH)/image -b 8192 -p 256 -s $(FILESYS_SIZE) $(BUILD_DIR_BASE)/spiffs_image.img
	@echo "----------------------"
	@echo "Flashing the image ..."
	@echo "----------------------"
	$(ESPTOOLPY_WRITE_FLASH) $(CONFIG_MICROPY_INTERNALFS_START) $(BUILD_DIR_BASE)/spiffs_image.img
	@echo "----------------------"

copyfs: 
	@echo "-----------------------------"
	@echo "Flashing default spiffs image ..."
	@echo "-----------------------------"
	@echo "$(SPIFFS_IMAGE_COMPONENT_PATH)/spiffs_image.img"
	@echo "-----------------------------"
	$(ESPTOOLPY_WRITE_FLASH) $(CONFIG_MICROPY_INTERNALFS_START) $(SPIFFS_IMAGE_COMPONENT_PATH)/spiffs_image.img

makefatfs:
	@echo "Making fatfs image; Flash address: $(CONFIG_MICROPY_INTERNALFS_START), Size: $(CONFIG_MICROPY_INTERNALFS_SIZE) KB ..."
	@echo "$(ESPTOOLPY_WRITE_FLASH)"
	$(PROJECT_PATH)/components/mkfatfs/src/$(MKFATFS_BIN) -c $(FATFS_IMAGE_COMPONENT_PATH)/image -s $(FILESYS_SIZE) $(BUILD_DIR_BASE)/fatfs_image.img -d 2
	@echo "--------------------------"
	@echo "To flash to ESP32 execute:"
	@echo "--------------------------"
	@echo "$(ESPTOOLPY_WRITE_FLASH) $(CONFIG_MICROPY_INTERNALFS_START) $(BUILD_DIR_BASE)/fatfs_image.img)"
	@echo "--------------------------------"
	@echo "or execute ./BUILD.sh flashfatfs"
	@echo "--------------------------------"

flashfatfs:
	@echo "Making fatfs image; Flash address: $(CONFIG_MICROPY_INTERNALFS_START), Size: $(CONFIG_MICROPY_INTERNALFS_SIZE) KB ..."
	@echo "$(ESPTOOLPY_WRITE_FLASH)"
	$(PROJECT_PATH)/components/mkfatfs/src/$(MKFATFS_BIN) -c $(FATFS_IMAGE_COMPONENT_PATH)/image -s $(FILESYS_SIZE) $(BUILD_DIR_BASE)/fatfs_image.img -d 2
	@echo "----------------------"
	@echo "Flashing the image ..."
	@echo "----------------------"
	$(ESPTOOLPY_WRITE_FLASH) $(CONFIG_MICROPY_INTERNALFS_START) $(BUILD_DIR_BASE)/fatfs_image.img
	@echo "----------------------"

copyfatfs: 
	@echo "----------------------------"
	@echo "Flashing default fatfs image ..."
	@echo "----------------------------"
	@echo "$(FATFS_IMAGE_COMPONENT_PATH)/fatfs_image.img"
	@echo "-----------------------------"
	$(ESPTOOLPY_WRITE_FLASH) $(CONFIG_MICROPY_INTERNALFS_START) $(FATFS_IMAGE_COMPONENT_PATH)/fatfs_image.img

include $(IDF_PATH)/make/project.mk
