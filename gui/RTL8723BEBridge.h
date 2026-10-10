// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group

#ifndef _RTL8723BE_BRIDGE_H_
#define _RTL8723BE_BRIDGE_H_

#include <stdint.h>
#include <IOKit/IOKitLib.h>
#include "../src/RTL8723BE_ipc.h"

static inline int rtl_open_client(io_connect_t *outConn) {
    CFMutableDictionaryRef matchDict = IOServiceMatching("RTL8723BE");
    if (!matchDict) return -1;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, matchDict);
    if (service == IO_OBJECT_NULL) return -2;
    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS || conn == IO_OBJECT_NULL) return -3;
    *outConn = conn;
    return 0;
}

static inline void rtl_close_client(io_connect_t conn) {
    if (conn != IO_OBJECT_NULL) {
        IOServiceClose(conn);
    }
}

static inline int rtl_get_status(io_connect_t conn, struct RTL8723BEStatus *outStatus) {
    size_t outSize = sizeof(struct RTL8723BEStatus);
    kern_return_t kr = IOConnectCallStructMethod(conn, kMethodGetStatus, NULL, 0, outStatus, &outSize);
    return (kr == KERN_SUCCESS) ? 0 : (int)kr;
}

static inline int rtl_trigger_scan(io_connect_t conn) {
    kern_return_t kr = IOConnectCallMethod(conn, kMethodScan, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
    return (kr == KERN_SUCCESS) ? 0 : (int)kr;
}

static inline int rtl_get_scan_results(io_connect_t conn, struct RTL8723BEScanResults *outResults) {
    size_t outSize = sizeof(struct RTL8723BEScanResults);
    kern_return_t kr = IOConnectCallStructMethod(conn, kMethodGetScanResults, NULL, 0, outResults, &outSize);
    return (kr == KERN_SUCCESS) ? 0 : (int)kr;
}

static inline int rtl_connect_ap(io_connect_t conn, const struct RTL8723BEConnectParams *params) {
    kern_return_t kr = IOConnectCallStructMethod(conn, kMethodConnect, params, sizeof(struct RTL8723BEConnectParams), NULL, NULL);
    return (kr == KERN_SUCCESS) ? 0 : (int)kr;
}

static inline int rtl_disconnect_ap(io_connect_t conn) {
    kern_return_t kr = IOConnectCallMethod(conn, kMethodDisconnect, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
    return (kr == KERN_SUCCESS) ? 0 : (int)kr;
}

static inline int rtl_set_antenna(io_connect_t conn, uint8_t ant) {
    uint64_t scalar = (uint64_t)ant;
    kern_return_t kr = IOConnectCallScalarMethod(conn, kMethodSetAntenna, &scalar, 1, NULL, NULL);
    return (kr == KERN_SUCCESS) ? 0 : (int)kr;
}

static inline int rtl_get_diag(io_connect_t conn, struct RTL8723BEDiag *outDiag) {
    size_t outSize = sizeof(struct RTL8723BEDiag);
    kern_return_t kr = IOConnectCallStructMethod(conn, kMethodGetDiag, NULL, 0, outDiag, &outSize);
    return (kr == KERN_SUCCESS) ? 0 : (int)kr;
}

/* Copies the BSD name (e.g. "en2") of the driver's network interface. */
static inline int rtl_copy_bsd_name(char *out, size_t outLen) {
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RTL8723BE"));
    if (service == IO_OBJECT_NULL) return -1;
    CFTypeRef name = IORegistryEntrySearchCFProperty(service, kIOServicePlane, CFSTR("BSD Name"),
                                                     kCFAllocatorDefault, kIORegistryIterateRecursively);
    IOObjectRelease(service);
    if (!name) return -2;
    Boolean ok = CFGetTypeID(name) == CFStringGetTypeID() &&
                 CFStringGetCString((CFStringRef)name, out, (CFIndex)outLen, kCFStringEncodingUTF8);
    CFRelease(name);
    return ok ? 0 : -3;
}

/* 1 if the RTL8723BE PCI function is present in the IORegistry. */
static inline int rtl_hardware_present(void) {
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceNameMatching("pci10ec,b723"));
    if (service == IO_OBJECT_NULL) return 0;
    IOObjectRelease(service);
    return 1;
}

#endif // _RTL8723BE_BRIDGE_H_
