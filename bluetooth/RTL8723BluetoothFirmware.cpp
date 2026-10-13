// SPDX-License-Identifier: GPL-2.0-or-later
// Linux btrtl.c copyright (C) 2015 Endless Mobile, Inc.
// Copyright (c) 2026 Issen Software Group
// Firmware protocol: Linux v6.12 btrtl.c, (c) 2014-2024 Realtek Semiconductor.
// IOUSBHost handoff follows thegwchr/RealtekBluetoothFirmware and IntelBluetoothFirmware.
#include "RTL8723BluetoothFirmware.hpp"
#include "firmware.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/usb/StandardUSB.h>
#include <pexpert/pexpert.h>

#define super IOService
OSDefineMetaClassAndStructors(RTL8723BluetoothFirmware, IOService);
bool RTL8723BluetoothFirmware::init(OSDictionary *properties) {
    device = nullptr; interface = nullptr; events = nullptr; eventBuffer = nullptr; prepared = false;
    return super::init(properties);
}
void RTL8723BluetoothFirmware::releaseUSB() {
    if (events) { events->abort(); events->release(); events = nullptr; }
    if (eventBuffer) {
        if (prepared) eventBuffer->complete(kIODirectionIn);
        eventBuffer->release(); eventBuffer = nullptr; prepared = false;
    }
    if (interface) {
        if (interface->isOpen(this)) interface->close(this);
        interface->release(); interface = nullptr;
    }
    if (device) {
        if (device->isOpen(this)) device->close(this);
        device->release(); device = nullptr;
    }
}
bool RTL8723BluetoothFirmware::configureUSB() {
    const auto *configuration = device->getConfigurationDescriptor(0);
    if (!configuration || device->setConfiguration(configuration->bConfigurationValue) != kIOReturnSuccess) return false;
    OSIterator *children = device->getChildIterator(gIOServicePlane);
    if (!children) return false;
    while (OSObject *child = children->getNextObject()) {
        IOUSBHostInterface *candidate = OSDynamicCast(IOUSBHostInterface, child);
        if (!candidate) continue;
        const auto *desc = candidate->getInterfaceDescriptor();
        // Linux btusb: interface zero carries commands/events; interface one is SCO.
        if (!desc || desc->bInterfaceNumber != 0 || desc->bAlternateSetting != 0) continue;
        interface = candidate; interface->retain(); break;
    }
    children->release();
    if (!interface || !interface->open(this)) return false;
    const auto *config = interface->getConfigurationDescriptor();
    const auto *desc = interface->getInterfaceDescriptor();
    const StandardUSB::EndpointDescriptor *ep = nullptr;
    while ((ep = StandardUSB::getNextEndpointDescriptor(config, desc, ep))) {
        if (StandardUSB::getEndpointType(ep) == StandardUSB::kEndpointDescriptorTransferTypeInterrupt && StandardUSB::getEndpointDirection(ep) == StandardUSB::kEndpointDescriptorDirectionIn) {
            events = interface->copyPipe(StandardUSB::getEndpointAddress(ep)); break;
        }
    }
    eventBuffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionIn, 260);
    if (!events || !eventBuffer || eventBuffer->prepare(kIODirectionIn) != kIOReturnSuccess) return false;
    prepared = true;
    return true;
}
bool RTL8723BluetoothFirmware::command(uint16_t opcode, const uint8_t *parameters, uint8_t length,
                                      const uint8_t *&response, size_t &responseLength) {
    response = nullptr; responseLength = 0;
    commandBuffer[0] = uint8_t(opcode); commandBuffer[1] = uint8_t(opcode >> 8); commandBuffer[2] = length;
    if (length) memcpy(commandBuffer + 3, parameters, length);
    StandardUSB::DeviceRequest request = {};
    request.bmRequestType = 0x20; // USB class/device OUT: Bluetooth HCI command
    request.wLength = uint16_t(length + 3);
    uint32_t actual = 0;
    IOReturn rc = interface->deviceRequest(request, commandBuffer, actual, 1000);
    if (rc != kIOReturnSuccess || actual != unsigned(length) + 3) return false;
    // Synchronous USB reads have a bounded timeout and no stack callback lifetime race.
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        actual = 0;
        rc = events->io(eventBuffer, 260, actual, 1000);
        if (rc != kIOReturnSuccess) return false;
        const uint8_t *e = static_cast<const uint8_t *>(eventBuffer->getBytesNoCopy());
        if (actual < 2 || unsigned(e[1]) + 2 != actual) return false;
        if (e[0] != 0x0e) continue;
        if (actual < 6 || rtl_bt::le16(e + 3) != opcode) continue;
        response = e + 5; responseLength = actual - 5;
        return response[0] == 0;
    }
    return false;
}
bool RTL8723BluetoothFirmware::upload() {
    const uint8_t *response; size_t length;
    if (!command(0x1001, nullptr, 0, response, length) || length != 9) return false;
    uint16_t lmp = rtl_bt::le16(response + 7), revision = rtl_bt::le16(response + 2);
    uint8_t version = response[1];
    // Linux IC_INFO(RTL_ROM_LMP_8723B, 0xb, 0x6, HCI_USB).
    if (lmp != 0x8723 || revision != 0x000b || version != 6) {
        IOLog("RTL8723Bluetooth: HCI %u rev=%04x LMP=%04x does not match unpatched RTL8723B; skipping upload\n", version, revision, lmp);
        return false;
    }
    if (!command(0xfc6d, nullptr, 0, response, length) || length != 2) return false;
    uint8_t rom = response[1];
    rtl_bt::Patch patch;
    if (!rtl_bt::parse(rtl8723b_bt_firmware, rtl8723b_bt_firmware_size, rom, patch)) return false;
    uint8_t *data = static_cast<uint8_t *>(IOMalloc(patch.length));
    if (!data) return false;
    memcpy(data, rtl8723b_bt_firmware + patch.offset, patch.length);
    for (unsigned i = 0; i < 4; ++i) data[patch.length - 4 + i] = uint8_t(patch.version >> (8 * i));
    size_t count = rtl_bt::fragmentCount(patch.length), offset = 0;
    bool ok = true;
    for (size_t fragment = 0; fragment < count; ++fragment) {
        uint8_t parameters[253];
        size_t amount = patch.length - offset;
        if (amount > 252) amount = 252;
        parameters[0] = rtl_bt::fragmentIndex(fragment, count);
        memcpy(parameters + 1, data + offset, amount);
        if (!command(0xfc20, parameters, uint8_t(amount + 1), response, length) || length != 2 || response[1] != parameters[0]) {
            ok = false; break;
        }
        offset += amount;
    }
    IOFree(data, patch.length);
    if (!ok || !command(0x1001, nullptr, 0, response, length) || length != 9) return false;
    IOLog("RTL8723Bluetooth: firmware %08x uploaded (ROM %u, %lu bytes); HCI rev=%04x LMP=%04x\n",
          patch.version, rom, static_cast<unsigned long>(patch.length), rtl_bt::le16(response + 2), rtl_bt::le16(response + 7));
    setProperty("FirmwareVersion", patch.version, 32);
    setProperty("ROMVersion", rom, 8);
    return true;
}
bool RTL8723BluetoothFirmware::start(IOService *provider) {
    int enabled = 0;
    const bool bootOptIn = PE_parse_boot_argn("rtl8723be_bluetooth", &enabled, sizeof(enabled)) && enabled == 1;
    // Host opt-in only: the manual loader sets this on its temporary signed copy.
    // The shipped personalities have no upload opt-in and remain inert at boot.
    if (!bootOptIn && getProperty("FirmwareUploadEnabled") != kOSBooleanTrue) return false;
    if (!super::start(provider)) return false;
    device = OSDynamicCast(IOUSBHostDevice, provider);
    if (!device) { super::stop(provider); return false; }
    device->retain();
    if (!device->open(this)) { releaseUSB(); super::stop(provider); return false; }
    bool ok = configureUSB() && upload();
    if (ok) {
        device->setProperty("FirmwareLoaded", true);
        device->setProperty("RTL8723BluetoothFirmwareLoaded", true);
        device->setName("Bluetooth USB Host Controller");
        setProperty("FirmwareLoaded", true);
    }
    releaseUSB(); // Apple owns ACL, SCO, pairing and power control after handoff.
    if (!ok) { IOLog("RTL8723Bluetooth: upload failed; native handoff not advertised\n"); super::stop(provider); return false; }
    registerService();
    return true;
}
void RTL8723BluetoothFirmware::stop(IOService *provider) { releaseUSB(); super::stop(provider); }
void RTL8723BluetoothFirmware::free() { releaseUSB(); super::free(); }
