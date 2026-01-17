#include "usb_mitm_service.hpp"
#include "logger.hpp"
#include "sniffer.hpp"
#include "usb_shim.h"

#define STUB_LOG() ::usb::util::Log("%s (stubbed)\n", __func__)
#define R_FUNCTION_LOG(res) ::usb::util::Log("%s = %x\n", __func__, res.GetValue())
#define UNEXPECTED_CALL() ::usb::util::Log("Call to %s was unexpected!\n", __func__); AMS_ABORT("Call to function %s was unexpected", __func__)

#define PAGE_ALIGN(e) (((e) + (ams::os::MemoryPageSize - 1)) & ~(ams::os::MemoryPageSize - 1))

namespace ams::mitm::usb {
    namespace {
        constexpr size_t g_HeapMemorySize = 64_KB;
        alignas(os::MemoryPageSize) u8 g_HeapMemory[g_HeapMemorySize];

        constinit lmem::HeapHandle g_HeapHandle;
        constinit sf::ExpHeapAllocator g_SfAllocator = {};

        static ::Service g_ProxyUsbService;

        
        alignas(os::MemoryPageSize) u8 g_MuxerThreadStack[os::MemoryPageSize];
        static os::ThreadType g_MuxerThread;
        Handle g_ProxyStateChangeEvent = INVALID_HANDLE;
        Handle g_ClientStateChangeEvent = INVALID_HANDLE;
        Handle g_InterfaceAvailableEvent = INVALID_HANDLE;
        Event g_ForwardStateChangeEvent;
        Event g_ForwardInterfaceAvailableEvent;
        constexpr s32 g_MuxerThreadPriority = -11;

        void MuxerThreadFunction(void*)
        {
            os::MultiWaitType waiter;
            os::MultiWaitHolderType proxySignaled;
            os::MultiWaitHolderType clientSignaled;
            os::MultiWaitHolderType interfaceSignaled;

            while ((g_ClientStateChangeEvent == INVALID_HANDLE) || (g_ProxyStateChangeEvent == INVALID_HANDLE) || (g_InterfaceAvailableEvent == INVALID_HANDLE))
            {
                os::SleepThread(TimeSpan::FromMilliSeconds(10));
            }

            os::InitializeMultiWait(&waiter);
            os::InitializeMultiWaitHolder(&proxySignaled, g_ProxyStateChangeEvent);
            os::InitializeMultiWaitHolder(&clientSignaled, g_ClientStateChangeEvent);
            os::InitializeMultiWaitHolder(&interfaceSignaled, g_InterfaceAvailableEvent);
            os::LinkMultiWaitHolder(&waiter, &proxySignaled);
            os::LinkMultiWaitHolder(&waiter, &clientSignaled);
            os::LinkMultiWaitHolder(&waiter, &interfaceSignaled);

            while (true)
            {
                os::MultiWaitHolderType* pSignaled = os::WaitAny(&waiter);
                if (pSignaled == &proxySignaled)
                {
                    DEBUG("Proxy interface state changed\n");
                    R_ABORT_UNLESS(eventFire(&g_ForwardStateChangeEvent));
                    R_ABORT_UNLESS(svc::ResetSignal(g_ProxyStateChangeEvent));
                }
                else if (pSignaled == &clientSignaled)
                {
                    DEBUG("Client interface state changed\n");
                    R_ABORT_UNLESS(eventFire(&g_ForwardStateChangeEvent));
                    svc::ResetSignal(g_ClientStateChangeEvent);
                }
                else if (pSignaled == &interfaceSignaled)
                {
                    DEBUG("New interface available\n");
                    R_ABORT_UNLESS(eventFire(&g_ForwardInterfaceAvailableEvent));
                    R_ABORT_UNLESS(svc::ResetSignal(g_InterfaceAvailableEvent));
                }
                else
                {
                    AMS_ABORT("Unreachable code path entered");
                }
            }
        }
    }
}

