#include "pch.h"
#include "CameraException.h"
#include "CameraTask.h"
#include "Logger.h"
#include "Camera.h"
#include "CameraWorkerFunctions.h"

StateResult
refreshSettings(CameraTaskInfo& info)
{
    try
    {
        bool succeeded = info.pCamera->RefreshSettings((int)info.pTask->GetParam1() != 0);
        info.pCamera->RecordSettingsRefreshResult(succeeded);
    }
    catch (CameraException& ex)
    {
        info.pCamera->RecordSettingsRefreshResult(false);

        LOGWARN(L"Error refreshing settings... going to ignore '%s'", ex.GetMessage().c_str());
    }

    // Still Success on failure, deliberately.  This is step 0 of the capture,
    // download and preview tasks, all of which have failure branches, and a
    // camera declining a single property must not abort a capture.  The
    // failure is instead recorded above, and acted on by the worker's idle
    // loop, which is the only place that polls without being asked to.
    return StateResult::Success;
}

StateResult
getPreview(CameraTaskInfo& info)
{
    info.pTask->SetResult(info.pCamera->GetImage(PREVIEW_IMAGE));

    return StateResult::Success;
}

StateResult
initializeCamera(CameraTaskInfo& info)
{
    return info.pCamera->Initialize() ? StateResult::Success : StateResult::Fail;
}

StateResult
refreshDeviceInfo(CameraTaskInfo& info)
{
    return info.pCamera->RefreshDeviceInfo() ? StateResult::Success : StateResult::Fail;
}

StateResult
openCamera(CameraTaskInfo& info)
{
    info.pTask->SetResult((VOID*)info.pCamera->Open());

    return StateResult::Success;
}

StateResult
closeCamera(CameraTaskInfo& info)
{
    if (info.pCamera->Close())
    {
        info.pTask->SetResult((void*)1);
    }

    return StateResult::Success;
}

StateResult
setProperty(CameraTaskInfo& info)
{
    SetPropertyTaskParams* params = (SetPropertyTaskParams*)info.pTask->GetParam1();
    info.pCamera->SetProperty(params->id, &params->value);

    return StateResult::Success;
}

// A release that does not reach the camera is worth retrying: the write costs
// milliseconds and the alternative is a body that has to be drained flat.
static const int SHUTTER_RELEASE_ATTEMPTS = 3;
static const DWORD SHUTTER_RELEASE_RETRY_PAUSE_MS = 200;

// How much longer than its own exposure a hold may last before it is reported.
// The release is two MTP writes, so a few hundred milliseconds is normal.
static const ULONGLONG SHUTTER_HOLD_TOLERANCE_MS = 2000;

// One write of a shutter button property, with the exception the MTP layer can
// throw turned into a false. A write that fails must not stop the other button
// property being written: the camera is left with a button held either way, so
// both have to be attempted.
static bool
writeShutterButton(CameraTaskInfo& info, Property id, PropertyValue* value)
{
    try
    {
        return info.pCamera->SetProperty(id, value);
    }
    catch (CameraException& ex)
    {
        LOGERROR(L"Shutter button property x%04x could not be written: %s", (int)id, ex.GetMessage().c_str());
        return false;
    }
    catch (...)
    {
        LOGERROR(L"Shutter button property x%04x could not be written", (int)id);
        return false;
    }
}

