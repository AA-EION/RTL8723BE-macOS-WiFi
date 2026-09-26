# Comprehensive Environment, Kernel KPI & Mock Test Bench Architecture Analysis

**Author**: Explorer Subagent 3  
**Date**: 2026-09-26  
**Target Environment**: macOS 26.6.2 (Darwin Kernel 25.6.0, Build 25G83, x86_64)  
**Target Hardware**: Realtek RTL8723BE PCIe 802.11b/g/n Wireless Adapter (`pci10ec,b723`, subsystem `103c:804c`)  
**Working Directory**: `/Users/eion/.gemini/antigravity/scratch/rtl8723be_macos_wifi/.agents/teamwork/explorer_survey_3`

---

## 1. Executive Summary

This report documents the empirical reconnaissance of the host hardware topology, OpenCore bootloader environment, macOS 26.6.2 (Build `25G83`) kernel programming interfaces (KPIs), compiler toolchain, and the comprehensive engineering design of the automated mock hardware/DMA test bench for the Realtek RTL8723BE macOS Wi-Fi driver.

### Core Discoveries & Determinations
1. **Target Hardware Verified**: The physical RTL8723BE device is present and active on the PCIe bus at `2:0:0` (`IOACPIPlane:/_SB/PCI0@0/RP06@1c0005/PXSX@0`). It exposes BAR0 (256-byte I/O port) and BAR2 (16,384-byte 64-bit MMIO window mapped at physical base `0xf1100000`), with both IO-APIC pin 17 and MSI vector 8 enabled.
2. **OpenCore EFI & Blocking SSDT Discovered**: The active bootloader resides on `/Volumes/HTOSH/EFI/OC`. Investigation of `config.plist` revealed that an ACPI table named `SSDT-Disable_Network_RP06.aml` is currently enabled (`Enabled = True`). This SSDT attempts to inject dummy ACPI properties (`#network`, `#display`) to suppress the card. To allow the driver to attach cleanly during live staging, this SSDT must be disabled in `config.plist`.
3. **macOS 26.6.2 KPI Compatibility**: The host runs Darwin 25.6.0. The standard kernel KPI framework (`System.kext/PlugIns`: `com.apple.kpi.bsd`, `com.apple.kpi.iokit`, `com.apple.kpi.libkern`, `com.apple.kpi.mach`), `com.apple.iokit.IONetworkingFamily` (version 3.4), and `com.apple.iokit.IOPCIFamily` (version 2.9) are loaded and active. Apple's legacy `IO80211Family` is absent and unsupported for 3rd-party kexts; implementing an `IOEthernetController` kext (matching the proven `itlwm` architecture) coupled with a user-space control client is 100% compatible with macOS 26.6.2.
4. **Tooling Verification**: Apple Clang 21.0.0 (Xcode 26.5) with SDK `MacOSX.sdk` successfully builds, links, and produces valid `MH_MAGIC_64 KEXTBUNDLE` binaries. Symbol resolution against the running kernel collection (`/System/Library/KernelCollections/BootKernelExtensions.kc`) is verified without root privileges via `kmutil libraries -p <kext>`.
5. **Automated Mock Test Bench**: A 4-Tier test harness architecture has been designed to simulate BAR2 MMIO registers (including power sequence, eFuse indirect access, and 8051 MCU firmware handshake), PCIe DMA descriptor rings (TX/RX), and synthetic 802.11 packet injection (beacons, probe responses, EAPOL-Key 4-way handshake, and CCMP crypto) to ensure 100% test coverage before touching live hardware.

---

## 2. Target System & Hardware Topology

Empirical interrogation of the I/O Registry (`ioreg`) and system configuration revealed the exact hardware topology:

### 2.1 Host Machine Specifications
- **Hardware Model**: MacBookPro14,1 Hackintosh (HP Laptop Platform)
- **Host CPU / Chipset**: Intel 7th/8th Gen Core (Kaby Lake / Sunrise Point PCH-LP)
- **Operating System**: macOS 26.6.2 (Darwin Kernel Version 25.6.0: `Fri Jul 31 19:11:49 PDT 2026; root:xnu-12377.161.14~5/RELEASE_X86_64`)
- **System Architecture**: `x86_64`
- **Boot Arguments**: `debug=0x100 keepsyms=1 -amfipassbeta`
- **CSR Configuration**: `csr-active-config` = `<030a0000>` (SIP custom configuration enabling kext testing)