/* UsbMitmEpSession Implementation */
namespace ams::mitm::usb
{
    UsbMitmEpSession::~UsbMitmEpSession() {
        STUB_LOG();
    }

    Result UsbMitmEpSession::ReOpen()
    {
        this->mIsClosed = false;
        R_SUCCEED();
    }

    Result UsbMitmEpSession::Close()
    {
        this->mIsClosed = true;
        if (this->mIsWriteEndpoint)
        {
            return this->mpDriver->CloseWrite();
        }
        else
        {
            return this->mpDriver->CloseRead();
        }
        // R_SUCCEED();
    }

    Result UsbMitmEpSession::GetCompletionEvent(sf::OutCopyHandle out)
    {
        if (this->mIsWriteEndpoint)
        {
            out.SetValue(this->mpDriver->WriteEvent(), false);
        }
        else
        {
            out.SetValue(this->mpDriver->ReadEvent(), false);
        }
        R_SUCCEED();
    }

    Result UsbMitmEpSession::PopulateRing()
    {
        STUB_LOG();
        R_SUCCEED();
    }

    Result UsbMitmEpSession::PostBufferAsync(sf::Out<u32> xferId, u32 size, u64 buffer, u64 id)
    {
        AMS_UNUSED(id);

        xferId.SetValue(0);
        if (this->mIsWriteEndpoint)
        {
            this->mpDriver->WritePacket(buffer, size, &this->mReport);
        }
        else
        {
            this->mpDriver->ReadPacket(buffer, size, &this->mReport);
        }

        R_SUCCEED();
    }

    Result UsbMitmEpSession::GetXferReport(const sf::OutAutoSelectBuffer &out, sf::Out<u32> count, u32 max)
    {
        if (max == 0)
        {
            count.SetValue(0);
        }
        else
        {
            DEBUG("GetXferReport: {.res = %x, .reqSize = %x, .transSize = %x, .xferId = %x, .id = %llx}\n", this->mReport.res, this->mReport.requestedSize, this->mReport.transferredSize, this->mReport.xferId, this->mReport.id);
            *reinterpret_cast<UsbHsXferReport*>(out.GetPointer()) = mReport;
        }

        count.SetValue(1);
        R_SUCCEED();
    }

    Result UsbMitmEpSession::BatchBufferAsync(sf::Out<u32> xferId, u32 urbCount, u32 unk1, u32 unk2, u64 buffer, u64 id)
    {
        AMS_UNUSED(xferId, urbCount, unk1, unk2, buffer, id);
        UNEXPECTED_CALL();
    }

    Result UsbMitmEpSession::CreateSmmuSpace(u32 size, u64 buffer)
    {
        AMS_UNUSED(size);
        if (this->mIsWriteEndpoint)
        {
            this->mpDriver->MapWritePage(buffer);
        }
        else
        {
            this->mpDriver->MapReadPage(buffer);
        }
        R_SUCCEED();
    }

    Result UsbMitmEpSession::ShareReportRing(sf::CopyHandle &&xfer_mem, u32 size)
    {
        AMS_UNUSED(xfer_mem, size);
        UNEXPECTED_CALL();
    }
}

/* UsbMitmIfSession Implementation */
namespace ams::mitm::usb
{
    UsbMitmIfSession::~UsbMitmIfSession() {
        DEBUG("UsbMitmIfSession::~UsbMitmIfSession()\n");
        this->mpDriver->Release();
        serviceClose(this->mpProxySession);
        free(this->mpProxySession);
    }

