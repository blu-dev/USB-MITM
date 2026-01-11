#include "driver_thread.hpp"
#include "usb_shim.h"
#include "logger.hpp"

#define PAGE_ALIGN(e) (((e) + (ams::os::MemoryPageSize - 1)) & ~(ams::os::MemoryPageSize - 1))

namespace usb::gc
{
    using namespace ams::literals;
    namespace
    {
        static constexpr size_t g_MaxAsyncXfers = 4;

        /* Structure defining our adapter interface details */
        struct ProxyInterfaceImpl
        {
            enum CompletionEventId : u32
            {
                Interface = 0,
                ReadEndpoint = 1,
                WriteEndpoint = 2,

                MAX = 3
            };

            Service mIfSession;
            Service mReadEpSession;
            Service mWriteEpSession;

            Handle mClientProcess;
            Handle mStateChangeEvent;
            Handle mCompletionEvents[CompletionEventId::MAX];
            Event mExposedCompletionEvents[CompletionEventId::MAX];

            /* These structs go unused aside for a sanity check during initialization */
            struct usb_endpoint_descriptor mReadDescriptor;
            struct usb_endpoint_descriptor mWriteDescriptor;

            /* Pending async transfers so that the HID service can get it's state required to initialize */
            IntfAsyncXfer mPendingXfers[g_MaxAsyncXfers];
            u32 mNumPending;

            UsbHsXferReport mLatestWriteReport;
            UsbHsXferReport mLatestReadReport;

            bool mHasStarted;
            bool mIsAcquired;
            bool mIsRequestShutdown;

            void Initialize(Handle ClientProcess, Service IfSession, const UsbHsInterface* intf)
            {
                AMS_ASSERT(!mIsAcquired);

                mIfSession = IfSession;
                mClientProcess = ClientProcess;

                R_ABORT_UNLESS(usbHsIfGetCtrlXferCompletionEventFwd(&mIfSession, &mCompletionEvents[CompletionEventId::Interface]));
                const struct usb_endpoint_descriptor *ReadEndpoint, *WriteEndpoint;

                size_t i;
                for (i = 0; i < 15; i++)
                {
                    ReadEndpoint = intf->inf.input_endpoint_descs + i;
                    if (ReadEndpoint->bLength != 0)
                        break;
                }
                AMS_ABORT_UNLESS(i < 15, "ReadEndpoint not found");

                for (i = 0; i < 15; i++)
                {
                    WriteEndpoint = intf->inf.output_endpoint_descs + i;
                    if (WriteEndpoint->bLength != 0)
                        break;
                }
                AMS_ABORT_UNLESS(i < 15, "WriteEndpoint not found");

                /* NOTE: We could hardcode the paramters here for these endpoints, but I'd rather showcase what exactly we're passing in like the libnx */
                /* base implementation of these methods does */
                R_ABORT_UNLESS(usbHsIfOpenUsbEpFwd(
                    &mIfSession, &mReadEpSession, 1,
                    (ReadEndpoint->bmAttributes & USB_TRANSFER_TYPE_MASK) + 1,
                    ReadEndpoint->bEndpointAddress & USB_ENDPOINT_ADDRESS_MASK,
                    (ReadEndpoint->bEndpointAddress & USB_ENDPOINT_IN) == 0 ? 0x1 : 0x2,
                    ReadEndpoint->wMaxPacketSize,
                    &mReadDescriptor
                ));

                R_ABORT_UNLESS(usbHsIfOpenUsbEpFwd(
                    &mIfSession, &mWriteEpSession, 1,
                    (WriteEndpoint->bmAttributes & USB_TRANSFER_TYPE_MASK) + 1,
                    WriteEndpoint->bEndpointAddress & USB_ENDPOINT_ADDRESS_MASK,
                    (WriteEndpoint->bEndpointAddress & USB_ENDPOINT_IN) == 0 ? 0x1 : 0x2,
                    WriteEndpoint->wMaxPacketSize,
                    &mWriteDescriptor
                ));

                /* Done automatically by libnx, must be explicitly done here */
                R_ABORT_UNLESS(usbHsEpPopulateRingFwd(&mReadEpSession));
                R_ABORT_UNLESS(usbHsEpPopulateRingFwd(&mWriteEpSession));

                /* Endpoints have been opened, let's get the transfer completion events for all of them */
                R_ABORT_UNLESS(usbHsIfGetStateChangeEventFwd(&mIfSession, &mStateChangeEvent));
                R_ABORT_UNLESS(usbHsEpGetCompletionEventFwd(&mReadEpSession, &mCompletionEvents[CompletionEventId::ReadEndpoint]));
                R_ABORT_UNLESS(usbHsEpGetCompletionEventFwd(&mWriteEpSession, &mCompletionEvents[CompletionEventId::WriteEndpoint]));

                /* Let's create proxy events for all of the xfer completions, since we need control over when they get signaled or not */
                R_ABORT_UNLESS(eventCreate(&mExposedCompletionEvents[CompletionEventId::Interface], false));
                R_ABORT_UNLESS(eventCreate(&mExposedCompletionEvents[CompletionEventId::ReadEndpoint], false));
                R_ABORT_UNLESS(eventCreate(&mExposedCompletionEvents[CompletionEventId::WriteEndpoint], false));

                /* Quick Sanity Check */
                /* Originally, this also did a sanity check on the bInterval values, but since the */
                /* code for that could be possible to disable in the future, I removed them*/
                AMS_ABORT_UNLESS(mReadDescriptor.bEndpointAddress == 0x81);
                AMS_ABORT_UNLESS(mWriteDescriptor.bEndpointAddress == 0x2);

                mLatestReadReport.xferId = UINT32_MAX;
                mLatestWriteReport.xferId = UINT32_MAX;
                mNumPending = 0;
                mHasStarted = false;
                mIsAcquired = true;
            }

