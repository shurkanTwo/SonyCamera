#include "pch.h"
#include "Camera.h"
#include "Registry.h"
#include "Logger.h"
#include "CameraException.h"
#include <iostream>

constexpr auto THREAD_WAIT_EXIT_LOOPS = 20;
constexpr auto THREAD_WAIT_EXIT_SLEEP = 100;
constexpr auto MUTEX_TIMEOUT = 10000;

Camera::Camera(Device* device)
    : m_device(device)
{
    m_hBusyMutex = CreateMutex(nullptr, false, nullptr);
    pWorker = new CameraWorker(this);
}

Camera::~Camera()
{
    m_device->StopNotifications();
    delete pWorker;
    pWorker = nullptr;

    if (m_settings)
    {
        delete m_settings;
        m_settings = nullptr;
    }

    CloseHandle(m_hBusyMutex);

    m_hBusyMutex = INVALID_HANDLE_VALUE;
}

CameraWorker*
Camera::GetWorker()
{
    return pWorker;
}

const std::wstring
Camera::GetId() const
{
    return m_device->GetId();
}

void
Camera::SetInitialized(bool value)
{
    initialized = value;
}

bool
Camera::IsInitialized() const
{
    return initialized;
}

void
Camera::RecordSettingsRefreshResult(bool succeeded)
{
    if (succeeded)
    {
        if (InterlockedExchange(&m_consecutiveRefreshFailures, 0) >= DISCONNECTED_REFRESH_FAILURES)
        {
            LOGINFO(L"Camera is answering again after being treated as disconnected");
        }
    }
    else
    {
        if (InterlockedIncrement(&m_consecutiveRefreshFailures) == DISCONNECTED_REFRESH_FAILURES)
        {
            LOGWARN(L"%d settings refreshes have failed in a row - treating the camera as disconnected and stopping the idle refresh until it answers again", DISCONNECTED_REFRESH_FAILURES);
        }
    }
}

bool
Camera::LooksDisconnected() const
{
    // The device saying outright that it has gone is worth more than any
    // number of failures - act on the first one, not the fifth.
    if (m_device && m_device->IsGone())
    {
        return true;
    }

    return m_consecutiveRefreshFailures >= DISCONNECTED_REFRESH_FAILURES;
}

void
Camera::OnPropertiesUpdated()
{
    RefreshPropertiesTask* task = new RefreshPropertiesTask(false);

    if (!task->QueueAndForget(this, 0))
    {
        LOGTRACE(L"Removing unqueued task");
        // It didn't get queued
        delete task;
    }
}

namespace
{
    class ImageBufferNotificationTask : public CameraTask
    {
    public:
        ImageBufferNotificationTask(ImageBufferStatus status)
            : CameraTask(L"ImageBufferNotification")
        {
            SetParam1(reinterpret_cast<void*>(static_cast<UINT_PTR>(status)));
            StateStepInfo step = {};
            step.id = 0;
            step.isTerminal = true;
            step.handler = [](CameraTaskInfo& info) {
                info.pCamera->OnImageBufferStatus(static_cast<ImageBufferStatus>(
                    reinterpret_cast<UINT_PTR>(info.pTask->GetParam1())));
                return StateResult::Success;
            };
            AddStep(step);
        }
    };
}

void
Camera::QueueImageBufferStatus(ImageBufferStatus status)
{
    auto* task = new ImageBufferNotificationTask(status);
    if (!task->QueueAndForget(this, 0)) delete task;
}

void
Camera::OnImageBufferStatus(ImageBufferStatus status)
{
    LOGINFO(L"Image Buffer Status Update %d", status);

    if (status == ImageBufferStatus::ImageReady)
    {
        if (!m_takePhotoTaskParams) {
            m_takePhotoTaskParams = new TakePhotoTaskParams(0, OutputMode::PASSTHRU);
        }

        m_downloadAndProcessImageTask = new DownloadAndProcessImageTask(m_takePhotoTaskParams);

        m_downloadAndProcessImageTask->Start(this);
    }
}

HANDLE
Camera::Open()
{
    pWorker->OnCameraStateChange(CameraState::Connecting);

    HANDLE hCamera = m_device->Open();

    if (hCamera != INVALID_HANDLE_VALUE)
    {
        if (m_device->GetOpenCount() == 1)
        {
            m_device->StartNotifications(this);
        }

        pWorker->OnCameraStateChange(CameraState::Connected);
        RefreshDeviceInfo();
    }

    return hCamera;
}

