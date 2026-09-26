# Comprehensive Architecture Survey & Prior Art Analysis: Realtek RTL8723BE Wi-Fi Driver for macOS 26.6.2 (x86_64)

**Document Version**: 1.0.0  
**Author**: Explorer Survey 1 (`explorer_survey_1`)  
**Date**: 2026-09-26  
**Target Hardware**: Realtek RTL8723BE 802.11b/g/n PCIe Wireless Network Adapter (`pci10ec,b723`, subsystem `103c:804c`, PCI `2:0:0`, BAR2 MMIO `0xf1100000`)  
**Target OS**: macOS 26.6.2 (Darwin 25.6.0, Kernel Build `25G83`, x86_64)

---

## 1. Executive Summary & Hardware Context

### 1.1 The Challenge
The Realtek RTL8723BE is a widely deployed PCIe 802.11b/g/n (1T1R, 2.4 GHz) wireless network adapter commonly paired with Realtek Bluetooth on laptop motherboards (notably HP, Lenovo, and Dell laptops). On macOS, third-party Wi-Fi drivers have historically faced substantial technical hurdles due to Apple's closed, proprietary Wi-Fi architecture.

On the target system:
- **Device Identity**: `pci10ec,b723` (Vendor `0x10EC`, Device `0xB723`, Subsystem Vendor `0x103C`, Subsystem `0x804C`).
- **Location**: PCI `2:0:0`, ACPI path `IOACPIPlane:/_SB/PCI0@0/RP06@1c0005/PXSX@0`.
- **Memory & Interrupt Resources**:
  - BAR0: I/O port (256 bytes).
  - BAR2: 64-bit MMIO (16,384 bytes at `0xf1100000`).
  - Interrupts: MSI (`IOPCIMessagedInterruptController`) and IO-APIC (`io-apic-0`).
- **OS Environment**: macOS 26.6.2 (Darwin `25.6.0`, Kernel `xnu-12377.161.14~5/RELEASE_X86_64`).
- **Bootloader & Injected Infrastructure**: OpenCore with Lilu 1.7.3, AMFIPass 1.4.1, RestrictEvents 1.1.7, VirtualSMC 1.3.8, RealtekRTL8111 2.4.2.

Currently, the device node `pci10ec,b723` exists in the IORegistry with class `pciclass,028000` (Network Controller), but has zero attached driver instances.

