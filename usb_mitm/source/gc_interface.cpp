#include "gc_interface.hpp"
#include "usb_shim.h"

namespace ams::usb::gc
{
    namespace
    {
        constexpr s32 g_ThreadPriority = -11; 
        alignas(os::MemoryPageSize) u8 g_InitPacket[os::MemoryPageSize] = { 0x13 };
    }

    GameCubeDriver g_GameCubeDriver1;
    GameCubeDriver g_GameCubeDriver2;

    void GameCubeDriver::DriverThread(void* driver)
    {
        GameCubeDriver* pThis = reinterpret_cast<GameCubeDriver*>(driver);
        pThis->DriverThreadImpl();
    }

    GameCubeDriver::AsyncCtrlXferCached* GameCubeDriver::DoCtrlXferAsync(
        u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u32 expectedSize
    )
    {
        AMS_ABORT_UNLESS(((bmRequestType & 0x80) == 0x80) || (wLength == 0x0), "Host-to-device must have zero-length");

        UsbHsXferReport report;
        R_ABORT_UNLESS(usbHsIfCtrlXferAsyncFwd(this->mpInterface, bmRequestType, bRequest, wValue, wIndex, wLength, (u64)this->mAsyncCtrlXferPage));
        R_ABORT_UNLESS(eventWait(&this->mInterfaceXferCompleted, UINT64_MAX));
        R_ABORT_UNLESS(eventClear(&this->mInterfaceXferCompleted));
        R_ABORT_UNLESS(usbHsIfGetCtrlXferReportFwd(this->mpInterface, &report, sizeof(UsbHsXferReport)));
        R_ABORT_UNLESS(report.res); /* AMS_ABORT_UNLESS(report.transferredSize == expectedSize, "Control Xfer Didn't Finish"); */

        AsyncCtrlXferCached* pCached = reinterpret_cast<AsyncCtrlXferCached*>(malloc(sizeof(AsyncCtrlXferCached) + report.transferredSize));
        pCached->pNext = this->pCachedXfers;
        pCached->bmRequestType = bmRequestType;
        pCached->bRequest = bRequest;
        pCached->wValue = wValue;
        pCached->wIndex = wIndex;
        pCached->wLength = wLength;
        pCached->expectedOutput = expectedSize;
        memcpy(pCached->output, this->mAsyncCtrlXferPage, report.transferredSize);
        this->pCachedXfers = pCached;
        return pCached;
    }

    usb_endpoint_descriptor GameCubeDriver::OpenEndpoint(Service* pOutService, u8 endpointId, bool isInput, u32 maxXferSize)
    {
        usb_endpoint_descriptor desc;
        R_ABORT_UNLESS(usbHsIfOpenUsbEpFwd(this->mpInterface, pOutService, 1, 4, endpointId, isInput ? 0x2 : 0x1, maxXferSize, &desc));
        return desc;
    }

