#include "RTL8723BEUserClient.hpp"
#include "RTL8723BE.hpp"
#include <string.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(RTL8723BEUserClient, IOUserClient);

const IOExternalMethodDispatch RTL8723BEUserClient::sMethods[kNumberOfMethods] = {
    { // kMethodScan
        (IOExternalMethodAction)&RTL8723BEUserClient::sScan,
        0, 0,
        0, 0
    },
    { // kMethodGetScanResults
        (IOExternalMethodAction)&RTL8723BEUserClient::sGetScanResults,
        0, 0,
        0, sizeof(RTL8723BEScanResults)
    },
    { // kMethodConnect
        (IOExternalMethodAction)&RTL8723BEUserClient::sConnect,
        0, sizeof(RTL8723BEConnectParams),
        0, 0
    },
    { // kMethodDisconnect
        (IOExternalMethodAction)&RTL8723BEUserClient::sDisconnect,
        0, 0,
        0, 0
    },
    { // kMethodGetStatus
        (IOExternalMethodAction)&RTL8723BEUserClient::sGetStatus,
        0, 0,
        0, sizeof(RTL8723BEStatus)
    },
    { // kMethodSetAntenna
        (IOExternalMethodAction)&RTL8723BEUserClient::sSetAntenna,
        1, 0, // 1 scalar input (antenna: 1 or 2)
        0, 0
    },
    { // kMethodReadReg
        (IOExternalMethodAction)&RTL8723BEUserClient::sReadReg,
        2, 0,
        1, 0
    },
    { // kMethodWriteReg
        (IOExternalMethodAction)&RTL8723BEUserClient::sWriteReg,
        3, 0,
        0, 0
    },
    { // kMethodReadRF
        (IOExternalMethodAction)&RTL8723BEUserClient::sReadRF,
        1, 0,
        1, 0
    },
    { // kMethodWriteRF
        (IOExternalMethodAction)&RTL8723BEUserClient::sWriteRF,
        2, 0,
        0, 0
    },
    { // kMethodGetDiag
        (IOExternalMethodAction)&RTL8723BEUserClient::sGetDiag,
        0, 0,
        0, sizeof(RTL8723BEDiag)
    },
    { // kMethodIQCalibrate
        (IOExternalMethodAction)&RTL8723BEUserClient::sIQCalibrate,
        0, 0,
        1, 0
    }
};