    Result UsbMitmIfSession::GetStateChangeEvent(sf::OutCopyHandle out)
    {
        out.SetValue(this->mpDriver->InterfaceChangeEvent(), false);
        R_SUCCEED();
    }
    Result UsbMitmIfSession::SetInterface(const sf::OutBuffer &out, u8 id)
    {
        AMS_UNUSED(out, id);
        UNEXPECTED_CALL();
    }
    Result UsbMitmIfSession::GetInterface(const sf::OutBuffer &out)
    {
        AMS_UNUSED(out);
        UNEXPECTED_CALL();
    }
    Result UsbMitmIfSession::GetAlternateInterface(const sf::OutBuffer &out, u8 id)
    {
        AMS_UNUSED(out, id);
        UNEXPECTED_CALL();
    }
    Result UsbMitmIfSession::GetCurrentFrame(sf::Out<u32> current_frame)
    {
        AMS_UNUSED(current_frame);
        UNEXPECTED_CALL();
    }
    Result UsbMitmIfSession::CtrlXferAsync(u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u64 buffer)
    {
        // DEBUG("UsbMitmIfSession::CtrlXferAsync(%x, %x, %x, %x, %x, %llx):\n", mProxy.mId, bmRequestType, bRequest, wValue, wIndex, wLength, buffer);

        this->mpDriver->ControlTransfer(
            bmRequestType,
            bRequest,
            wValue,
            wIndex,
            wLength,
            buffer,
            &this->mLastReport
        );

        R_SUCCEED();
    }
    Result UsbMitmIfSession::GetCtrlXferCompletionEvent(sf::OutCopyHandle out)
    {
        // DEBUG("UsbMitmIfSession[%u]::GetCtrlXferCompletionEvent()\n", mProxy.mId);
        out.SetValue(this->mpDriver->InterfaceEvent(), false);
        // out.SetValue(mProxy.mCtrlXferCompletionEvent, false);
        R_SUCCEED();
    }
    Result UsbMitmIfSession::GetCtrlXferReport(const sf::OutBuffer &out)
    {
        // DEBUG("UsbMitmIfSession[%u]::GetCtrlXferReport(): { .res = %x, .requestedSize = %x, .transferredSize = %x }\n", mProxy.mId, mFakedReport.res, mFakedReport.requestedSize, mFakedReport.transferredSize);
        DEBUG("AsyncXferReport: {.res = %x, .reqSize = %x, .transSize = %x, .xferId = %x, .id = %llx}\n", this->mLastReport.res, this->mLastReport.requestedSize, this->mLastReport.transferredSize, this->mLastReport.xferId, this->mLastReport.id);
        *reinterpret_cast<UsbHsXferReport*>(out.GetPointer()) = this->mLastReport;

        R_SUCCEED();
    }
    Result UsbMitmIfSession::ResetDevice()
    {
        UNEXPECTED_CALL();
    }
    Result UsbMitmIfSession::OpenUsbEp(sf::Out<sf::SharedPointer<::ams::usb::IClientEpSession>> out_session, sf::Out<usb_endpoint_descriptor> out_desc, u16 maxUrbCount, u32 epType, u32 epNumber, u32 epDirection, u32 maxXferSize)
    {
        // DEBUG("UsbMitmIfSession[%u]::OpenUsb(%x, %x, %x, %x, %x):\n", mProxy.mId, maxUrbCount, epType, epNumber, epDirection, maxXferSize);
        AMS_UNUSED(out_desc, maxUrbCount, epType, epNumber, epDirection, maxXferSize);
        bool IsWriteEndpoint = epDirection == 1;
        // if (IsWriteEndpoint)
        // {
        //     DEBUG("\tOpening write endpoint to GameCube adapter %u\n", mProxy.mId);
        // }
        // else
        // {
        //     DEBUG("\tOpening read endpoint to GameCube adapter %u\n", mProxy.mId);
        // }

        out_session.SetValue(sf::ObjectFactory<sf::ExpHeapAllocator::Policy>::CreateSharedEmplaced<ams::usb::IClientEpSession, UsbMitmEpSession>(
            std::addressof(g_SfAllocator),
            mpDriver, IsWriteEndpoint
        ));
        R_SUCCEED();
    }
}

namespace ams::mitm::usb
{
    namespace
    {
        constexpr uint16_t GameCubeAdapterVendor = 0x057E;
        constexpr uint16_t GameCubeAdapterProduct = 0x0337;