    void GameCubeDriver::SetupDevice()
    {
        Handle tmp;

        R_ABORT_UNLESS(usbHsIfGetCtrlXferCompletionEventFwd(this->mpInterface, &tmp));
        eventLoadRemote(&this->mInterfaceXferCompleted, tmp, false);

        /* This implementation mimics official software and then stores the responses for later so that they can be */
        /* proxied to official software */

        /* Get Descriptor 0 */
        this->DoCtrlXferAsync(
            0x80, USB_REQUEST_GET_DESCRIPTOR, (u16)usb_descriptor_type::USB_DT_CONFIG << 8, 0x0,
            0x29, 0x29
        );

        /* Acquire & Setup Read Endpoint */
        this->OpenEndpoint(&this->mReadEndpoint, 1, true, 0x25);
        R_ABORT_UNLESS(usbHsEpPopulateRingFwd(&this->mReadEndpoint));

        /* Halt In Endpoint */
        this->DoCtrlXferAsync(
            0x02, USB_REQUEST_CLEAR_FEATURE, 0x0, 0x81,
            0x0, 0x0
        );

        R_ABORT_UNLESS(usbHsEpGetCompletionEventFwd(&this->mReadEndpoint, &tmp));
        eventLoadRemote(&this->mReadCompletedEvent, tmp, false);

        /* Official software creates an SMMU space here, I'm going to opt to not do that because I don't know what it does */
        /* R_ABORT_UNLESS(usbHsEpCreateSmmuSpace(&this->mReadEndpoint, this->mReadEpPage, os::MemoryPageSize)); */

        /* Acquire & Setup Write Endpoint */
        this->OpenEndpoint(&this->mWriteEndpoint, 2, false, 0x5);
        R_ABORT_UNLESS(usbHsEpPopulateRingFwd(&this->mWriteEndpoint));

        /* Halt Out Endpoint */
        this->DoCtrlXferAsync(
            0x02, USB_REQUEST_CLEAR_FEATURE, 0x0, 0x02,
            0x0, 0x0
        );

        R_ABORT_UNLESS(usbHsEpGetCompletionEventFwd(&this->mWriteEndpoint, &tmp));
        eventLoadRemote(&this->mWriteCompletedEvent, tmp, false);

        /* Again, official sw creates an SMMU space */
        /* R_ABORT_UNLESS(usbHsEpCreateSmmuSpace(&this->mWriteEndpoint, this->mWriteEpPage, os::MemoryPageSize)); */

        /* Get Configuration Options, I don't know what these transfers map to directly :( */
        this->DoCtrlXferAsync(
            0x81, 0x6, 0x2200, 0x0,
            0xD6, 0xD6
        );

        this->DoCtrlXferAsync(
            0x80, 0x6, 0x300, 0x0,
            0xff, 0x4
        );

        this->DoCtrlXferAsync(
            0x80, 0x6, 0x301, 0x409,
            0x40, 0x20
        );

        this->DoCtrlXferAsync(
            0x80, 0x6, 0x302, 0x409,
            0x40, 0x36
        );

        this->DoCtrlXferAsync(
            0x80, 0x6, 0x303, 0x409,
            0x40, 0x4
        );

        /* Begin Operation */
        this->DoCtrlXferAsync(
            0x21, 0xb, 0x1, 0x0,
            0x0, 0x0
        );
    }

    void GameCubeDriver::SteadyState()
    {
        os::MultiWaitHolderType readFinished;
        os::MultiWaitHolderType writeFinished;
        os::MultiWaitType waiter;
        UsbHsXferReport report;
        u32 dummy;

        os::InitializeMultiWait(&waiter);
        os::InitializeMultiWaitHolder(&readFinished, this->mReadCompletedEvent.revent);
        os::InitializeMultiWaitHolder(&writeFinished, this->mWriteCompletedEvent.revent);
        os::LinkMultiWaitHolder(&waiter, &readFinished);
        os::LinkMultiWaitHolder(&waiter, &writeFinished);

        R_ABORT_UNLESS(usbHsEpPostBufferAsyncFwd(&this->mWriteEndpoint, g_InitPacket, 1, 0, &dummy));
        R_ABORT_UNLESS(usbHsEpPostBufferAsyncFwd(&this->mReadEndpoint, this->mReadXferPage, 0x25, 0, &dummy));

        while (true)
        {
            os::MultiWaitHolderType* pSignaled = os::WaitAny(&waiter);

            if (pSignaled == &readFinished)
            {
                R_ABORT_UNLESS(eventClear(&this->mReadCompletedEvent));
                /* Read operations are done constantly and at 1000hz, so we don't bother sending the event here */
                /* instead we send it in the PostBufferAsync request itself */
                R_ABORT_UNLESS(usbHsEpGetXferReportFwd(&this->mReadEndpoint, &report, 1, &dummy));

                if (dummy != 1)
                {
                    R_ABORT_UNLESS(0x234567);
                }

                if (report.res == 0)
                {
                    /* If it was a success, then simply queue another read */
                    R_ABORT_UNLESS(usbHsEpPostBufferAsyncFwd(
                        &this->mReadEndpoint, this->mReadXferPage, 0x25, 0, &dummy
                    ));
                }
                else if (report.res == 0x3228c)
                {
                    /* If it's this result code, then we need to break out of our steady state loop */
                    /* And wait for the adapter to shut down */
                    break;
                }
                else
                {
                    R_ABORT_UNLESS(report.res);
                }
            }
            else if (pSignaled == &writeFinished)
            {
                R_ABORT_UNLESS(eventClear(&this->mWriteCompletedEvent));
                R_ABORT_UNLESS(usbHsEpGetXferReportFwd(&this->mWriteEndpoint, &report, 1, &dummy));
                os::SleepThread(TimeSpan::FromMilliSeconds(1));

                if (dummy != 1)
                {
                    R_ABORT_UNLESS(0x345678);
                }

                if (report.res == 0)
                {
                    /* If it was a success, fire off the event */
                    R_ABORT_UNLESS(eventFire(&this->mWriteCompletedEventFwd));
                }
                else if (report.res == 0x3228c)
                {
                    break;
                }
                else
                {
                    R_ABORT_UNLESS(report.res);
                }
            }
            else
            {
                R_ABORT_UNLESS(0x123456);
            }
        }

        os::FinalizeMultiWait(&waiter);
        os::FinalizeMultiWaitHolder(&readFinished);
        os::FinalizeMultiWaitHolder(&writeFinished);
    }

