#include "pch.h"
#include "MTPDevice.h"
#include "Camera.h"
#include "CameraException.h"
#include "CameraTask.h"
#include "CameraWorkerFunctions.h"
#include <PortableDevice.h>
#include <WpdMtpExtensions.h>
#include <atomic>
#include <thread>
#include <stdexcept>
#include <vector>

namespace
{
    int checks = 0;
    void Check(bool condition, const char* message)
    {
        ++checks;
        if (!condition) throw std::runtime_error(message);
    }

    enum class Phase { None, Execute, Read, Write, End };

    // The real MTPDevice uses real WPD parameter/result collections. Only the
    // physical IPortableDevice endpoint is replaced; no USB camera is enumerated.
    class PortableDevice : public IPortableDevice
    {
    public:
        LONG references = 1; // The test fixture owns the initial stack lifetime.
        HRESULT openResult = S_OK;
        HRESULT closeResult = S_OK;
        HRESULT unadviseResult = S_OK;
        HRESULT failure = HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
        Phase failAt = Phase::None;
        bool embeddedError = false;
        int opens = 0, closes = 0, advises = 0, unadvises = 0;
        std::vector<Phase> commands;
        IPortableDeviceEventCallback* callback = nullptr;
        std::wstring cookie = L"fixture-cookie";