        const UsbHsInterfaceFilter GameCubeFilter = {
            .Flags = 0x03,
            .idVendor = GameCubeAdapterVendor,
            .idProduct = GameCubeAdapterProduct,
            .bcdDevice_Min = 0,
            .bcdDevice_Max = 0,
            .bDeviceClass = 0,
            .bDeviceSubClass = 0,
            .bDeviceProtocol = 0,
            .bInterfaceClass = 0x03,
            .bInterfaceSubClass = 0,
            .bInterfaceProtocol = 0,
        };
    }

    UsbMitmService::UsbMitmService(std::shared_ptr<::Service> &&s, const sm::MitmProcessInfo &c)
        : MitmServiceImplBase(std::move(s), c)
    {
        DEBUG("UsbMitmService::UsbMitmService():\n");
        DEBUG("\tConnecting to pm:dmnt\n");
        R_ABORT_UNLESS(pmdmntInitialize());

        ams::ncm::ProgramLocation DummyLocation;
        ams::cfg::OverrideStatus DummyStatus;
        DEBUG("\tGetting client process info\n");
        R_ABORT_UNLESS(ams::pm::dmnt::AtmosphereGetProcessInfo(&mClientProcess, &DummyLocation, &DummyStatus, c.process_id));
        DEBUG("\tAcquired process client info. Client handle is %x\n", mClientProcess);
        pmdmntExit();

    }