            void Finalize()
            {
                AMS_ASSERT(mIsAcquired);

                eventClose(&mExposedCompletionEvents[CompletionEventId::WriteEndpoint]);
                eventClose(&mExposedCompletionEvents[CompletionEventId::ReadEndpoint]);
                eventClose(&mExposedCompletionEvents[CompletionEventId::Interface]);

                svcCloseHandle(mCompletionEvents[CompletionEventId::WriteEndpoint]);
                svcCloseHandle(mCompletionEvents[CompletionEventId::ReadEndpoint]);
                svcCloseHandle(mCompletionEvents[CompletionEventId::Interface]);

                // R_ABORT_UNLESS(usbHsEpCloseFwd(&mWriteEpSession));
                // R_ABORT_UNLESS(usbHsEpCloseFwd(&mReadEpSession));

                serviceClose(&mWriteEpSession);
                serviceClose(&mReadEpSession);
                serviceClose(&mIfSession);

                mIsAcquired = false;
                mIsRequestShutdown = false;
            }
        };

        /* Structure that contains metadata for our multi-waiter in the driver thread, allowing us */
        /* to know which interfaces/events need to be processed when it wakes up */
        struct WaitHolderData
        {
            enum class Kind : u32 {
                InterfaceChangeRequested,
                InterfaceOperationFinished
            };

            Kind mKind;
            u32 mIntfId;
            ProxyInterfaceImpl::CompletionEventId mEventId;
        };

        /* Global variables defining our thread state */
        static constexpr size_t g_MaxSupportedAdapters = 4;
        static constexpr size_t g_ThreadStackSize = 16_KB;
        static constexpr s32 g_ThreadPriority = -11;
        alignas(ams::os::MemoryPageSize) static u8 g_ThreadStack[g_ThreadStackSize];
        static ams::os::ThreadType g_Thread;

        /* Lock on mutating the adapter state */
        /* Lots of the adapter state is read-only and read by the thread, so there isn't any reason to lock this mutex */
        /* Outside of looking for adapters to shut down/initialize */
        static ams::os::MutexType g_InterfaceMutex;

        /* Our 4 Adapter Interfaces. These contain all of the state required for interacting with the GC adapters and proxying their */
        /* packets to/from the HID service */
        static ProxyInterfaceImpl g_Interfaces[g_MaxSupportedAdapters];

        /* Event signaled when there is a large enough change to our thread state that we need to reconstruct our multi-waiter */
        static ams::os::EventType g_InterfaceUpdateRequested;

        /* Packet Memory */
        /* These are page-aligned regions of memory that we are going to read/write the packets to/from */
        /* There are two pages assigned to each adapter (assigned statically) */
        /* There is no synchronization here. If the client runs multiple "write" packets before we've read one, then it overrides the previous */
        /* Packets they sent. This is similar to a mailbox */
        alignas(ams::os::MemoryPageSize) static u8 g_AdapterRwMemory[g_MaxSupportedAdapters * ams::os::MemoryPageSize * 2];

        /* We special case this packet so that if HID requests a write before we've initialized properly (very unlikely) they don't overwrite this packet in a race condition */
        alignas(ams::os::MemoryPageSize) static u8 g_InitializePacket[ams::os::MemoryPageSize] = { 0x13 };

