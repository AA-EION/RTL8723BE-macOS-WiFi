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
        fprintf(stderr, "    Run `sudo ./scripts/stage_opencore.sh --load` or reboot with OpenCore EFI staged.\n");
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

static int cmdScan(io_connect_t conn) {
    printf("[*] Triggering 2.4 GHz active/passive scan across channels 1..13 on pci10ec,b723...\n");
    kern_return_t kr = IOConnectCallMethod(conn, kMethodScan, nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[!] kMethodScan failed (0x%08x)\n", kr);
        return 1;
    }
    // Wait for channel dwell scan (13 channels * ~100ms)
    usleep(1600 * 1000);

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
               ap.is_wpa2 ? "WPA2-PSK" : "OPEN");
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

static void printUsage(const char *prog) {
    printf("Usage: %s <command> [args]\n\n", prog);
    printf("Commands:\n");
    printf("  status                           Display RTL8723BE hardware, MAC, channel, RSSI & counters\n");
    printf("  scan                             Trigger 2.4 GHz scan (Ch 1-13) and list discovered APs\n");
    printf("  connect <SSID> [PSK] [BSSID]     Authenticate, associate, and run WPA2 4-way handshake\n");
    printf("  disconnect                       Deauthenticate and tear down active association\n");
    printf("  antenna <1|2>                    Select RF antenna diversity path (1=Main, 2=Aux — HP laptop fix)\n");
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
    } else {
        printUsage(argv[0]);
        rc = 1;
    }

    IOServiceClose(conn);
    return rc;
}
