# Source provenance

Hardware/protocol choices in the 1.4.0 development work were checked against
actual upstream source, rather than a chipset family guess. Linux references
are pinned to **v6.12**. OpenBSD file revisions and SHA-256 hashes are recorded
in `upstream-sources.json`; master URLs may advance, so the recorded hash is
the reference identity, not an assertion that current master is unchanged.

Run `python3 scripts/fetch_upstream.py` to cache and verify all listed source
files in `.references/`. A changed upstream file fails its recorded hash check;
the script never silently substitutes newer code. This cache survives a reboot
and is excluded from version control.

| Ported behavior | Primary code used |
|---|---|
| HT rates, SEC/SGI/CTS descriptor bits, C2H report discriminator | Linux `rtl8723be/trx.h`, `rtl8723be_tx_fill_desc()` / `rtl8723be_rx_query_desc()` in `trx.c` |
| Seven-byte H2C mailbox, extension committed first | Linux `rtl8723be/fw.c`, `_rtl8723be_fill_h2c_command()` |
| RA mask/ARFR index, short slot, SIFS, ACK preamble | Linux `rtl8723be/hw.c`, `rtl8723be_update_hal_rate_mask()` / `rtl8723be_set_hw_reg()` |
| Firmware RSSI report | Linux `rtl8723be/dm.c`, `rtl8723be_dm_check_rssi_monitor()`; MACID 0, byte 1 = 0x20, H2C 0x42 |
| Pairwise CAM entry, AES cipher and valid-last order | Linux `rtlwifi/cam.c`, `rtl_cam_program_entry()`; descriptor security type 3 from `trx.c` |
| Hardware CCMP packet contents | Linux `net/mac80211/wpa.c`, `ccmp_encrypt_skb()`; software IV + plaintext, no MIC tail when hardware key is used |
| Replay counters per security queue | Linux `net/mac80211/rx.c`, `ieee80211_parse_qos()` and `wpa.c`, CCMP receive; non-QoS data uses queue 0 |
| Stop/restart and ring reset on sleep/wake | Linux `rtlwifi/core.c` `rtl_op_stop()` / `rtl_op_start()` and `pci.c` `rtl_pci_stop()` / `rtl_pci_start()`; mac80211 stops the adapter before PCI suspend |
| Single-antenna internal-switch PTA baseline | Linux `btcoexist/halbtc8723b1ant.c`, `halbtc8723b1ant_set_ant_path()` and `halbtc8723b1ant_ps_tdma(false, 8)`; main 0x200, auxiliary 0x80 |
| WPA2 validation and rekey handling | OpenBSD `ieee80211_pae_input.c`, four-way/group receive handlers; message-3 RSN must match the beacon, authenticated GTK RSC, no same-key reinstall |
| WPA2 reply flags and key length | OpenBSD `ieee80211_pae_output.c`, message-2/message-4/group-2 generation; RSN response key length zero |
| A-MSDU validation / decapsulation | OpenBSD `ieee80211_input.c` revision 1.263, `ieee80211_amsdu_decap_validate()` / `ieee80211_decap()`; reject RFC1042 destination and foreign subframe DA |
| Explicit Block Ack refusal | OpenBSD `ieee80211_output.c` revision 1.148, `ieee80211_get_addba_resp()` refusal branch, status 37 |
| RTL8723B Bluetooth identity, epatch selection and fragments | Linux `drivers/bluetooth/btrtl.c`, IC_INFO entry, `rtlbt_parse_firmware()`, `rtl_download_firmware()` |
| HCI USB control/event transport | Linux `drivers/bluetooth/btusb.c`; interface zero, class/device control request and interrupt-IN events |

The portable WPA2 code is an adaptation for this small standalone station
driver; it is **not a complete mac80211/net80211 port**. Kernel IOKit plumbing,
the app, deadlines and synchronous USB lifetime handling are host adaptations.
The same portable protocol/parser implementation is compiled into the real
drivers and host regression tests.

Linux source root: https://github.com/torvalds/linux/tree/v6.12

OpenBSD source root: https://github.com/openbsd/src/tree/master/sys/net80211

macOS Bluetooth handoff and dependencies were checked against
`thegwchr/RealtekBluetoothFirmware` commit
`22e501a8a0a7aba8bb4e594bc6f40b1a67391af9`. Its callback transport was not copied;
this loader uses synchronous bounded reads instead.

## Bluetooth firmware

Original download:
https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main/rtl_bt/rtl8723b_fw.bin

45,048 bytes, SHA-256
`c9c8a9f6ebd90b7449a7e4d38ee39952dc7cc47d6c1d054e42cfbb5e4d818853`.
The build pins the bytes by that hash. linux-firmware WHENCE identifies this
as RTL8723B firmware from Realtek's `linux_bt_usb_2.11.20140423_8723be.rar`.
The original firmware licence is retained in LICENSES.

Linux marks the USB RTL8723B configuration file optional (`config_needed=false`),
so no guessed board configuration is appended. Boards requiring vendor-specific
configuration need their actual configuration checked before being claimed
supported. USB IDs narrow matching; the HCI LMP/version/revision and ROM/patch
identity must also match before any bytes are uploaded.