    Result UsbMitmService::QueryAllInterfaces(UsbHsInterfaceFilter filter, const sf::OutMapAliasArray<UsbHsInterface> &out, sf::Out<s32> total_out)
    {
        AMS_UNUSED(out, total_out);
        DEBUG("QueryAll { .Flags = %hx, .idVendor = %hx, .idProduct = %hx, .bcdDevice_Min = %hx, .bcdDevice_Max = %hx, .bDeviceClass = %x, .bDeviceSubClass = %x, .bDeviceProtocol = %x, .bInterfaceClass = %x, .bInterfaceSubClass = %x, .bInterfaceProtocol = %hx }\n", filter.Flags, filter.idVendor, filter.idProduct, filter.bcdDevice_Min, filter.bcdDevice_Max, filter.bDeviceClass, filter.bDeviceSubClass, filter.bDeviceProtocol, filter.bInterfaceClass, filter.bInterfaceSubClass, filter.bInterfaceProtocol);
        return sm::mitm::ResultShouldForwardToSession();
        // UsbHsInterface intfs[8];
        // Result r = usbHsQueryAllInterfacesFwd(m_forward_service.get(), &filter, out.GetPointer(), out.GetSize(), total_out.GetPointer());
        // for (s32 i = 0; i < total_out.GetValue(); i++)
        // {
        //     UsbHsInterface* intf = &out[i];
        //     u16 product = intf->device_desc.idProduct;
        //     u16 vendor = intf->device_desc.idVendor;
        //     DEBUG("\tPID: %hx VID: %hx\n", product, vendor);
        // }

        // s32 ourCount = 0;
        // R_TRY(usbHsQueryAllInterfacesFwd(&g_ProxyUsbService, &filter, intfs, 8, &ourCount));

        // for (s32 i = 0; i < ourCount; i++)
        // {
        //     UsbHsInterface* intf = &out[i];
        //     u16 product = intf->device_desc.idProduct;
        //     u16 vendor = intf->device_desc.idVendor;
        //     DEBUG("\t[PROXY] PID: %hx VID: %hx\n", product, vendor);  
        // }

        // UsbHsInterface QueryInterfaces[4];

        // s32 NumOut;

        // R_TRY(usbHsQueryAvailableInterfacesFwd(
        //     m_forward_service.get(),
        //     &GameCubeFilter,
        //     QueryInterfaces,
        //     4,
        //     &NumOut
        // ));

        // for (s32 i = 0; i < NumOut; i++)
        // {
        //     UsbHsInterface* intf = &QueryInterfaces[i];
        //     u16 product = intf->device_desc.idProduct;
        //     u16 vendor = intf->device_desc.idVendor;
        //     DEBUG("\t[PROXY-CUSTOM] PID: %hx VID: %hx\n", product, vendor);  
        // }

        // return r;
    }
    Result UsbMitmService::QueryAvailableInterfaces(UsbHsInterfaceFilter filter, const sf::OutMapAliasArray<UsbHsInterface> &out, sf::Out<s32> total_out)
    {
        AMS_UNUSED(out, total_out);
        DEBUG("QueryAvailable { .Flags = %hx, .idVendor = %hx, .idProduct = %hx, .bcdDevice_Min = %hx, .bcdDevice_Max = %hx, .bDeviceClass = %x, .bDeviceSubClass = %x, .bDeviceProtocol = %x, .bInterfaceClass = %x, .bInterfaceSubClass = %x, .bInterfaceProtocol = %hx }\n", filter.Flags, filter.idVendor, filter.idProduct, filter.bcdDevice_Min, filter.bcdDevice_Max, filter.bDeviceClass, filter.bDeviceSubClass, filter.bDeviceProtocol, filter.bInterfaceClass, filter.bInterfaceSubClass, filter.bInterfaceProtocol);
        return sm::mitm::ResultShouldForwardToSession();
        // UsbHsInterface intfs[8];
        // Result r = usbHsQueryAvailableInterfacesFwd(m_forward_service.get(), &filter, out.GetPointer(), out.GetSize(), total_out.GetPointer());

        // for (s32 i = 0; i < total_out.GetValue(); i++)
        // {
        //     UsbHsInterface* intf = &out[i];
        //     u16 product = intf->device_desc.idProduct;
        //     u16 vendor = intf->device_desc.idVendor;
        //     DEBUG("\tPID: %hx VID: %hx\n", product, vendor);
        // }

        // s32 ourCount = 0;
        // R_TRY(usbHsQueryAvailableInterfacesFwd(&g_ProxyUsbService, &filter, intfs, 8, &ourCount));

        // for (s32 i = 0; i < ourCount; i++)
        // {
        //     UsbHsInterface* intf = &out[i];
        //     u16 product = intf->device_desc.idProduct;
        //     u16 vendor = intf->device_desc.idVendor;
        //     DEBUG("\t[PROXY] PID: %hx VID: %hx\n", product, vendor);  
        // }

        // return r;
    }
    Result UsbMitmService::QueryAcquiredInterfaces(const sf::OutMapAliasArray<UsbHsInterface> &out, sf::Out<s32> total_out)
    {
        AMS_UNUSED(out, total_out);
        DEBUG("QueryAcquired");
        return sm::mitm::ResultShouldForwardToSession();
        // UsbHsInterface intfs[8];
        // s32 count = 0;
        // R_TRY(usbHsQueryAcquiredInterfacesFwd(m_forward_service.get(), &filter, intfs, 8, &count));

        // for (s32 i = 0; i < count; i++)
        // {
        //     out[i] = intfs[i];
        // }

        // s32 ourCount = 0;
        // R_TRY(usbHsQueryAcquiredInterfacesFwd(&g_ProxyUsbService, &filter, intfs, 8, &ourCount));

        // for (s32 i = 0; i < ourCount; i++)
        // {
        //     out[i + count] = intfs[i];
        // }

        // total_out.SetValue(ourCount + count);

        // for (s32 i = 0; i < total_out.GetValue(); i++)
        // {
        //     UsbHsInterface* intf = &out[i];
        //     u16 product = intf->device_desc.idProduct;
        //     u16 vendor = intf->device_desc.idVendor;
        //     DEBUG("\tPID: %hx VID: %hx\n", product, vendor);
        // }
        // R_SUCCEED();
    }

