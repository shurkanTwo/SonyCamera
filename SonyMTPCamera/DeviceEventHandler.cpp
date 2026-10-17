#include "pch.h"
#include "DeviceEventHandler.h"
#include <PortableDevice.h>
#include <PortableDeviceTypes.h>
#include <WpdMtpExtensions.h>
#include "Logger.h"
#include <wtypes.h>
#include <objbase.h>
#include <propvarutil.h>
#include "ResourceLoader.h"

#define DUMP_EVENT_PARAMS

DeviceEventHandler::DeviceEventHandler(IPortableDevice* device, Camera* camera)
    : m_device(device), m_camera(camera)
{
    if (!device) return;
    device->AddRef(); // Own the interface until Stop, including concurrent Close.
    LPWSTR cookie = nullptr;
    HRESULT hr = device->Advise(0, this, nullptr, &cookie);
    if (SUCCEEDED(hr) && cookie) m_cookie = cookie;
    else LOGERROR(L"Unable to register device notifications: %08x", hr);
    CoTaskMemFree(cookie);
}

DeviceEventHandler::~DeviceEventHandler()
{
    // Registration is stopped explicitly before the owner's reference is released.
    // A failed Unadvise may leave WPD holding us; Release controls final destruction.
    if (m_device) m_device->Release();
}

bool DeviceEventHandler::IsRegistered() const
{
    return !m_cookie.empty();
}

HRESULT DeviceEventHandler::Stop()
{
    IPortableDevice* device = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_camera = nullptr; // No callback may access Camera after this lock is released.
        device = m_device;
        m_device = nullptr;
        // Transfer the handler's owned interface reference to this local.
    }
    if (!device) return S_OK;
    // Outside the callback lock: Unadvise can itself deliver/release a callback.
    HRESULT hr = m_cookie.empty() ? S_OK : device->Unadvise(m_cookie.c_str());
    if (FAILED(hr)) LOGWARN(L"Unable to unregister device notifications: %08x", hr);
    device->Release();
    return hr;
}

HRESULT __stdcall 
DeviceEventHandler::QueryInterface(REFIID  riid, LPVOID* ppvObj)
{
    HRESULT hr = S_OK;

    if (ppvObj == NULL)
    {
        hr = E_INVALIDARG;
        return hr;
    }

    *ppvObj = nullptr;

    if ((riid == IID_IUnknown) ||
        (riid == IID_IPortableDeviceEventCallback))
    {
        AddRef();
        *ppvObj = this;
    }
    else
    {
        hr = E_NOINTERFACE;
    }

    return hr;
}

ULONG __stdcall
DeviceEventHandler::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

ULONG __stdcall
DeviceEventHandler::Release()
{
    ULONG remaining = InterlockedDecrement(&m_cRef);
    if (remaining == 0) delete this;
    return remaining;
}

