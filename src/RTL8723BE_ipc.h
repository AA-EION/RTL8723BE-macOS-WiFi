// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group

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
    // Diagnostics (appended; indices above are ABI-stable for the GUI).
    kMethodReadReg        = 6,  // in: addr, width(1/2/4)   out: value
    kMethodWriteReg       = 7,  // in: addr, width, value   (administrator only)
    kMethodReadRF         = 8,  // in: rf offset            out: 20-bit value
    kMethodWriteRF        = 9,  // in: rf offset, value     (administrator only)
    kMethodGetDiag        = 10, // out: RTL8723BEDiag
    kMethodIQCalibrate    = 11, // out: bit0 path A (S1), bit1 path B (S0) calibrated (administrator only)
    kMethodGetLinkInfo    = 12, // appended; existing IPC sizes stay unchanged
    kMethodSetOptions     = 13, // scalar: RTL8723BEOption mask; disconnected only
    kNumberOfMethods      = 14
};

enum RTL8723BEOption {
    kRTLOptionHT = 1U << 0,
    kRTLOptionTxCrypto = 1U << 1,
    kRTLOptionBluetoothCoexist = 1U << 2 // Linux 1-antenna internal-switch PTA baseline
};
struct RTL8723BELinkInfo {
    uint32_t version;
    uint32_t options;
    uint32_t rate_mask;
    uint8_t ht20;
    uint8_t short_gi;
    uint8_t wmm;
    uint8_t firmware_rate_control;
    uint8_t tx_crypto;
    uint8_t max_rate; // hardware encoding: 0..11 legacy, 12..19 MCS0..7
    uint8_t reserved[2];
};

struct RTL8723BEDiag {
    uint32_t version;          // RTL8723BE_DIAG_VERSION
    uint32_t init_stage;       // last completed hardware-init stage (see kext log)
    uint32_t fw_version;       // (version << 16) | subversion
    uint32_t mcufwdl;          // REG_MCUFWDL
    uint32_t cr;               // REG_CR
    uint32_t rcr;              // REG_RCR
    uint32_t himr, hisr;
    uint32_t rxdma_ctrl;       // REG_RXDMA_CONTROL (0x286) | RXPKT_NUM (0x287) << 8
    uint32_t rf_chnlbw;        // RF 0x18
    uint32_t rf_mode;          // RF 0x00
    uint32_t bb_ofdm_cca;      // OFDM CCA hits (0xDA0[15:0]) - BB detected OFDM preambles
    uint32_t bb_cck_cca;       // CCK CCA hits (0xA60) - BB detected CCK/DSSS preambles
    uint32_t reg_948, reg_92c, reg_4c, reg_765, reg_76e, reg_67;
    uint32_t interrupts;       // filter hits
    uint32_t rx_desc_done;     // descriptors returned by hardware
    uint32_t rx_mgmt, rx_beacons, rx_data, rx_ctrl, rx_crc_err, rx_dropped;
    uint32_t tx_submitted, tx_ring_full;
    uint16_t efuse_used;
    uint8_t  efuse_autoload_ok;
    uint8_t  package_type;
    uint8_t  ant_num;          // 1 or 2 (from eFuse 0xC3 / boot-arg)
    uint8_t  ant_path;         // 1 = main (S1), 2 = aux (S0)
    uint8_t  channel;
    uint8_t  reserved;
    uint16_t svid, smid;
    // v2: per-rate RX quality and unicast data path accounting
    uint32_t rx_ok_cck, rx_ok_ofdm, rx_ok_ht;      // good frames by modulation
    uint32_t rx_crc_cck, rx_crc_ofdm, rx_crc_ht;   // CRC failures by modulation (needs RCR ACRC32)
    uint32_t uc_data_rx;       // protected data frames from our BSSID to our MAC
    uint32_t uc_delivered;     // ...decrypted and handed to the stack
    uint32_t uc_replay, uc_mic_fail, bc_delivered, bc_mic_fail;
    uint32_t last_uc_fc_pn_lo; // FC(16) | PN[15:0] of the last unicast-to-us frame
    uint32_t last_uc_rate_len; // rxmcs(8) << 16 | len
    // v3: payload byte counters (Ethernet frames delivered to / accepted from the stack).
    // macOS 26 does not fold IONetworkInterface input stats into if_data, so apps use these.
    uint64_t rx_bytes;
    uint64_t tx_bytes;
};
#define RTL8723BE_DIAG_VERSION 3

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