bool
Camera::Close()
{
    if (m_device->GetOpenCount() == 1)
    {
        m_device->StopNotifications();
    }

    bool result = m_device->Close();

    if (result)
    {
        SetInitialized(false);
    }

    pWorker->OnCameraStateChange(CameraState::Disconnected);

    return result;
}

const DeviceInfo
Camera::GetDeviceInfo() const
{
    return m_deviceInfo;
}

bool
Camera::RefreshDeviceInfo()
{
    HANDLE handle = m_device->Open();
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return false;

    Message* tx;
    Message* rx;

    tx = new Message(COMMAND_GET_DEVICE_INFO);
    rx = m_device->Receive(tx);

    LOGTRACE(L"GetDeviceInfo() >> %s", rx->Dump().c_str());

    DeviceInfo newDeviceInfo(rx);

    delete tx;
    delete rx;

    m_device->Close();

    ProcessDeviceInfoOverrides(newDeviceInfo);
    newDeviceInfo.DumpToLog();

    DWORD waitResult = WaitForSingleObject(m_hBusyMutex, 1000);

    if (waitResult == WAIT_OBJECT_0)
    {
        m_deviceInfo = newDeviceInfo;
    }

    ReleaseMutex(m_hBusyMutex);

    return true;
}

void
Camera::LoadFakeProperties(CameraSettings* settings) const
{
    // Add Magic infos
    CameraPropertyFactory f;

    CameraProperty* p = f.Create(Property::BatteryTemperature);
    p->SetCurrentValue(new PropertyValue((INT16)0xffff));

    PropertyInfo* inf = new PropertyInfo();

    inf->SetDefault(new PropertyValue((INT16)0xffff));
    inf->SetId(Property::BatteryTemperature);
    inf->SetType(DataType::INT16);
    inf->SetFormMode(FormMode::NONE);

    p->SetInfo(inf);

    settings->AddProperty(p);

    p = f.Create(Property::FocusPosition);
    p->SetCurrentValue(new PropertyValue((UINT16)0xffff));

    inf = new PropertyInfo();

    inf->SetDefault(new PropertyValue((UINT16)0xffff));
    inf->SetId(Property::FocusPosition);
    inf->SetType(DataType::UINT16);
    inf->SetFormMode(FormMode::NONE);

    p->SetInfo(inf);

    settings->AddProperty(p);

    p = f.Create(Property::PossibleExposureTimes);
    DeviceInfo deviceInfo = GetDeviceInfo();
    p->SetCurrentValue(new PropertyValue(0));

    inf = new PropertyInfo();

    inf->SetDefault(new PropertyValue((UINT32)0));
    inf->SetId(Property::PossibleExposureTimes);
    inf->SetType(DataType::UINT32);
    inf->SetFormMode(FormMode::ENUMERATION);

    std::list<PropertyValue*> values;
    std::list<DWORD> et = deviceInfo.GetExposureTimes();

    for (auto pv: et)
    {
        values.push_back(new PropertyValue((UINT32)pv));
    }

    inf->SetEnumeration(values);
    p->SetInfo(inf);

    settings->AddProperty(p);

    p = f.Create(Property::PossibleISOs);

    p->SetCurrentValue(new PropertyValue(0));

    inf = new PropertyInfo();

    inf->SetDefault(new PropertyValue((UINT32)0));
    inf->SetId(Property::PossibleISOs);
    inf->SetType(DataType::UINT32);
    inf->SetFormMode(FormMode::ENUMERATION);

    values.clear();

    et = deviceInfo.GetISOs();

    for (auto pv: et)
    {
        values.push_back(new PropertyValue((UINT32)pv));
    }

    inf->SetEnumeration(values);
    p->SetInfo(inf);
    settings->AddProperty(p);

    // Some cameras do not return info for the shutter control "buttons"
    // if they're missing, lets create some fake ones that always return "up"
    // but mimic the expected button-type so other pieces of the code can
    // continue to work
    if (!settings->GetProperty(Property::ShutterHalfDown))
    {
        LOGTRACE(L"Adding fake 'ShutterHalfDown' property as camera forgot to enumerate");
        p = f.Create(Property::ShutterHalfDown);

        p->SetCurrentValue(new PropertyValue((UINT8)0));

        inf = new PropertyInfo();

        inf->SetDefault(new PropertyValue((UINT8)0));
        inf->SetId(Property::ShutterHalfDown);
        inf->SetType(DataType::UINT8);
        inf->SetFormMode(FormMode::NONE);
        inf->SetAccess(Accessibility::WRITE_ONLY_BUTTON);
        inf->SetSonySpare(1);
        p->SetInfo(inf);

        settings->AddProperty(p);
    }

    if (!settings->GetProperty(Property::ShutterFullDown))
    {
        LOGTRACE(L"Adding fake 'ShutterFullDown' property as camera forgot to enumerate");
        p = f.Create(Property::ShutterFullDown);

        p->SetCurrentValue(new PropertyValue((UINT8)0));

        inf = new PropertyInfo();

        inf->SetDefault(new PropertyValue((UINT8)0));
        inf->SetId(Property::ShutterFullDown);
        inf->SetType(DataType::UINT8);
        inf->SetFormMode(FormMode::NONE);
        inf->SetAccess(Accessibility::WRITE_ONLY_BUTTON);
        inf->SetSonySpare(1);
        p->SetInfo(inf);
        settings->AddProperty(p);
    }

    // Some cameras don't return the focus-control property
    if (!settings->GetProperty(Property::FocusControl))
    {
        LOGTRACE(L"Adding fake 'ShutterControl' property as camera forgot to enumerate");
        p = f.Create(Property::FocusControl);

        p->SetCurrentValue(new PropertyValue((INT16)0));

        inf = new PropertyInfo();

        inf->SetDefault(new PropertyValue((UINT16)0));
        inf->SetId(Property::FocusControl);
        inf->SetType(DataType::INT16);
        inf->SetFormMode(FormMode::RANGE);
        inf->SetAccess(Accessibility::UNKNOWN_82);
        inf->SetSonySpare(1);
        inf->SetRange(new PropertyValue((UINT16)1), new PropertyValue((UINT16)7), new PropertyValue((UINT16)0xfff9));
        p->SetInfo(inf);
        settings->AddProperty(p);
    }
}

