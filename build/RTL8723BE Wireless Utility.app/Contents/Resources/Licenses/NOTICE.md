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