    void GameCubeDriver::WaitSteady()
    {
        while (this->mState == GameCubeDriverState::Initializing)
        {
            os::SleepThread(TimeSpan::FromMicroSeconds(100));
        }
    }


    void GameCubeDriver::DriverThreadImpl()
    {
        while (true)
        {
            /* Wait on the acquisition of this driver */
            os::WaitEvent(&mAcquireEvent);
            os::ClearEvent(&mAcquireEvent);
            AMS_ABORT_UNLESS(this->mState == GameCubeDriverState::Initializing);

            /* Phase 1 - Device Initialization */
            /* This is mostly control transfers to initialize the configuration of the gamecube adapter */
            this->SetupDevice();

            this->mState = GameCubeDriverState::SteadyState;

            /* Wait until our pages have been mapped */
            /* We don't have to do that for our interface page because our interface page only needs to be mapped */
            /* once the HID service begins requesting transfers */
            while ((this->mReadEpMappee == 0) || (this->mWriteEpMappee == 0))
            {
                os::SleepThread(TimeSpan::FromMicroSeconds(100));
            }


            /* Phase 2 - Steady State */
            /* This is the continuous reads and writes for the GameCube adapter */
            /* Normally we only have reads to perform, but if the HID service wants us to write a packet, we should write one */
            this->SteadyState();
            this->mState = GameCubeDriverState::Finalizing;
            // usbHsEpCloseFwd(&this->mReadEndpoint);
            // usbHsEpCloseFwd(&this->mWriteEndpoint);
            /* Wake up the HID thread if it's waiting on a write (reads always immediately get fired) */
            R_ABORT_UNLESS(eventFire(&this->mInterfaceStateChangeFwd));
            R_ABORT_UNLESS(eventFire(&this->mWriteCompletedEventFwd));

            /* Phase 3 - Finalization */
            /* This is where the HID service begins to shut down the driver */
            /* It's going to continue to attempt to drive it even after closing down the endpoints */
            /* So our thread is actually going to do nothing and we are just going to manually fake */
            /* the data from the session impls */
            /* Because of this, we are simply going to continue the loop, and wait for the next acquire event */
        }
    }