Device *
Camera::GetDevice()
{
    return m_device;
}

bool
Camera::ProcessDeviceInfoOverrides(DeviceInfo& deviceInfo)
{
    std::wostringstream builder;

    builder << L"Cameras\\" << m_device->GetRegistryPath();

    std::wstring cameraPath = builder.str();

    registry.Open();

    deviceInfo.SetSensorName(registry.GetString(cameraPath, L"Sensor Name", deviceInfo.GetSensorName()));
    deviceInfo.SetSensorPixelWidth(registry.GetDouble(cameraPath, L"Sensor X Size um", deviceInfo.GetSensorPixelWidth()));
    deviceInfo.SetSensorPixelHeight(registry.GetDouble(cameraPath, L"Sensor Y Size um", deviceInfo.GetSensorPixelHeight()));
    deviceInfo.SetSensorXResolution(registry.GetDWORD(cameraPath, L"Sensor X Resolution", deviceInfo.GetSensorXResolution()));
    deviceInfo.SetSensorYResolution(registry.GetDWORD(cameraPath, L"Sensor Y Resolution", deviceInfo.GetSensorYResolution()));
    deviceInfo.SetSensorXCroppedResolution(registry.GetDWORD(cameraPath, L"AutoCropped X Resolution", deviceInfo.GetSensorXCroppedResolution()));
    deviceInfo.SetSensorYCroppedResolution(registry.GetDWORD(cameraPath, L"AutoCropped Y Resolution", deviceInfo.GetSensorYCroppedResolution()));
    deviceInfo.SetPreviewXResolution(registry.GetDWORD(cameraPath, L"Preview X Resolution", deviceInfo.GetPreviewXResolution()));
    deviceInfo.SetPreviewYResolution(registry.GetDWORD(cameraPath, L"Preview Y Resolution", deviceInfo.GetPreviewYResolution()));
    deviceInfo.SetExposureTimeMin(registry.GetDouble(cameraPath, L"Exposure Time Min", deviceInfo.GetExposureTimeMin()));
    deviceInfo.SetExposureTimeMax(registry.GetDouble(cameraPath, L"Exposure Time Max", deviceInfo.GetExposureTimeMax()));
    deviceInfo.SetExposureTimeStep(registry.GetDouble(cameraPath, L"Exposure Time Step", deviceInfo.GetExposureTimeStep()));
    deviceInfo.SetSensorType((SensorType)registry.GetDWORD(cameraPath, L"Sensor Type", (DWORD)deviceInfo.GetSensorType()));
    deviceInfo.SetSupportsLiveview((bool)registry.GetDWORD(cameraPath, L"Supports Liveview", deviceInfo.GetSupportsLiveview()));
    deviceInfo.SetCropMode((CropMode)registry.GetDWORD(cameraPath, L"Crop Mode", (DWORD)deviceInfo.GetCropMode()));
    deviceInfo.SetLeftCrop((UINT16)registry.GetDWORD(cameraPath, L"Crop Left", deviceInfo.GetLeftCrop()));
    deviceInfo.SetRightCrop((UINT16)registry.GetDWORD(cameraPath, L"Crop Right", deviceInfo.GetRightCrop()));
    deviceInfo.SetTopCrop((UINT16)registry.GetDWORD(cameraPath, L"Crop Top", deviceInfo.GetTopCrop()));
    deviceInfo.SetBottomCrop((UINT16)registry.GetDWORD(cameraPath, L"Crop Bottom", deviceInfo.GetBottomCrop()));
    deviceInfo.SetButtonPropertiesInverted((bool)registry.GetDWORD(cameraPath, L"Button Properties Inverted", deviceInfo.GetButtonPropertiesInverted()));
    deviceInfo.SetBitsPerPixel(registry.GetDWORD(cameraPath, L"Bits Per Pixel", deviceInfo.GetBitsPerPixel()));
//    deviceInfo.SetFocusLimit((UINT16)registry.GetDWORD(cameraPath, L"Focus Limit", deviceInfo.GetFocusLimit()));
//    deviceInfo.SetFocusMagicNumber(registry.GetDouble(cameraPath, L"Focus Magic Number", deviceInfo.GetFocusMagicNumber()));
    deviceInfo.SetFocusStartMode((FocusStartMode)registry.GetDWORD(cameraPath, L"Focus Start Mode", (DWORD)deviceInfo.GetFocusStartMode()));
//    deviceInfo.SetHandsOffFocus(registry.GetDWORD(cameraPath, L"Hands Off Focus", deviceInfo.GetHandsOffFocus() ? 1 : 0) ? true : false);

    std::wstring s;
    std::wistringstream exposureTimes(registry.GetString(cameraPath, L"Exposure Times", L""));
    std::list<DWORD> times;

    while (std::getline(exposureTimes, s, L',')) {
        times.push_back(_wtoi(s.c_str()));
    }

    deviceInfo.SetExposureTimes(times);

    std::wistringstream isos(registry.GetString(cameraPath, L"ISOs", L""));
    std::list<DWORD> iso;

    while (std::getline(isos, s, L',')) {
        iso.push_back(_wtoi(s.c_str()));
    }

    deviceInfo.SetISOs(iso);

    registry.Close();

    return true;
}

