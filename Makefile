# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Issen Software Group

SDK_PATH := $(shell xcrun --sdk macosx --show-sdk-path)
CXX := clang++
CC := clang

KEXT_CXXFLAGS := -arch x86_64 -mmacosx-version-min=26.5 -mkernel -fno-builtin -fno-common \
                 -fno-exceptions -fno-rtti -nostdinc -std=c++14 -O2 \
                 -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
                 -Wno-deprecated-declarations \
                 -isysroot "$(SDK_PATH)" \
                 -I"$(SDK_PATH)/System/Library/Frameworks/Kernel.framework/Headers" \
                 -Isrc -MMD -MP

KEXT_CFLAGS   := -arch x86_64 -mmacosx-version-min=26.5 -mkernel -fno-builtin -fno-common \
                 -nostdinc -O2 \
                 -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
                 -isysroot "$(SDK_PATH)" \
                 -I"$(SDK_PATH)/System/Library/Frameworks/Kernel.framework/Headers"

KEXT_LDFLAGS  := -arch x86_64 -mmacosx-version-min=26.5 -Wl,-kext -nostdlib \
                 -lkmodc++ -lkmod -lcc_kext -isysroot "$(SDK_PATH)"

KEXT_BUNDLE   := build/RTL8723BEWiFi.kext
KEXT_BIN      := $(KEXT_BUNDLE)/Contents/MacOS/RTL8723BEWiFi
CLI_BIN       := tools/rtl8723be_cli

KEXT_OBJS     := build/RTL8723BE.o \
                 build/RTL8723BEUserClient.o \
                 build/RTL8723BE_crypto.o \
                 build/RTL8723BE_protocol.o \
                 build/RTL8723BE_firmware.o \
                 build/RTL8723BE_firmware36.o \
                 build/RTL8723BE_tables.o \
                 build/RTL8723BE_kmod.o

-include $(KEXT_OBJS:.o=.d)

.DEFAULT_GOAL := all
.PHONY: all kext cli test test-hardware diagnose clean

all: kext cli bluetooth

BT_BUNDLE := build/RTL8723BluetoothFirmware.kext
BT_OBJECTS := build/RTL8723BluetoothFirmware.o build/bt_firmware.o build/bt_kmod.o

build/RTL8723BluetoothFirmware.o: bluetooth/RTL8723BluetoothFirmware.cpp bluetooth/RTL8723BluetoothFirmware.hpp bluetooth/firmware.hpp
	@mkdir -p build
	$(CXX) $(KEXT_CXXFLAGS) -Ibluetooth -c $< -o $@

build/bt_firmware.cpp: bluetooth/firmware/rtl8723b_fw.bin scripts/embed_bt_firmware.py
	python3 scripts/embed_bt_firmware.py $< $@

build/bt_firmware.o: build/bt_firmware.cpp bluetooth/firmware.hpp
	$(CXX) $(KEXT_CXXFLAGS) -Ibluetooth -c $< -o $@

build/bt_kmod.o: bluetooth/kmod.c
	$(CC) $(KEXT_CFLAGS) -c $< -o $@

.PHONY: bluetooth
bluetooth: $(BT_OBJECTS) bluetooth/Info.plist
	@mkdir -p $(BT_BUNDLE)/Contents/MacOS
	$(CXX) $(KEXT_LDFLAGS) $(BT_OBJECTS) -o $(BT_BUNDLE)/Contents/MacOS/RTL8723BluetoothFirmware
	cp bluetooth/Info.plist $(BT_BUNDLE)/Contents/Info.plist
	codesign --force --deep --sign - $(BT_BUNDLE)

build/%.o: src/%.cpp
	@mkdir -p build
	$(CXX) $(KEXT_CXXFLAGS) -c $< -o $@

build/RTL8723BE_kmod.o: src/RTL8723BE_kmod.c
	@mkdir -p build
	$(CC) $(KEXT_CFLAGS) -c $< -o $@

kext: $(KEXT_OBJS) src/Info.plist
	@mkdir -p $(KEXT_BUNDLE)/Contents/MacOS
	$(CXX) $(KEXT_LDFLAGS) $(KEXT_OBJS) -o $(KEXT_BIN)
	cp src/Info.plist $(KEXT_BUNDLE)/Contents/Info.plist
	codesign --force --deep --sign - $(KEXT_BUNDLE)

cli: tools/rtl8723be_cli.cpp src/RTL8723BE_ipc.h
	$(CXX) -arch x86_64 -std=c++17 -O2 -framework IOKit -framework CoreFoundation \
		tools/rtl8723be_cli.cpp -o $(CLI_BIN)

test: test-hardware test-crypto test-protocol test-bluetooth test-credentials test-boot-config
	$(MAKE) -C tests test

diagnose: kext
	kmutil print-diagnostics --bundle-path $(KEXT_BUNDLE)

clean:
	rm -rf build/*.o build/*.d $(KEXT_BUNDLE) $(BT_BUNDLE) $(CLI_BIN) build/bt_firmware.cpp

# Production descriptor ABI checks, independent of the legacy simulated driver.
test-hardware:
	@mkdir -p build
	$(CXX) -std=c++14 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc tests/test_hardware_contract.cpp src/RTL8723BE_tables.cpp -o build/test_hardware_contract
	./build/test_hardware_contract

.PHONY: test-crypto
test-crypto:
	@mkdir -p build
	$(CXX) -std=c++14 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc tests/test_crypto_bounds.cpp src/RTL8723BE_crypto.cpp -o build/test_crypto_bounds
	./build/test_crypto_bounds

.PHONY: test-boot-config
test-boot-config:
	python3 tests/test_boot_config.py

.PHONY: test-protocol test-bluetooth
test-protocol:
	@mkdir -p build
	$(CXX) -std=c++14 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc tests/test_protocol.cpp src/RTL8723BE_protocol.cpp src/RTL8723BE_crypto.cpp -o build/test_protocol
	./build/test_protocol

test-bluetooth:
	@mkdir -p build
	$(CXX) -std=c++14 -Wall -Wextra -Werror -fsanitize=address,undefined -Ibluetooth tests/test_bluetooth_firmware.cpp -o build/test_bluetooth_firmware
	./build/test_bluetooth_firmware

.PHONY: test-credentials
test-credentials:
	@mkdir -p build
	swiftc -parse-as-library -module-cache-path build/swift-module-cache -framework Security gui/Sources/NetworkCredentials.swift tests/test_credentials.swift -o build/test_credentials
	./build/test_credentials
