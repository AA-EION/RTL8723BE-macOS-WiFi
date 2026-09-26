# Context: Realtek RTL8723BE macOS Wi-Fi Driver

## Host System Information
- OS: macOS 26.6.2 (Darwin kernel, Build `25G83`)
- Architecture: x86_64
- Bootloader: OpenCore with Lilu 1.7.3, AMFIPass 1.4.1, RestrictEvents 1.1.7, VirtualSMC 1.3.8, RealtekRTL8111 2.4.2

## Target Hardware
- Device: Realtek RTL8723BE 802.11b/g/n PCIe Wireless Network Adapter
- Vendor ID: `0x10EC`
- Device ID: `0xB723`
- Subsystem Vendor ID: `0x103C`
- Subsystem ID: `0x804C`
- PCI Address: `2:0:0` (Bus 2, Device 0, Function 0)
- ACPI Path: `IOACPIPlane:/_SB/PCI0@0/RP06@1c0005/PXSX@0`
- Resources:
  - BAR0: I/O port (256 bytes)
  - BAR2: 64-bit MMIO (16384 bytes, base `0xf1100000`)
  - Interrupt: MSI (`IOPCIMessagedInterruptController`) / IO-APIC support

## Reference Implementations & Assets
- Linux kernel: `drivers/net/wireless/realtek/rtlwifi/rtl8723be/`, `rtl_pci.c`, `btcoexist/`
- OpenBSD: `sys/dev/pci/if_rtwn.c`, `sys/dev/ic/rtwn.c`
- macOS prior art: `itlwm` (Intel Wi-Fi on macOS via IOEthernetController + HeliPort), `AirportItlwm`, `RealtekRTL8111`
- Firmware: `rtl8723befw.bin` (Realtek 8051 MCU microcode)

## Key Technical Decisions
- macOS 26.6.2 (25G83) Kernel Programming Interface (KPI):
  - In modern macOS (macOS 11+ through macOS 15/26), `IO80211Family` has internal private KPIs and Apple has deprecated third-party 802.11 family kext attachments.
  - The standard, robust community architecture (proven by `itlwm`) is to present an `IOEthernetController` (`IONetworkInterface`) to the macOS networking subsystem and handle 802.11 scan/auth/assoc/WPA2 management internally with a user-space control daemon or HeliPort-compatible IPC.
  - We will verify this architecture in Survey Phase and document the exact KPI bindings.