ObjectInfo*
Camera::GetImageInfo(DWORD id)
{
    LOGTRACE(L"In: Camera::GetImageInfo(x%08x)", id);

    Message* tx;
    Message* rx;

    tx = new Message(COMMAND_GET_OBJECT_INFO);
    tx->AddParam(id);

    rx = m_device->Receive(tx);

    // Only parse a response that actually carries one.  When the camera has
    // gone the send fails and comes back empty, and ObjectInfo then reads
    // field after field off the end of the buffer - every one of them logged
    // as "would extend beyond end of data".  Returning null instead lets the
    // caller give up, which is what it does with a null image anyway.
    ObjectInfo* info = nullptr;

    if (rx && rx->IsSuccess() && rx->GetDataLen() > 0)
    {
        info = new ObjectInfo(rx);
    }
    else
    {
        LOGWARN(L"No object info returned for image x%08x - the camera did not answer", id);
    }

    delete tx;
    delete rx;

    LOGTRACE(L"Out: Camera::GetImageInfo(x%08x)", id);

    return info;
}

Image*
Camera::GetImage(DWORD id)
{
    LOGTRACE(L"In: Camera::GetImage(x%08x)", id);

    Image* image = nullptr;
    ObjectInfo* info = GetImageInfo(id);

    if (info == nullptr)
    {
        // The camera did not answer at all, so there is nothing to fetch.
        // Checked before the dereference below, which would otherwise fault.
        LOGWARN(L"Unable to fetch image x%08x - no object info", id);
    }
    else if (info->GetCompressedSize())
    {
        Message* tx;
        Message* rx;

        tx = new Message(COMMAND_GET_OBJECT);
        tx->AddParam(id);
        rx = m_device->Receive(tx);

        if (rx && rx->IsSuccess() && rx->GetDataLen() > 0)
        {
            image = new Image(info, rx);
        }

        delete rx;
        delete tx;
    }
    else
    {
        // Failed
        LOGWARN(L"Unable to fetch image x%08x", id);
    }

    if (!image) delete info;

    LOGTRACE(L"Out: Camera::GetImage(x%08x)", id);

    return image;
}