// The exposure is timed by holding the shutter button down, so this release is
// the only thing that ends it as far as the camera is concerned. When it does
// not arrive the body is left with its shutter button pressed, and that is how
// a camera that dropped off the bus comes back asking to be turned off and on
// with the shutter parked over the sensor - a state an ordinary power cycle
// does not clear.
//
// It used to be two unchecked writes. Neither result was read, so a failure was
// silent, and because the first one can throw, a throw there meant the second
// was never even attempted - leaving the half-press down as well.
static bool
releaseShutterButton(CameraTaskInfo& info, PropertyValue* up)
{
    bool fullReleased = false;
    bool halfReleased = false;

    for (int attempt = 1; attempt <= SHUTTER_RELEASE_ATTEMPTS; attempt++)
    {
        if (!fullReleased)
        {
            fullReleased = writeShutterButton(info, Property::ShutterFullDown, up);
        }

        if (!halfReleased)
        {
            halfReleased = writeShutterButton(info, Property::ShutterHalfDown, up);
        }

        if (fullReleased && halfReleased)
        {
            return true;
        }

        LOGERROR(L"Shutter button release attempt %d of %d incomplete (full down: %s, half down: %s)",
            attempt, SHUTTER_RELEASE_ATTEMPTS,
            fullReleased ? L"released" : L"STILL DOWN",
            halfReleased ? L"released" : L"STILL DOWN");

        if (attempt < SHUTTER_RELEASE_ATTEMPTS)
        {
            Sleep(SHUTTER_RELEASE_RETRY_PAUSE_MS);
        }
    }

    LOGERROR(L"Shutter button could NOT be released. The camera is left holding its shutter button; the next capture that reaches it will clear the button before starting.");

    return false;
}

StateResult
doCapture(CameraTaskInfo& info)
{
    PropertyValue up((WORD)(info.pCamera->GetDeviceInfo().GetButtonPropertiesInverted() ? 2 : 1));
    PropertyValue down((WORD)(info.pCamera->GetDeviceInfo().GetButtonPropertiesInverted() ? 1 : 2));

    std::unique_ptr<CameraSettings> cs(info.pCamera->GetSettings());

    // If auto-focus is enabled, we should add a short pause between 1/2 down and full down to let it
    // do its thing
    bool isManualFocus = cs->GetProperty(Property::FocusMode)->GetCurrentValue()->GetUINT16() == 1;
    double exposureTime = ((TakePhotoTaskParams*)info.pTask->GetParam1())->exposureTime;

    info.pCamera->SetCaptureStatus(CaptureStatus::Capturing);

    LOGINFO(L"Step 1: Shutter button down");
    info.pCamera->SetProperty(Property::ShutterHalfDown, &down);

    if (!isManualFocus)
    {
        LOGINFO(L"Step 1a: Pausing to allow auto-focus... why is it on?");
        Sleep(500);
    }

    info.pCamera->SetProperty(Property::ShutterFullDown, &down);

    ULONGLONG heldFrom = GetTickCount64();

    WaitForSingleObject(info.pTask->GetCancelEventHandle(), (DWORD)(exposureTime * 1000));

    bool released = releaseShutterButton(info, &up);
    ULONGLONG heldMs = GetTickCount64() - heldFrom;

    LOGINFO(L"Step 2: Shutter button up");

    // Reported at error level so it survives the default log level. The hold is
    // the only thing deciding how long the shutter stays open, and at the
    // default level it left no trace at all: a hold that outlasted its exposure
    // was invisible unless Info logging happened to be on.
    if (!released || heldMs > (ULONGLONG)(exposureTime * 1000.0) + SHUTTER_HOLD_TOLERANCE_MS)
    {
        LOGERROR(L"Shutter button was held %llums for a %.3fs exposure (release %s)",
            heldMs, exposureTime, released ? L"succeeded" : L"FAILED");
    }

    // Still a success even when the release failed. The camera may well have
    // the frame, and the download that follows is the only chance of getting
    // it; failing here would throw away an image to report a fault the log line
    // above already reports.
    return StateResult::Success;
}

bool
isImageReady(CameraSettings* cs)
{
    CameraProperty* bufferStatus = cs->GetProperty(Property::PhotoBufferStatus);
    DWORD value = bufferStatus->GetCurrentValue()->GetUINT16();
    bool imageReady = false;

    switch (value)
    {
    case 0x0000:
        LOGINFO(L"  Nothing yet");
        break;

    case 0x0001:
        LOGINFO(L"  Camera processing");
        break;

    default:
        if (value & 0x8000)
        {
            LOGINFO(L"  %d Photo(s) ready for retrieval", value & 0xfff);
            imageReady = true;
        }
        else
        {
            LOGWARN(L"  No idea what this means x%08x", value);
        }
        break;
    }

    return imageReady;
}