        /* Async Transfer Scratch Memory */
        /* Because we only support up to a max of 4 adapters plugged in (already likely more than enough), we can set aside a static */
        /* region of memory that is used for asynchronous interface transfers. */
        /* Based on testing, the HID process does not double up on these transfers on the same interface, so it's fine to assign these regions */
        /* statically based on interface IDs */
        alignas(ams::os::MemoryPageSize) static u8 g_AsyncXferScratch[ams::os::MemoryPageSize * g_MaxSupportedAdapters];

        /* Memory Transfer */
        /* We need to have a page of memory that we are able to map/unmap at runtime in order to read/write to/from the client buffers */
        /* This memory space must be identified at runtime, we have no way to unmap our own processes's data memory region, otherwise */
        /* I would just use static variables like we do the rest of the time */
        static ams::os::MutexType g_TransferMutex;
        static uintptr_t g_TransferMemory;

        static u8* MemoryForInterface(u32 id, bool read) {
            return g_AdapterRwMemory + ams::os::MemoryPageSize * (id * 2 + (read ? 1 : 0));
        }

        static void DriverThreadFunction(void*)
        {
            ams::os::MultiWaitType Waiter;
            ams::os::MultiWaitHolderType WaitHolders[MAX_WAIT_OBJECTS];
            WaitHolderData WaitHolderDatas[MAX_WAIT_OBJECTS];
            u32 WaitHolderCount = 0;

            u32 EnabledInterfaces[g_MaxSupportedAdapters] = { 0 };
            u32 EnabledIntfCount = 0;

            DEBUG("[DriverThread::Driver] Initializing thread\n");

            ams::os::InitializeMultiWait(&Waiter);

            /* Wait for the first time an interface update is requested */
            /* After this initial wait, this event gets added to a multi-wait list */
            DEBUG("[DriverThread::Driver] Waiting for first interface request\n");
            ams::os::WaitEvent(&g_InterfaceUpdateRequested);
            while (true) {
                DEBUG("[DriverThread::Driver] Interface state change requested, reconstructing async waiters\n");
                /* We clear the event explicitly (and declare it without autoclear) */
                /* This is because when the event gets signaled from a MultiWait it does not clear even if autoclear is set */
                ams::os::ClearEvent(&g_InterfaceUpdateRequested);
                EnabledIntfCount = 0;

                /* Find the number of enabled interfaces */
                ams::os::LockMutex(&g_InterfaceMutex);
                for (u32 i = 0; i < g_MaxSupportedAdapters; i++)
                {
                    if (g_Interfaces[i].mIsRequestShutdown)
                        g_Interfaces[i].Finalize();
                    else if (g_Interfaces[i].mIsAcquired)
                        EnabledInterfaces[EnabledIntfCount++] = i;
                }
                ams::os::UnlockMutex(&g_InterfaceMutex);

                /* If we currently don't have any interfaces, then restart the loop waiting for the next update event */
                if (EnabledIntfCount == 0) {
                    DEBUG("[DriverThread::Driver] No enabled interfaces, waiting until another interface request\n");
                    ams::os::WaitEvent(&g_InterfaceUpdateRequested);
                    continue;
                }

                DEBUG("[DriverThread::Driver] There are %u enabled interfaces\n", EnabledIntfCount);

                DEBUG("[DriverThread::Driver] Cleaning up previous waiter\n");
                /* Initialize our MultiWait holder again */
                ams::os::UnlinkAllMultiWaitHolder(&Waiter);
                for (u32 i = 0; i < WaitHolderCount; i++)
                {
                    ams::os::FinalizeMultiWaitHolder(&WaitHolders[i]);
                }

                DEBUG("[DriverThread::Driver] Initializing new waiter\n");
                /* Reinitialize our first event (InterfaceChangeRequested) */
                /* TODO: Abstract this into a struct with an associated method since this is very easy to make mistakes on */
                WaitHolderCount = 0;
                WaitHolderDatas[WaitHolderCount] = (WaitHolderData){
                    .mKind = WaitHolderData::Kind::InterfaceChangeRequested
                };
                ams::os::InitializeMultiWaitHolder(&WaitHolders[WaitHolderCount], &g_InterfaceUpdateRequested);
                ams::os::SetMultiWaitHolderUserData(&WaitHolders[WaitHolderCount], reinterpret_cast<uintptr_t>(&WaitHolderDatas[WaitHolderCount]));
                ams::os::LinkMultiWaitHolder(&Waiter, &WaitHolders[WaitHolderCount++]);


                /* For each of our enabled interfaces, add their read/write endpoints to the MultiWaiter */
                for (u32 i = 0; i < EnabledIntfCount; i++)
                {
                    u32 IntfId = EnabledInterfaces[i];
                    /* Before adding to the multi-wait list, we check to see if the adapter has started yet */
                    /* Starting the adapter means that you send a packet that simply indicates it's time to start polling*/
                    /* TODO: Should this really be handled here? I don't know, maybe we just allow the HID service to send this */
                    if (!g_Interfaces[IntfId].mHasStarted)
                    {
                        DEBUG("[DriverThread::Driver] Adapter interface %u has not yet started, sending initialization packet and requesting read\n", IntfId);
                        u32 dummy;
                        R_ABORT_UNLESS(usbHsEpPostBufferAsyncFwd(
                            &g_Interfaces[IntfId].mWriteEpSession,
                            g_InitializePacket,
                            1,
                            0,
                            &dummy
                        ));
                        R_ABORT_UNLESS(usbHsEpPostBufferAsyncFwd(
                            &g_Interfaces[IntfId].mReadEpSession,
                            MemoryForInterface(IntfId, true),
                            37,
                            0,
                            &dummy
                        ));
                        g_Interfaces[IntfId].mHasStarted = true;
                    }

                    /* Priority matters here */
                    /* The MultiWait will signal the first one of these events which gets fired, and if multiple of them are active at a time it will */
                    /* the one first in the list (I believe). So putting these at the start (and read before write) means that it won't get blocked by async xfer */
                    /* requests. */
                    WaitHolderDatas[WaitHolderCount] = (WaitHolderData){
                        .mKind = WaitHolderData::Kind::InterfaceOperationFinished,
                        .mIntfId = IntfId,
                        .mEventId = ProxyInterfaceImpl::CompletionEventId::ReadEndpoint
                    };
                    WaitHolderDatas[WaitHolderCount + 1] = (WaitHolderData){
                        .mKind = WaitHolderData::Kind::InterfaceOperationFinished,
                        .mIntfId = IntfId,
                        .mEventId = ProxyInterfaceImpl::CompletionEventId::WriteEndpoint
                    };

                    ams::os::InitializeMultiWaitHolder(&WaitHolders[WaitHolderCount], g_Interfaces[IntfId].mCompletionEvents[ProxyInterfaceImpl::CompletionEventId::ReadEndpoint]);
                    ams::os::InitializeMultiWaitHolder(&WaitHolders[WaitHolderCount + 1], g_Interfaces[IntfId].mCompletionEvents[ProxyInterfaceImpl::CompletionEventId::WriteEndpoint]);
                    ams::os::SetMultiWaitHolderUserData(&WaitHolders[WaitHolderCount], reinterpret_cast<uintptr_t>(&WaitHolderDatas[WaitHolderCount]));
                    ams::os::SetMultiWaitHolderUserData(&WaitHolders[WaitHolderCount + 1], reinterpret_cast<uintptr_t>(&WaitHolderDatas[WaitHolderCount + 1]));
                    ams::os::LinkMultiWaitHolder(&Waiter, &WaitHolders[WaitHolderCount++]);
                    ams::os::LinkMultiWaitHolder(&Waiter, &WaitHolders[WaitHolderCount++]);

                    /* We only add one pending async xfer at a time since 1.) the HID service does not stack these, and 2.) we only have so many memory regions available */
                    if (g_Interfaces[IntfId].mNumPending > 0)
                    {
                        DEBUG("[DriverThread::Driver] Adapter interface %u has %u pending async transfer requests, processing the first one\n", IntfId, g_Interfaces[IntfId].mNumPending);
                        WaitHolderDatas[WaitHolderCount] = (WaitHolderData){
                            .mKind = WaitHolderData::Kind::InterfaceOperationFinished,
                            .mIntfId = IntfId,
                            .mEventId = ProxyInterfaceImpl::CompletionEventId::Interface
                        };
                        ams::os::InitializeMultiWaitHolder(&WaitHolders[WaitHolderCount], g_Interfaces[IntfId].mCompletionEvents[ProxyInterfaceImpl::CompletionEventId::Interface]);
                        ams::os::SetMultiWaitHolderUserData(&WaitHolders[WaitHolderCount], reinterpret_cast<uintptr_t>(&WaitHolderDatas[WaitHolderCount]));
                        ams::os::LinkMultiWaitHolder(&Waiter, &WaitHolders[WaitHolderCount++]);
                    }
                }

                /* Now that we've configured out multi-waiter, we are going to wait in a loop and continuously process requests */
                bool NeedsBreak = false;
                while (!NeedsBreak)
                {
                    ams::os::MultiWaitHolderType* pSignaled = ams::os::WaitAny(&Waiter);

                    WaitHolderData* pUserData = reinterpret_cast<WaitHolderData*>(ams::os::GetMultiWaitHolderUserData(pSignaled));
                    switch (pUserData->mKind)
                    {
                        /* If the client did something that would indicate a change in interface, we need to restart our loop */
                        case WaitHolderData::Kind::InterfaceChangeRequested: 
                        DEBUG("[DriverThread::Driver] Waiter signaled with interface state change, reinitializing the thread variables\n");
                        NeedsBreak = true;
                        break;

                        /* Otherwise, process our data requests as usual */
                        case WaitHolderData::Kind::InterfaceOperationFinished:
                        ProxyInterfaceImpl* pIntf = &g_Interfaces[pUserData->mIntfId];
                        IntfAsyncXfer* pRequest;

                        /* Reset the signal on the event since it's not autocleared */
                        R_ABORT_UNLESS(ams::svc::ResetSignal(pIntf->mCompletionEvents[pUserData->mEventId]));

                        u32 dummy;
                        switch (pUserData->mEventId)
                        {
                            case ProxyInterfaceImpl::CompletionEventId::Interface:
                                /* Technically super thread unsafe, but the HID service only submits one of these per intf at a time */
                                pRequest = &pIntf->mPendingXfers[--pIntf->mNumPending];

                                /* Populate the response regions of the async xfer request and then recreate the multi-wait because this one is now invalid */
                                R_ABORT_UNLESS(usbHsIfGetCtrlXferReportFwd(&pIntf->mIfSession, pRequest->mpReport, sizeof(UsbHsXferReport)));

                                if ((pRequest->bmRequestType & USB_ENDPOINT_IN) != 0)
                                {
                                    WriteWithTransfer(pIntf->mClientProcess, g_AsyncXferScratch + (ams::os::MemoryPageSize * pUserData->mIntfId), pRequest->mClientBuffer, pRequest->wValue);
                                }

                                R_ABORT_UNLESS(eventFire(&pIntf->mExposedCompletionEvents[pUserData->mEventId]));
                                NeedsBreak = true;
                                break;
                            case ProxyInterfaceImpl::CompletionEventId::ReadEndpoint:
                                /* Do nothing except request another read, but only if the xfer was successful (an error here will cause a deadlock) */
                                R_ABORT_UNLESS(usbHsEpGetXferReportFwd(&pIntf->mReadEpSession, &pIntf->mLatestReadReport, 1, &dummy));
                                if (AMS_UNLIKELY(dummy != 1))
                                {
                                    DEBUG("[DriverThread::Driver] Unable to get xfer report for latest read for adapter interface %u, not-continuing read operations\n", pUserData->mIntfId);
                                    R_ABORT_UNLESS(usbHsEpCloseFwd(&pIntf->mReadEpSession));
                                    break;
                                }

                                if (AMS_UNLIKELY(R_FAILED(pIntf->mLatestReadReport.res)))
                                {
                                    DEBUG(
                                        "[DriverThread::Driver] Latest read failed for adapter interface %u: { .res = %x, .requestedSize = %x, .transferredSize = %x }\n",
                                        pUserData->mIntfId, pIntf->mLatestReadReport.res, pIntf->mLatestReadReport.requestedSize, pIntf->mLatestReadReport.transferredSize
                                    );
                                    R_ABORT_UNLESS(usbHsEpCloseFwd(&pIntf->mReadEpSession));
                                }
                                else
                                {
                                    R_ABORT_UNLESS(usbHsEpPostBufferAsyncFwd(&pIntf->mReadEpSession, MemoryForInterface(pUserData->mIntfId, true), 37, 0, &dummy));
                                }

                                break;
                            case ProxyInterfaceImpl::CompletionEventId::WriteEndpoint:
                                /* TODO: Make this write request on-demand? Will take up less resources... */
                                /* Do nothing except request another write, but only if the xfer was successful (an error here will cause a deadlock) */
                                R_ABORT_UNLESS(usbHsEpGetXferReportFwd(&pIntf->mWriteEpSession, &pIntf->mLatestWriteReport, 1, &dummy));
                                if (AMS_UNLIKELY(dummy != 1))
                                {
                                    DEBUG("[DriverThread::Driver] Unable to get xfer report for latest write for adapter interface %u, not-continuing write operations\n", pUserData->mIntfId);
                                    R_ABORT_UNLESS(usbHsEpCloseFwd(&pIntf->mWriteEpSession));
                                    break;
                                }

                                if (AMS_UNLIKELY(R_FAILED(pIntf->mLatestWriteReport.res)))
                                {
                                    DEBUG(
                                        "[DriverThread::Driver] Latest write failed for adapter interface %u: { .res = %x, .requestedSize = %x, .transferredSize = %x }\n",
                                        pUserData->mIntfId, pIntf->mLatestWriteReport.res, pIntf->mLatestWriteReport.requestedSize, pIntf->mLatestWriteReport.transferredSize
                                    );
                                    R_ABORT_UNLESS(usbHsEpCloseFwd(&pIntf->mWriteEpSession));
                                }
                                else
                                {
                                    *MemoryForInterface(pUserData->mIntfId, false) = 0x11;
                                    R_ABORT_UNLESS(usbHsEpPostBufferAsyncFwd(&pIntf->mWriteEpSession, MemoryForInterface(pUserData->mIntfId, false), 5, 0, &dummy));
                                }
                                break;
                            AMS_UNREACHABLE_DEFAULT_CASE();
                        }
                    }
                }
            }
        }
    }