void
Camera::DoRefreshProperties()
{
    RefreshPropertiesTask rpt(false);

    rpt.Run(this);
}

bool
Camera::StartCapture(double duration, OutputMode outputMode, DWORD flags)
{
    LOGTRACE(L"In: Camera::StartCapture(duration = %f, output = %d)", duration,(DWORD)outputMode);

    bool result = false;

    // Set when a task from the previous capture refuses to cancel. While such a
    // task is still running nothing of its state may be reset or freed, and the
    // caller must not be told the camera is ready for a fresh capture.
    bool previousWorkOutstanding = false;

    DWORD waitResult = WaitForSingleObject(m_hBusyMutex, 1000);

    if (waitResult == WAIT_OBJECT_0)
    {
        if (m_takePhotoTask)
        {
            {
                LOGINFO(L"There's a pre-existing capture, attempting to clean up");
                if (m_takePhotoTask->Cancel(m_previousTaskCancelTimeoutMs) == WAIT_OBJECT_0)
                {
                    // Task successfully cancelled
                    delete m_takePhotoTask;
                    m_takePhotoTask = nullptr;
                }
                else
                {
                    LOGERROR(L"The pre-existing capture would not cancel; leaving it in place");
                    previousWorkOutstanding = true;
                }
            }
        }

        if (m_downloadAndProcessImageTask)
        {
            {
                LOGINFO(L"There's a pre-existing download, attempting to clean up");
                if (m_downloadAndProcessImageTask->Cancel(m_previousTaskCancelTimeoutMs) == WAIT_OBJECT_0)
                {
                    // Task successfully cancelled
                    delete m_downloadAndProcessImageTask;
                    m_downloadAndProcessImageTask = nullptr;
                }
                else
                {
                    LOGERROR(L"The pre-existing download would not cancel; leaving it in place");
                    previousWorkOutstanding = true;
                }
            }
        }

        // TakePhotoTask keeps this pointer for as long as it runs, so freeing
        // it while a task that would not cancel is still alive is a
        // use-after-free in precisely the case that matters.
        if (m_takePhotoTaskParams && !previousWorkOutstanding)
        {
            delete m_takePhotoTaskParams;
            m_takePhotoTaskParams = nullptr;
        }

        if (!previousWorkOutstanding)
        {
            SetCaptureStatus(CaptureStatus::Created);
        }
    }

    ReleaseMutex(m_hBusyMutex);

    // Reported only once the mutex is released, so the failure path cannot leak
    // it. The status used to be set to Created either way, which told the
    // caller a fresh capture was pending while the previous download was still
    // running - and the next shutter release then went out on top of it.
    if (previousWorkOutstanding)
    {
        throw CameraException(L"Previous capture is still running; not starting another");
    }

    // Get camera settings... this serves two purposes
    // 1. Ensure camera is in a state that will allow a photo to be taken
    // 2. Allows us to clean out any "pending" images that could have been
    //    created by user pressing shutter button
    PropertyValue up((UINT16)(GetDeviceInfo().GetButtonPropertiesInverted() ? 2 : 1));

    LOGTRACE(L"Up value is %d (%s)", up.GetUINT16(), up.ToString().c_str());
    LOGTRACE(L"Getting latest camera settings");

    DoRefreshProperties();

    std::unique_ptr<CameraSettings> cs(GetSettings());

    // Ensure the shutter control properties are correct

    // These null checks are necessary as some models advertise the property
    // but then don't actually return it.
    // If there are too many I may add some code to support default values
    // so we don't need all these checks.
    PropertyValue* v = cs->GetPropertyValue(Property::ShutterFullDown);

    if (!v || v->GetUINT16() != up.GetUINT16())
    {
        LOGWARN(L"ShutterFullDown is %s, clearing", v ? L"set" : L"unreported");

        SetProperty(Property::ShutterFullDown, &up);

        // Re-fetch settings
        DoRefreshProperties();
    }

    std::unique_ptr<CameraSettings> cs2(GetSettings());
    v = cs2->GetPropertyValue(Property::ShutterHalfDown);

    if (!v || v->GetUINT16() != up.GetUINT16())
    {
        LOGWARN(L"ShutterHalfDown is %s, clearing", v ? L"set" : L"unreported");
        PropertyValue up((WORD)1);

        SetProperty(Property::ShutterHalfDown, &up);

        // Re-fetch settings
        DoRefreshProperties();
    }

    // Clear any photo the camera is already holding, whether the user took it
    // with the shutter button or a previous download left it behind. Releasing
    // the shutter while an image is still pending is what wedges the camera, so
    // this has to actually succeed before the capture goes ahead.
    //
    // Three defects used to live in this loop, and the first two together meant
    // that entering it was the same as never leaving it:
    //
    //   - `WORD bufferStatus = ...->GetUINT16() != 0` assigned the result of the
    //     comparison, not the status word, because != binds tighter than =. So
    //     bufferStatus was only ever 0 or 1, and `bufferStatus & 0x8000` - the
    //     branch that actually retrieves the pending photo - was unreachable.
    //   - the condition re-read a CameraSettings captured before the loop, and
    //     GetSettings() returns a copy. The re-fetch in the body declared a new
    //     `cs` that shadowed it and was then discarded, so DoRefreshProperties()
    //     could never change the value the condition tested.
    //   - PhotoBufferStatus was dereferenced without the null check the rest of
    //     this function applies, and some models advertise a property without
    //     ever returning it.
    //
    // The timeout exists so that a camera which reports a pending photo it will
    // not hand over fails the capture instead of blocking the caller forever.
    const DWORD drainTimeoutMs = m_pendingPhotoDrainTimeoutMs;
    const ULONGLONG drainStartedAt = GetTickCount64();

    for (;;)
    {
        std::unique_ptr<CameraSettings> drainSettings(GetSettings());
        PropertyValue* bufferValue = drainSettings ? drainSettings->GetPropertyValue(Property::PhotoBufferStatus) : nullptr;

        if (!bufferValue)
        {
            LOGWARN(L"PhotoBufferStatus is unreported; assuming the camera holds no photo");
            break;
        }

        WORD bufferStatus = bufferValue->GetUINT16();

        if (bufferStatus == 0)
        {
            break;
        }

        if (bufferStatus & 0x8000)
        {
            // Photo is ready... we don't actually want it, so we'll just toss whatever we get back
            LOGTRACE(L"  Previous photo is ready to retrieve (buffer status x%04x)", bufferStatus);
            Image* iimage = GetImage(FULL_IMAGE);

            if (iimage)
            {
                delete iimage;
            }

            LOGTRACE(L"  Previous photo retrieved and discarded");
        }
        else
        {
            // Camera is busy getting a photo ready for us
            LOGTRACE(L"  Waiting for camera to have previous photo ready (buffer status x%04x)", bufferStatus);
            Sleep(100);
        }

        if (GetTickCount64() - drainStartedAt > drainTimeoutMs)
        {
            LOGERROR(L"Camera still reports a pending photo (buffer status x%04x) after %dms; not releasing the shutter on top of it", bufferStatus, drainTimeoutMs);
            throw CameraException(L"Camera is still holding a previous photo");
        }

        // Re-fetch settings
        DoRefreshProperties();
    }

    cs = std::unique_ptr<CameraSettings>(GetSettings());

    // Null checks for the same reason as the ones above: a model may advertise
    // a property and then never return it, and these two were dereferenced
    // unconditionally - a crash on the line after the loop this change fixes.
    PropertyValue* shutterButton = cs->GetPropertyValue(Property::ShutterButtonStatus);

    if (!shutterButton)
    {
        LOGERROR(L"StartCapture: ShutterButtonStatus is unreported; cannot tell whether the shutter is free");
        throw CameraException(L"Could not read shutter button status");
    }

    if (shutterButton->GetUINT8() == 1)
    {
        LOGTRACE(L"  Shutter button is UP");

        PropertyValue* exposureTime = cs->GetPropertyValue(Property::ShutterSpeed);

        if (!exposureTime || exposureTime->GetType() == DataType::UNKNOWN)
        {
            throw CameraException(L"Could not fetch exposure time");
        }

        DWORD exposureTimeRaw = exposureTime->GetUINT32();

        if (exposureTimeRaw == 0)
        {
            LOGTRACE(L"  Shutter time is BULB, using supplied duration %f", duration);
        }
        else
        {
            double requestedDuration = duration;
            duration = ((double)(exposureTimeRaw >> 16) / (double)(exposureTimeRaw & 0x0000ffff));
            LOGTRACE(L"  Shutter time is %s (%f s), we wanted (%f s) - letting camera figure it out", exposureTime->ToString().c_str(), duration, requestedDuration);
        }

        m_takePhotoTaskParams = new TakePhotoTaskParams(duration, outputMode);
        m_takePhotoTask = new TakePhotoTask(m_takePhotoTaskParams);

        m_takePhotoTask->Start(this);

        result = true;
    }
    else
    {
        LOGWARN(L"StartCapture: Cannot start capture as someone has their finger on the shutter button (%s)", shutterButton->ToString().c_str());
        throw CameraException(L"Cannot start capture as shutter already open");
    }

    LOGTRACE(L"Out: Camera::StartCapture() - returning %d", result);

    return result;
}