StateResult
downloadImage(CameraTaskInfo& info)
{
    info.pCamera->SetCaptureStatus(CaptureStatus::Reading);

    // If we get here, there must be at least one image ready
    bool imageReady = true;
    int imageCount = 0;

    while (imageReady)
    {
        LOGINFO(L"Step 4: Retrieve image #%d", imageCount + 1);

        Image* image = info.pCamera->GetImage(FULL_IMAGE);

        if (image)
        {
            image->SaveFile();

            // Use the RAW image data and discard JPEG
            if (info.pTask->GetResult() == nullptr || (image->GetInputMode() == InputMode::ARW))
            {
                // Delete is happy with null ptr
                delete (Image*)info.pTask->GetResult();
                info.pTask->SetResult(image);
            }
            else
            {
                delete image;
            }

            imageCount++;

            refreshSettings(info);
            std::unique_ptr<CameraSettings> cs(info.pCamera->GetSettings());
            imageReady = isImageReady(cs.get());
        }
        else
        {
            // No image came back.  Without this the loop never re-evaluates
            // imageReady and spins forever - which is what happens when the
            // camera is disconnected mid-download: every pass asks a device
            // that is no longer there for an object info, builds one from the
            // failed response, and reads past the end of an empty buffer.
            LOGWARN(L"No image returned for image #%d - stopping the download loop", imageCount + 1);

            delete static_cast<Image*>(info.pTask->GetResult());
            info.pTask->SetResult(nullptr);
            info.pCamera->SetCaptureStatus(CaptureStatus::Failed);
            return StateResult::Fail;
        }
    }

    if (imageCount == 0) info.pCamera->SetCaptureStatus(CaptureStatus::Failed);
    return imageCount > 0 ? StateResult::Success : StateResult::Fail;
}

StateResult
failCapture(CameraTaskInfo& info)
{
    refreshSettings(info);
    info.pCamera->SetCaptureStatus(CaptureStatus::Failed);
    return StateResult::Fail;
}

StateResult
processImage(CameraTaskInfo& info)
{
    info.pCamera->SetCaptureStatus(CaptureStatus::Processing);
    StateResult result = StateResult::Success;

    Image* image = (Image*)info.pTask->GetResult();

    if (info.pTask->IsCancelled())
    {
        delete image;
        info.pTask->SetResult(nullptr);
        info.pCamera->SetCaptureStatus(CaptureStatus::Cancelled);
    }
    else
    {
        if (image->GetDataLength())
        {
            // Render as RGB or RGGB
            // We do it in here as there is a chance we'll get multiple hits on other threads that will cause... issues
            OutputMode mode = ((TakePhotoTaskParams*)info.pTask->GetParam1())->mode;

            image->SetOutputMode(mode);
            image->SetDuration(((TakePhotoTaskParams*)info.pTask->GetParam1())->exposureTime);

            // Tell the image how it should be cropped
            DeviceInfo deviceInfo = info.pCamera->GetDeviceInfo();

            switch (deviceInfo.GetCropMode())
            {
            case CropMode::AUTO:
                image->SetCrop(-1, -1, -1, -1);
                break;

            case CropMode::NONE:
                image->SetCrop(0, 0, 0, 0);
                break;

            case CropMode::USER:
                image->SetCrop(deviceInfo.GetTopCrop(), deviceInfo.GetLeftCrop(), deviceInfo.GetBottomCrop(), deviceInfo.GetRightCrop());
                break;

            default:
                // Image default is 0, 0, 0, 0
                break;
            }

            bool success = image->EnsurePixelsProcessed();

            if (success)
            {
                info.pCamera->SetCaptureStatus(CaptureStatus::Complete);
            }
            else
            {
                LOGERROR(L"Unable to generate pixel data, setting status failed");
                result = StateResult::Fail;
            }
        }
        else
        {
            info.pCamera->SetCaptureStatus(CaptureStatus::Failed);
            result = StateResult::Fail;
        }
    }

    return result;
}
