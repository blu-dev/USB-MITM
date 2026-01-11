#pragma once
#include <switch.h>
#include <stratosphere.hpp>
#include "driver_thread.hpp"
#include "logger.hpp"

#define AMS_USB_MITM_INTERFACE_INFO(C, H)                                                                                                                                                         \
    AMS_SF_METHOD_INFO(C, H, 7, Result, AcquireUsbIf, (const sf::OutMapAliasBuffer &out1, const sf::OutMapAliasBuffer &out2, sf::Out<sf::SharedPointer<::ams::usb::IClientIfSession>> out_session, u32 interfaceId), (out1, out2, out_session, interfaceId))

    // AMS_SF_METHOD_INFO(C, H, 1, Result, QueryAllInterfaces, (UsbHsInterfaceFilter filter, const sf::OutMapAliasArray<UsbHsInterfaceInfo> &out, sf::Out<s32> total_out), (filter, out, total_out)) 
    // AMS_SF_METHOD_INFO(C, H, 2, Result, QueryAvailableInterfaces, (UsbHsInterfaceFilter filter, const sf::OutMapAliasArray<UsbHsInterfaceInfo> &out, sf::Out<s32> total_out), (filter, out, total_out))

#define AMS_USB_CLIENT_IF_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, GetStateChangeEvent, (sf::OutCopyHandle out), (out)) \
    AMS_SF_METHOD_INFO(C, H, 1, Result, SetInterface, (const sf::OutBuffer &out, u8 id), (out, id)) \
    AMS_SF_METHOD_INFO(C, H, 2, Result, GetInterface, (const sf::OutBuffer &out), (out)) \
    AMS_SF_METHOD_INFO(C, H, 3, Result, GetAlternateInterface, (const sf::OutBuffer &out, u8 id), (out, id)) \
    AMS_SF_METHOD_INFO(C, H, 4, Result, GetCurrentFrame, (sf::Out<u32> current_frame), (current_frame)) \
    AMS_SF_METHOD_INFO(C, H, 5, Result, CtrlXferAsync, (u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u64 buffer), (bmRequestType, bRequest, wValue, wIndex, wLength, buffer)) \
    AMS_SF_METHOD_INFO(C, H, 6, Result, GetCtrlXferCompletionEvent, (sf::OutCopyHandle out), (out))  \
    AMS_SF_METHOD_INFO(C, H, 7, Result, GetCtrlXferReport, (const sf::OutBuffer &out), (out)) \
    AMS_SF_METHOD_INFO(C, H, 8, Result, ResetDevice, (), ()) \
    AMS_SF_METHOD_INFO(C, H, 9, Result, OpenUsbEp, (sf::Out<sf::SharedPointer<::ams::usb::IClientEpSession>> out_session, sf::Out<usb_endpoint_descriptor> out_desc, u16 maxUrbCount, u32 epType, u32 epNumber, u32 epDirection, u32 maxXferSize), (out_session, out_desc, maxUrbCount, epType, epNumber, epDirection, maxXferSize))


#define AMS_USB_CLIENT_EP_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, ReOpen, (), ()) \
    AMS_SF_METHOD_INFO(C, H, 1, Result, Close, (), ()) \
    AMS_SF_METHOD_INFO(C, H, 2, Result, GetCompletionEvent, (sf::OutCopyHandle out), (out)) \
    AMS_SF_METHOD_INFO(C, H, 3, Result, PopulateRing, (), ()) \
    AMS_SF_METHOD_INFO(C, H, 4, Result, PostBufferAsync, (sf::Out<u32> xferId, u32 size, u64 buffer, u64 id), (xferId, size, buffer, id)) \
    AMS_SF_METHOD_INFO(C, H, 5, Result, GetXferReport, (const sf::OutAutoSelectBuffer &out, sf::Out<u32> count, u32 max), (out, count, max)) \
    AMS_SF_METHOD_INFO(C, H, 6, Result, BatchBufferAsync, (sf::Out<u32> xferId, u32 urbCount, u32 unk1, u32 unk2, u64 buffer, u64 id), (xferId, urbCount, unk1, unk2, buffer, id)) \
    AMS_SF_METHOD_INFO(C, H, 7, Result, CreateSmmuSpace, (u32 size, u64 buffer), (size, buffer)) \
    AMS_SF_METHOD_INFO(C, H, 8, Result, ShareReportRing, (sf::CopyHandle &&xfer_mem, u32 size), (std::move(xfer_mem), size))