bool RTL8723BEUserClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type) {
    if (!super::initWithTask(owningTask, securityToken, type)) {
        return false;
    }
    fTask = owningTask;
    fOwner = nullptr;
    // Register/RF writes can wedge the radio; only allow them for root clients.
    fIsAdmin = clientHasPrivilege(securityToken, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
    return true;
}

bool RTL8723BEUserClient::start(IOService *provider) {
    if (!super::start(provider)) {
        return false;
    }
    fOwner = OSDynamicCast(RTL8723BE, provider);
    if (!fOwner) {
        return false;
    }
    return true;
}

void RTL8723BEUserClient::stop(IOService *provider) {
    fOwner = nullptr;
    super::stop(provider);
}

IOReturn RTL8723BEUserClient::clientClose() {
    terminate();
    return kIOReturnSuccess;
}

IOReturn RTL8723BEUserClient::externalMethod(uint32_t selector,
                                             IOExternalMethodArguments *arguments,
                                             IOExternalMethodDispatch *dispatch,
                                             OSObject *target,
                                             void *reference) {
    if (selector >= kNumberOfMethods) {
        return kIOReturnBadArgument;
    }
    dispatch = const_cast<IOExternalMethodDispatch*>(&sMethods[selector]);
    if (!target) {
        target = this;
    }
    return super::externalMethod(selector, arguments, dispatch, target, reference);
}

IOReturn RTL8723BEUserClient::sScan(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    RTL8723BEUserClient *me = OSDynamicCast(RTL8723BEUserClient, target);
    if (!me || !me->fOwner) return kIOReturnNotAttached;
    return me->fOwner->startScan();
}

IOReturn RTL8723BEUserClient::sGetScanResults(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    RTL8723BEUserClient *me = OSDynamicCast(RTL8723BEUserClient, target);
    if (!me || !me->fOwner) return kIOReturnNotAttached;
    if (!args || !args->structureOutput || args->structureOutputSize < sizeof(RTL8723BEScanResults)) {
        return kIOReturnBadArgument;
    }
    RTL8723BEScanResults *out = (RTL8723BEScanResults*)args->structureOutput;
    return me->fOwner->getScanResults(out);
}

IOReturn RTL8723BEUserClient::sConnect(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    RTL8723BEUserClient *me = OSDynamicCast(RTL8723BEUserClient, target);
    if (!me || !me->fOwner) return kIOReturnNotAttached;
    if (!args || !args->structureInput || args->structureInputSize < sizeof(RTL8723BEConnectParams)) {
        return kIOReturnBadArgument;
    }
    const RTL8723BEConnectParams *params = (const RTL8723BEConnectParams*)args->structureInput;
    return me->fOwner->connect(params);
}

IOReturn RTL8723BEUserClient::sDisconnect(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    RTL8723BEUserClient *me = OSDynamicCast(RTL8723BEUserClient, target);
    if (!me || !me->fOwner) return kIOReturnNotAttached;
    return me->fOwner->disconnect();
}

IOReturn RTL8723BEUserClient::sGetStatus(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    RTL8723BEUserClient *me = OSDynamicCast(RTL8723BEUserClient, target);
    if (!me || !me->fOwner) return kIOReturnNotAttached;
    if (!args || !args->structureOutput || args->structureOutputSize < sizeof(RTL8723BEStatus)) {
        return kIOReturnBadArgument;
    }
    RTL8723BEStatus *status = (RTL8723BEStatus*)args->structureOutput;
    return me->fOwner->getStatus(status);
}

IOReturn RTL8723BEUserClient::sSetAntenna(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    RTL8723BEUserClient *me = OSDynamicCast(RTL8723BEUserClient, target);
    if (!me || !me->fOwner) return kIOReturnNotAttached;
    if (!args || args->scalarInputCount < 1) {
        return kIOReturnBadArgument;
    }
    uint8_t ant = (uint8_t)args->scalarInput[0];
    return me->fOwner->setAntennaPath(ant);
}

IOReturn RTL8723BEUserClient::sReadReg(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    RTL8723BEUserClient *me = OSDynamicCast(RTL8723BEUserClient, target);
    if (!me || !me->fOwner) return kIOReturnNotAttached;
    uint32_t value = 0;
    IOReturn ret = me->fOwner->debugReadReg((uint32_t)args->scalarInput[0],
                                            (uint32_t)args->scalarInput[1], &value);
    args->scalarOutput[0] = value;
    return ret;
}

IOReturn RTL8723BEUserClient::sWriteReg(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    RTL8723BEUserClient *me = OSDynamicCast(RTL8723BEUserClient, target);
    if (!me || !me->fOwner) return kIOReturnNotAttached;
    if (!me->fIsAdmin) return kIOReturnNotPrivileged;
    return me->fOwner->debugWriteReg((uint32_t)args->scalarInput[0], (uint32_t)args->scalarInput[1],
                                     (uint32_t)args->scalarInput[2]);
}

IOReturn RTL8723BEUserClient::sReadRF(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    RTL8723BEUserClient *me = OSDynamicCast(RTL8723BEUserClient, target);
    if (!me || !me->fOwner) return kIOReturnNotAttached;
    uint32_t value = 0;
    IOReturn ret = me->fOwner->debugReadRF((uint32_t)args->scalarInput[0], &value);
    args->scalarOutput[0] = value;
    return ret;
}

IOReturn RTL8723BEUserClient::sWriteRF(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    RTL8723BEUserClient *me = OSDynamicCast(RTL8723BEUserClient, target);
    if (!me || !me->fOwner) return kIOReturnNotAttached;
    if (!me->fIsAdmin) return kIOReturnNotPrivileged;
    return me->fOwner->debugWriteRF((uint32_t)args->scalarInput[0], (uint32_t)args->scalarInput[1]);
}

IOReturn RTL8723BEUserClient::sGetDiag(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    RTL8723BEUserClient *me = OSDynamicCast(RTL8723BEUserClient, target);
    if (!me || !me->fOwner) return kIOReturnNotAttached;
    if (!args || !args->structureOutput || args->structureOutputSize < sizeof(RTL8723BEDiag)) {
        return kIOReturnBadArgument;
    }
    return me->fOwner->getDiag((RTL8723BEDiag *)args->structureOutput);
}

IOReturn RTL8723BEUserClient::sIQCalibrate(OSObject *target, void *reference, IOExternalMethodArguments *args) {
    RTL8723BEUserClient *me = OSDynamicCast(RTL8723BEUserClient, target);
    if (!me || !me->fOwner) return kIOReturnNotAttached;
    if (!me->fIsAdmin) return kIOReturnNotPrivileged;
    uint32_t mask = 0;
    IOReturn ret = me->fOwner->runIQK(&mask);
    args->scalarOutput[0] = mask;
    return ret;
}