    Result UsbMitmService::CreateInterfaceAvailableEvent(sf::OutCopyHandle out, u8 id, UsbHsInterfaceFilter filter)
    {
        // AMS_UNUSED(out, id, filter);
        // DEBUG("CreateIntfAvailEvent { .Flags = %hx, .idVendor = %hx, .idProduct = %hx, .bcdDevice_Min = %hx, .bcdDevice_Max = %hx, .bDeviceClass = %x, .bDeviceSubClass = %x, .bDeviceProtocol = %x, .bInterfaceClass = %x, .bInterfaceSubClass = %x, .bInterfaceProtocol = %hx }\n", filter.Flags, filter.idVendor, filter.idProduct, filter.bcdDevice_Min, filter.bcdDevice_Max, filter.bDeviceClass, filter.bDeviceSubClass, filter.bDeviceProtocol, filter.bInterfaceClass, filter.bInterfaceSubClass, filter.bInterfaceProtocol);
        // // return sm::mitm::ResultShouldForwardToSession();
        Result r = usbHsCreateInterfaceAvailableEventFwd(&g_ProxyUsbService, &filter, id, &g_InterfaceAvailableEvent);
        if (R_SUCCEEDED(r))
        {
            out.SetValue(g_ForwardInterfaceAvailableEvent.revent, false);
        }
        return r;
        // return sm::mitm::ResultShouldForwardToSession();
    }

    Result UsbMitmService::DestroyInterfaceAvailableEvent(u8 id)
    {
        AMS_UNUSED(id);
        DEBUG("DestroyIntfAvailEvent\n");
        // return sm::mitm::ResultShouldForwardToSession();
        return usbHsDestroyInterfaceAvailableEventFwd(m_forward_service.get(), id);
    }

    Result UsbMitmService::GetInterfaceStateChangeEvent(sf::OutCopyHandle out)
    {
        AMS_UNUSED(out);
        // return sm::mitm::ResultShouldForwardToSession();
        // DEBUG("GetIntfStateChangeEvent\n");
        R_ABORT_UNLESS(usbHsGetInterfaceStateChangeEventFwd(m_forward_service.get(), &g_ClientStateChangeEvent));
        out.SetValue(g_ForwardStateChangeEvent.revent, false);
        R_SUCCEED();
    }

    Result UsbMitmService::AcquireUsbIf(const sf::OutMapAliasBuffer &out1, const sf::OutMapAliasBuffer &out2, sf::Out<sf::SharedPointer<::ams::usb::IClientIfSession>> out_session, u32 interfaceId)
    {
        // AMS_UNUSED(out1, out2, out_session, interfaceId);
        // return sm::mitm::ResultShouldForwardToSession();
        DEBUG("UsbMitmService::AcquireUsbIf()\n");
        UsbHsInterface QueryInterfaces[4];

        s32 NumOut;

        Result res = usbHsQueryAvailableInterfacesFwd(
            m_forward_service.get(),
            &GameCubeFilter,
            QueryInterfaces,
            4,
            &NumOut
        );

        if (AMS_UNLIKELY(R_FAILED(res)))
        {
            DEBUG("\tFailed to acquire available interfaces to compare GameCube adapter to, forwarding request to usb:hs\n");
            return sm::mitm::ResultShouldForwardToSession();
        }

        bool IsAcquiringGameCubeAdapter = false;
        s32 i = 0;
        for (; i < NumOut; i++) {
            if (QueryInterfaces[i].inf.ID == (s32)interfaceId) {
                IsAcquiringGameCubeAdapter = true;
                break;
            }
        }

        if (!IsAcquiringGameCubeAdapter)
        {
            DEBUG("\tClient did not attempt to acquire GameCube Adapter, forwarding request to usb:hs service\n");
            return sm::mitm::ResultShouldForwardToSession();
        }

        DEBUG("\tClient is attempting to acquire GameCube Adapter, redirecting request to usb:hs:a service with our process\n");
        // Service IfSession;
        // res = usbHsAcquireUsbIfFwd(
        //     m_forward_service.get(), &IfSession,
        //     out1.GetPointer(), out1.GetSize(),
        //     out2.GetPointer(), out2.GetSize(),
        //     interfaceId
        // );

        // if (R_SUCCEEDED(res))
        // {
        //     out_session.SetValue(sf::ObjectFactory<sf::ExpHeapAllocator::Policy>::CreateSharedEmplaced<ams::usb::IClientIfSession, ams::usb::sniffer::UsbIfSessionSniffer>(std::addressof(g_SfAllocator), mClientProcess, IfSession));
        // }
        
        // return res;

        Service* pIfSession = (Service*)malloc(sizeof(Service));
        res = usbHsAcquireUsbIfFwd(
            &g_ProxyUsbService, pIfSession,
            out1.GetPointer(), out1.GetSize(),
            out2.GetPointer(), out2.GetSize(),
            interfaceId
        );

        if (R_SUCCEEDED(res))
        {
            DEBUG("\tSuccessfully acquired the GameCube Adapter via usb:hs:a service, sending device to driver thread\n");
            ams::usb::gc::GameCubeDriver* pDriver = &ams::usb::gc::g_GameCubeDriver1;
            if (pDriver->IsInUse())
            {
                pDriver = &ams::usb::gc::g_GameCubeDriver2;
                AMS_ABORT_UNLESS(!pDriver->IsInUse(), "Could not find an available driver");
            }

            pDriver->Acquire(mClientProcess, pIfSession);

            out_session.SetValue(sf::ObjectFactory<sf::ExpHeapAllocator::Policy>::CreateSharedEmplaced<ams::usb::IClientIfSession, UsbMitmIfSession>(std::addressof(g_SfAllocator), mClientProcess, pIfSession, pDriver));
            R_SUCCEED();
        }
        else
        {
            free(pIfSession);
            DEBUG("\tAcquiring USB device failed, forwarding to usb:hs session so that the device still works\n");
            return sm::mitm::ResultShouldForwardToSession();
        }
    }

