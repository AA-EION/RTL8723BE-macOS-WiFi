# Realtek RTL8723BE (`pci10ec,b723`) Wi-Fi driver for macOS

A kernel extension (`RTL8723BEWiFi.kext`), menu-bar app (`RTL8723BE Wireless Utility.app`)
and command-line tool (`rtl8723be_cli`) for the **Realtek RTL8723BE PCIe 802.11b/g/n** card,
aimed at OpenCore hackintoshes. The hardware bring-up is a port of Linux
`drivers/net/wireless/realtek/rtlwifi/rtl8723be`.

**Status (v1.3.0): working.** Verified on an HP laptop (subsystem `103c:804c`) running macOS 26
(Darwin 25): scan, WPA2-PSK connect, DHCP, and internet traffic over Wi-Fi.

| | |
|---|---|
| Scan (2.4 GHz, channels 1–13) | ✅ ~30 networks |
| WPA2-PSK (CCMP) connect, DHCP | ✅ |
| Ping to router / internet | ✅ 0% loss (~4 ms / ~30 ms) |
| Throughput | ~3.5 Mbit/s (802.11g rates; see limitations) |

## Install

1. Add **`rtl8723be_experimental=1`** to `boot-args` in your OpenCore `config.plist`
   (NVRAM → `7C436110-AB2A-4BBB-A880-FE41995C9F82`) and reboot. Without it the driver refuses
   to start. Your SIP/OpenCore setup must allow third-party kexts.
2. Download the DMG from [Releases](https://github.com/AA-EION/RTL8723BE-macOS-WiFi/releases),
   open it and run **Install RTL8723BE WiFi.pkg**. It installs:
   - `RTL8723BE Wireless Utility.app` → `/Applications`
   - `RTL8723BEWiFi.kext` → `/Library/Extensions`
   - `rtl8723be_cli` → `/usr/local/bin`
3. Allow the driver in **System Settings → Privacy & Security** and restart.
4. Connect with the app (Scan → pick network → password), or:
   ```bash
   rtl8723be_cli scan
   rtl8723be_cli connect "Network Name" 'password'
   ```

The package is unsigned: if macOS blocks it, right-click → Open. The installer never touches
NVRAM or the EFI partition. To remove everything: `sudo ./uninstall.sh` from the DMG.

Do not inject this kext through OpenCore; install it with the package.

## How it works / notes

- macOS sees the card as a **wired Ethernet port** (`en*`). Networks are listed in the app and
  CLI, not in the macOS Wi-Fi menu: that needs Apple's private `IO80211Family`, which third-party
  drivers cannot use on current macOS.
- **Antenna:** many HP boards have a single antenna on the AUX port while the eFuse claims two
  (Linux needs `ant_sel=1` there). The first scan measures both RF paths and keeps the better
  one. Override with the boot arg `rtl8723be_ant_sel=1|2` or `rtl8723be_cli antenna 1|2`.
- **Firmware:** the driver embeds Realtek's `rtl8723befw_36.bin` from
  [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git)
  (redistributable, see [LICENCE.rtlwifi_firmware.txt](LICENCE.rtlwifi_firmware.txt)). The older
  v15 image is selectable with `rtl8723be_fw=15`; it breaks OFDM reception on this hardware.
- **Diagnostics:** `rtl8723be_cli diag` (RX/TX counters per rate, antenna and coex registers),
  `reg read|write`, `rf read|write`, `dump`, `iqk`. Writes need `sudo`.
- Driver log: `/usr/bin/log show --last 10m --predicate 'eventMessage CONTAINS "RTL8723BE"'`.

## Limitations

- 802.11g association only (no 802.11n/HT yet), fixed 24 Mbit/s TX rate, no rate adaptation.
- WPA2-PSK only (no open/WEP/WPA3/enterprise); no group-key rekey handshake.
- No power management, sleep/wake re-initialisation, or roaming.
- 2.4 GHz only (the hardware has no 5 GHz).

## Building from source

```bash
make all                 # kext + CLI
make test-hardware test-crypto   # descriptor/eFuse/table and CCMP (IEEE 802.11 vector) tests
./scripts/build_dmg.sh   # app, installer .pkg and dist/RTL8723BE_WiFi_Installer.dmg
```

## Repository structure

- [`src/`](src/) — kernel extension (`RTL8723BE.cpp` hardware/802.11, `RTL8723BEUserClient.cpp`,
  `RTL8723BE_crypto.cpp`, `RTL8723BE_tables.cpp` Linux PHY/RF tables, firmware images)
- [`gui/`](gui/) — Swift/AppKit app; [`tools/`](tools/) — CLI
- [`installer/`](installer/) — package scripts and installer texts;
  [`scripts/`](scripts/) — `build_dmg.sh`, `load_test_kext.sh`, `uninstall.sh`
- [`tests/`](tests/) — unit tests; [`docs/`](docs/) — design notes and earlier test/crash write-ups