        ~PortableDevice()
        {
            DropCallback();
        }
        void DropCallback()
        {
            auto* held = callback;
            callback = nullptr;
            if (held) held->Release();
        }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override
        {
            if (!value) return E_POINTER;
            *value = nullptr;
            if (iid != IID_IUnknown && iid != IID_IPortableDevice) return E_NOINTERFACE;
            *value = static_cast<IPortableDevice*>(this);
            AddRef();
            return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references); }
        ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&references); }
        HRESULT STDMETHODCALLTYPE Open(LPCWSTR, IPortableDeviceValues*) override
        {
            ++opens;
            return openResult;
        }
        HRESULT STDMETHODCALLTYPE Close() override { ++closes; return closeResult; }
        HRESULT STDMETHODCALLTYPE Content(IPortableDeviceContent**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE Capabilities(IPortableDeviceCapabilities**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE Cancel() override { return S_OK; }
        HRESULT STDMETHODCALLTYPE GetPnPDeviceID(LPWSTR*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE Advise(DWORD, IPortableDeviceEventCallback* value,
            IPortableDeviceValues*, LPWSTR* result) override
        {
            ++advises;
            Check(!callback, "duplicate registration on one fixture");
            callback = value;
            callback->AddRef();
            *result = static_cast<LPWSTR>(CoTaskMemAlloc((cookie.size() + 1) * sizeof(wchar_t)));
            if (!*result) return E_OUTOFMEMORY;
            wcscpy_s(*result, cookie.size() + 1, cookie.c_str());
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE Unadvise(LPCWSTR value) override
        {
            ++unadvises;
            Check(cookie == value, "registration cookie belongs to another device");
            if (SUCCEEDED(unadviseResult)) DropCallback();
            return unadviseResult;
        }
        HRESULT STDMETHODCALLTYPE SendCommand(DWORD, IPortableDeviceValues* parameters,
            IPortableDeviceValues** result) override
        {
            DWORD id = 0;
            Check(SUCCEEDED(parameters->GetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, &id)), "missing command ID");
            Phase phase = id == WPD_COMMAND_MTP_EXT_READ_DATA.pid ? Phase::Read
                : id == WPD_COMMAND_MTP_EXT_WRITE_DATA.pid ? Phase::Write
                : id == WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER.pid ? Phase::End : Phase::Execute;
            commands.push_back(phase);
            *result = nullptr;
            if (phase == failAt && !embeddedError) return failure;
            HRESULT hr = CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER,
                IID_IPortableDeviceValues, reinterpret_cast<void**>(result));
            if (FAILED(hr)) return hr;
            (*result)->SetErrorValue(WPD_PROPERTY_COMMON_HRESULT, phase == failAt ? failure : S_OK);
            if (phase == failAt) return S_OK;
            if (id == WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE.pid || phase == Phase::End)
            {
                (*result)->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE, COMMAND_RESULT_SUCCESS);
            }
            else if (phase == Phase::Execute)
            {
                (*result)->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, L"test-transfer");
                (*result)->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPTIMAL_TRANSFER_BUFFER_SIZE, 1);
                (*result)->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE, 12);
            }
            else if (phase == Phase::Write)
            {
                (*result)->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_WRITTEN, 12);
            }
            else if (phase == Phase::Read)
            {
                BYTE data[12] = {};
                (*result)->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, data, sizeof(data));
            }
            return S_OK;
        }
    };

    class Transport : public MTPDevice
    {
    public:
        explicit Transport(PortableDevice& device) : MTPDevice(nullptr, L"test-device"), endpoint(device) {}
    protected:
        HRESULT CreatePortableDevice(IPortableDevice** result) override
        {
            endpoint.AddRef();
            *result = &endpoint;
            return S_OK;
        }
    private:
        PortableDevice& endpoint;
    };

    class TestCamera : public Camera
    {
    public:
        explicit TestCamera(Device* device) : Camera(device)
        {
            m_settings = new CameraSettings();
            auto* property = new CameraProperty();
            property->SetId(Property::PhotoBufferStatus);
            property->SetCurrentValue(new PropertyValue(static_cast<UINT16>(0)));
            m_settings->AddProperty(property);
        }
        ~TestCamera() { delete firstImage; }
        Image* firstImage = nullptr;
        bool refreshResult = true;
        bool throwRefresh = false;
        bool Initialize() override { return true; }
        bool RefreshSettings(bool) override
        {
            if (throwRefresh) throw CameraException(L"Injected missing device");
            return refreshResult;
        }
        bool SetProperty(Property, PropertyValue*) override { return false; }
        void SetFocusSteps(std::wstring, std::wstring, bool) override {}
        UINT16 SetFocus(UINT16) override { return 0; }
        UINT16 GetFocusLimit() override { return 0; }
        UINT16 GetFocus() override { return 0; }
        Image* GetImage(DWORD) override
        {
            Image* image = firstImage;
            firstImage = nullptr;
            return image;
        }
        void SetImageReady()
        {
            m_settings->GetProperty(Property::PhotoBufferStatus)->SetCurrentValue(new PropertyValue(static_cast<UINT16>(0x8001)));
        }
    };

    class DownloadTask : public DownloadAndProcessImageTask
    {
    public:
        explicit DownloadTask(TakePhotoTaskParams* params) : DownloadAndProcessImageTask(params) {}
        bool Succeeded() const { return success; }
    };

    void TerminalFailures()
    {
        const HRESULT failures[] = { static_cast<HRESULT>(0x802A0002L), static_cast<HRESULT>(0x802A0006L),
            HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED), HRESULT_FROM_WIN32(ERROR_DEVICE_REMOVED),
            HRESULT_FROM_WIN32(ERROR_NO_SUCH_DEVICE), RPC_E_DISCONNECTED };
        for (HRESULT failure : failures) for (bool embedded : { false, true })
            for (Phase phase : { Phase::Execute, Phase::Read, Phase::Write, Phase::End })
        {
            PortableDevice endpoint;
            Transport device(endpoint);
            Check(device.Open() != INVALID_HANDLE_VALUE, "fake device did not open");
            Check(device.StartNotifications(nullptr), "notification registration failed");
            endpoint.failure = failure;
            endpoint.failAt = phase;
            endpoint.embeddedError = embedded;
            Message command(0x1009);
            if (phase == Phase::Read)
            {
                delete device.Receive(&command);
            }
            else
            {
                if (phase == Phase::Write || phase == Phase::End) { BYTE data[12] = {}; command.SetData(data, sizeof(data)); }
                Check(!device.Send(&command), "failed transfer reported successful send");
            }
            Check(device.IsGone(), "terminal WPD failure did not mark device gone");
            Check(endpoint.commands.back() == phase, "another WPD transfer command followed terminal failure");
            Check(endpoint.unadvises == 1, "events were not stopped exactly once");
            size_t commands = endpoint.commands.size();
            delete device.Receive(&command);
            Check(!device.Send(&command), "already-gone send reported success");
            Check(endpoint.commands.size() == commands, "later request reached departed device");
            device.MarkGone();
            Check(endpoint.unadvises == 1, "repeated gone notification unregistered twice");
            Check(device.Close(), "departed device did not close");
            Check(endpoint.closes == 0 && endpoint.references == 1, "departed session was not released without hardware close");
        }
    }

    void ReopenAndNonterminalErrors()
    {
        PortableDevice endpoint;
        Transport device(endpoint);
        Check(device.Open() != INVALID_HANDLE_VALUE, "initial open failed");
        Check(device.Open() != INVALID_HANDLE_VALUE && device.GetOpenCount() == 2, "healthy nested open failed");
        device.Close();
        endpoint.failAt = Phase::Execute;
        endpoint.failure = E_ACCESSDENIED;
        Message command(0x1009);
        Check(!device.Send(&command) && !device.IsGone(), "ordinary error declared physical removal");
        endpoint.failAt = Phase::None;
        Check(device.Send(&command), "healthy send did not recover from ordinary error");
        Check(endpoint.commands.back() == Phase::Execute, "no-data command incorrectly required END/context");
        device.MarkGone();
        Check(device.Open() == INVALID_HANDLE_VALUE && device.IsGone() && device.GetOpenCount() == 1,
            "nested open erased terminal removal without new session");
        {
            TestCamera camera(&device);
            size_t commands = endpoint.commands.size();
            Check(!camera.RefreshDeviceInfo(), "device-info refresh reopened departed session");
            Check(device.GetOpenCount() == 1 && endpoint.commands.size() == commands, "failed nested refresh consumed live session reference");
            Check(camera.Camera::GetImage(FULL_IMAGE) == nullptr, "missing image metadata was parsed");
        }
        Check(device.Close(), "terminal close failed");
        endpoint.openResult = E_FAIL;
        Check(device.Open() == INVALID_HANDLE_VALUE && device.IsGone(), "failed reopen erased removal");
        endpoint.openResult = S_OK;
        Check(device.Open() != INVALID_HANDLE_VALUE && !device.IsGone(), "successful genuine reopen did not recover");
        Check(device.StartNotifications(nullptr), "reopened session did not register events");
        endpoint.closeResult = E_FAIL;
        Check(device.Close() && endpoint.references == 1, "failed physical close leaked interface");
    }

    void CallbackOwnership()
    {
        PortableDevice first, second;
        first.cookie = L"first-cookie"; second.cookie = L"second-cookie";
        Transport a(first), b(second);
        a.Open(); b.Open();
        auto camera = std::make_unique<TestCamera>(&a);
        Check(a.StartNotifications(camera.get()) && b.StartNotifications(nullptr), "multiple devices share one registration cookie");
        IPortableDeviceValues* event = nullptr;
        Check(SUCCEEDED(CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER,
            IID_IPortableDeviceValues, reinterpret_cast<void**>(&event))), "event values unavailable");
        Check(SUCCEEDED(first.callback->OnEvent(event)), "event without GUID was unsafe");
        event->SetGuidValue(WPD_EVENT_PARAMETER_EVENT_ID, WPD_EVENT_DEVICE_REMOVED);
        first.callback->OnEvent(event); // No optional event-parameter collection.
        first.unadviseResult = E_FAIL;
        auto* late = first.callback;
        late->AddRef(); // Simulates an event already owned by WPD when teardown starts.
        a.MarkGone();
        Check(first.callback == late, "failed unregistration lost WPD callback ownership");
        camera.reset();
        GUID sonyProperty = {};
        sonyProperty.Data1 = 0xc2030000;
        event->SetGuidValue(WPD_EVENT_PARAMETER_EVENT_ID, sonyProperty);
        Check(late->OnEvent(event) == S_OK, "late Sony callback accessed destroyed camera");
        Check(a.Close(), "removed callback owner did not close");
        Check(late->OnEvent(event) == S_OK, "callback did not survive interface teardown");
        event->Release();
        first.DropCallback();
        Check(late->Release() == 0, "callback lifetime did not end at final COM release");
        b.Close();
        Check(second.unadvises == 1 && second.callback == nullptr, "second device unregistered wrong callback");
    }

    void RefreshAndDownload()
    {
        PortableDevice endpoint;
        Transport device(endpoint);
        TestCamera camera(&device);
        RefreshPropertiesTask refresh(false);
        CameraTaskInfo info{ &camera, &refresh };
        camera.refreshResult = false;
        for (int i = 0; i < 4; ++i) refreshSettings(info);
        Check(!camera.LooksDisconnected(), "fewer than five failures suppressed polling");
        refreshSettings(info);
        Check(camera.LooksDisconnected(), "five false refresh results did not suppress polling");
        camera.refreshResult = true;
        refreshSettings(info);
        Check(!camera.LooksDisconnected(), "successful refresh did not restore polling");
        camera.throwRefresh = true;
        for (int i = 0; i < 5; ++i) refreshSettings(info);
        Check(camera.LooksDisconnected(), "exception refresh failures were not recorded");
        camera.throwRefresh = false;
        TakePhotoTaskParams params(0, OutputMode::PASSTHRU);
        DownloadTask task(&params);
        CameraTaskInfo download{ &camera, &task };
        int steps = 0;
        while (!task.IsComplete() && steps++ < 5) task.Step(download);
        Check(task.IsComplete() && !task.Succeeded(), "missing-image task did not finish as failure");
        Check(camera.GetCaptureStatus() == CaptureStatus::Failed && task.GetImage() == nullptr,
            "missing image left capture pending or returned image");

        Message jpeg(COMMAND_RESULT_SUCCESS);
        BYTE bytes[] = { 0xff, 0xd8, 0xff, 0xe1 };
        jpeg.SetData(bytes, sizeof(bytes));
        camera.firstImage = new Image(new ObjectInfo(InputMode::JPEG), &jpeg);
        camera.SetImageReady(); // Another (RAW) image is still expected after JPEG.
        DownloadTask partial(&params);
        CameraTaskInfo partialInfo{ &camera, &partial };
        steps = 0;
        while (!partial.IsComplete() && steps++ < 5) partial.Step(partialInfo);
        Check(partial.IsComplete() && !partial.Succeeded() && partial.GetImage() == nullptr,
            "partial JPEG/RAW download reported success or retained incomplete image");
        Check(camera.GetCaptureStatus() == CaptureStatus::Failed, "partial download left capture pending");
    }

    void ConcurrentShutdown()
    {
        for (int i = 0; i < 64; ++i)
        {
            PortableDevice endpoint;
            Transport device(endpoint);
            device.Open();
            device.StartNotifications(nullptr);
            HANDLE start = CreateEvent(nullptr, TRUE, FALSE, nullptr);
            std::thread removal([&] { WaitForSingleObject(start, 5000); device.MarkGone(); });
            std::thread shutdown([&] { WaitForSingleObject(start, 5000); device.StopNotifications(); });
            SetEvent(start);
            removal.join(); shutdown.join();
            CloseHandle(start);
            Check(endpoint.unadvises == 1 && endpoint.callback == nullptr, "concurrent shutdown acquired callback owner twice");
            device.Close();
            Check(endpoint.references == 1, "concurrent shutdown leaked interface reference");
        }
    }

    void BusyWorkerCallback()
    {
        PortableDevice endpoint;
        Transport device(endpoint);
        device.Open();
        TestCamera camera(&device);
        device.StartNotifications(&camera);
        HANDLE entered = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        HANDLE resume = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        HANDLE notified = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        class BlockingTask : public CameraTask
        {
        public:
            BlockingTask(HANDLE entered, HANDLE resume) : CameraTask(L"TestBlockingWorker")
            {
                SetParam1(entered); SetParam2(resume);
                StateStepInfo step = {};
                step.id = 0; step.isTerminal = true;
                step.handler = [](CameraTaskInfo& info) {
                    SetEvent(info.pTask->GetParam1());
                    WaitForSingleObject(info.pTask->GetParam2(), 5000);
                    return StateResult::Success;
                };
                AddStep(step);
            }
        };
        camera.GetWorker()->OnCameraStateChange(CameraState::Connected);
        auto* task = new BlockingTask(entered, resume);
        task->QueueAndForget(&camera);
        Check(WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0, "worker did not enter blocking task");
        IPortableDeviceValues* event = nullptr;
        CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER,
            IID_IPortableDeviceValues, reinterpret_cast<void**>(&event));
        GUID sonyImage = {}; sonyImage.Data1 = 0xc2010000;
        event->SetGuidValue(WPD_EVENT_PARAMETER_EVENT_ID, sonyImage);
        auto* callback = endpoint.callback;
        callback->AddRef();
        std::thread notification([&] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            callback->OnEvent(event);
            SetEvent(notified);
            CoUninitialize();
        });
        bool returnedWhileWorkerBusy = WaitForSingleObject(notified, 1000) == WAIT_OBJECT_0;
        // Always release before joining, including on a regression, so the test
        // fails with evidence rather than deadlocking the CI process.
        SetEvent(resume);
        notification.join();
        callback->Release(); event->Release();
        camera.GetWorker()->OnCameraStateChange(CameraState::Disconnected);
        device.StopNotifications();
        CloseHandle(entered); CloseHandle(resume); CloseHandle(notified);
        Check(returnedWhileWorkerBusy, "callback blocked on busy worker during teardown");
        device.Close();
    }
}

int main()
{
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return 2;
    try
    {
        TerminalFailures();
        ReopenAndNonterminalErrors();
        CallbackOwnership();
        RefreshAndDownload();
        ConcurrentShutdown();
        BusyWorkerCallback();
        std::cout << "{\"checks\":" << checks
            << ",\"terminalFaultScenarios\":48,\"productionObjectsLinked\":true,\"physicalCamera\":false}\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "Device removal regression failed: " << error.what() << '\n';
        CoUninitialize();
        return 1;
    }
    CoUninitialize();
    return 0;
}
