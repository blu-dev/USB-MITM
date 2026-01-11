#pragma once
#include <stratosphere.hpp>

#define USB_GC_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, ams::Result, GetAdapterPacketState, (const ::ams::sf::OutBuffer &out, ::ams::sf::Out<u32> num_adapters), (out, num_adapters))

AMS_SF_DEFINE_INTERFACE(ams::usb::gc, IUsbGcInterface, USB_GC_INTERFACE_INFO, 0xC79F8BC)

namespace ams::usb::gc
{
    class UsbGcInterfaceImpl 
    {
    public:
        ams::Result GetAdapterPacketState(const ams::sf::OutBuffer& out, ams::sf::Out<u32> num_adapters);
    };

    static_assert(IsIUsbGcInterface<UsbGcInterfaceImpl>);

    void Launch();
    void WaitFinish();
}