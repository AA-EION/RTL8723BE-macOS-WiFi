// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include "../src/RTL8723BE_ipc.h"

static const char *stateToString(uint32_t state) {
    switch (state) {
        case kRTLStateDisconnected:   return "DISCONNECTED";
        case kRTLStateScanning:       return "SCANNING";
        case kRTLStateAuthenticating: return "AUTHENTICATING";
        case kRTLStateAssociating:    return "ASSOCIATING";
        case kRTLStateHandshake:      return "WPA2_4WAY_HANDSHAKE";
        case kRTLStateConnected:      return "CONNECTED";
        default:                      return "UNKNOWN";
    }
}

static io_connect_t openDriverConnection(void) {
    CFMutableDictionaryRef matchDict = IOServiceMatching("RTL8723BE");
    if (!matchDict) {
        fprintf(stderr, "[!] Failed to create IOServiceMatching(\"RTL8723BE\") dictionary.\n");
        return IO_OBJECT_NULL;
    }

    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, matchDict);
    if (service == IO_OBJECT_NULL) {
        fprintf(stderr, "[!] RTL8723BE kernel service not currently loaded in IORegistry (pci10ec,b723).\n");
        fprintf(stderr, "    See docs/TESTING.md for manual post-login loading; EFI injection should stay disabled.\n");
        return IO_OBJECT_NULL;
    }

    io_connect_t connect = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connect);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS || connect == IO_OBJECT_NULL) {
        fprintf(stderr, "[!] IOServiceOpen failed with IOReturn 0x%08x.\n", kr);
        return IO_OBJECT_NULL;
    }
    return connect;
}

static int cmdStatus(io_connect_t conn) {
    RTL8723BEStatus st;
    memset(&st, 0, sizeof(st));
    size_t outSize = sizeof(st);
    kern_return_t kr = IOConnectCallStructMethod(conn, kMethodGetStatus, nullptr, 0, &st, &outSize);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[!] kMethodGetStatus failed (0x%08x)\n", kr);
        return 1;
    }
    printf("=== Realtek RTL8723BE PCIe Wi-Fi Status ===\n");
    printf("  State      : %s (%u)\n", stateToString(st.state), st.state);
    printf("  MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
           st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
    printf("  SSID       : %s\n", st.ssid[0] ? st.ssid : "<none>");
    printf("  BSSID      : %02x:%02x:%02x:%02x:%02x:%02x\n",
           st.bssid[0], st.bssid[1], st.bssid[2], st.bssid[3], st.bssid[4], st.bssid[5]);
    printf("  Channel    : %u (2.4 GHz)\n", st.channel);
    printf("  RSSI       : %d dBm\n", st.rssi);
    printf("  Antenna    : %s (#%u)\n", st.antenna == 2 ? "AUX" : "MAIN", st.antenna);
    printf("  TX Packets : %llu (Errors: %llu)\n",
           (unsigned long long)st.tx_packets, (unsigned long long)st.tx_errors);
    printf("  RX Packets : %llu (Errors: %llu)\n",
           (unsigned long long)st.rx_packets, (unsigned long long)st.rx_errors);
    return 0;
}

static int cmdLink(io_connect_t conn) {
    RTL8723BELinkInfo info = {};
    size_t size = sizeof(info);
    kern_return_t rc = IOConnectCallStructMethod(conn, kMethodGetLinkInfo, nullptr, 0, &info, &size);
    if (rc != KERN_SUCCESS || size != sizeof(info) || info.version != 1) {
        fprintf(stderr, "Link information requires driver 1.4.0 (IOReturn 0x%08x).\n", rc);
        return 1;
    }
    printf("Radio: %s; WMM: %s; short GI: %s; supported rate mask: 0x%08x\n",
           info.ht20 ? "HT20 MCS0-7" : "802.11b/g", info.wmm ? "yes" : "no", info.short_gi ? "yes" : "no", info.rate_mask);
    printf("Rate control: %s; transmit CCMP: %s; options: %u\n",
           info.firmware_rate_control ? "firmware" : "descriptor fallback", info.tx_crypto ? "hardware" : "software", info.options);
    printf("Maximum negotiated descriptor rate: %u (capability, not measured throughput)\n", info.max_rate);
    return 0;
}