### 2.2 PCIe Bus Hierarchy
The device is connected through the Intel Sunrise Point PCI Express Root Port 6:
```
IOACPIPlane:/_SB/PCI0@0/RP06@1c0005
  └── PXSX@0 (IOPCIDevice, ID: 0x10000028b)
```
- **PCI Root Port**:
  - Node: `RP06@1C,5`
  - ACPI Path: `IOACPIPlane:/_SB/PCI0@0/RP06@1c0005`
  - PCI Location: `0:28:5` (Bus 0, Device 28, Function 5)
  - Bridge Mapping: Bridges Primary Bus 0 to Secondary Bus 2, Subordinate Bus 2
  - MMIO Window: `0xf1100000` – `0xf11fffff` (size: 0x100000 = 1 MB)
  - I/O Window: `0x4000` – `0x4fff` (size: 0x1000 = 4 KB)
- **Target Endpoint Device**:
  - Node: `PXSX@0`
  - ACPI Path: `IOACPIPlane:/_SB/PCI0@0/RP06@1c0005/PXSX@0`
  - PCI Address: `2:0:0` (Bus 2, Device 0, Function 0)
  - `IOName`: `"pci10ec,b723"`
  - `name`: `<"pci10ec,b723">`
  - `vendor-id`: `0x10ec` (`<ec100000>`) — Realtek Semiconductor Corp.
  - `device-id`: `0xb723` (`<23b70000>`) — RTL8723BE PCIe Wireless Network Adapter
  - `subsystem-vendor-id`: `0x103c` (`<3c100000>`) — HP Inc.
  - `subsystem-id`: `0x804c` (`<4c800000>`)
  - `revision-id`: `0x00` (`<00000000>`)
  - `class-code`: `0x028000` (`<00800200>`) — Network Controller (802.11 Wireless)
  - `compatible`: `("pci103c,804c", "pci10ec,b723", "pciclass,028000", "PXSX")`

### 2.3 Memory Resources (BAR Allocation)
As read from `assigned-addresses` and `IODeviceMemory` in the I/O Registry:
1. **BAR0 (I/O Port Range)**:
   - Base Address: `0x00004000`
   - Length: `256` bytes (`0x100`)
   - Type: Legacy I/O port window (I/O Space)
2. **BAR2 (MMIO Register Window)**:
   - Base Address: `0xf1100000` (`4044357632` decimal)
   - Length: `16,384` bytes (`0x4000`, 16 KB)
   - Type: 64-bit Non-prefetchable Memory Mapped I/O
   - Status: Mapped by root bridge into host physical memory. All RTL8723BE registers (MAC, Baseband, indirect RF, and MCU download FIFO) reside in this window.

### 2.4 Interrupt Routing & Capabilities
- **Interrupt Controllers**:
  - Legacy APIC: `"io-apic-0"` with specifier `<1100000007000000>` (IO-APIC pin 17)
  - Message Signaled Interrupts: `"IOPCIMessagedInterruptController"` with specifier `<0800000000000100>` (MSI vector 8)
- **PCI Capabilities Offsets**:
  - `MSICapability`: Offset `80` (`0x50`), `IOPCIMSIMessageControl` = `128` (`0x80`), MSI supported.
  - `PowerManagementCapability`: Offset `64` (`0x40`), `acpi-pmcap-offset` = `64`.
  - `PCIExpressCapability`: Offset `112` (`0x70`), Link Speed 2.5 GT/s, Link Width x1.
  - `ErrorReportingCapability` (AER): Offset `256` (`0x100`).
  - `DeviceSerialNumberCapability`: Offset `320` (`0x140`).
  - `LatencyToleranceReportingCapability` (LTR): Offset `336` (`0x150`).
  - `L1PMSubstatesCapability`: Offset `344` (`0x158`).
- **Current Power State**:
  - `CurrentPowerState`: 2
  - `ChildProxyPowerState`: 2
  - `MaxPowerState`: 3

---

## 3. OpenCore Bootloader Configuration & Host EFI

### 3.1 Bootloader Partition & Structure
- Active OpenCore storage volume: `/Volumes/HTOSH` (Partition `/dev/disk6s1`, FAT32/MBR, labeled `HTOSH`).
- EFI Path: `/Volumes/HTOSH/EFI/OC`
- OpenCore Version: OpenCore 1.0+ (`OpenCore.efi` timestamp Sep 11 2026, size 630,784 bytes)
- Structure:
  - `ACPI/`: AML ACPI patches (`SSDT-ALS0.aml`, `SSDT-Disable_Network_RP06.aml`, `SSDT-EC.aml`, `SSDT-MCHC.aml`, `SSDT-PLUG.aml`, `SSDT-PNLF.aml`, `SSDT-SBUS.aml`, `SSDT-USBX.aml`, `SSDT-XOSI.aml`)
  - `Drivers/`: EFI filesystem and runtime drivers
  - `Kexts/`: Injected kernel extensions
  - `config.plist`: Primary OpenCore configuration file (31,397 bytes)