    void GameCubeDriver::Initialize(GameCubeDriverId id)
    {
        svc::MemoryInfo memInfo;
        svc::PageInfo pageInfo;

        uintptr_t currentAddress = reinterpret_cast<uintptr_t>(DriverThread);
        R_ABORT_UNLESS(svc::QueryMemory(&memInfo, &pageInfo, currentAddress));

        currentAddress = memInfo.base_address + memInfo.size;
        /* Terrible, abysmal hack to not find the same memory */
        size_t counter = id == GameCubeDriverId::One ? 0 : 3;
        while (currentAddress > memInfo.base_address)
        {
            R_ABORT_UNLESS(svc::QueryMemory(&memInfo, &pageInfo, currentAddress));
            if (memInfo.state == svc::MemoryState_Free)
            {
                while (memInfo.size > 0)
                {
                    if (this->mInterfacePage == 0)
                    {
                        if (counter > 0)
                        {
                            counter--;
                        }
                        else
                        {
                            this->mInterfacePage = memInfo.base_address;
                        }
                        memInfo.base_address += os::MemoryPageSize;
                        memInfo.size -= os::MemoryPageSize;
                    }
                    else if (this->mReadEpPage == 0)
                    {
                        this->mReadEpPage = memInfo.base_address;
                        memInfo.base_address += os::MemoryPageSize;
                        memInfo.size -= os::MemoryPageSize;
                    }
                    else if (this->mWriteEpPage == 0)
                    {
                        this->mWriteEpPage = memInfo.base_address;
                        memInfo.base_address += os::MemoryPageSize;
                        memInfo.size -= os::MemoryPageSize;
                    }
                    else
                    {
                        break;
                    }
                }

                if (memInfo.size > 0)
                    break;
            }

            currentAddress = memInfo.base_address + memInfo.size;
        }

        this->mInterfaceMappee = 0;
        this->mReadEpMappee = 0;
        this->mWriteEpMappee = 0;

        R_ABORT_UNLESS(os::CreateThread(
            &this->mThread,
            GameCubeDriver::DriverThread,
            this,
            this->mThreadStack,
            os::MemoryPageSize,
            g_ThreadPriority
        ));

        const char* pName = "GCDriverThread1";
        if (id == GameCubeDriverId::Two)
        {
            pName = "GCDriverThread2";
        }

        os::SetThreadNamePointer(&this->mThread, pName);
        os::StartThread(&this->mThread);
        os::InitializeEvent(&this->mAcquireEvent, false, os::EventClearMode_ManualClear);
        this->mId = id;

        R_ABORT_UNLESS(eventCreate(&this->mInterfaceStateChangeFwd, false));
        R_ABORT_UNLESS(eventCreate(&this->mInterfaceXferCompletedFwd, false));
        R_ABORT_UNLESS(eventCreate(&this->mReadCompletedEventFwd, false));
        R_ABORT_UNLESS(eventCreate(&this->mWriteCompletedEventFwd, false));

        this->pCachedXfers = nullptr;
        this->mState = GameCubeDriverState::Unacquired;
        this->mClientProcess = INVALID_HANDLE;
    }

    void GameCubeDriver::Acquire(os::NativeHandle clientProcess, Service* pInterface)
    {
        AMS_ABORT_UNLESS(this->mState == GameCubeDriverState::Unacquired);
        AMS_ABORT_UNLESS(this->mClientProcess == INVALID_HANDLE);
        AMS_ABORT_UNLESS(this->mpInterface == nullptr);
        AMS_ABORT_UNLESS(
            this->mInterfaceMappee == 0
                && this->mReadEpMappee == 0
                && this->mWriteEpMappee == 0
        );

        this->mClientProcess = clientProcess;
        this->mpInterface = pInterface;
        this->mState = GameCubeDriverState::Initializing;
        os::SignalEvent(&this->mAcquireEvent);
    }

    os::NativeHandle GameCubeDriver::InterfaceChangeEvent()
    {
        return this->mInterfaceStateChangeFwd.revent;
    }

    os::NativeHandle GameCubeDriver::InterfaceEvent()
    {
        return this->mInterfaceXferCompletedFwd.revent;
    }

    os::NativeHandle GameCubeDriver::ReadEvent()
    {
        return this->mReadCompletedEventFwd.revent;
    }

    os::NativeHandle GameCubeDriver::WriteEvent()
    {
        return this->mWriteCompletedEventFwd.revent;
    }

