# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Issen Software Group

# One kext, one CLI and one app for every macOS from 10.13 High Sierra up. Override on the command
# line, e.g. `make kext SDK_PATH=/path/MacOSX10.13.sdk` to build against the High Sierra SDK itself.
MIN_OS   ?= 10.13
SDK_PATH ?= $(shell xcrun --sdk macosx --show-sdk-path)
CXX := clang++
CC := clang

KEXT_CXXFLAGS := -arch x86_64 -mmacosx-version-min=$(MIN_OS) -mkernel -fno-builtin -fno-common \
                 -fno-exceptions -fno-rtti -nostdinc -std=c++14 -O2 \
                 -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
                 -Wno-deprecated-declarations \
                 -isysroot "$(SDK_PATH)" \
                 -I"$(SDK_PATH)/System/Library/Frameworks/Kernel.framework/Headers" \
                 -Isrc -MMD -MP

KEXT_CFLAGS   := -arch x86_64 -mmacosx-version-min=$(MIN_OS) -mkernel -fno-builtin -fno-common \
                 -nostdinc -O2 \
                 -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
                 -isysroot "$(SDK_PATH)" \
                 -I"$(SDK_PATH)/System/Library/Frameworks/Kernel.framework/Headers"

KEXT_LDFLAGS  := -arch x86_64 -mmacosx-version-min=$(MIN_OS) -Wl,-kext -nostdlib \
                 -lkmodc++ -lkmod -lcc_kext -isysroot "$(SDK_PATH)"

KEXT_BUNDLE   := build/RTL8723BEWiFi.kext
KEXT_BIN      := $(KEXT_BUNDLE)/Contents/MacOS/RTL8723BEWiFi
CLI_BIN       := tools/rtl8723be_cli

KEXT_OBJS     := build/RTL8723BE.o \
                 build/RTL8723BEUserClient.o \
                 build/RTL8723BE_crypto.o \
                 build/RTL8723BE_firmware.o \
                 build/RTL8723BE_firmware36.o \
                 build/RTL8723BE_tables.o \
                 build/RTL8723BE_kmod.o

-include $(KEXT_OBJS:.o=.d)

.DEFAULT_GOAL := all
.PHONY: all kext cli ui-classic ui-modern test test-hardware diagnose clean

all: kext cli

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
	$(CXX) -arch x86_64 -mmacosx-version-min=$(MIN_OS) -isysroot "$(SDK_PATH)" -std=c++14 -O2 -framework IOKit -framework CoreFoundation \
		tools/rtl8723be_cli.cpp -o $(CLI_BIN)

# App: one bundle, two interfaces. The classic AppKit executable is the bundle's main executable
# (macOS $(MIN_OS)+, no Swift runtime) and hands over to the SwiftUI one on macOS 14+.
# The SwiftUI part is always built against the host's own SDK, whatever SDK_PATH is.
CLASSIC_BIN := build/RTL8723BEWirelessUtility
MODERN_BIN  := build/RTL8723BEWirelessUtilityModern
MODERN_SDK  := $(shell xcrun --sdk macosx --show-sdk-path)
MODERN_MIN  := 14.0

ui-classic: gui/Classic/main.m gui/RTL8723BEBridge.h src/RTL8723BE_ipc.h
	@mkdir -p build
	$(CC) -fobjc-arc -O2 -Wall -Wno-deprecated-declarations -arch x86_64 -mmacosx-version-min=$(MIN_OS) \
		-isysroot "$(SDK_PATH)" -framework Cocoa -framework IOKit gui/Classic/main.m -o $(CLASSIC_BIN)

ui-modern: $(wildcard gui/Sources/*.swift gui/Sources/Views/*.swift) gui/RTL8723BEBridge.h
	@mkdir -p build
	swiftc -O -parse-as-library -swift-version 5 -target x86_64-apple-macosx$(MODERN_MIN) -sdk "$(MODERN_SDK)" \
		-import-objc-header gui/RTL8723BEBridge.h \
		-framework AppKit -framework SwiftUI -framework IOKit -framework ServiceManagement \
		gui/Sources/*.swift gui/Sources/Views/*.swift -o $(MODERN_BIN)

test: test-hardware test-crypto test-boot-config
	$(MAKE) -C tests test

diagnose: kext
	kmutil print-diagnostics --bundle-path $(KEXT_BUNDLE)

clean:
	rm -rf build/*.o $(KEXT_BUNDLE) $(CLI_BIN) $(CLASSIC_BIN) $(MODERN_BIN)

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
