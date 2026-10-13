# 1.4.0 development feature status

This is an experimental development build. Implemented code, host tests and
hardware validation are separate evidence. It is **not a full Wi-Fi 4 compliance
claim**. Prior 1.3.0 throughput results do not measure this build.

| Feature | Implementation | Validation / remaining work |
|---|---|---|
| 802.11b/g | Negotiate supported/basic rates, short slot/preamble, ERP protection, firmware rate mask; non-WMM peers get ordinary data frames | Source/descriptor checks; retest b-only and g-only APs |
| 802.11n | HT20, one spatial stream, MCS0–7, optional SGI20, WMM information/parameters, checked RX A-MSDU up to 3839 bytes | HT20/WMM/SGI association and interface-bound traffic passed on this host; A-MSDU has host tests |
| Full Wi-Fi 4 | Not complete | Block Ack reorder/agreements, TX A-MPDU, HT40 coexistence/channel policy, fragmented MSDUs and certification-style interop remain |
| WPA2 Personal | CCMP/PSK only; ASCII 8–63 passphrase or 64 hexadecimal PSK; four-way exchange, GTK renewal, pairwise rekey, MIC/ANonce/RSN checks | Production ASan/UBSan tests and real AP four-way exchange/DHCP/traffic passed; real rekey and broader AP interoperability remain |
| Replay / reinstall protection | Per-TID PTK and per-KeyID/per-TID GTK PN; authenticated initial GTK RSC; repeated M3/group messages ACK without resetting counters | Production supplicant tests cover replay, retransmission and rekey |
| Saved networks | Login Keychain per SSID; save after successful join, reuse, forget, replace password; preferences hold network names/options only | Isolated real Keychain save/read/update/delete passed; real AP join and app rejoin after quit/relaunch passed; in-app replacement/forget remain |
| Auto-join | Choose strongest visible saved WPA2 network; 30-second retry spacing; manual disconnect pauses auto-join for this app session | App rejoined the test AP following a CLI disconnect; app must remain running |
| CPU offload | Firmware rate adaptation plus RSSI reports; optional pairwise hardware TX CCMP via Linux CAM/SEC formats | Both encryption paths passed interface-bound pings/HTTPS; offload defaults off, CPU savings not measured; RX MIC/replay remains software |
| TX pressure | Stalled output queues preserve mbufs and resume after descriptor completion | Build/descriptor contract checks; saturated-traffic test remains |
| Recovery | Auth/association 5-second deadlines, handshake 15-second deadline, BSS inactivity 30-second deadline | Host policy values; AP loss/rejection tests remain |
| Sleep / wake | IOKit PM shuts down MAC/DMA, erases keys, resets rings and reinitializes on wake; app can rejoin | Real sleep/wake testing remains; no WoWLAN |
| Bluetooth | Separate RTL8723B USB firmware kext, same app/resources/installer; HCI/ROM identity checked before upload; native macOS pairing/power | Actual firmware parser and fragment tests pass; USB upload/native attachment/audio/LE/wake tests remain |
| Wi-Fi + Bluetooth | Optional Linux single-antenna internal-switch PTA baseline; app option or older Bluetooth test boot flag | Full Linux dynamic SCO/HID/A2DP coexistence policy is not ported; simultaneous-load test required |
| Native Wi-Fi | Feasibility investigated; existing interface remains IOEthernetController | Requires a separate IO80211/AirPort adapter, private ABI/dependencies and OS-specific tests |

ADDBA requests are explicitly refused using the BSD response format. There is no
reorder queue yet; accepting an agreement would invalidate the receive replay
logic. Fragmented frames are explicitly dropped. No unsupported features are
advertised as enabled. There is no checksum/TSO/LRO capability advertisement:
the Linux RTL8723BE source does not justify an Ethernet segmentation/checksum
offload in this port.

## Radio options

Change these while disconnected in Settings, or use `rtl8723be_cli options MASK`.
The bitmask is 1 for HT20, 2 for hardware TX CCMP, and 4 for single-antenna
Bluetooth PTA. Default is 1, or 5 with `rtl8723be_bluetooth=1`. The app persists
its selected mask. `rtl8723be_cli link` reports negotiated capabilities and the
active offload path. The maximum descriptor rate is a capability, not measured
throughput or the firmware's instantaneous transmission rate.

## Native AirPort assessment

An app-free interface is technically possible through a real AirPort adapter;
changing the PCI identity cannot implement Apple's wireless control interface.
[AirportItlwm](https://openintelwireless.github.io/itlwm/FAQ) demonstrates the
IOEthernetController versus IO80211Family distinction.
[Realtek AirPort Family](https://github.com/xnoah222/Realtek-AirPort-Family) lists
RTL8822BE/CE and RTL8821CE, and explicitly excludes unlisted chipsets. Its current
modern OS path also requires legacy IO80211/IOSkywalk components. The RTL8723BE
is not one of its supported devices; no source from that project is copied here.

The next native integration work is an adapter that implements actual scan,
association, key, link and power callbacks for this driver's RTL8723BE hardware
backend, using pinned framework definitions for each supported macOS version.
Until that is built and tested, the app/CLI controls Wi-Fi and it appears as an
Ethernet interface. Apple continuity features are not promised.

## Upstream evidence

See [UPSTREAM.md](UPSTREAM.md) for functions and
[upstream-sources.json](upstream-sources.json) for reference hashes/revisions.
Hardware tests and build commands are in [TESTING.md](TESTING.md).
The current run's results and hardware blockers are in [VALIDATION.md](VALIDATION.md).
An unresolved internal-audio incident now blocks further radio tests on the test
host; see [AUDIO_INCIDENT.md](AUDIO_INCIDENT.md). Restore audio first.
