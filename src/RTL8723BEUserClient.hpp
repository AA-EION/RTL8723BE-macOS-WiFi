#ifndef _RTL8723BE_USER_CLIENT_HPP_
#define _RTL8723BE_USER_CLIENT_HPP_

#include <IOKit/IOUserClient.h>
#include "RTL8723BE_ipc.h"

class RTL8723BE;

class RTL8723BEUserClient : public IOUserClient {
    OSDeclareDefaultStructors(RTL8723BEUserClient);

public:
    virtual bool initWithTask(task_t owningTask, void *securityToken, UInt32 type) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual IOReturn clientClose() override;

    virtual IOReturn externalMethod(uint32_t selector,
                                    IOExternalMethodArguments *arguments,
                                    IOExternalMethodDispatch *dispatch,
                                    OSObject *target,
                                    void *reference) override;

    static const IOExternalMethodDispatch sMethods[kNumberOfMethods];

    // Method actions
    static IOReturn sScan(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sGetScanResults(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sConnect(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sDisconnect(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sGetStatus(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sSetAntenna(OSObject *target, void *reference, IOExternalMethodArguments *args);

private:
    RTL8723BE *fOwner;
    task_t     fTask;
};

#endif // _RTL8723BE_USER_CLIENT_HPP_