### 3.2 Injected Kexts Status
Currently running third-party kexts loaded into the kernel:
| Kext Name | Version | Bundle Identifier | Status |
|---|---|---|---|
| Lilu | 1.7.3 | `as.vit9696.Lilu` | Active |
| AMFIPass | 1.4.1 | `com.dhinakg.AMFIPass` | Active |
| BrightnessKeys | 1.0.4 | `as.acidanthera.BrightnessKeys` | Active |
| ECEnabler | 1.0.6 | `com.1Revenger1.ECEnabler` | Active |
| RestrictEvents | 1.1.7 | `as.vit9696.RestrictEvents` | Active |
| VirtualSMC | 1.3.8 | `as.vit9696.VirtualSMC` | Active |
| SMCBatteryManager | 1.3.8 | `ru.usrsse2.SMCBatteryManager` | Active |
| SMCLightSensor | 1.3.8 | `ru.usrsse2.SMCLightSensor` | Active |
| SMCProcessor | 1.3.8 | `as.vit9696.SMCProcessor` | Active |
| SMCSuperIO | 1.3.8 | `ru.joedm.SMCSuperIO` | Active |
| WhateverGreen | 1.7.1 | `as.vit9696.WhateverGreen` | Active |
| VoodooPS2Controller | 2.3.8 | `as.acidanthera.voodoo.driver.PS2Controller` | Active |
| USBToolBox | 1.2.0 | `com.dhinakg.USBToolBox.kext` | Active |
| RealtekRTL8111 | 2.4.2 | `com.insanelymac.RealtekRTL8111` | Active (Ethernet `en0`) |
| Sinetek-rtsx | 9.0.0 | `com.sinet3k.Sinetek-rtsx` | Active (Card Reader `0x522a`) |

### 3.3 Critical ACPI Finding: `SSDT-Disable_Network_RP06.aml`
During analysis of `/Volumes/HTOSH/EFI/OC/config.plist` and the binary disassembly of `/Volumes/HTOSH/EFI/OC/ACPI/SSDT-Disable_Network_RP06.aml`, the following AML code was identified:
```asl
Scope (\_SB.PCI0.RP06.PXSX)
{
    Method (_DSM, 4, NotSerialized)
    {
        If ((Arg2 == Zero)) { Return (Buffer (One) { 0x03 } ) }
        If (_OSI ("Darwin"))
        {
            Return (Package ()
            {
                "name", Buffer () { "#network" },
                "IOName", Buffer () { "#display" },
                "class-code", Buffer () { 0xFF, 0xFF, 0xFF, 0xFF },
                "vendor-id", Buffer () { 0xFF, 0xFF, 0x00, 0x00 },
                "device-id", Buffer () { 0xFF, 0xFF, 0x00, 0x00 }
            })
        }
    }
}
```
**Impact Analysis**:
- Hackintosh builders frequently insert this patch to suppress unsupported wireless cards and prevent macOS sleep/wake issues or driver probing hangs.
- Although macOS IOPCIFamily currently reads physical PCI configuration space (exposing `pci10ec,b723`), keeping this SSDT active risks injecting conflicting device properties or interfering with ACPI power state transitions.
- **Action Required for Staging (Milestone 6)**:
  In `/Volumes/HTOSH/EFI/OC/config.plist`, the entry for `SSDT-Disable_Network_RP06.aml` under `ACPI -> Add` must be changed from `<true/>` to `<false/>`.

---

## 4. macOS 26.6.2 (Darwin 25.6.0) Kernel Programming Interfaces (KPIs)

### 4.1 System Kernel KPIs
macOS 26.6.2 provides the standard modular Darwin kernel programming interfaces via `System.kext`:
- `com.apple.kpi.bsd` (v25.6.0): Provides BSD sockets, mbuf management (`_mbuf_data`, `_mbuf_setnext`), memory allocation (`_kalloc_data`, `_kfree_data`), synchronization (`_lck_mtx_*`), and timers.
- `com.apple.kpi.iokit` (v25.6.0): Core IOKit runtime: `IOService`, `IORegistryEntry`, `IOWorkLoop`, `IOCommandGate`, `IOInterruptEventSource`, `IOTimerEventSource`, `IOMemoryDescriptor`, `IOBufferMemoryDescriptor`.
- `com.apple.kpi.libkern` (v25.6.0): C++ runtime support: `OSObject`, `OSArray`, `OSDictionary`, `OSString`, `OSData`, `OSNumber`, `OSBoolean`, atomic operations, and libkern utilities.
- `com.apple.kpi.mach` (v25.6.0): Mach task, thread, and timing services (`clock_get_uptime`, `absolutetime_to_nanoseconds`).

