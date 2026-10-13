# Notices

RTL8723BE Wi-Fi is made by **Issen Software Group**. See [LICENSE](LICENSE) for which
licence applies to which part.

## Linux rtlwifi / rtl8723be (GPL-2.0)

The kernel extension's hardware support — power sequences, MAC/BB/RF initialisation,
eFuse decoding, PHY/RF/AGC tables, IQ/LC calibration, BT-coexistence antenna setup and
the H2C command protocol — is ported from the Linux kernel driver
`drivers/net/wireless/realtek/rtlwifi` (`rtl8723be`, `rtl8723com`, `efuse.c`, `btcoexist`).

    Copyright(c) 2009-2014  Realtek Corporation.
    SPDX-License-Identifier: GPL-2.0

The ported files are licensed GPL-2.0-only ([LICENSES/GPL-2.0-only.txt](LICENSES/GPL-2.0-only.txt)).

## Realtek firmware

`rtl8723befw_36.bin` (and the older `rtl8723befw.bin` image) come from
[linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git)
(`rtlwifi/`), Copyright (c) 2010 Realtek Semiconductor Corporation, redistributed
unmodified under [LICENSES/LicenseRef-rtlwifi-firmware.txt](LICENSES/LicenseRef-rtlwifi-firmware.txt).

## TetherKit / TetherKitNext (MIT)

Parts of the app's user interface (the design system, status ring, log view and page
layout) are adapted from [TetherKitNext](https://github.com/AA-EION/TetherKitNext) by
Issen Software Group, itself based on [TetherKit](https://github.com/XiaoMiku01/TetherKit)
by XiaoMiku01.

    Copyright (c) 2026 Issen Software Group (TetherKitNext)
    Copyright (c) 2026 TetherKit contributors
    SPDX-License-Identifier: MIT

## OpenBSD net80211 (BSD-3-Clause / ISC)

The portable WPA2 state machine and receive validation follow OpenBSD's
`ieee80211_pae_input.c`, `ieee80211_pae_output.c`, and `ieee80211_input.c`.
A-MSDU destination validation, LLC decapsulation and ADDBA refusal are adaptations
of its net80211 code. Complete author notices and terms are retained in the
source and [LICENSES/LicenseRef-OpenBSD-net80211.txt](LICENSES/LicenseRef-OpenBSD-net80211.txt).
Source revisions and hashes are recorded in [docs/upstream-sources.json](docs/upstream-sources.json).

## Linux Bluetooth (GPL-2.0-or-later)

`bluetooth/` ports the RTL8723B epatch selection and download protocol from
Linux v6.12 `drivers/bluetooth/btrtl.c` and USB transport conventions from
`btusb.c`. The btrtl.c copyright notice is (C) 2015 Endless Mobile, Inc. The synchronous
IOUSBHost implementation is original. The native-controller handoff was checked
against thegwchr/RealtekBluetoothFirmware at commit
`22e501a8a0a7aba8bb4e594bc6f40b1a67391af9`; its implementation is not vendored.

`bluetooth/firmware/rtl8723b_fw.bin` is redistributed unmodified from linux-firmware
under [Realtek's firmware terms](LICENSES/LicenseRef-rtl-bt-firmware.txt).
The pinned SHA-256 is `c9c8a9f6ebd90b7449a7e4d38ee39952dc7cc47d6c1d054e42cfbb5e4d818853`.
The build verifies this hash before embedding the original bytes.

BlueToolFixup and Lilu are external dependencies, not bundled or modified here.