    void GameCubeDriver::ControlTransfer(u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u64 buffer, UsbHsXferReport* pReport)
    {
        AMS_ABORT_UNLESS(this->mState != GameCubeDriverState::Unacquired, "Cannot request control transfer on unacquired driver");
        this->WaitSteady();
        if (buffer != 0)
        {
            this->MapInterfacePage(buffer);
        }

        /* If we are not in steady state, then the interface is shutting down */
        if (this->mState == GameCubeDriverState::Finalizing)
        {
            R_ABORT_UNLESS(svc::FlushProcessDataCache(this->mClientProcess, this->mInterfaceMappee, os::MemoryPageSize));
            armDCacheClean((void*)this->mInterfacePage, os::MemoryPageSize);
            memcpy(this->mAsyncCtrlXferPage, (const void*)this->mInterfacePage, os::MemoryPageSize);
            R_ABORT_UNLESS(usbHsIfCtrlXferAsyncFwd(this->mpInterface, bmRequestType, bRequest, wValue, wIndex, wLength, (u64)this->mAsyncCtrlXferPage));
            R_ABORT_UNLESS(eventWait(&this->mInterfaceXferCompleted, UINT64_MAX));
            R_ABORT_UNLESS(usbHsIfGetCtrlXferReportFwd(this->mpInterface, pReport, sizeof(UsbHsXferReport)));
            memcpy((void*)this->mInterfacePage, this->mAsyncCtrlXferPage, os::MemoryPageSize);
            R_ABORT_UNLESS(svc::FlushDataCache(this->mInterfacePage, os::MemoryPageSize));
            R_ABORT_UNLESS(svc::InvalidateProcessDataCache(this->mClientProcess, this->mInterfaceMappee, os::MemoryPageSize));
            R_ABORT_UNLESS(eventFire(&this->mInterfaceXferCompletedFwd));
        }
        else
        {
            AsyncCtrlXferCached* pNext = this->pCachedXfers;
            while (pNext)
            {
                if (
                    (pNext->bmRequestType == bmRequestType)
                    && (pNext->bRequest == bRequest)
                    && (pNext->wValue == wValue)
                    && (pNext->wIndex == wIndex)
                    && (pNext->wLength == wLength)
                )
                    break;
                pNext = pNext->pNext;
            }

            AMS_ABORT_UNLESS(pNext != nullptr, "Unexpected async ctrl request");
            if (pNext->expectedOutput != 0)
            {
                memcpy(reinterpret_cast<void*>(this->mInterfacePage), pNext->output, pNext->expectedOutput);
                R_ABORT_UNLESS(svc::FlushDataCache(this->mInterfacePage, os::MemoryPageSize));
                R_ABORT_UNLESS(svc::InvalidateProcessDataCache(this->mClientProcess, this->mInterfaceMappee, os::MemoryPageSize));
            }
            pReport->xferId = 0;
            pReport->res = 0x0;
            pReport->requestedSize = wLength,
            pReport->transferredSize = pNext->expectedOutput;
            pReport->id = 0;
            R_ABORT_UNLESS(eventFire(&this->mInterfaceXferCompletedFwd));
        }
    }

    void GameCubeDriver::MapInterfacePage(uintptr_t page)
    {
        if (AMS_UNLIKELY(this->mInterfaceMappee == 0))
        {
            R_ABORT_UNLESS(svc::MapProcessMemory(
                this->mInterfacePage,
                this->mClientProcess,
                page,
                os::MemoryPageSize
            ));

            this->mInterfaceMappee = page;
        }
        else if (AMS_UNLIKELY(this->mInterfaceMappee != page))
        {
            AMS_ABORT("Mapped read page is not the same as what was provided in MapReadPage");
        }
    }

    void GameCubeDriver::MapReadPage(uintptr_t page)
    {
        if (AMS_UNLIKELY(this->mReadEpMappee == 0))
        {
            R_ABORT_UNLESS(svc::MapProcessMemory(
                this->mReadEpPage,
                this->mClientProcess,
                page,
                os::MemoryPageSize
            ));

            this->mReadEpMappee = page;
        }
        else if (AMS_UNLIKELY(this->mReadEpMappee != page))
        {
            AMS_ABORT("Mapped read page is not the same as what was provided in MapReadPage");
        }
    }