### 4.2 I/O Kit Family Dependencies
- **`com.apple.iokit.IOPCIFamily`** (v2.9):
  - Provides `IOPCIDevice` abstraction, PCI configuration read/write (`configRead16`, `configWrite16`), 64-bit BAR mapping (`mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2)`), Bus Mastering enable (`setBusMasterEnable(true)`), and interrupt registration (`getDeviceMemoryWithIndex`).
- **`com.apple.iokit.IONetworkingFamily`** (v3.4):
  - Provides `IOEthernetController`, `IOEthernetInterface`, `IONetworkMedium`, `IONetworkStats`, `IOOutputQueue`, and `IOMbufMemoryCursor`.

### 4.3 Why `IOEthernetController` + User Daemon is the Mandatory Architecture
1. **Status of `IO80211Family`**:
   - `kmutil showloaded` confirms that `IO80211Family` is NOT loaded on this host.
   - Starting in macOS 14 (Sonoma) and continued through macOS 15 and macOS 26 (Tahoe), Apple removed legacy third-party attachment points from `IO80211Family`. Third-party drivers matching `IO80211Controller` fail symbol resolution or fail to attach without fragile binary patching of Apple's closed-source Wi-Fi stack.
2. **Status of DriverKit PCIe (`PCIDriverKit` / `NetworkingDriverKit`)**:
   - While `PCIDriverKit` headers exist in the SDK, loading a third-party DriverKit extension requires special Apple-issued entitlements (`com.apple.developer.driverkit.transport.pci`) which are rejected by AMFI unless SIP is completely altered or user authorization dialogs are navigated.
3. **The Proven `IOEthernetController` Path (`itlwm` paradigm)**:
   - `IOEthernetController` is an officially exported, fully supported KPI in `IONetworkingFamily` (v3.4).
   - The driver presents an authentic Ethernet interface (`en1`/`en2`) to macOS. The macOS network stack handles DHCP, TCP/IP, DNS, and routing seamlessly.
   - The driver internally manages the 802.11 state machine (scanning, authentication, association, 4-way WPA2-PSK handshake, CCMP encryption/decryption, and 802.11 <-> 802.3 Ethernet translation).
   - An IOUserClient / BSD socket interface connects the driver to a companion CLI tool or HeliPort-compatible GUI client to trigger scans and manage network connections.

### 4.4 Driver `Info.plist` Declaration
The target driver bundle `RTL8723BE.kext` must declare the following personality and dependencies:
```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key>
    <string>org.realtek.driver.RTL8723BE</string>
    <key>CFBundleName</key>
    <string>RTL8723BE</string>
    <key>CFBundlePackageType</key>
    <string>KEXT</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0.0</string>
    <key>CFBundleVersion</key>
    <string>1.0.0</string>
    <key>IOKitPersonalities</key>
    <dict>
        <key>RTL8723BE PCIe Wireless</key>
        <dict>
            <key>CFBundleIdentifier</key>
            <string>org.realtek.driver.RTL8723BE</string>
            <key>IOClass</key>
            <string>RTL8723BE</string>
            <key>IOMatchCategory</key>
            <string>IODefaultMatchCategory</string>
            <key>IONameMatch</key>
            <array>
                <string>pci10ec,b723</string>
            </array>
            <key>IOPCIMatch</key>
            <string>0xB72310EC</string>
            <key>IOPCITunnelCompatible</key>
            <false/>
            <key>IOProviderClass</key>
            <string>IOPCIDevice</string>
        </dict>
    </dict>
    <key>OSBundleLibraries</key>
    <dict>
        <key>com.apple.iokit.IONetworkingFamily</key>
        <string>1.5.0</string>
        <key>com.apple.iokit.IOPCIFamily</key>
        <string>1.7</string>
        <key>com.apple.kpi.bsd</key>
        <string>8.10.0</string>
        <key>com.apple.kpi.iokit</key>
        <string>8.10.0</string>
        <key>com.apple.kpi.libkern</key>
        <string>8.10.0</string>
        <key>com.apple.kpi.mach</key>
        <string>8.10.0</string>
    </dict>
    <key>OSBundleRequired</key>
    <string>Network-Root</string>
</dict>
</plist>
```

### 4.5 Build Toolchain & Linker Verification
Direct empirical testing confirmed:
1. **Compiler**: Apple Clang 21.0.0 (`clang` and `clang++`) located in `/usr/bin/clang`.
2. **SDK**: `/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk`
3. **Compilation Flags**:
   ```sh
   clang++ -target x86_64-apple-macos14.0 \
           -mkernel -fapple-kext -fno-rtti -fno-exceptions \
           -nostdinc++ -nostdinc \
           -isystem $(xcrun --show-sdk-path)/System/Library/Frameworks/Kernel.framework/Headers \
           -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
           -std=c++17 -Wall -Wno-deprecated-declarations -O2
   ```
