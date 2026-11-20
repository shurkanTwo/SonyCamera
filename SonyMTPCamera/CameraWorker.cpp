#include "pch.h"
#include "CameraWorker.h"
#include "CameraTask.h"
#include "CameraManager.h"
#include "CameraWorkerFunctions.h"
#include "Logger.h"
#include "CameraException.h"

CameraWorker::CameraWorker(Camera* pCamera)
    : camera(pCamera)
{
    mutexHandle = CreateMutex(nullptr, FALSE, nullptr);
    threadHandle = CreateThread(NULL, 0, &_run, this, 0, &threadId);
}

CameraWorker::~CameraWorker()
{
    // We need to wait a long time as the camera may be in the middle of a long exposure (shouldn't be, but... users!)
    DWORD waitResult = WaitForSingleObject(mutexHandle, INFINITE);

    if (waitResult != WAIT_OBJECT_0)
    {
        // Need to error here
    }

    stopRequested = true;

    ReleaseMutex(mutexHandle);

    // Wait for run to exit
    DWORD exitCode = 0;
    BOOL ret = GetExitCodeThread(threadHandle, &exitCode);

    int waitCount = 0;

    while (exitCode == STILL_ACTIVE && waitCount < WORKER_THREAD_WAIT_FOR_EXIT_RETRIES)
    {
        waitCount++;

        LOGINFO(L"Waiting for camera handler thread to exit %d / %d loops", waitCount, WORKER_THREAD_WAIT_FOR_EXIT_RETRIES);

        Sleep(WORKER_THREAD_WAIT_FOR_EXIT);
        GetExitCodeThread(threadHandle, &exitCode);
    }

    if (exitCode == STILL_ACTIVE)
    {
        LOGERROR(L"Camera handler thread would not exit, terminating with prejudice");
        TerminateThread(threadHandle, -1);
    }

/*    if (currentTask)
    {
        // How do we go about stopping the task?
    }*/

    CloseHandle(mutexHandle);
    CloseHandle(threadHandle);
}

void
CameraWorker::OnCameraStateChange(CameraState newState)
{
    if (newState != lastCameraState)
    {
#ifdef DEBUG
        LOGTRACE(L"Camera State changed from %d to %d", lastCameraState, newState);
#endif
        lastCameraState = newState;
    }
}

DWORD WINAPI
CameraWorker::_run(LPVOID lpParameter)
{
    LOGTRACE(L"In: CameraWorker::_run(x%08x)", lpParameter);

    DWORD result = ((CameraWorker*)lpParameter)->Run();

    LOGTRACE(L"Out: CameraWorker::_run(x%08x) - returning %d", lpParameter, result);

    return result;
}

