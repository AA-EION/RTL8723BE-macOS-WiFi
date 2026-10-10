// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group

#ifndef _RTL8723BE_FIRMWARE_HPP_
#define _RTL8723BE_FIRMWARE_HPP_

#include <stdint.h>
#include <stddef.h>

extern const uint8_t rtl8723befw_bin[];
extern const size_t rtl8723befw_bin_len;

// Firmware v36 (linux-firmware rtlwifi/rtl8723befw_36.bin), the version Linux loads by default.
extern const uint8_t rtl8723befw36_bin[];
extern const size_t rtl8723befw36_bin_len;

#endif // _RTL8723BE_FIRMWARE_HPP_