CameraSettings*
Camera::GetSettings() const
{
    DWORD waitResult = WaitForSingleObject(m_hBusyMutex, 1000);
    CameraSettings* result = nullptr;

    if (waitResult == WAIT_OBJECT_0)
    {
        result = new CameraSettings(*m_settings);
    }

    ReleaseMutex(m_hBusyMutex);

    return result;
}

CameraProperty*
Camera::GetProperty(Property id) const
{
    DWORD waitResult = WaitForSingleObject(m_hBusyMutex, 1000);
    CameraProperty* result = nullptr;

    if (waitResult == WAIT_OBJECT_0)
    {
        CameraProperty* existing = m_settings->GetProperty(id);

        if (existing)
        {
            result = existing->Clone();
        }
    }

    ReleaseMutex(m_hBusyMutex);

    return result;
}

void
Camera::SetCaptureStatus(CaptureStatus status)
{
    LOGTRACE(L"Set capture status to: %d", status);
    DWORD waitResult = WaitForSingleObject(m_hBusyMutex, 1000);

    if (waitResult == WAIT_OBJECT_0)
    {
        m_captureStatus = status;
    }

    ReleaseMutex(m_hBusyMutex);
}

const CaptureStatus
Camera::GetCaptureStatus()
{
    CaptureStatus result = CaptureStatus::Failed;

    DWORD waitResult = WaitForSingleObject(m_hBusyMutex, 1000);

    if (waitResult == WAIT_OBJECT_0)
    {
        result = m_captureStatus;
    }

    ReleaseMutex(m_hBusyMutex);

    switch (result)
    {
    case CaptureStatus::Complete:
        LOGTRACE(L"Camera::GetCaptureStatus() - Capture Complete");
        break;

    case CaptureStatus::Cancelled:
        LOGTRACE(L"Camera::GetCaptureStatus() - Capture Cancelled");
        break;

    case CaptureStatus::Capturing:
        LOGTRACE(L"Camera::GetCaptureStatus() - Capture Capturing");
        break;

    case CaptureStatus::Created:
        LOGTRACE(L"Camera::GetCaptureStatus() - Capture Created (not started yet)");
        break;

    case CaptureStatus::Failed:
        LOGTRACE(L"Camera::GetCaptureStatus() - Capture Failed");
        break;

    case CaptureStatus::Processing:
        LOGTRACE(L"Camera::GetCaptureStatus() - Capture Processing Data");
        break;

    case CaptureStatus::Reading:
        LOGTRACE(L"Camera::GetCaptureStatus() - Capture Reading From Camera");
        break;

    case CaptureStatus::Starting:
        LOGTRACE(L"Camera::GetCaptureStatus() - Capture Starting");
        break;

    default:
        LOGWARN(L"Camera::GetCaptureStatus() - Unknown Capture State %d", result);
        break;
    }

    return result;
}