    void GameCubeDriver::MapWritePage(uintptr_t page)
    {
        if (AMS_UNLIKELY(this->mWriteEpMappee == 0))
        {
            R_ABORT_UNLESS(svc::MapProcessMemory(
                this->mWriteEpPage,
                this->mClientProcess,
                page,
                os::MemoryPageSize
            ));

            this->mWriteEpMappee = page;
        }
        else if (AMS_UNLIKELY(this->mWriteEpMappee != page))
        {
            AMS_ABORT("Mapped read page is not the same as what was provided in MapReadPage");
        }
    }

    void GameCubeDriver::ReadPacket(u64 clientBuffer, u32 clientSize, UsbHsXferReport* pReport)
    {
        u32 dummy;
        this->MapReadPage(clientBuffer);
        while (true)
        {
            switch (this->mState)
            {
                case GameCubeDriverState::Unacquired:
                    AMS_ABORT("Cannot read packet while driver is unacquired");
                    break;
                case GameCubeDriverState::Initializing:
                    this->WaitSteady();
                    continue;
                case GameCubeDriverState::SteadyState:
                    memcpy((void*)this->mReadEpPage, this->mReadXferPage, clientSize);
                    R_ABORT_UNLESS(svc::FlushDataCache(this->mReadEpPage, os::MemoryPageSize));
                    R_ABORT_UNLESS(svc::InvalidateProcessDataCache(this->mClientProcess, this->mReadEpMappee, os::MemoryPageSize));
                    pReport->xferId = 0;
                    pReport->res = 0;
                    pReport->requestedSize = clientSize;
                    pReport->transferredSize = clientSize;
                    pReport->id = 0;
                    R_ABORT_UNLESS(eventFire(&this->mReadCompletedEventFwd));
                    break;
                case GameCubeDriverState::Finalizing:
                    /* In the finalized state, we just want to drive the device as the HID service would */
                    R_ABORT_UNLESS(usbHsEpPostBufferAsyncFwd(&this->mReadEndpoint, this->mReadXferPage, clientSize, 0, &dummy));
                    R_ABORT_UNLESS(eventWait(&this->mReadCompletedEvent, UINT64_MAX));
                    R_ABORT_UNLESS(usbHsEpGetXferReportFwd(&this->mReadEndpoint, pReport, 1, &dummy));
                    memcpy((void*)this->mReadEpPage, this->mReadXferPage, os::MemoryPageSize);
                    R_ABORT_UNLESS(svc::FlushDataCache(this->mReadEpPage, os::MemoryPageSize));
                    R_ABORT_UNLESS(svc::InvalidateProcessDataCache(this->mClientProcess, this->mReadEpMappee, os::MemoryPageSize));
                    R_ABORT_UNLESS(eventFire(&this->mReadCompletedEventFwd));
                    break;
                AMS_UNREACHABLE_DEFAULT_CASE();
            }
            break;
        }
    }

    void GameCubeDriver::WritePacket(u64 clientBuffer, u32 clientSize, UsbHsXferReport* pReport)
    {
        u32 dummy;
        this->MapWritePage(clientBuffer);
        while (true)
        {
            switch (this->mState)
            {
                case GameCubeDriverState::Unacquired:
                    AMS_ABORT("Cannot write packet while driver is unacquired");
                    break;
                case GameCubeDriverState::Initializing:
                    this->WaitSteady();
                    continue;
                case GameCubeDriverState::SteadyState:
                    /* Skip init packet */
                    if (clientSize != 1)
                    {
                        R_ABORT_UNLESS(svc::FlushProcessDataCache(this->mClientProcess, this->mWriteEpMappee, os::MemoryPageSize));
                        armDCacheClean((void*)this->mWriteEpPage, os::MemoryPageSize);
                        memcpy(this->mWriteXferPage, (const void*)this->mWriteEpPage, clientSize);
                        R_ABORT_UNLESS(usbHsEpPostBufferAsyncFwd(
                            &this->mWriteEndpoint,
                            this->mWriteXferPage,
                            clientSize,
                            0,
                            &dummy
                        ));
                    }
                    pReport->xferId = 0;
                    pReport->res = 0;
                    pReport->requestedSize = clientSize;
                    pReport->transferredSize = clientSize;
                    pReport->id = 0;
                    break;
                case GameCubeDriverState::Finalizing:
                    /* In the finalized state, we just want to drive the device as the HID service would */
                    R_ABORT_UNLESS(svc::FlushProcessDataCache(this->mClientProcess, this->mWriteEpMappee, os::MemoryPageSize));
                    armDCacheClean((void*)this->mWriteEpPage, os::MemoryPageSize);
                    memcpy(this->mWriteXferPage, (const void*)this->mWriteEpPage, os::MemoryPageSize);
                    R_ABORT_UNLESS(usbHsEpPostBufferAsyncFwd(&this->mWriteEndpoint, this->mWriteXferPage, clientSize, 0, &dummy));
                    R_ABORT_UNLESS(eventWait(&this->mWriteCompletedEvent, UINT64_MAX));
                    R_ABORT_UNLESS(usbHsEpGetXferReportFwd(&this->mWriteEndpoint, pReport, 1, &dummy));
                    R_ABORT_UNLESS(eventFire(&this->mWriteCompletedEventFwd));
                    break;
                AMS_UNREACHABLE_DEFAULT_CASE();
            }
            break;
        }
    }