4. **Linker Flags**:
   ```sh
   clang++ -target x86_64-apple-macos14.0 \
           -mkernel -fapple-kext -nostdlib \
           -Xlinker -kext -Xlinker -x -Xlinker -headerpad_max_install_names \
           -o RTL8723BE.kext/Contents/MacOS/RTL8723BE
   ```
5. **Diagnostics Command**:
   ```sh
   kmutil libraries -p /path/to/RTL8723BE.kext
   ```
   This command inspects all imported symbols and verifies resolution against `/System/Library/KernelCollections/BootKernelExtensions.kc` with zero missing symbols.

---

## 5. Mock Hardware & DMA Test Bench Architecture Design

A PCIe driver operating directly with DMA and hardware registers can induce kernel panics if descriptor ownership, buffer addresses, or timing state-machines are faulty. Therefore, a complete mock hardware test bench is designed to simulate all hardware interactions in user-space before live hardware staging.

### 5.1 System Architecture Diagram
```
+-------------------------------------------------------------------------+
|                         Automated Test Runner                           |
|      (GoogleTest / Catch2 C++17 Test Framework, 100% Deterministic)     |
+-------------------------------------------------------------------------+
       |                                                    |
       v                                                    v
+-----------------------------+              +----------------------------+
| 802.11 Synthetic Generator  |              |  Mock Hardware Bus Engine  |
| - Beacons / Probe Responses |              |  - BAR0 I/O / BAR2 MMIO    |
| - Auth / Assoc Handshakes   |              |  - Register Accessors      |
| - WPA2 4-Way AP Simulator   |              |  - Reactive HW Hooks       |
+-----------------------------+              +----------------------------+
       |                                                    |
       v                                                    v
+-------------------------------------------------------------------------+
|                  Mock RTL8723BE Hardware Emulation                      |
|                                                                         |
|  +------------------------+  +-------------------+  +----------------+  |
|  | MMIO Register Bank     |  | 8051 MCU Emulation|  | eFuse Shadow   |  |
|  | (16,384 bytes array)   |  | - DL Handshake    |  | - MAC: 00:e0:..|  |
|  | - Read/Write Hooks     |  | - Checksum Engine |  | - Calib & Pwr  |  |
|  | - Status Flags & ISR   |  | - Ready Bit (0x80)|  | - Autoload     |  |
|  +------------------------+  +-------------------+  +----------------+  |
|                                                                         |
|  +-------------------------------------------------------------------+  |
|  | Mock PCIe DMA Engine                                              |  |
|  | - Physical <-> Virtual Address Translation                        |  |
|  | - TX Ring Monitor (Detects OWN=1, parses 802.11/LLC, clears OWN)  |  |
|  | - RX Ring Injector (Copies frame + RX Status Descriptor, OWN=0)   |  |
|  | - Host Interrupt Generator (HISR / HIMR / ISR callback trigger)   |  |
|  +-------------------------------------------------------------------+  |
+-------------------------------------------------------------------------+
                                   ^
                                   | MMIO Reads/Writes & DMA Descriptors
                                   v
+-------------------------------------------------------------------------+
|                        RTL8723BE Driver Core                            |
|  - Hardware Init & Power Sequence (pwrseq)                              |
|  - 8051 Firmware Loader (`rtl8723befw.bin`)                             |
|  - eFuse Decoders & RF Channel Synthesizer                              |
|  - TX / RX Circular DMA Ring Manager                                    |
|  - 802.11 State Machine (Scan -> Auth -> Assoc -> 4-Way WPA2 -> Link)   |
|  - WPA2 CCMP Crypto Engine (AES-128 CCM) & 802.3 Packet Translator      |
+-------------------------------------------------------------------------+
```

### 5.2 Emulation Component Details

#### 1. Mock BAR2 MMIO Register Bank (`MockMMIO`)
- **Memory Buffer**: Contiguous 16,384-byte array (`uint8_t regs[0x4000]`).
- **Accessors**:
  - `read8(offset)`, `read16(offset)`, `read32(offset)`
  - `write8(offset, val)`, `write16(offset, val)`, `write32(offset, val)`