    void Initialize() {
        DEBUG("ams::mitm::usb::Initialize():\n");
        ams::os::NativeHandle UsbHandle;

        /* We connect to usb:hs:a since the GameCube adapter is only acquired through usb:hs when done in HID */
        /* This allows us to proxy the USB device packets */
        DEBUG("\tConnecting to the usb:hs:a service\n");
        R_ABORT_UNLESS(ams::sm::GetServiceHandle(&UsbHandle, ams::sm::ServiceName::Encode("usb:hs")));

        serviceCreate(&g_ProxyUsbService, UsbHandle);

        R_ABORT_UNLESS(serviceConvertToDomain(&g_ProxyUsbService));
        serviceAssumeDomain(&g_ProxyUsbService);

        DEBUG("\tRegistering with usb:hs:a service\n");
        R_ABORT_UNLESS(serviceDispatch(&g_ProxyUsbService, 0, .in_num_handles = 1, .in_handles { CUR_PROCESS_HANDLE }));

        R_ABORT_UNLESS(usbHsGetInterfaceStateChangeEventFwd(&g_ProxyUsbService, &g_ProxyStateChangeEvent));
        R_ABORT_UNLESS(eventCreate(&g_ForwardStateChangeEvent, false));
        R_ABORT_UNLESS(eventCreate(&g_ForwardInterfaceAvailableEvent, false));

        DEBUG("\tConnected and registered with usb:hs:a\n");

        g_HeapHandle = lmem::CreateExpHeap(g_HeapMemory, g_HeapMemorySize, lmem::CreateOption_ThreadSafe);
        AMS_ABORT_UNLESS(g_HeapHandle != nullptr);
        g_SfAllocator.Attach(g_HeapHandle);

        DEBUG("\tCreated heap for service objects\n");

        R_ABORT_UNLESS(os::CreateThread(&g_MuxerThread, MuxerThreadFunction, nullptr, g_MuxerThreadStack, os::MemoryPageSize, g_MuxerThreadPriority));
        os::SetThreadNamePointer(&g_MuxerThread, "usb::mitm::EventMuxerThread");
        os::StartThread(&g_MuxerThread);
    }
}