static int cmdOptions(io_connect_t conn, const char *argument) {
    char *end = nullptr;
    unsigned long value = argument ? strtoul(argument, &end, 0) : 8;
    if (!argument || end == argument || *end || value > 7) {
        fprintf(stderr, "Usage: options <0..7> (bit 1: HT20, bit 2: hardware TX CCMP, bit 4: Bluetooth PTA); disconnect first.\n");
        return 1;
    }
    uint64_t input = value;
    kern_return_t rc = IOConnectCallScalarMethod(conn, kMethodSetOptions, &input, 1, nullptr, nullptr);
    if (rc != KERN_SUCCESS) { fprintf(stderr, "Cannot set options (IOReturn 0x%08x). Disconnect first.\n", rc); return 1; }
    return cmdLink(conn);
}

static int cmdScan(io_connect_t conn) {
    printf("[*] Triggering 2.4 GHz active/passive scan across channels 1..13 on pci10ec,b723...\n");
    kern_return_t kr = IOConnectCallMethod(conn, kMethodScan, nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[!] kMethodScan failed (0x%08x)\n", kr);
        return 1;
    }
    // The driver returns once every channel has been dwelt on (~1.6 s per antenna path).
    RTL8723BEScanResults res;
    memset(&res, 0, sizeof(res));
    size_t outSize = sizeof(res);
    kr = IOConnectCallStructMethod(conn, kMethodGetScanResults, nullptr, 0, &res, &outSize);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[!] kMethodGetScanResults failed (0x%08x)\n", kr);
        return 1;
    }

    printf("\n%-32s  %-17s  %-4s  %-9s  %-8s\n", "SSID", "BSSID", "CH", "RSSI", "SECURITY");
    printf("--------------------------------  -----------------  ----  ---------  --------\n");
    for (uint32_t i = 0; i < res.count && i < RTL8723BE_MAX_SCAN_APS; ++i) {
        const RTL8723BEDiscoveredAP &ap = res.aps[i];
        char bssidStr[24];
        snprintf(bssidStr, sizeof(bssidStr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        printf("%-32s  %-17s  %-4u  %4d dBm   %-8s\n",
               ap.ssid[0] ? ap.ssid : "<hidden>",
               bssidStr,
               ap.channel,
               ap.rssi,
               ap.is_wpa2 ? "WPA2-PSK" : "OTHER");
    }
    printf("\nTotal discovered APs: %u\n", res.count);
    return 0;
}

static int cmdConnect(io_connect_t conn, const char *ssid, const char *passphrase, const char *bssidOpt) {
    RTL8723BEConnectParams params;
    memset(&params, 0, sizeof(params));
    strncpy(params.ssid, ssid, RTL8723BE_MAX_SSID_LEN);
    if (passphrase) {
        strncpy(params.passphrase, passphrase, RTL8723BE_MAX_PASSPHRASE_LEN);
    }
    if (bssidOpt) {
        unsigned int b[6] = {0};
        if (sscanf(bssidOpt, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
            for (int i = 0; i < 6; ++i) params.bssid[i] = (uint8_t)b[i];
        }
    } else {
        // Pick the strongest BSS advertising this SSID from the last scan.
        RTL8723BEScanResults res;
        memset(&res, 0, sizeof(res));
        size_t outSize = sizeof(res);
        IOConnectCallStructMethod(conn, kMethodGetScanResults, nullptr, 0, &res, &outSize);
        int best = -1;
        for (uint32_t i = 0; i < res.count && i < RTL8723BE_MAX_SCAN_APS; ++i) {
            if (strcmp(res.aps[i].ssid, ssid) == 0 && (best < 0 || res.aps[i].rssi > res.aps[best].rssi)) {
                best = (int)i;
            }
        }
        if (best < 0) {
            fprintf(stderr, "[!] '%s' not in the last scan results; run 'scan' first or pass a BSSID.\n", ssid);
            return 1;
        }
        memcpy(params.bssid, res.aps[best].bssid, 6);
    }
    printf("[*] Connecting to SSID '%s' via WPA2-PSK / 802.11 state machine...\n", params.ssid);
    kern_return_t kr = IOConnectCallStructMethod(conn, kMethodConnect, &params, sizeof(params), nullptr, nullptr);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[!] kMethodConnect failed (0x%08x)\n", kr);
        return 1;
    }
    usleep(800 * 1000);
    return cmdStatus(conn);
}

static int cmdDisconnect(io_connect_t conn) {
    kern_return_t kr = IOConnectCallMethod(conn, kMethodDisconnect, nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[!] kMethodDisconnect failed (0x%08x)\n", kr);
        return 1;
    }
    printf("[+] Disconnected from Wi-Fi network.\n");
    return 0;
}

static int cmdAntenna(io_connect_t conn, uint64_t ant) {
    if (ant != 1 && ant != 2) {
        fprintf(stderr, "Usage: rtl8723be_cli antenna <1|2> (1=MAIN, 2=AUX)\n");
        return 1;
    }
    kern_return_t kr = IOConnectCallScalarMethod(conn, kMethodSetAntenna, &ant, 1, nullptr, nullptr);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[!] kMethodSetAntenna failed (0x%08x)\n", kr);
        return 1;
    }
    printf("[+] Active RF antenna path switched to #%llu (%s).\n",
           (unsigned long long)ant, ant == 2 ? "AUX" : "MAIN");
    return 0;
}