AMS_SF_DEFINE_INTERFACE(ams::usb, IClientEpSession, AMS_USB_CLIENT_EP_INTERFACE_INFO, 0x3120393)
AMS_SF_DEFINE_INTERFACE(ams::usb, IClientIfSession, AMS_USB_CLIENT_IF_INTERFACE_INFO, 0x372F8BD)
AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::usb, IUsbMitmInterface, AMS_USB_MITM_INTERFACE_INFO, 0xE71F7BB)

namespace ams::mitm::usb
{
    class UsbMitmEpSession
    {
    private: 
        ams::os::NativeHandle mClientProcess;
        ::usb::gc::InterfaceId mIntfId;
        UsbHsXferReport mReport;
        Handle mCompletionEvent;
        bool mIsWriteEndpoint;
    public:
        UsbMitmEpSession(ams::os::NativeHandle client, ::usb::gc::InterfaceId id, Handle completion, bool is_write)
            : mClientProcess(client), mIntfId(id), mCompletionEvent(completion), mIsWriteEndpoint(is_write)
        {}

        ~UsbMitmEpSession();

        Result ReOpen();
        Result Close();
        Result GetCompletionEvent(sf::OutCopyHandle out);
        Result PopulateRing();
        Result PostBufferAsync(sf::Out<u32> xferId, u32 size, u64 buffer, u64 id);
        Result GetXferReport(const sf::OutAutoSelectBuffer &out, sf::Out<u32> count, u32 max);
        Result BatchBufferAsync(sf::Out<u32> xferId, u32 urbCount, u32 unk1, u32 unk2, u64 buffer, u64 id);
        Result CreateSmmuSpace(u32 size, u64 buffer);
        Result ShareReportRing(sf::CopyHandle &&xfer_mem, u32 size);
    };

    static_assert(::ams::usb::IsIClientEpSession<UsbMitmEpSession>);

    class UsbMitmIfSession
    {
    private:
        ams::os::NativeHandle mClientProcess;
        ::usb::gc::ProxyInterface mProxy;
        UsbHsXferReport mFakedReport;
    public:
        UsbMitmIfSession(ams::os::NativeHandle client, ::usb::gc::ProxyInterface proxy) : mClientProcess(client), mProxy(proxy) {}
        ~UsbMitmIfSession();

        Result GetStateChangeEvent(sf::OutCopyHandle out);
        Result SetInterface(const sf::OutBuffer &out, u8 id);
        Result GetInterface(const sf::OutBuffer &out);
        Result GetAlternateInterface(const sf::OutBuffer &out, u8 id);
        Result GetCurrentFrame(sf::Out<u32> current_frame);
        Result CtrlXferAsync(u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u64 buffer);
        Result GetCtrlXferCompletionEvent(sf::OutCopyHandle out);
        Result GetCtrlXferReport(const sf::OutBuffer &out);
        Result ResetDevice();
        Result OpenUsbEp(sf::Out<sf::SharedPointer<::ams::usb::IClientEpSession>> out_session, sf::Out<usb_endpoint_descriptor> out_desc, u16 maxUrbCount, u32 epType, u32 epNumber, u32 epDirection, u32 maxXferSize);
    };



    static_assert(::ams::usb::IsIClientIfSession<UsbMitmIfSession>);

    class UsbMitmService : public sf::MitmServiceImplBase
    {
    protected:
        ams::os::NativeHandle mClientProcess;

    public:
        UsbMitmService(std::shared_ptr<::Service> &&s, const sm::MitmProcessInfo &c);

    public:
        static bool ShouldMitm(const sm::MitmProcessInfo &client_info)
        {
            return client_info.program_id == ncm::SystemProgramId::Hid;
        }

    public:
        Result QueryAllInterfaces(UsbHsInterfaceFilter filter, const sf::OutMapAliasArray<UsbHsInterfaceInfo> &out, sf::Out<s32> total_out);
        Result QueryAvailableInterfaces(UsbHsInterfaceFilter filter, const sf::OutMapAliasArray<UsbHsInterfaceInfo> &out, sf::Out<s32> total_out);
        Result AcquireUsbIf(const sf::OutMapAliasBuffer &out1, const sf::OutMapAliasBuffer &out2, sf::Out<sf::SharedPointer<::ams::usb::IClientIfSession>> out_session, u32 interfaceId);
    };

    static_assert(IsIUsbMitmInterface<UsbMitmService>);
}
