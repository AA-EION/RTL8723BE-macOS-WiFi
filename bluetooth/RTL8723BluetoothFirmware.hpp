// SPDX-License-Identifier: GPL-2.0-or-later
// Linux btrtl.c copyright (C) 2015 Endless Mobile, Inc.
// RTL8723B USB firmware protocol ported from Linux drivers/bluetooth/btrtl.c.
#ifndef RTL8723_BLUETOOTH_FIRMWARE_HPP
#define RTL8723_BLUETOOTH_FIRMWARE_HPP
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>

class RTL8723BluetoothFirmware : public IOService {
    OSDeclareDefaultStructors(RTL8723BluetoothFirmware);
public:
    bool init(OSDictionary *properties = nullptr) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
private:
    void releaseUSB();
    bool configureUSB();
    bool upload();
    bool command(uint16_t opcode, const uint8_t *parameters, uint8_t length,
                 const uint8_t *&response, size_t &responseLength);
    IOUSBHostDevice *device;
    IOUSBHostInterface *interface;
    IOUSBHostPipe *events;
    IOBufferMemoryDescriptor *eventBuffer;
    bool prepared;
    uint8_t commandBuffer[258];
};
#endif