static bool callRead(io_connect_t conn, uint32_t sel, const uint64_t *in, uint32_t inCnt, uint64_t *out) {
    uint32_t outCnt = 1;
    kern_return_t kr = IOConnectCallScalarMethod(conn, sel, in, inCnt, out, &outCnt);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[!] method %u failed (0x%08x)%s\n", sel, kr,
                kr == (kern_return_t)0xe00002c1 ? " - run with sudo" : "");
        return false;
    }
    return true;
}

static int cmdReg(io_connect_t conn, int argc, char **argv) {
    // reg read <addr> [1|2|4] | reg write <addr> <value> [1|2|4]
    if (argc < 4) { fprintf(stderr, "Usage: reg read <addr> [width] | reg write <addr> <value> [width]\n"); return 1; }
    uint64_t addr = strtoull(argv[3], nullptr, 0);
    if (strcmp(argv[2], "read") == 0) {
        uint64_t in[2] = {addr, argc >= 5 ? strtoull(argv[4], nullptr, 0) : 4}, out = 0;
        if (!callRead(conn, kMethodReadReg, in, 2, &out)) return 1;
        printf("0x%04llx = 0x%0*llx\n", addr, (int)(in[1] * 2), out);
        return 0;
    }
    if (strcmp(argv[2], "write") == 0 && argc >= 5) {
        uint64_t in[3] = {addr, argc >= 6 ? strtoull(argv[5], nullptr, 0) : 4, strtoull(argv[4], nullptr, 0)};
        kern_return_t kr = IOConnectCallScalarMethod(conn, kMethodWriteReg, in, 3, nullptr, nullptr);
        if (kr != KERN_SUCCESS) { fprintf(stderr, "[!] write failed (0x%08x); writes need sudo\n", kr); return 1; }
        printf("0x%04llx <- 0x%llx\n", addr, in[2]);
        return 0;
    }
    fprintf(stderr, "Usage: reg read <addr> [width] | reg write <addr> <value> [width]\n");
    return 1;
}

static int cmdRF(io_connect_t conn, int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "Usage: rf read <off> | rf write <off> <value>\n"); return 1; }
    uint64_t off = strtoull(argv[3], nullptr, 0);
    if (strcmp(argv[2], "read") == 0) {
        uint64_t out = 0;
        if (!callRead(conn, kMethodReadRF, &off, 1, &out)) return 1;
        printf("RF[0x%02llx] = 0x%05llx\n", off, out);
        return 0;
    }
    if (strcmp(argv[2], "write") == 0 && argc >= 5) {
        uint64_t in[2] = {off, strtoull(argv[4], nullptr, 0)};
        kern_return_t kr = IOConnectCallScalarMethod(conn, kMethodWriteRF, in, 2, nullptr, nullptr);
        if (kr != KERN_SUCCESS) { fprintf(stderr, "[!] write failed (0x%08x); writes need sudo\n", kr); return 1; }
        printf("RF[0x%02llx] <- 0x%05llx\n", off, in[1]);
        return 0;
    }
    fprintf(stderr, "Usage: rf read <off> | rf write <off> <value>\n");
    return 1;
}