DWORD CameraWorker::Run()
{
    LOGTRACE(L"In: CameraWorker::Run");

    CameraTask* currentTask = nullptr;
    HANDLE waiter = INVALID_HANDLE_VALUE;
    bool isIdleTask = false;
    int idleCount = 0;

    HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    LOGTRACE(L"CoInitialize returned: %d", coInit);

    while (!stopRequested)
    {
        // How long to pause at the end of this iteration, once the mutex is
        // back.  Sleeping while still holding it starves QueueTask, which waits
        // on the same mutex: the comment below says the mutex is held only
        // momentarily, and an idle sleep inside it made that untrue - a quarter
        // of a second at a time, or half a second once the camera stopped
        // answering, every iteration, with only microseconds in between for
        // another thread to get in.
        DWORD idlePause = 0;

        // Only reason mutex would be claimed is to set the exit flag, so while INFINITE may not be ideal,
        // we're expecting (essentially) an instant result
        DWORD waitResult = WaitForSingleObject(mutexHandle, INFINITE);

        if (waitResult != WAIT_OBJECT_0)
        {
            // Need to error here
        }

        if (!currentTask)
        {
            if (!queue.empty())
            {
                std::pair<CameraTask*, HANDLE> next = queue.front();
                queue.pop_front();
                currentTask = next.first;
                waiter = next.second;
#ifdef DEBUG
                LOGTRACE(L"Request pulled from queue");
#endif
            }
        }

        // We can start something
        if (currentTask)
        {
            isIdleTask = false;
            idleCount = 0;
        }
        else if (camera->LooksDisconnected() || lastCameraState != CameraState::Connected)
        {
            // The camera has stopped answering, so do not keep asking.  This
            // idle refresh only exists to spot a photo taken with the camera's
            // own shutter button, and it is not worth polling a device that is
            // no longer there twenty times a second - doing so eventually
            // faults inside the WPD stack and takes the hosting application
            // with it.  Anything genuinely queued still runs, and a queued
            // task that succeeds clears this state.
            //
            // The same applies before the camera has been opened at all, and
            // after it has been closed: an idle refresh cannot run in any state
            // but Connected, so creating one only to abandon it below would
            // spin this loop and fill the log.  Worse, until the state check
            // moved here it took the currentTask slot, and this loop only
            // drains the queue while that slot is free - so a worker that
            // started before the open was queued could never open its camera.
#ifdef DEBUG
            LOGTRACE(L"Nothing to do, and the camera is not answering - not queuing an idle refresh");
#endif
            isIdleTask = false;
            idleCount = 0;

            idlePause = 500;
        }
        else
        {
#ifdef DEBUG
            LOGTRACE(L"Nothing to do, queuing up a refresh properties task");
#endif
            currentTask = new RefreshPropertiesTask(false);
            isIdleTask = true;
            idleCount++;

            idlePause = 50;
        }

        if (currentTask)
        {
            bool complete = false;
            bool stepped = false;

            if (currentTask->SupportsCameraState(lastCameraState))
            {
                CameraTaskInfo info;

                info.pCamera = camera;
                info.pTask = currentTask;

                complete = currentTask->Step(info);
                stepped = true;
            }
            else
            {
                // Nothing here will produce the state this task needs.  The
                // only state transitions are the ones Camera::Open and
                // Camera::Close make, and both of those run as tasks on this
                // very thread - which cannot reach them while this slot is
                // occupied.  Keeping the task therefore waited forever: the
                // wait in CameraTask::Run never returned, the queue behind it
                // was never drained, and this loop spun at full speed for as
                // long as the process lived.
                //
                // Measured on the rig on 2026-11-20: an open that failed with
                // x80070002 left the state at Connecting, the
                // InitializeCameraTask that OpenDeviceEx runs next could never
                // step, and NINA sat burning a whole core with its camera
                // unusable until it was restarted.  Reporting the failure to
                // whoever is waiting is both true and recoverable.
                LOGERROR(L"Task %s cannot run while the camera is in state %d - failing it rather than waiting for a state that nothing will set",
                    currentTask->Name().c_str(), (int)lastCameraState);

                currentTask->Abandon();
                complete = true;
            }

            if (complete)
            {
                if (waiter != INVALID_HANDLE_VALUE)
                {
#ifdef DEBUG
                    LOGTRACE(L"Releasing wait for task %s", currentTask->Name().c_str());
#endif
                    ReleaseSemaphore(waiter, 1, nullptr);
                }
                else
                {
                    // If there is no waiter, there is no way to indicate the task is complete.
                    // It is expected the only task that will fall into this category is the
                    // one auto-generated when there is nothing to do
//                    LOGTRACE(L"Deleting un-waited-for task %s", currentTask->Name().c_str());
                    delete currentTask;
                }

                if (stepped && isIdleTask && idleCount % 20 == 0) {
                    // Check to see if there's a photo waiting that we didn't get notified of
                    // This will just queue up a download - when the download completes
#ifdef DEBUG
                    LOGTRACE(L"Checking to see if an image is waiting we haven't been told about");
#endif
                    std::unique_ptr<CameraSettings> cs(camera->GetSettings());

                    if (isImageReady(cs.get())) {
#ifdef DEBUG
                        LOGINFO(L"There is an image ready, but we didn't get told about it!");
#endif
                        camera->OnImageBufferStatus(ImageBufferStatus::ImageReady);
                    }
                }

                currentTask = nullptr;
                waiter = INVALID_HANDLE_VALUE;
            }
        }

        ReleaseMutex(mutexHandle);

        // Outside the mutex, so an idle pause cannot hold up a caller queueing
        // work.  Nothing below the release touches shared state.
        if (idlePause)
        {
            Sleep(idlePause);
        }
    }

    CoUninitialize();

    LOGTRACE(L"Out: CameraWorker::Run");

    return 0;
}

bool
CameraWorker::QueueTask(CameraTask* task, HANDLE waiter, DWORD timeout)
{
    // Wait for runner thread to be idle
    DWORD waitResult = WaitForSingleObject(mutexHandle, timeout);

    if (waitResult != WAIT_OBJECT_0)
    {
        return false;
    }

    // If there is a waiter, just queue it up - if there is no waiter, look thru
    // queue and only add if there isn't one of same type
    if (!waiter)
    {
        for (const auto queued: queue)
        {
            if (queued.first->Name() == task->Name())
            {
                LOGINFO(L"Not queuing task %s as there is already one in queue", task->Name().c_str());
                task = nullptr;
            }
        }
    }

    if (task)
    {
        queue.push_back(std::pair<CameraTask*, HANDLE>(task, waiter));
    }

    // Let runner continue
    ReleaseMutex(mutexHandle);

    return task;
}