### 1.2 Core Assessment
1. **No Existing Ready-to-Use Solution**: There is no existing functional macOS driver for Realtek RTL8723BE PCIe in the Hackintosh community or vendor repositories. Historical community advice has uniformly been to replace the internal PCIe card or use a USB dongle.
2. **Architecture Winner**: The **`IOEthernetController` kext + user-space companion daemon/client** (the architectural model pioneered by OpenIntelWireless's `itlwm` + `HeliPort`) is unequivocally the only stable, maintainable, and viable architecture on macOS 26.6.2.
3. **DriverKit & IO80211Family Inviability**:
   - `NetworkingDriverKit` does not support 802.11 Wi-Fi (Apple restricts DriverKit WLAN to private, undocumented, internal `IOSkywalkFamily` interfaces). Furthermore, DriverKit dexts cannot be injected from OpenCore EFI.
   - `IO80211Family` shimming (e.g. `AirportItlwm`) is fundamentally broken on modern macOS (macOS 14 Sonoma through macOS 26) because Apple excised legacy 802.11 kexts from the kernel collections, heavily refactored private Apple80211 KPIs, and enforced strict entitlement checks.

---

## 2. Comprehensive Survey of macOS Wi-Fi Architectures & Prior Art

### 2.1 Intel Wi-Fi: `itlwm` vs `AirportItlwm`

The OpenIntelWireless project created two distinct drivers for Intel PCIe/CNVi Wi-Fi cards:

| Feature / Metric | `itlwm` | `AirportItlwm` |
| :--- | :--- | :--- |
| **Kernel Base Class** | `IOEthernetController` (`IONetworkingFamily`) | `IO80211Controller` / `IO80211Family` shim |
| **Presented Interface** | Virtual Ethernet (`enX`, e.g., `en1`) | Native AirPort (`enX` with AirPort capabilities) |
| **System UI Integration** | Custom user-space UI (HeliPort / CLI) | Native macOS Wi-Fi menu bar & System Settings |
| **Dependency Stability** | **Public, stable KPIs** (unchanged since OS X 10.0) | **Private, unstable KPIs** (rewritten/excised in macOS 14+) |
| **macOS 14+ / 26 Compatibility** | **100% Native compatibility** without OS patches | **Broken**; requires OCLP root patches & kernel downgrades |
| **Root Patching / SIP** | **Zero root patches**; works with full SIP/AMFI | Requires blocking `IOSkywalkFamily` & disabling AMFI/SIP |
| **OpenCore EFI Injection** | Drop-in `.kext` injection via `config.plist` | Complex multi-kext injection + blocker rules |
| **Failure Modes** | Network drop handled gracefully in user space | Host kernel panics on sleep/wake or vtable mismatch |

#### Why `AirportItlwm` Failed on Modern macOS
In macOS Sonoma (14.0), Sequoia (15.0), and macOS 26, Apple initiated a massive purge of legacy wireless drivers.
1. Apple transitioned its internal wireless stack to `IOSkywalkFamily` and user-space DriverKit daemons (`com.apple.DriverKit-AppleBCMWLAN.dext`).
2. Legacy `IO80211Family` plugins (such as `AirPortAtheros40`, `AirPortBrcm4360`, and classic Broadcom NIC drivers) were removed from `BootKernelExtensions.kc` and `SystemKernelExtensions.kc`.
3. Private Apple80211 KPIs were altered or stripped of external compatibility symbols.
4. `airportd` introduced mandatory entitlement enforcement for `IO80211APIUserClient` connections, rejecting third-party drivers without private Apple entitlements.
To keep `AirportItlwm` running on modern macOS, users are forced to inject legacy Ventura versions of `IO80211Family.kext`, block macOS's native `IOSkywalkFamily.kext` in OpenCore, disable System Integrity Protection (`csr-active-config`), pass `amfi_get_out_of_my_way=1`, and run OpenCore Legacy Patcher (OCLP) root patches. This fragile house of cards collapses with minor OS point updates and induces kernel panics.

In contrast, `itlwm` subclasses `IOEthernetController`. To macOS, it is simply a standard Gigabit/Fast Ethernet adapter. The host kernel's TCP/IP stack (`bsd`), DHCP client (`bootpd`), DNS resolver, and routing tables operate seamlessly over the Ethernet abstraction.

### 2.2 Realtek Community Drivers & Prior Art

#### 2.2.1 `chris1111/Wireless-USB-Adapter`
- **Scope**: Supports Realtek **USB** Wi-Fi dongles (e.g., RTL8812AU, RTL8811AU, RTL8192EU, RTL8814AU).
- **Core Technology**: Packages the closed-source Realtek vendor macOS USB driver (`RtWlanU.kext` / `RtWlanU1827.kext`).
- **Limitation**:
  - Exclusively targets USB endpoints; contains zero support for PCIe MMIO or PCIe DMA rings.
  - Relies on an ancient proprietary GUI (`Wireless Network Utility.app`).
  - Requires disabling SIP and Gatekeeper.
  - Not applicable to the internal PCIe `pci10ec,b723`.

#### 2.2.2 Legacy Realtek PCIe Attempts & BSD Ports
- **OpenBSD `rtwn(4)`**:
  - The OpenBSD `rtwn` driver supports Realtek 802.11b/g/n wireless devices, including USB (`urtwn`) and certain PCIe models (`rtwn_pci`).
  - **Crucial Finding**: As verified through OpenBSD source tree inspection and documentation, OpenBSD `rtwn` supports RTL8188CE, RTL8188EE, RTL8192CE, and RTL8723AE. **It does NOT support RTL8723BE**.
  - The "AE" and "BE" revisions possess incompatible MAC register mappings, different firmware handshake sequences, and distinct TX/RX descriptor layouts.
- **FreeBSD `rtwn`**:
  - Directly ported from OpenBSD; shares the same lack of RTL8723BE support.
- **Linux Mainline `rtlwifi/rtl8723be`**:
  - The mainline Linux kernel (`drivers/net/wireless/realtek/rtlwifi/rtl8723be/`) contains the only complete, mature, and production-tested open-source driver for RTL8723BE PCIe.
  - It handles all hardware idiosyncrasies:
    - 8051 MCU firmware upload and boot verification (`rtl8723befw.bin`).
    - eFuse / EEPROM reading, MAC address extraction, RF calibration values.
    - Radio frequency (RF), baseband (BB), and MAC register initialization tables.
    - TX/RX ring buffer descriptor management and MSI interrupt processing.
    - Dynamic Mechanism (`dm.c`), Dig (dynamic initial gain), dynamic TX power, and antenna diversity (`ant_sel=1` vs `ant_sel=2`).

### 2.3 DriverKit vs Kernel Extension (Kext) on macOS 26.6.2

Apple introduced DriverKit in macOS Catalina (10.15) to transition hardware drivers out of kernel space. We conducted a deep inspection of the macOS 26.6.2 / Xcode SDK (`DriverKit.platform/Developer/SDKs/DriverKit.sdk`) to evaluate DriverKit viability for this project:

| Criterion | DriverKit Extension (`.dext`) | Kernel Extension (`.kext`) |
| :--- | :--- | :--- |
| **802.11 Wi-Fi Family Support** | **None (Private/Restricted)**. Only `NetworkingDriverKit` (`IOUserNetworkEthernet`) is public. Apple keeps `IOUserNetworkWLAN` internal to `IOSkywalkFamily`. | **Full support** via standard `IOEthernetController` (`IONetworkingFamily`). |
| **OpenCore EFI Injection** | **Impossible**. Dexts are user-space daemons launched by `sysextd` and cannot be injected at boot time by OpenCore. | **Native**. Injected seamlessly into the kernel cache by OpenCore from `EFI/OC/Kexts`. |
| **Entitlements & Signing** | Requires restricted Apple capabilities (`com.apple.developer.driverkit.transport.pci`, etc.) or root developer mode. | Standard IOKit kext; loads without special Apple signing when staged via OpenCore EFI. |
| **Hardware MMIO & DMA** | Mediated through `IOBufferMemoryDescriptor` and user-space IPC; adds latency for packet descriptors. | Direct physical memory mapping and zero-copy kernel DMA buffer allocation (`IOMbufMemoryCursor`). |
| **Kernel Crash Resilience** | High (dext crashes as user-space process). | Moderate (requires robust driver defensive programming to avoid kernel panic). |
| **Deployment Complexity** | High (requires macOS `.app` bundle, `OSSystemExtensionManager`, user approval in System Settings). | Low (single `.kext` directory in OpenCore EFI). |

**Conclusion**: DriverKit is unsuitable and unviable for RTL8723BE due to lack of public Wi-Fi classes and OpenCore EFI injection incompatibility. A standard Kernel Extension (`IOEthernetController` kext) is the necessary and optimal implementation path.

---

## 3. The IOEthernetController + User-Space Daemon Paradigm

### 3.1 Resolving the Wi-Fi Abstraction Dilemma

macOS networking is partitioned into:
1. **Physical / Link Layer**: Managed by IOKit drivers (`IONetworkingFamily`).
2. **BSD Network Layer**: Managed by XNU BSD (`ifnet`, `mbuf`, routing, ARP, PF).
3. **Apple Wi-Fi Subsystem**: CoreWLAN framework (`CoreWLAN.framework`), `airportd` daemon, `WiFiAgent`, `IO80211Family.kext`, `IOSkywalkFamily.kext`.

When a driver attempts to register as an Apple AirPort device (`IO80211Family`), it enters Apple's private domain. It must implement hundreds of undocumented Apple80211 ioctls, conform to private vtables, and undergo entitlement validation by `airportd`. When Apple refactors its Wi-Fi stack (as occurred in macOS 14/15/26 with Skywalk integration), the third-party driver breaks completely.

By contrast, when a driver subclasses **`IOEthernetController`**:
- The driver registers an `IOEthernetInterface` (which instantiates a standard BSD `ifnet` interface `enX`).
- macOS treats the device as a standard IEEE 802.3 Ethernet adapter.
- The BSD network stack handles IP configuration via DHCP (`bootpd`), IPv6 SLAAC, DNS configuration, and packet routing out of the box.
- The 802.11 layer (scanning, association, authentication, encryption) is entirely decoupled from macOS's internal Wi-Fi stack.

### 3.2 Network Interface Mechanics in IOEthernetController

To implement an `IOEthernetController`, the driver adheres to the following contract defined in `IOEthernetController.h` and `IONetworkController.h`:

1. **Instantiation and Matching**:
   - `init(OSDictionary *dict)`: Initializes instance variables, locks, and state machines.
   - `start(IOService *provider)`:
     - Attaches to `IOPCIDevice`.
     - Maps BAR2 MMIO registers via `provider->mapDeviceMemoryWithIndex(2)`.
     - Reads permanent station MAC address from RTL8723BE eFuse.
     - Creates the workloop and interrupt event source (`IOInterruptEventSource` or `IOFilterInterruptEventSource`).
     - Publishes the medium dictionary (`publishMediumDictionary()`).
     - Calls `attachInterface(&fEthernetInterface)` to publish the `IOEthernetInterface` to the system.
     - Registers the service (`registerService()`).
2. **Medium Dictionary & Link State**:
   - The driver creates medium entries representing network speeds (e.g., 100 Mbps / 150 Mbps 802.11n):
     ```cpp
     IONetworkMedium *medium = IONetworkMedium::medium(
         kIOMediumEthernetAuto, 150 * 1000000, 0, 0, "Auto"
     );
     IONetworkMedium::addMedium(mediumDict, medium);
     setMediumDictionary(mediumDict);
     setSelectedMedium(medium);
     ```
   - Link State Signaling:
     - Disconnected: `setLinkStatus(0, 0)` (notifies BSD that the link is down; DHCP pauses).
     - Connected (WPA2 handshake complete): `setLinkStatus(kIONetworkLinkActive | kIONetworkLinkValid, getSelectedMedium())` (triggers instant DHCP request from macOS).
3. **Packet Transmission (`outputPacket`)**:
   - The BSD network stack calls `outputPacket(mbuf_t m, void *param)` with an `mbuf` chain containing an Ethernet II frame.
   - The driver inspects the Ethernet header, wraps the payload in an 802.11 QoS Data frame, encapsulates the original EtherType in an LLC/SNAP header, applies CCMP encryption, maps physical memory fragments, queues to the RTL8723BE TX DMA ring, and notifies the hardware.
4. **Packet Reception (`inputPacket`)**:
   - Upon RX interrupt, the driver decodes descriptors from the RX DMA ring.
   - 802.11 Data frames are validated (CCMP MIC verification), stripped of 802.11/LLC/SNAP headers, reconstructed into Ethernet II frames in a freshly allocated `mbuf_t`, and handed to BSD via:
     ```cpp
     fEthernetInterface->inputPacket(m, packetLength, 0);
     ```

---

## 4. Architecture Recommendation for RTL8723BE on macOS 26.6.2

The recommended architecture consists of three cleanly decoupled layers:
1. **Kernel Driver**: `RTL8723BE.kext` (Hardware control, PCIe DMA, 802.11 translation, state machine).
2. **Control Plane / IPC**: Custom `IOUserClient` (`RTL8723BEUserClient`) providing structured, typed control methods.
3. **User-Space Companion**: `rtl8723be_cli` (standalone CLI tool) and HeliPort integration (menu-bar GUI).

```
+---------------------------------------------------------------------------------+
|                                USER SPACE                                       |
|                                                                                 |
|   +-----------------------+              +----------------------------------+   |
|   |   HeliPort Menu App   |              |  rtl8723be_cli (Diagnostic Tool) |   |
|   +-----------+-----------+              +-----------------+----------------+   |
|               |                                            |                    |
|               +--------------------+  +--------------------+                    |
|                                    |  |                                         |
|                                    v  v                                         |
|                        [ IOKit / IOUserClient IPC ]                             |
+-------------------------------------+-------------------------------------------+
|                                     |                                           |
|                                     v                                           |
|                            RTL8723BEUserClient                                  |
|                                     |                                           |
| +-----------------------------------+-----------------------------------------+ |
| |                                   v                                         | |
| |                            RTL8723BEController                              | |
| |                       (subclass of IOEthernetController)                    | |
| |                                                                             | |
| |   +------------------------------------+  +-------------------------------+ | |
| |   |         802.11 State Machine       |  |       Ethernet <-> 802.11     | | |
| |   |  - Passive / Active Scanning       |  |          Data Translator      | | |
| |   |  - Auth / Assoc State Machine      |  |  - LLC/SNAP Encapsulation     | | |
| |   |  - WPA2 4-Way Handshake Engine     |  |  - CCMP IV / MIC (AES-128)    | | |
| |   |  - Beacon / Probe Parser Cache     |  |  - Ethernet II Reconstruction | | |
| |   +-----------------+------------------+  +---------------+---------------+ | |
| |                     |                                     |                 | |
| |                     +------------------+  +---------------+                 | |
| |                                        |  |                                 | |
| |                                        v  v                                 | |
| |                           +----------------------------+                    | |
| |                           |    RTL8723BE Hardware Core |                    | |
| |                           |  - PCIe MMIO BAR2 Mapping  |                    | |
| |                           |  - eFuse & Firmware Loader |                    | |
| |                           |  - MAC / BB / RF Tuning    |                    | |
| |                           |  - TX / RX DMA Ring Engine |                    | |
| |                           |  - Antenna Diversity Logic |                    | |
| |                           +--------------+-------------+                    | |
| |                                          |                                  | |
| +------------------------------------------+----------------------------------+ |
|                                            |                                    |
|                                            v                                    |
|                              pci10ec,b723 (Hardware)                            |
+---------------------------------------------------------------------------------+
```

### 4.1 Component Responsibilities

#### Kernel Extension (`RTL8723BE.kext`)
1. **PCIe Lifecycle & Power**: Attach to `IOPCIDevice`, configure PCI command registers (bus mastering, memory write & invalidate), map BAR2 MMIO, register MSI interrupt.
2. **Hardware Initialization**: Execute power-on sequence (`pwrseq`), parse eFuse/EEPROM (MAC address, crystal calibration), upload `rtl8723befw.bin` to 8051 MCU, initialize MAC/BB/RF register tables.
3. **Antenna Diversity**: For HP subsystem `103c:804c`, default or dynamically switch to Antenna 2 (Auxiliary) to resolve the notorious RTL8723BE single-antenna weak-signal flaw.
4. **DMA Ring Management**: Allocate contiguous DMA rings for TX (High, Normal, Low priority queues) and RX, handle descriptor wrap-around, sync DMA memory barriers.
5. **802.11 Engine**: Active channel dwelling (Channels 1–13), Beacon/Probe Response extraction, 802.11 Authentication, 802.11 Association.
6. **WPA2 4-Way Handshake**: In-kernel EAPOL-Key parsing and message dispatch (Message 1–4), SNonce generation, PTK calculation (PRF-512), MIC validation, key installation.
7. **Frame Conversion**: LLC/SNAP header encapsulation for outbound Ethernet frames; stripping LLC/SNAP and regenerating Ethernet II frames for inbound packets.

#### User-Space Utility (`rtl8723be_cli` / HeliPort)
1. **Scan UI**: Trigger scan via `IOUserClient`, query cached BSSID table, render SSID, RSSI, channel, BSSID, security type.
2. **Association Request**: Send connect command with selected SSID and WPA2-PSK passphrase.
3. **Diagnostics & Status**: Display hardware link statistics (RSSI, noise floor, TX/RX packets, dropped frames, current data rate).

### 4.2 Control Path Specification (`IOUserClient`)

The driver exports an `IOUserClient` subclass (`RTL8723BEUserClient`). Communication uses `IOConnectCallStructMethod` and `IOConnectCallScalarMethod` with the following defined external method selectors:

```cpp
enum RTL8723BEMethodIndex {
    kRTL8723BEMethodGetInfo        = 0,  // Fetch MAC address, firmware version, link state
    kRTL8723BEMethodStartScan      = 1,  // Trigger active/passive 802.11 scan
    kRTL8723BEMethodGetScanResults = 2,  // Retrieve discovered BSSID entries
    kRTL8723BEMethodConnect        = 3,  // Initiate connection to SSID with passphrase
    kRTL8723BEMethodDisconnect     = 4,  // Disassociate from current AP
    kRTL8723BEMethodGetStats       = 5,  // Query RSSI, noise, packet counters, current channel
    kRTL8723BEMethodSetAntenna     = 6,  // Switch antenna (Ant 1 vs Ant 2)
    kRTL8723BENumMethods
};
```

#### Key IPC Data Structures
```cpp
// Scan Request
struct RTL8723BEScanRequest {
    uint32_t channelMask;    // Bitmask of 2.4 GHz channels (bits 1-13)
    uint32_t dwellTimeMs;    // Dwell time per channel (e.g. 50-100 ms)
    bool     passive;        // True for passive listen, false for active probe
};

// Scan Result Entry (Returned in array to user-space)
struct RTL8723BEBSSInfo {
    uint8_t  bssid[6];
    char     ssid[33];
    uint8_t  ssidLength;
    int16_t  rssi;           // dBm (e.g. -65)
    uint8_t  channel;        // 1 - 13
    uint16_t beaconInterval;
    uint16_t capabilities;
    uint32_t securityFlags;  // Bit 0: Open, Bit 1: WEP, Bit 2: WPA, Bit 3: WPA2-PSK (RSN)
};

// Connection Request
struct RTL8723BEConnectParams {
    char     ssid[33];
    uint8_t  ssidLength;
    uint8_t  bssid[6];       // Optional specific BSSID; all zeros for auto
    char     passphrase[65]; // WPA2-PSK ASCII passphrase (or 64-char hex PSK)
    uint8_t  passphraseLen;
};

// Driver Status
struct RTL8723BEStatus {
    uint8_t  macAddress[6];
    uint8_t  state;          // 0: Idle, 1: Scanning, 2: Authenticating, 3: Associating, 4: 4-Way Handshake, 5: Connected
    char     currentSSID[33];
    uint8_t  currentBSSID[6];
    uint8_t  currentChannel;
    int16_t  currentRSSI;
    uint32_t txPackets;
    uint32_t rxPackets;
    uint32_t txErrors;
    uint32_t rxErrors;
    uint8_t  activeAntenna;  // 1 or 2
};
```

### 4.3 Data Path Specification: Frame Encapsulation & Crypto

#### 4.3.1 Transmit (TX) Encapsulation Pipeline
When macOS outputs an Ethernet packet (`mbuf_t m`):

1. **Header Parsing**:
   - Destination MAC ($DA$): bytes $0 \dots 5$.
   - Source MAC ($SA$): bytes $6 \dots 11$.
   - EtherType ($ET$): bytes $12 \dots 13$ (e.g., `0x0800` IPv4, `0x0806` ARP).
   - Payload: bytes $14 \dots (L - 1)$.
2. **802.11 Header Assembly**:
   - Construct IEEE 802.11 QoS Data header (26 bytes):
     - `Frame Control`: `0x8801` (Type: Data, Subtype: QoS Data, `To DS` = 1, `From DS` = 0).
     - `Duration`: `0x0000` (or computed TXOP duration).
     - `Address 1` (RA / BSSID): AP MAC address.
     - `Address 2` (TA / SA): Station MAC address (`pci10ec,b723`).
     - `Address 3` (DA): Final destination MAC from Ethernet frame.
     - `Sequence Control`: 12-bit sequence number (incremented modulo 4096), 4-bit fragment 0.
     - `QoS Control`: `0x0000` (TID 0, normal ACK).
3. **LLC / SNAP Header (RFC 1042)**:
   - Insert 8-byte LLC/SNAP header between 802.11 header and payload:
     - `DSAP`: `0xAA`
     - `SSAP`: `0xAA`
     - `Control`: `0x03` (Unnumbered Information)
     - `OUI`: `0x00, 0x00, 0x00`
     - `EtherType`: original 2-byte EtherType ($ET$) from Ethernet header.
4. **WPA2 CCMP Encryption**:
   - Packet Number ($PN$): 48-bit counter, incremented monotonically for each transmitted frame to prevent replay attacks.
   - Insert 8-byte CCMP header:
     - Byte 0: $PN_0$
     - Byte 1: $PN_1$
     - Byte 2: `0x00` (Reserved)
     - Byte 3: `0x20` (ExtIV flag set, KeyID 0)
     - Byte 4: $PN_2$
     - Byte 5: $PN_3$
     - Byte 6: $PN_4$
     - Byte 7: $PN_5$
   - Encrypt payload + LLC/SNAP using AES-128-CCM with PTK-TK (128-bit Temporal Key).
   - Calculate and append 8-byte CCMP MIC (Message Integrity Code).
   - *Hardware Acceleration*: When SEC CAM is configured, the RTL8723BE hardware can perform CCMP encapsulation and MIC generation in silicon; the software driver simply sets `tx_desc->enc_type = 3` (AES) and provides the plaintext payload.
5. **RTL8723BE TX Descriptor Preparation**:
   - Write packet length, queue index, rate ID, and physical address into TX descriptor ring.
   - Advance TX ring write index register (`REG_TXBD_IDX`).

#### 4.3.2 Receive (RX) Decapsulation Pipeline
When the RTL8723BE DMA engine receives a packet:

1. **Descriptor Inspection**:
   - Check RX descriptor status bits: verify CRC OK, ICV/MIC OK, frame length.
2. **Frame Filter & Demux**:
   - Check Frame Control:
     - If Management (Type `00`): Route to Scan / Beacon parser.
     - If Control (Type `01`): Handled by MAC.
     - If Data (Type `10`): Proceed to data processing.
3. **CCMP Decryption & MIC Verification**:
   - Extract 48-bit received $PN$; verify $PN > \text{last\_rx\_}PN$ (replay protection).
   - Verify 8-byte CCMP MIC and decrypt payload using PTK-TK (or GTK for broadcast/multicast).
4. **LLC/SNAP Validation**:
   - Inspect bytes after 802.11 header:
     - If `DSAP == 0xAA && SSAP == 0xAA && Control == 0x03 && OUI == 0x000000`:
       - Extract EtherType from bytes 6-7 of LLC/SNAP.
       - If `EtherType == 0x888E` (EAPOL): Intercept and route directly to WPA2 4-way handshake engine. Do NOT pass to BSD stack.
       - For standard protocols (IPv4, ARP, IPv6):
         - Construct standard Ethernet II frame:
           - Destination MAC = 802.11 Address 1 ($DA$).
           - Source MAC = 802.11 Address 3 ($SA$).
           - EtherType = LLC/SNAP EtherType.
           - Payload = decrypted 802.11 payload following LLC/SNAP header.
         - Allocate `mbuf_t m = allocatePacket(ether_len)`.
         - Copy reconstructed Ethernet II frame into `m`.
         - Call `fEthernetInterface->inputPacket(m, ether_len, 0)`.
5. **Descriptor Recycling**:
   - Re-arm RX descriptor buffer with clean DMA address and clear ownership bit.

---

## 5. Implementation Roadmap & Hardware Bring-Up Strategy

### Phase 1: Baseline Harness, Symbols & PCIe Discovery
- Create the kext structure matching `IOPCIMatch` = `0xB72310EC`.
- Implement `init`, `start`, `stop`, `free`, and PCI MMIO mapping.
- Register `IOEthernetInterface` and publish medium dictionary.
- Verify with `kmutil print-diagnostics` / `kextutil` that all symbols resolve against the running Darwin 25.6.0 kernel with zero unresolved KPI dependencies.

### Phase 2: Hardware Init, Firmware & Antenna Diversity
- Implement the RTL8723BE power-sequence state machine.
- Read eFuse/EEPROM to extract the hardware MAC address and RF parameters.
- Port the 8051 MCU firmware loader for `rtl8723befw.bin`, including page-by-page register streaming and ready-handshake verification.
- Apply baseband, MAC, and RF channel register tables for 2.4 GHz Channels 1–13.
- Configure antenna diversity defaults (setting RF switch to Aux Antenna 2 for HP `103c:804c`).

### Phase 3: TX/RX DMA Engine & Scanning
- Set up TX descriptor rings (High, Normal, Low) and RX descriptor ring in non-cacheable DMA memory (`IOBufferMemoryDescriptor`).
- Attach interrupt handler (`IOFilterInterruptEventSource`).
- Implement channel dwelling and 802.11 Beacon/Probe Response parsing.
- Implement `RTL8723BEUserClient` scan methods; verify scan results via `rtl8723be_cli`.

### Phase 4: WPA2-PSK Handshake & Data Path
- Implement 802.11 Open System Authentication and Association state machines.
- Implement WPA2 4-way handshake (EAPOL-Key handling, SNonce, PTK derivation, MIC verification).
- Implement Ethernet II <-> 802.11 frame translation (LLC/SNAP header handling, CCMP IV and MIC).
- Signal `setLinkStatus(kIONetworkLinkActive)` on association completion to trigger macOS DHCP.

---

## 6. Summary Comparison Matrix

| Architecture Option | Viability on macOS 26.6.2 | OpenCore EFI Ingestion | Stability & Maintenance | UI Management | Recommendation |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **IOEthernetController + User-Space Client** | **100% Viable** | Seamless drop-in kext | High; relies on public, frozen `IONetworkingFamily` KPIs | HeliPort / CLI | **PRIMARY CHOICE (Gold Standard)** |
| **IO80211Family Shim (AirportItlwm style)** | **Unviable** | Difficult; requires framework overrides | Extremely low; broke in macOS 14+, requires OCLP root patching | Native Wi-Fi menu | **REJECTED** |
| **DriverKit (`.dext`)** | **Unviable** | Impossible (no EFI injection) | N/A (no public 802.11 Wi-Fi DriverKit framework) | Custom App | **REJECTED** |