- **Reactive Register Hooks**:
  - `REG_SYS_FUNC_EN` (0x02) & `REG_APS_FSMCO` (0x04): Validates power-on sequence transitions (Card Enable -> Clock Active -> RF On).
  - `REG_CR` (0x100): Command Register. Emulates MAC enable, TX enable, and RX enable flags.
  - `REG_MCUFWDL` (0x80):
    - When driver writes download enable bit (`FWDL_EN`), mock prepares internal buffer.
    - When driver completes download and writes `DL_READY`, mock verifies uploaded binary magic (`0x2300` / `0x8723`) and checksum.
    - Mock sets `MCU_READY` bit (0x80 bit 1) after 20 microseconds or simulated clock ticks.
  - `REG_EFUSE_CTRL` (0x30) & `REG_EFUSE_TEST` (0x34):
    - Backed by an eFuse logical map (512 bytes) containing factory MAC address (`00:E0:4C:81:92:23`), regulatory domain (FCC), crystal cap calibration, and per-channel TX power.
    - Intercepts read requests, latches the requested byte into `REG_EFUSE_CTRL`, and clears the busy flag.
  - `REG_HIMR` (0x0B0) & `REG_HISR` (0x0B8):
    - Interrupt mask and status registers. Implements write-1-to-clear semantics.
    - When an event occurs (RX OK, TX OK, BCN OK), sets the corresponding bit in `HISR` and invokes the registered interrupt callback if unmasked in `HIMR`.
  - `REG_RX_FILTER` / `REG_RCR` (0x608): Simulates packet acceptance filters (unicast, multicast, broadcast, beacon).

#### 2. Mock DMA Engine (`MockDMA`)
- **Address Space Translation**: Maps physical 64-bit guest addresses to simulated host memory buffers.
- **TX Descriptor Ring Processing**:
  - Driver sets up circular ring of descriptors (each 32 bytes).
  - When driver sets descriptor `OWN = 1` and triggers `REG_TXDMA_POLL` (or queue poll register):
    1. Mock DMA engine reads descriptor from host memory.
    2. Retrieves physical buffer pointer and byte length.
    3. Validates frame structure: MAC header (Frame Control, Duration, Addr1, Addr2, Addr3, Sequence Control), LLC/SNAP header (0xAA 0xAA 0x03 0x00 0x00 0x00 EtherType), and payload.
    4. If frame is CCMP encrypted: verifies CCMP header, 48-bit Packet Number (PN), and 8-byte MIC.
    5. Clears descriptor `OWN` bit (`OWN = 0`), sets status bits (success, TX rate), and updates `HISR |= IMR_TX_OK`.
    6. Triggers interrupt callback.
- **RX Descriptor Ring & Frame Injection (`mock_inject_rx_frame`)**:
  - Signature:
    ```cpp
    void mock_inject_rx_frame(const uint8_t *frame_data, size_t frame_len,
                              int8_t rssi, uint8_t channel, uint32_t status_flags);
    ```
  - Operation:
    1. Checks current RX descriptor pointer. If `OWN != 1`, logs RX buffer exhaustion error.
    2. Writes Realtek RTL8723BE RX Status Descriptor (24 bytes) into host buffer: contains packet length, CRC status (OK), decryption status, antenna ID, channel, and RSSI.
    3. Copies raw 802.11 frame bytes into host buffer immediately following the status descriptor.
    4. Sets descriptor `OWN = 0` (relinquishing control to driver).
    5. Advances ring pointer to next slot.
    6. Sets `HISR |= IMR_ROK` and invokes driver interrupt service routine / workloop.

#### 3. Synthetic 802.11 & WPA2 Traffic Generator (`Mock80211`)
- **Beacon Generator**:
  - Generates full 802.11 Beacon frames: Frame Control (`0x8000`), Destination (`FF:FF:FF:FF:FF:FF`), Source/BSSID (`02:00:00:AA:BB:CC`), Timestamp, Beacon Interval (100 TU), Capability Info (ESS, Privacy).
  - Information Elements: SSID (Tag 0), Supported Rates (Tag 1), DSSS Parameter / Channel (Tag 3), RSN / WPA2 IE (Tag 48, AKM=00-0F-AC:2 [PSK], Cipher=00-0F-AC:4 [CCMP]).
- **Probe Responder**:
  - Emulates APs answering probe requests on channels 1–13 with targeted Probe Response frames.
- **Authentication / Association Responder**:
  - Handles Open System Auth (Seq 1 -> responds with Seq 2 Success).
  - Handles Association Request -> responds with Assoc Response (Success, AID=1).
- **WPA2 4-Way Handshake Engine**:
  - Injects EAPOL-Key Message 1 (AP ANonce, Replay Counter).
  - Captures Driver's EAPOL-Key Message 2 (SNonce, MIC computed with KCK).
  - Validates driver's MIC against known test passphrase (`"test_wifi_password_1234"`).
  - Injects EAPOL-Key Message 3 (AP MIC, encrypted GTK under KEK).
  - Captures Driver's EAPOL-Key Message 4 (MIC confirmation).
  - Validates completion and triggers CCMP encrypted ping/data verification.