    /* Locates the closest memory after our executable section that we can map at least a page to for transfer memory */
    /* We should only need a page for HID client stuff */
    void LocateTransferMemory()
    {
        ams::svc::MemoryInfo MemInfo;
        ams::svc::PageInfo PageInfo;

        R_ABORT_UNLESS(ams::svc::QueryMemory(&MemInfo, &PageInfo, reinterpret_cast<uintptr_t>(Initialize)));

        uintptr_t SelfBase = MemInfo.base_address;
        uintptr_t CurrentPtr = SelfBase;

        while (true)
        {
            if (MemInfo.state == ams::svc::MemoryState_Free && MemInfo.size >= ams::os::MemoryPageSize)
            {
                break;
            }

            CurrentPtr += MemInfo.size;
            AMS_ABORT_UNLESS(CurrentPtr > SelfBase);
            R_ABORT_UNLESS(ams::svc::QueryMemory(&MemInfo, &PageInfo, CurrentPtr));
        }

        g_TransferMemory = MemInfo.base_address;
    }

    /* Read memory, using the transfer pages to proxy it out of the HID process */
    void ReadWithTransfer(
        Handle ForeignProcess,
        uintptr_t ForeignMemory,
        void* LocalMemory,
        size_t size
    )
    {
        if (size == 0)
        {
            DEBUG("[DriverThread::Api::ReadWithTransfer] Early exiting because a size of 0x0 was requested for transfer\n");
            return;
        }

        ams::os::LockMutex(&g_TransferMutex);

        R_ABORT_UNLESS(ams::svc::MapProcessMemory(g_TransferMemory, ForeignProcess, ForeignMemory, PAGE_ALIGN(size)));
        armDCacheClean(reinterpret_cast<void*>(g_TransferMemory), ams::os::MemoryPageSize);

        for (size_t i = 0; i < size; i++)
        {
            reinterpret_cast<uint8_t*>(LocalMemory)[i] = reinterpret_cast<uint8_t*>(g_TransferMemory)[i];
        }

        armDCacheFlush(reinterpret_cast<void*>(g_TransferMemory), ams::os::MemoryPageSize);
        R_ABORT_UNLESS(ams::svc::UnmapProcessMemory(g_TransferMemory, ForeignProcess, ForeignMemory, PAGE_ALIGN(size)));
        /* TODO: Are both of these really necessary? */
        R_ABORT_UNLESS(ams::svc::InvalidateProcessDataCache(ForeignProcess, ForeignMemory, PAGE_ALIGN(size)));
        R_ABORT_UNLESS(ams::svc::FlushProcessDataCache(ForeignProcess, ForeignMemory, PAGE_ALIGN(size)));

        ams::os::UnlockMutex(&g_TransferMutex);
    }

