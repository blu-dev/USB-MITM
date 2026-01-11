#include "usb_mitm_service.hpp"
#include "logger.hpp"
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
        STUB_LOG();
        R_SUCCEED();
    }

    Result UsbMitmEpSession::Close()
    {
        STUB_LOG();
        R_SUCCEED();
    }

    Result UsbMitmEpSession::GetCompletionEvent(sf::OutCopyHandle out)
    {
        out.SetValue(this->mCompletionEvent, false);
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
            ::usb::gc::WritePacket(mIntfId, buffer, size, &mReport);
        }
        else
        {
            ::usb::gc::ReadPacket(mIntfId, buffer, size, &mReport);
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
            *reinterpret_cast<UsbHsXferReport*>(out.GetPointer()) = mReport;
        }

        AMS_UNUSED(out, max);
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
        /* This call was expected and I'm hoping that not fulfilling it won't matter */
        AMS_UNUSED(size, buffer);
        STUB_LOG();
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
        DEBUG("UsbMitmIfSession[%u]::~UsbMitmIfSession()\n", mProxy.mId);
        ::usb::gc::CloseInterface(mProxy.mId);
    }

    Result UsbMitmIfSession::GetStateChangeEvent(sf::OutCopyHandle out)
    {
        DEBUG("UsbMitmIfSession[%u]::GetStateChangeEvent()\n", mProxy.mId);
        out.SetValue(mProxy.mIfStateChangeEvent, false);
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
        DEBUG("UsbMitmIfSession[%u]::CtrlXferAsync(%x, %x, %x, %x, %x, %llx):\n", mProxy.mId, bmRequestType, bRequest, wValue, wIndex, wLength, buffer);

        ::usb::gc::IntfAsyncTransfer(mProxy.mId, {
            .mpReport = &mFakedReport,
            .mClientBuffer = buffer,
            .bmRequestType = bmRequestType,
            .bRequest = bRequest,
            .wValue = wValue,
            .wIndex = wIndex,
            .wLength = wLength
        });

        R_SUCCEED();
    }
    Result UsbMitmIfSession::GetCtrlXferCompletionEvent(sf::OutCopyHandle out)
    {
        DEBUG("UsbMitmIfSession[%u]::GetCtrlXferCompletionEvent()\n", mProxy.mId);
        out.SetValue(mProxy.mCtrlXferCompletionEvent, false);
        R_SUCCEED();
    }
    Result UsbMitmIfSession::GetCtrlXferReport(const sf::OutBuffer &out)
    {
        DEBUG("UsbMitmIfSession[%u]::GetCtrlXferReport(): { .res = %x, .requestedSize = %x, .transferredSize = %x }\n", mProxy.mId, mFakedReport.res, mFakedReport.requestedSize, mFakedReport.transferredSize);
        if (mFakedReport.res != 0)
        {
            mFakedReport.res = 0;
            mFakedReport.transferredSize = mFakedReport.requestedSize;
        }
        *reinterpret_cast<UsbHsXferReport*>(out.GetPointer()) = mFakedReport;

        R_SUCCEED();
    }
    Result UsbMitmIfSession::ResetDevice()
    {
        UNEXPECTED_CALL();
    }
    Result UsbMitmIfSession::OpenUsbEp(sf::Out<sf::SharedPointer<::ams::usb::IClientEpSession>> out_session, sf::Out<usb_endpoint_descriptor> out_desc, u16 maxUrbCount, u32 epType, u32 epNumber, u32 epDirection, u32 maxXferSize)
    {
        DEBUG("UsbMitmIfSession[%u]::OpenUsb(%x, %x, %x, %x, %x):\n", mProxy.mId, maxUrbCount, epType, epNumber, epDirection, maxXferSize);
        AMS_UNUSED(out_desc, maxUrbCount, epType, epNumber, epDirection, maxXferSize);
        bool IsWriteEndpoint = epDirection == 1;
        if (IsWriteEndpoint)
        {
            DEBUG("\tOpening write endpoint to GameCube adapter %u\n", mProxy.mId);
        }
        else
        {
            DEBUG("\tOpening read endpoint to GameCube adapter %u\n", mProxy.mId);
        }

        out_session.SetValue(sf::ObjectFactory<sf::ExpHeapAllocator::Policy>::CreateSharedEmplaced<ams::usb::IClientEpSession, UsbMitmEpSession>(
            std::addressof(g_SfAllocator),
            mClientProcess, mProxy.mId, IsWriteEndpoint ? mProxy.mWritePostBufferCompletionEvent : mProxy.mReadPostBufferCompletionEvent, IsWriteEndpoint
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

    Result UsbMitmService::AcquireUsbIf(const sf::OutMapAliasBuffer &out1, const sf::OutMapAliasBuffer &out2, sf::Out<sf::SharedPointer<::ams::usb::IClientIfSession>> out_session, u32 interfaceId)
    {
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

        /* We need to trick the process into thinking that we are the session driver for a very brief moment */
        Service IfSession;
        res = usbHsAcquireUsbIfFwd(
            &g_ProxyUsbService, &IfSession,
            out1.GetPointer(), out1.GetSize(),
            out2.GetPointer(), out2.GetSize(),
            interfaceId
        );

        if (R_SUCCEEDED(res))
        {
            DEBUG("\tSuccessfully acquired the GameCube Adapter via usb:hs:a service, sending device to driver thread\n");
            ::usb::gc::ProxyInterface proxy = ::usb::gc::OpenInterface(mClientProcess, IfSession, &QueryInterfaces[i]);
            out_session.SetValue(sf::ObjectFactory<sf::ExpHeapAllocator::Policy>::CreateSharedEmplaced<ams::usb::IClientIfSession, UsbMitmIfSession>(std::addressof(g_SfAllocator), mClientProcess, proxy));
            R_SUCCEED();
        }
        else
        {
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
        R_ABORT_UNLESS(ams::sm::GetServiceHandle(&UsbHandle, ams::sm::ServiceName::Encode("usb:hs:a")));

        serviceCreate(&g_ProxyUsbService, UsbHandle);

        R_ABORT_UNLESS(serviceConvertToDomain(&g_ProxyUsbService));
        serviceAssumeDomain(&g_ProxyUsbService);

        DEBUG("\tRegistering with usb:hs:a service\n");
        R_ABORT_UNLESS(serviceDispatch(&g_ProxyUsbService, 0, .in_num_handles = 1, .in_handles { CUR_PROCESS_HANDLE }));

        DEBUG("\tConnected and registered with usb:hs:a\n");

        g_HeapHandle = lmem::CreateExpHeap(g_HeapMemory, g_HeapMemorySize, lmem::CreateOption_ThreadSafe);
        AMS_ABORT_UNLESS(g_HeapHandle != nullptr);
        g_SfAllocator.Attach(g_HeapHandle);

        DEBUG("\tCreated heap for service objects\n");
    }
}