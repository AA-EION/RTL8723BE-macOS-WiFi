// SPDX-License-Identifier: GPL-2.0-or-later
#include <mach/mach_types.h>
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
KMOD_EXPLICIT_DECL(com.rtl8723be.macos.bluetooth, "1.4.0", _start, _stop)
__private_extern__ kern_return_t RTL8723Bluetooth_start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
__private_extern__ kern_return_t RTL8723Bluetooth_stop(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
__private_extern__ kmod_start_func_t *_realmain = RTL8723Bluetooth_start;
__private_extern__ kmod_stop_func_t *_antimain = RTL8723Bluetooth_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