    void GameCubeDriver::GetPacketForBypass(uint8_t* pOutBuffer)
    {
        armDCacheClean(this->mReadXferPage, os::MemoryPageSize);
        memcpy(pOutBuffer, this->mReadXferPage, 0x25);
    }

    Result GameCubeDriver::CloseRead()
    {
        return usbHsEpCloseFwd(&this->mReadEndpoint);
    }

    Result GameCubeDriver::CloseWrite()
    {
        return usbHsEpCloseFwd(&this->mWriteEndpoint);
    }
    
    void GameCubeDriver::Release()
    {
        if (this->mInterfaceMappee != 0)
        {
            R_ABORT_UNLESS(svc::InvalidateProcessDataCache(this->mClientProcess, this->mInterfaceMappee, os::MemoryPageSize));
            R_ABORT_UNLESS(svc::UnmapProcessMemory(this->mInterfacePage, this->mClientProcess, this->mInterfaceMappee, os::MemoryPageSize));
        }

        if (this->mReadEpMappee != 0)
        {
            R_ABORT_UNLESS(svc::InvalidateProcessDataCache(this->mClientProcess, this->mReadEpMappee, os::MemoryPageSize));
            R_ABORT_UNLESS(svc::UnmapProcessMemory(this->mReadEpPage, this->mClientProcess, this->mReadEpMappee, os::MemoryPageSize));
        }

        if (this->mWriteEpMappee != 0)
        {
            R_ABORT_UNLESS(svc::InvalidateProcessDataCache(this->mClientProcess, this->mWriteEpMappee, os::MemoryPageSize));
            R_ABORT_UNLESS(svc::UnmapProcessMemory(this->mWriteEpPage, this->mClientProcess, this->mWriteEpMappee, os::MemoryPageSize));
        }

        /* Note: We don't check results here because officially, these can return a failure result code (0x25a8c) */
        usbHsEpCloseFwd(&this->mReadEndpoint);
        usbHsEpCloseFwd(&this->mWriteEndpoint);

        serviceClose(&this->mReadEndpoint);
        serviceClose(&this->mWriteEndpoint);

        this->mInterfaceMappee = 0;
        this->mReadEpMappee = 0;
        this->mWriteEpMappee = 0;

        eventClose(&this->mInterfaceXferCompleted);
        eventClose(&this->mReadCompletedEvent);
        eventClose(&this->mWriteCompletedEvent);

        AsyncCtrlXferCached* pNext = this->pCachedXfers;
        while (pNext)
        {
            pNext = pNext->pNext;
            free(pNext);
        }
        this->pCachedXfers = nullptr;

        this->mClientProcess = INVALID_HANDLE;
        this->mpInterface = nullptr;

        this->mState = GameCubeDriverState::Unacquired;
    }

    bool GameCubeDriver::IsInUse()
    {
        return this->mState != GameCubeDriverState::Unacquired;
    }

    void GameCubeDriver::Finalize()
    {
        os::WaitThread(&mThread);
    }
}