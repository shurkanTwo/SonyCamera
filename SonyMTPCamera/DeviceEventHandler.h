#pragma once
#include "pch.h"
#include <PortableDeviceApi.h>
#include "Camera.h"
#include <mutex>

class DeviceEventHandler : public IPortableDeviceEventCallback
{
public:
    DeviceEventHandler(IPortableDevice* device, Camera* camera);
    ~DeviceEventHandler();
    HRESULT Stop();
    bool IsRegistered() const;

    HRESULT __stdcall QueryInterface(REFIID  riid, LPVOID* ppvObj);
    ULONG __stdcall AddRef();
    ULONG __stdcall Release();
    HRESULT __stdcall OnEvent(IPortableDeviceValues* pEventParameters);

private:
    volatile LONG m_cRef = 1;
    IPortableDevice* m_device = nullptr;
    Camera* m_camera;
    std::wstring m_cookie;
    std::mutex m_callbackMutex;
};