HRESULT __stdcall 
DeviceEventHandler::OnEvent(IPortableDeviceValues* pEventParameters)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    if (!m_camera) return S_OK;

    LOGTRACE(L"DeviceEventHandler::OnEvent() - in");
    HRESULT hr = S_OK;

    if (pEventParameters != NULL)
    {
        LPWSTR pStrValue = nullptr;
        GUID guid = {};
        RPC_WSTR guidString;

        pEventParameters->GetStringValue(WPD_EVENT_PARAMETER_PNP_DEVICE_ID, &pStrValue);
        LOGTRACE(L"New event from PNP Device ID: %s", pStrValue ? pStrValue : L"<not supplied>");
        CoTaskMemFree(pStrValue);

        // May be pointless, as the event-type always seems to be 0x0000 in USB packet
        if (FAILED(pEventParameters->GetGuidValue(WPD_EVENT_PARAMETER_EVENT_ID, &guid))) return S_OK;

        if (IsEqualGUID(guid, WPD_EVENT_NOTIFICATION))
        {
            LOGTRACE(L"Event Type: Event Notification");
        }
        else if (IsEqualGUID(guid, WPD_EVENT_OBJECT_ADDED))
        {
            LOGTRACE(L"Event Type: Object Added");
        }
        else if (IsEqualGUID(guid, WPD_EVENT_OBJECT_REMOVED))
        {
            LOGTRACE(L"Event Type: Object Removed");
        }
        else if (IsEqualGUID(guid, WPD_EVENT_OBJECT_UPDATED))
        {
            LOGTRACE(L"Event Type: Object Updated");
        }
        else if (IsEqualGUID(guid, WPD_EVENT_DEVICE_RESET))
        {
            LOGTRACE(L"Event Type: Device Reset");
        }
        else if (IsEqualGUID(guid, WPD_EVENT_DEVICE_CAPABILITIES_UPDATED))
        {
            LOGTRACE(L"Event Type: Device Capabilities Updated");
        }
        else if (IsEqualGUID(guid, WPD_EVENT_STORAGE_FORMAT))
        {
            LOGTRACE(L"Event Type: Storage Format");
        }
        else if (IsEqualGUID(guid, WPD_EVENT_OBJECT_TRANSFER_REQUESTED))
        {
            LOGTRACE(L"Event Type: Object Transfer Requested");
        }
        else if (IsEqualGUID(guid, WPD_EVENT_DEVICE_REMOVED))
        {
            LOGTRACE(L"Event Type: Device Removed");
        }
        else if (IsEqualGUID(guid, WPD_EVENT_SERVICE_METHOD_COMPLETE))
        {
            LOGTRACE(L"Event Type: Service Method Complete");
        }
        else
        {
            // See if it's a Sony event
            // 1st two bytes
            DWORD sonyEventId = guid.Data1;

            if ((sonyEventId & 0xc2000000) == 0xc2000000 )
            {
                DWORD sonyId = sonyEventId >> 16;

                switch (sonyId)
                {
                case 0xc201:
                    LOGTRACE(L"0xc201 - photo(s) available");
                    m_camera->QueueImageBufferStatus(ImageBufferStatus::ImageReady);
                    break;

                case 0xc202:
                    LOGTRACE(L"0xc202 - no more photo(s) available");
                    m_camera->QueueImageBufferStatus(ImageBufferStatus::ImageNotReady);
                    break;

                case 0xc203:
                    LOGTRACE(L"0xc203 - property updated");
                    m_camera->OnPropertiesUpdated();
                    break;

                default:
                    LOGTRACE(L"Unknown sony event: 0x%04x", sonyId);
                    break;
                }
            }
            else {
                UuidToString(&guid, &guidString);
                LOGTRACE(L"Event Type: Unknown - %s 0x%08x", guidString, sonyEventId);
                RpcStringFree(&guidString);
            }
        }

#ifdef DUMP_EVENT_PARAMS
        IPortableDevicePropVariantCollection* pdpvc = nullptr;

        hr = pEventParameters->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_EVENT_PARAMS, &pdpvc);

        if (SUCCEEDED(hr) && pdpvc)
        {
            DWORD count = 0;
            pdpvc->GetCount(&count);

            for (int i = 0; i < (int)count; i++)
            {
                LOGTRACE(L"--- Property %d", i);
                PROPVARIANT val;
                PropVariantInit(&val);
                if (FAILED(pdpvc->GetAt(i, &val))) continue;

                if (val.vt == VT_UI4)
                {
                    LOGTRACE(L"    UI4 => x%08x (%s)", val.uiVal, ResourceLoader::GetString(val.uiVal).c_str());
                }
                else
                {
                    LOGTRACE(L"    type = %d", val.vt);
                }
                PropVariantClear(&val);
            }
        }

        if (pdpvc) pdpvc->Release();
#endif
    }

    LOGTRACE(L"DeviceEventHandler::OnEvent() - out, returning %08x", hr);

    return hr;
}