    /* Write memory, using the transfer pages to proxy it into the HID process */
    void WriteWithTransfer(
        Handle ForeignProcess,
        void* LocalMemory,
        uintptr_t ForeignMemory,
        size_t size
    )
    {
        if (AMS_UNLIKELY(size == 0))
        {
            DEBUG("[DriverThread::Api::WriteWithTransfer] Early exiting because a size of 0x0 was requested for transfer\n");
            return;
        }
        /* I don't want to through any debug logs in this function unless it crashes, since this is run so frequently it would cause */
        /* runtime performance issues if debug logging is enabled, so bad that it would likely impact test results */

        ams::os::LockMutex(&g_TransferMutex);

        R_ABORT_UNLESS(ams::svc::MapProcessMemory(g_TransferMemory, ForeignProcess, ForeignMemory, PAGE_ALIGN(size)));
        armDCacheClean(reinterpret_cast<void*>(g_TransferMemory), ams::os::MemoryPageSize);

        for (size_t i = 0; i < size; i++)
        {
            reinterpret_cast<uint8_t*>(g_TransferMemory)[i] = reinterpret_cast<uint8_t*>(LocalMemory)[i];
        }

        armDCacheFlush(reinterpret_cast<void*>(g_TransferMemory), ams::os::MemoryPageSize);
        R_ABORT_UNLESS(ams::svc::UnmapProcessMemory(g_TransferMemory, ForeignProcess, ForeignMemory, PAGE_ALIGN(size)));
        /* TODO: Are both of these really necessary? */
        R_ABORT_UNLESS(ams::svc::InvalidateProcessDataCache(ForeignProcess, ForeignMemory, PAGE_ALIGN(size)));
        R_ABORT_UNLESS(ams::svc::FlushProcessDataCache(ForeignProcess, ForeignMemory, PAGE_ALIGN(size)));

        ams::os::UnlockMutex(&g_TransferMutex);
    }