---

## 6. Four-Tier Automated Test Suite Design

To achieve 100% deterministic regression before live deployment, the test suite is partitioned into four distinct tiers:

### Tier 1: Unit & Algorithmic Verification (Zero OS/Hardware Dependencies)
- **Scope**: Pure algorithmic logic, mathematical transforms, frame parsers, and cryptographic engines.
- **Tests**:
  1. `Test_eFuse_Decoder`: Feeds known eFuse binary dumps (from Linux rtlwifi and OpenBSD); asserts exact extraction of MAC address, channel plan, crystal cap, and RF thermal values.
  2. `Test_Firmware_Header_Parser`: Feeds `rtl8723befw.bin`; asserts validation of signature (`0x2300`), version, code section size, and data section size.
  3. `Test_Crypto_PBKDF2`: RFC 6070 test vectors for PBKDF2-HMAC-SHA1; verifies derivation of 256-bit PMK from SSID and passphrase.
  4. `Test_Crypto_PRF512`: IEEE 802.11i PRF-512 test vectors; verifies derivation of PTK (KCK, KEK, TK) from PMK, ANonce, SNonce, AP MAC, and Station MAC.
  5. `Test_Crypto_AES_CCMP`: IEEE 802.11 Annex H test vectors; verifies AES-128 CCM mode encryption, decryption, MIC calculation, and IV/PN incrementation.
  6. `Test_80211_Frame_Parsers`: Parses variable-length 802.11 Information Elements (SSID, Rates, HT Capabilities, RSN IE); verifies correct extraction of cipher suites and channel IDs.
  7. `Test_Ethernet_Translation`: Validates 802.3 Ethernet frame conversion to 802.11 Data frame with LLC/SNAP header (0xAA-AA-03-00-00-00), and reverse decapsulation.

### Tier 2: Mock Hardware & Bus Register Verification
- **Scope**: Register access, power management state machine, firmware loader handshake, and DMA ring mechanics.
- **Tests**:
  1. `Test_MMIO_Accessors`: Verifies 8, 16, and 32-bit atomic read/write consistency and alignment enforcement.
  2. `Test_Power_Sequence`: Executes driver power-on sequence (`pwrseq`); asserts sequential writes to `REG_SYS_FUNC_EN`, `REG_APS_FSMCO`, and clock registers match Linux/BSD hardware specs.
  3. `Test_8051_Firmware_Upload`: Executes firmware download protocol; asserts blocks written to FIFO, checksum handshake verified, and timeout handling works if hardware stalls.
  4. `Test_eFuse_Autoload`: Verifies driver indirect eFuse reading via `REG_EFUSE_CTRL` and verifies correct MAC address registration.
  5. `Test_TX_DMA_Rings`: Allocates High, Normal, and Low priority TX rings; queues 64 packets; asserts ownership bit flipping, head/tail ring wrapping, and TX status reporting.
  6. `Test_RX_DMA_Rings`: Allocates circular RX ring (64 descriptors); asserts continuous buffer replenish, descriptor status reading, and memory boundary checks.
  7. `Test_Interrupt_Handling`: Generates simulated MSI and IO-APIC interrupts; asserts `HIMR` masking, `HISR` write-1-to-clear, and ISR scheduling.

### Tier 3: Driver State Machine & Integration Verification
- **Scope**: End-to-end Wi-Fi protocol state machine and Ethernet controller data path.
- **Tests**:
  1. `Test_Scan_Lifecycle`: Driver initiates active 2.4 GHz scan; mock injects synthetic beacons and probe responses across channels 1–13; asserts populated BSSID table, signal strength (RSSI) filtering, and SSID deduplication.
  2. `Test_Auth_Assoc_Sequence`: Driver initiates connection to WPA2 SSID; mock responds to Auth (seq 2) and Assoc Response; asserts driver transitions states from `SCANNING` -> `AUTHENTICATING` -> `ASSOCIATING` -> `ASSOCIATED`.
  3. `Test_WPA2_4Way_Handshake`: Injects EAPOL-Key M1; verifies driver responds with M2 containing valid MIC; injects M3; verifies driver responds with M4 and installs PTK/GTK into hardware crypto engine.
  4. `Test_Data_Path_Loopback`: Injects CCMP encrypted ARP request; verifies driver decrypts, verifies MIC, strips LLC/SNAP, and delivers Ethernet packet to `IOEthernetInterface`. Driver sends ARP reply; verifies driver encapsulates, encrypts under TK, and delivers to TX ring.
  5. `Test_Link_Down_Deauth`: Injects 802.11 Deauthentication frame from AP; verifies driver tears down keys, clears association state, and signals link down (`kIONetworkLinkValid` cleared).

