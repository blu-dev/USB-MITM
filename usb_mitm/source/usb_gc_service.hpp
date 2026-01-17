#pragma once
#include <stratosphere.hpp>


namespace ams::usb::gc
{
    struct GameCubePacket {
        u8 packet[0x25];
    };
}

#define USB_GC_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, ams::Result, GetAdapterPacketState, (sf::Out<ams::usb::gc::GameCubePacket>& packet1, sf::Out<ams::usb::gc::GameCubePacket>& packet2, sf::Out<u8>& adapterMask), (packet1, packet2, adapterMask))

AMS_SF_DEFINE_INTERFACE(ams::usb::gc, IUsbGcInterface, USB_GC_INTERFACE_INFO, 0xC79F8BC)

namespace ams::usb::gc
{
    class UsbGcInterfaceImpl 
    {
    public:
        ams::Result GetAdapterPacketState(sf::Out<GameCubePacket>& packet1, sf::Out<GameCubePacket>& packet2, sf::Out<u8>& adapterMask);
    };

    static_assert(IsIUsbGcInterface<UsbGcInterfaceImpl>);

    void Launch();
    void WaitFinish();
}