    void Initialize()
    {
        DEBUG("[DriverThread::Api::Initialize] Looking for transfer memory page\n");
        LocateTransferMemory();
        DEBUG("[DriverThread::Api::Initialize] Transfer memory page found at %llx\n", g_TransferMemory);
        ams::os::InitializeMutex(&g_InterfaceMutex, false, 1);
        ams::os::InitializeEvent(&g_InterfaceUpdateRequested, false, ams::os::EventClearMode_ManualClear);
        ams::os::InitializeMutex(&g_TransferMutex, false, 1);

        R_ABORT_UNLESS(ams::os::CreateThread(
            &g_Thread,
            DriverThreadFunction,
            nullptr,
            g_ThreadStack,
            g_ThreadStackSize,
            g_ThreadPriority
        ));

        ams::os::SetThreadNamePointer(&g_Thread, "usb::gc::DriverThread");
        ams::os::StartThread(&g_Thread);
    }

    void WaitProcess()
    {
        ams::os::WaitThread(&g_Thread);
        ams::os::FinalizeEvent(&g_InterfaceUpdateRequested);
        ams::os::FinalizeMutex(&g_InterfaceMutex);
    }

    /* Open our endpoints and set up for proxying data, requires that there be < 4 adapters already connected */
    ProxyInterface OpenInterface(Handle ClientProcess, Service IfSession, const UsbHsInterface* pInterface)
    {
        ams::os::LockMutex(&g_InterfaceMutex);
        bool AwaitingShutdown = false;
        size_t i;
        for (i = 0; i < g_MaxSupportedAdapters; i++)
        {
            if (!g_Interfaces[i].mIsAcquired) break;
            else if (g_Interfaces[i].mIsRequestShutdown) AwaitingShutdown = true;
        }

        if (i == g_MaxSupportedAdapters)
        {
            ams::os::UnlockMutex(&g_InterfaceMutex);
            if (AwaitingShutdown)
            {
                DEBUG("[DriverThread::Api::OpenInterface] No available adatper spots found, but at least one is shutting down. Waiting for that to finish\n");
            }
            else
            {
                DEBUG("[DriverThread::Api::OpenInterface] No available adapter spots found, and none of them are shutting donw. Aborting\n");
                AMS_ABORT("Too many adapters plugged in");
            }
            ams::os::SleepThread(ams::TimeSpan::FromMicroSeconds(100));
            return OpenInterface(ClientProcess, IfSession, pInterface);
        }

        DEBUG("[DriverThread::Api::OpenInterface] Found available adapter slot %u, initializing adapter\n", i);

        g_Interfaces[i].Initialize(ClientProcess, IfSession, pInterface);

        DEBUG("[DriverThread::Api::OpenInterface] Adapter %u initialized\n", i);

        ams::os::UnlockMutex(&g_InterfaceMutex);
        ams::os::SignalEvent(&g_InterfaceUpdateRequested);

        /* We unlock before returning because all of this data is read-only after initialization */
        return (ProxyInterface) {
            .mId = (u32)i,
            .mIfStateChangeEvent = g_Interfaces[i].mStateChangeEvent,
            .mCtrlXferCompletionEvent = g_Interfaces[i].mExposedCompletionEvents[ProxyInterfaceImpl::CompletionEventId::Interface].revent,
            .mReadPostBufferCompletionEvent = g_Interfaces[i].mExposedCompletionEvents[ProxyInterfaceImpl::CompletionEventId::ReadEndpoint].revent,
            .mWritePostBufferCompletionEvent = g_Interfaces[i].mExposedCompletionEvents[ProxyInterfaceImpl::CompletionEventId::WriteEndpoint].revent,
        };
    }