Image*
Camera::GetCapturedImage()
{
    DWORD waitResult = WaitForSingleObject(m_hBusyMutex, 1000);
    Image* image = nullptr;

    if (waitResult == WAIT_OBJECT_0)
    {
        if (m_downloadAndProcessImageTask && GetCaptureStatus() == CaptureStatus::Complete)//m_downloadAndProcessImageTask && m_downloadAndProcessImageTask->IsComplete())
        {
            image = m_downloadAndProcessImageTask->GetImage();
        }
    }

    ReleaseMutex(m_hBusyMutex);

    if (image)
    {
        // Update a couple of settings based on some EXIF data read from the image
        DWORD waitResult = WaitForSingleObject(m_hBusyMutex, 1000);
        CameraSettings* result = nullptr;

        if (waitResult == WAIT_OBJECT_0)
        {
            LOGTRACE(L"Updating temperature and focus properties from EXIF");

            CameraProperty* existing = m_settings->GetProperty(Property::BatteryTemperature);

            if (existing)
            {
                existing->SetCurrentValue(new PropertyValue((INT16)(image->GetBatteryTemperature() * 10)));
            }
            else
            {
                LOGERROR(L"Unable to find BatteryTemperature property");
            }

            existing = m_settings->GetProperty(Property::FocusPosition);

            if (existing)
            {
                existing->SetCurrentValue(new PropertyValue((UINT16)image->GetFocusPosition()));
            }
            else
            {
                LOGERROR(L"Unable to find FocusPosition property");
            }
        }

        ReleaseMutex(m_hBusyMutex);

        return image;
    }

    throw CameraException(L"No active capture, therefore no image to retrieve");
}