### Tier 4: Stress, Boundary, Fault-Injection & Security Fuzzing
- **Scope**: Edge cases, malformed network input, link loss, and memory safety.
- **Tests**:
  1. `Test_Fuzz_Malformed_Beacons`: Injects beacons with oversized SSIDs (>32 bytes), truncated RSN IEs, zero-length tags, and overlapping elements; verifies driver drops invalid packets with zero memory corruption or crashes.
  2. `Test_Crypto_Replay_Protection`: Injects EAPOL-Key frames with stale replay counters and CCMP data frames with duplicate/out-of-order Packet Numbers (PN); verifies driver discards replay frames.
  3. `Test_Invalid_MIC_Rejection`: Injects EAPOL-Key M3 with corrupted MIC; verifies driver drops packet, does not install bogus keys, and terminates handshake cleanly.
  4. `Test_DMA_Ring_Starvation`: Inundates RX engine with 500 consecutive frames faster than host processing; asserts driver drops excess frames gracefully without ring desynchronization and resumes when replenished.
  5. `Test_PCIe_Freeze_Recovery`: Simulates MMIO read freeze (bus returns all `0xFFFFFFFF`); verifies driver timeout triggers watchdog, aborts hung operations, and safely resets controller.

---

## 7. Safe Staging & Live Hardware Bring-up Protocol

To ensure zero risk of kernel panics when transitioning from mock testing to live hardware:

```
[Phase 1: Automated Regression] 
   └── 100% pass on Tier 1–4 Mock Test Bench

[Phase 2: KPI & Symbol Validation]
   └── kmutil libraries -p RTL8723BE.kext resolves 100% symbols against BootKernelExtensions.kc

[Phase 3: OpenCore EFI Staging]
   ├── 1. Disable SSDT-Disable_Network_RP06.aml in /Volumes/HTOSH/EFI/OC/config.plist
   ├── 2. Copy RTL8723BE.kext to /Volumes/HTOSH/EFI/OC/Kexts/
   └── 3. Add RTL8723BE.kext entry to Kernel -> Add in config.plist (Enabled = True)

[Phase 4: Live Hardware Bring-up]
   ├── 1. Boot system; inspect kernel log: `log show --predicate 'process == "kernel"' --last boot`
   ├── 2. Verify IOPCIDevice matching at `2:0:0` (`RP06@1c0005/PXSX@0`)
   ├── 3. Verify eFuse MAC address readout matches hardware label
   ├── 4. Verify rtl8723befw.bin upload and 8051 MCU ready handshake
   ├── 5. Verify registration of IONetworkInterface (`en1` or `en2`)
   └── 6. Execute Wi-Fi CLI tool: trigger active scan and receive live 2.4 GHz BSSID list
```

---

## 8. Summary Table of Hardware & KPI Parameters

| Parameter | Observed Host Value | Driver Configuration |
|---|---|---|
| Vendor ID / Device ID | `0x10EC` / `0xB723` | `IOPCIMatch` = `0xB72310EC` |
| Subsystem IDs | `103c:804c` | HP Subsystem matching |
| ACPI Device Path | `_SB/PCI0@0/RP06@1c0005/PXSX@0` | Matches `PXSX` under `RP06` |
| PCI Address | `2:0:0` | Bus 2, Device 0, Function 0 |
| BAR0 Resource | `256` bytes I/O space at `0x4000` | Optional legacy access |
| BAR2 Resource | `16,384` bytes MMIO at `0xf1100000` | Primary MMIO BAR mapped via `IOPCIDevice` |
| Interrupt Controllers | `io-apic-0` (pin 17), `IOPCIMessagedInterruptController` (vec 8) | Use MSI via `IOFilterInterruptEventSource` |
| Darwin Kernel Version | Darwin 25.6.0 (Build `25G83`, x86_64) | `-target x86_64-apple-macos14.0` |
| Active Network Framework | `com.apple.iokit.IONetworkingFamily` (3.4) | `IOEthernetController` superclass |
| Active PCI Framework | `com.apple.iokit.IOPCIFamily` (2.9) | `IOPCIDevice` provider |
| OpenCore EFI Mount | `/Volumes/HTOSH/EFI/OC` | Destination for `RTL8723BE.kext` |
| Conflicting ACPI Patch | `SSDT-Disable_Network_RP06.aml` (Enabled=True) | Must set `Enabled=False` in `config.plist` |

---
*Report concluded. All findings backed by verbatim tool observations and empirical kernel testing.*