    void CloseInterface(InterfaceId id)
    {
        AMS_ABORT_UNLESS(id < g_MaxSupportedAdapters, "Invalid interface id");
        /* There's no need to lock here, since we don't actually mutate any state we are only sending a message to the driver thread */
        g_Interfaces[id].mIsRequestShutdown = true;
        ams::os::SignalEvent(&g_InterfaceUpdateRequested);
    }

    void IntfAsyncTransfer(InterfaceId id, IntfAsyncXfer xfer)
    {
        AMS_ABORT_UNLESS(id < g_MaxSupportedAdapters, "Invalid interface id");

        ProxyInterfaceImpl* pIntf = &g_Interfaces[id];

        if (pIntf->mNumPending == g_MaxAsyncXfers)
        {
            AMS_ABORT("Too many pending async xfers");
        }

        /* If it's an outbound write map & copy the buffer here */
        if ((xfer.bmRequestType & USB_ENDPOINT_IN) == 0)
        {
            DEBUG("[DriverThread::Api::IntfAsyncTransfer] Async interface transfer requested with write, transferring memory to scratch region %u\n", id);
            ReadWithTransfer(pIntf->mClientProcess, xfer.mClientBuffer, g_AsyncXferScratch + (ams::os::MemoryPageSize * id), xfer.wValue);
        }
        else
        {
            DEBUG("[DriverThread::Api::IntfAsyncTransfer] Async interface transfer requested with read, no transfer will happen until request has completed %u\n", id);
        }

        R_ABORT_UNLESS(usbHsIfCtrlXferAsyncFwd(
            &pIntf->mIfSession, xfer.bmRequestType, xfer.bRequest, xfer.wValue, xfer.wIndex, xfer.wLength,
            reinterpret_cast<u64>(g_AsyncXferScratch + (ams::os::MemoryPageSize * id))
        ));

        pIntf->mPendingXfers[pIntf->mNumPending++] = xfer;

        /* Signal to recreate the multi-wait setup */
        ams::os::SignalEvent(&g_InterfaceUpdateRequested);
    }