static int cmdDump(io_connect_t conn, int argc, char **argv) {
    uint64_t start = argc >= 3 ? strtoull(argv[2], nullptr, 0) : 0;
    uint64_t len = argc >= 4 ? strtoull(argv[3], nullptr, 0) : 0x100;
    start &= ~0xFULL;
    for (uint64_t a = start; a < start + len && a < 0x4000; a += 16) {
        printf("%04llx:", a);
        for (uint64_t w = 0; w < 16; w += 4) {
            uint64_t in[2] = {a + w, 4}, out = 0;
            if (!callRead(conn, kMethodReadReg, in, 2, &out)) return 1;
            printf(" %08llx", out);
        }
        printf("\n");
    }
    return 0;
}

static int cmdDiag(io_connect_t conn) {
    RTL8723BEDiag d;
    memset(&d, 0, sizeof(d));
    size_t outSize = sizeof(d);
    kern_return_t kr = IOConnectCallStructMethod(conn, kMethodGetDiag, nullptr, 0, &d, &outSize);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[!] kMethodGetDiag failed (0x%08x) - is the loaded kext older than this CLI?\n", kr);
        return 1;
    }
    printf("=== RTL8723BE diagnostics (v%u) ===\n", d.version);
    printf("  init stage     : %u%s\n", d.init_stage, d.init_stage == 11 ? " (ready)" : "");
    printf("  firmware       : v%u.%u  MCUFWDL=0x%08x\n", d.fw_version >> 16, d.fw_version & 0xFFFF, d.mcufwdl);
    printf("  eFuse          : %s, %u bytes used, package %u, SVID/SMID %04x/%04x\n",
           d.efuse_autoload_ok ? "valid" : "INVALID", d.efuse_used, d.package_type, d.svid, d.smid);
    printf("  CR=0x%04x RCR=0x%08x HIMR=0x%08x HISR=0x%08x RXDMA=0x%04x\n", d.cr, d.rcr, d.himr, d.hisr, d.rxdma_ctrl);
    printf("  RF18=0x%05x (ch %u) RF00=0x%05x\n", d.rf_chnlbw, d.rf_chnlbw & 0xFF, d.rf_mode);
    printf("  antenna        : %u-ant, Wi-Fi on %s | 948=0x%x 92c=0x%x 4c=0x%08x 765=0x%02x 76e=0x%02x 67=0x%02x\n",
           d.ant_num, d.ant_path == 2 ? "AUX/S0" : "MAIN/S1", d.reg_948, d.reg_92c, d.reg_4c, d.reg_765, d.reg_76e, d.reg_67);
    printf("  BB CCA hits    : OFDM %u  CCK %u\n", d.bb_ofdm_cca, d.bb_cck_cca);
    printf("  interrupts     : %u\n", d.interrupts);
    printf("  RX             : desc %u, mgmt %u (beacons %u), data %u, ctrl %u, crc-err %u, dropped %u\n",
           d.rx_desc_done, d.rx_mgmt, d.rx_beacons, d.rx_data, d.rx_ctrl, d.rx_crc_err, d.rx_dropped);
    printf("  TX             : submitted %u, ring-full %u\n", d.tx_submitted, d.tx_ring_full);
    if (d.version >= 2) {
        printf("  RX by rate     : CCK ok %u crc %u | OFDM ok %u crc %u | HT ok %u crc %u\n",
               d.rx_ok_cck, d.rx_crc_cck, d.rx_ok_ofdm, d.rx_crc_ofdm, d.rx_ok_ht, d.rx_crc_ht);
        printf("  unicast data   : rx %u, delivered %u, replay %u, mic-fail %u | bcast delivered %u, mic-fail %u\n",
               d.uc_data_rx, d.uc_delivered, d.uc_replay, d.uc_mic_fail, d.bc_delivered, d.bc_mic_fail);
        printf("  last unicast   : FC %02x%02x PN..%04x rate 0x%02x len %u\n",
               d.last_uc_fc_pn_lo >> 24, (d.last_uc_fc_pn_lo >> 16) & 0xFF, d.last_uc_fc_pn_lo & 0xFFFF,
               d.last_uc_rate_len >> 16, d.last_uc_rate_len & 0xFFFF);
    }
    return 0;
}

