#ifndef _RTL8723BE_IPC_H_
#define _RTL8723BE_IPC_H_

#include <stdint.h>

#define RTL8723BE_MAX_SSID_LEN        32
#define RTL8723BE_MAX_PASSPHRASE_LEN  64
#define RTL8723BE_MAX_SCAN_APS        32

enum RTL8723BEMethodIndex {
    kMethodScan           = 0,
    kMethodGetScanResults = 1,
    kMethodConnect        = 2,
    kMethodDisconnect     = 3,
    kMethodGetStatus      = 4,
    kMethodSetAntenna     = 5,
    kNumberOfMethods      = 6
};

enum RTL8723BEWifiState {
    kRTLStateDisconnected   = 0,
    kRTLStateScanning       = 1,
    kRTLStateAuthenticating = 2,
    kRTLStateAssociating    = 3,
    kRTLStateHandshake      = 4,
    kRTLStateConnected      = 5
};

struct RTL8723BEDiscoveredAP {
    uint8_t  bssid[6];
    char     ssid[RTL8723BE_MAX_SSID_LEN + 1];
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  is_wpa2;
    uint16_t beacon_interval;
    uint16_t capabilities;
};

struct RTL8723BEScanResults {
    uint32_t count;
    struct RTL8723BEDiscoveredAP aps[RTL8723BE_MAX_SCAN_APS];
};

struct RTL8723BEConnectParams {
    char    ssid[RTL8723BE_MAX_SSID_LEN + 1];
    char    passphrase[RTL8723BE_MAX_PASSPHRASE_LEN + 1];
    uint8_t bssid[6];
};

struct RTL8723BEStatus {
    uint32_t state;
    uint8_t  mac[6];
    uint8_t  bssid[6];
    char     ssid[RTL8723BE_MAX_SSID_LEN + 1];
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  antenna; // 1 = Main, 2 = Aux
    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t tx_errors;
    uint64_t rx_errors;
};

#endif // _RTL8723BE_IPC_H_