bool
Camera::CancelCapture()
{
    // Two callers, both of which need the capture to end up in a terminal
    // state: the CancelCapture export, when the host abandons an exposure, and
    // CleanupCapture, after a frame has been retrieved successfully.
    // GetCaptureStatus is what a host polls to decide whether another exposure
    // may start, so a capture left non-terminal refuses every later frame until
    // the camera is reconnected - one lost frame becomes a lost session.
    //
    // Two defects made this unusable for the abandonment case, which is why
    // native cancellation had to be turned off:
    //
    //   - m_takePhotoTask was deleted whether or not it had actually cancelled,
    //     so abandoning a hung exposure freed an object the worker thread was
    //     still running. A task that will not cancel is now left in place;
    //     StartCapture already refuses while one is there, which is a refused
    //     frame rather than an access violation.
    //   - m_downloadAndProcessImageTask was never touched, so a download still
    //     in flight survived the cancel. It is now cancelled on the same terms,
    //     which also makes good on "this deletes image too" in the comment at
    //     the GetCaptureStatus call site: the Image belongs to that task, and
    //     by the time CleanupCapture runs its bytes have already been copied
    //     out by exportBytes.
    bool cancelledCleanly = true;

    DWORD waitResult = WaitForSingleObject(m_hBusyMutex, 1000);

    if (waitResult == WAIT_OBJECT_0)
    {
        if (m_takePhotoTask)
        {
            if (m_takePhotoTask->Cancel(m_previousTaskCancelTimeoutMs) == WAIT_OBJECT_0)
            {
                delete m_takePhotoTask;
                m_takePhotoTask = nullptr;
            }
            else
            {
                LOGERROR(L"CancelCapture: the capture task would not cancel; leaving it in place rather than freeing it under the worker");
                cancelledCleanly = false;
            }
        }

        if (m_downloadAndProcessImageTask)
        {
            if (m_downloadAndProcessImageTask->Cancel(m_previousTaskCancelTimeoutMs) == WAIT_OBJECT_0)
            {
                delete m_downloadAndProcessImageTask;
                m_downloadAndProcessImageTask = nullptr;
            }
            else
            {
                LOGERROR(L"CancelCapture: the download task would not cancel; leaving it in place rather than freeing it under the worker");
                cancelledCleanly = false;
            }
        }

        // Held by TakePhotoTask for as long as it runs, so only safe to free
        // once nothing that could still read it is alive.
        if (cancelledCleanly && m_takePhotoTaskParams)
        {
            delete m_takePhotoTaskParams;
            m_takePhotoTaskParams = nullptr;
        }

        // Marked terminal in every case, including when a task refused to
        // cancel: the host has given up on this exposure either way, and
        // leaving the status non-terminal is the difference between losing a
        // frame and losing a night. Set while the mutex is still held - the
        // handle is recursive - so a capture started in between cannot be
        // marked cancelled by mistake.
        SetCaptureStatus(CaptureStatus::Cancelled);

        ReleaseMutex(m_hBusyMutex);
    }
    else
    {
        LOGERROR(L"CancelCapture: could not take the busy mutex; marking the capture cancelled regardless");
        SetCaptureStatus(CaptureStatus::Cancelled);
    }

    return cancelledCleanly;
}

void
Camera::CleanupCapture()
{
    CancelCapture();
}