static void printUsage(const char *prog) {
    printf("Usage: %s <command> [args]\n\n", prog);
    printf("Commands:\n");
    printf("  status                           Display RTL8723BE hardware, MAC, channel, RSSI & counters\n");
    printf("  link                             Negotiated HT/WMM/rates and offload status\n");
    printf("  options <0..7>                   Set radio options while disconnected (see docs/FEATURES.md)\n");
    printf("  scan                             Trigger 2.4 GHz scan (Ch 1-13) and list discovered APs\n");
    printf("  connect <SSID> [PSK] [BSSID]     Authenticate, associate, and run WPA2 4-way handshake\n");
    printf("  disconnect                       Deauthenticate and tear down active association\n");
    printf("  antenna <1|2>                    Select Wi-Fi RF path (1=Main/S1, 2=Aux/S0 — HP single-antenna fix)\n");
    printf("  diag                             Driver/hardware diagnostics (init stage, RX counters, key registers)\n");
    printf("  reg read <addr> [1|2|4]          Read a MAC/BB register\n");
    printf("  reg write <addr> <val> [1|2|4]   Write a MAC/BB register (sudo)\n");
    printf("  rf read <off>                    Read an RF register (path A)\n");
    printf("  rf write <off> <val>             Write an RF register (sudo)\n");
    printf("  dump [start] [len]               Hex dump of MAC/BB register space\n");
    printf("  iqk                              Run TX/RX IQ calibration on both RF paths (sudo)\n");
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        printUsage(argv[0]);
        return 0;
    }

    io_connect_t conn = openDriverConnection();
    if (conn == IO_OBJECT_NULL) {
        return 2;
    }

    int rc = 0;
    if (strcmp(argv[1], "status") == 0) {
        rc = cmdStatus(conn);
    } else if (strcmp(argv[1], "link") == 0) {
        rc = cmdLink(conn);
    } else if (strcmp(argv[1], "options") == 0) {
        rc = cmdOptions(conn, argc == 3 ? argv[2] : nullptr);
    } else if (strcmp(argv[1], "scan") == 0) {
        rc = cmdScan(conn);
    } else if (strcmp(argv[1], "connect") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: 'connect' requires <SSID> [passphrase] [bssid]\n");
            rc = 1;
        } else {
            rc = cmdConnect(conn, argv[2], argc >= 4 ? argv[3] : "", argc >= 5 ? argv[4] : nullptr);
        }
    } else if (strcmp(argv[1], "disconnect") == 0) {
        rc = cmdDisconnect(conn);
    } else if (strcmp(argv[1], "antenna") == 0) {
        uint64_t ant = (argc >= 3) ? (uint64_t)atoi(argv[2]) : 0;
        rc = cmdAntenna(conn, ant);
    } else if (strcmp(argv[1], "iqk") == 0) {
        uint64_t out = 0;
        uint32_t outCnt = 1;
        kern_return_t kr = IOConnectCallScalarMethod(conn, kMethodIQCalibrate, nullptr, 0, &out, &outCnt);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "[!] IQ calibration failed (0x%08x); needs sudo and a v1.2+ kext\n", kr);
            rc = 1;
        } else {
            printf("[+] IQ calibration: path A (S1) %s, path B (S0) %s\n",
                   (out & 1) ? "calibrated" : "FAILED", (out & 2) ? "calibrated" : "FAILED");
        }
    } else if (strcmp(argv[1], "diag") == 0) {
        rc = cmdDiag(conn);
    } else if (strcmp(argv[1], "reg") == 0) {
        rc = cmdReg(conn, argc, argv);
    } else if (strcmp(argv[1], "rf") == 0) {
        rc = cmdRF(conn, argc, argv);
    } else if (strcmp(argv[1], "dump") == 0) {
        rc = cmdDump(conn, argc, argv);
    } else {
        printUsage(argv[0]);
        rc = 1;
    }

    IOServiceClose(conn);
    return rc;
}