    void WritePacket(InterfaceId id, u64 buffer, size_t size, UsbHsXferReport* pReport)
    {
        /* If it's the initialization packet, just stub this and fire the event */
        if (size != 1)
        {
            u8* pWriteMemory = MemoryForInterface(id, false);
            ReadWithTransfer(g_Interfaces[id].mClientProcess, buffer, pWriteMemory, size);
            const UsbHsXferReport* pLatest = &g_Interfaces[id].mLatestWriteReport;
            if (AMS_UNLIKELY(pLatest->xferId == UINT32_MAX))
            {
                *pReport = (UsbHsXferReport){
                    .xferId = 0,
                    .res = 0,
                    .requestedSize = (u32)size,
                    .transferredSize = (u32)size,
                    .id = 0
                };
            }
            else
            {
                *pReport = *pLatest;
            }
        }
        else
        {
            *pReport = (UsbHsXferReport){
                .xferId = 0,
                .res = 0,
                .requestedSize = 1,
                .transferredSize = 1,
                .id = 0
            };
        }
        R_ABORT_UNLESS(eventFire(&g_Interfaces[id].mExposedCompletionEvents[ProxyInterfaceImpl::CompletionEventId::WriteEndpoint]));
    }

    void ReadPacket(InterfaceId id, u64 buffer, size_t size, UsbHsXferReport* pReport)
    {
        AMS_ABORT_UNLESS(id < g_MaxSupportedAdapters, "Invalid interface id");
        WriteWithTransfer(g_Interfaces[id].mClientProcess, MemoryForInterface(id, true), buffer, size);
        const UsbHsXferReport* pLatest = &g_Interfaces[id].mLatestReadReport;
        if (AMS_UNLIKELY(pLatest->xferId == UINT32_MAX))
        {
            *pReport = (UsbHsXferReport){
                .xferId = 0,
                .res = 0,
                .requestedSize = (u32)size,
                .transferredSize = (u32)size,
                .id = 0
            };
        }
        else
        {
            *pReport = *pLatest;
        }
        R_ABORT_UNLESS(eventFire(&g_Interfaces[id].mExposedCompletionEvents[ProxyInterfaceImpl::CompletionEventId::ReadEndpoint]));
    }
}