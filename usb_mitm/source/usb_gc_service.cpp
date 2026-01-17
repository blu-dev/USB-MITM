#include "usb_gc_service.hpp"
#include "gc_interface.hpp"

namespace ams::usb::gc
{
    namespace
    {
        const size_t ThreadStackSize = 0x4000;
        const s32 ThreadPriority = -11;
        alignas(ams::os::ThreadStackAlignment) u8 g_ThreadStack[ThreadStackSize];
        ams::os::ThreadType g_Thread;

        struct UsbGcServerManagerOptions
        {
            static constexpr size_t PointerBufferSize = 0x1000;
            static constexpr size_t MaxDomains = 0x10;
            static constexpr size_t MaxDomainObjects = 0x100;
            static constexpr bool CanDeferInvokeRequest = false;
            static constexpr bool CanManageMitmServers = true;
        };

        class ServerManager final : public ams::sf::hipc::ServerManager<1, UsbGcServerManagerOptions>
        {
        private:
            virtual ams::Result OnNeedsToAccept(int port_index, Server *server) override;
        };

        ServerManager g_ServerManager;


        ams::Result ServerManager::OnNeedsToAccept(int port_index, Server *server)
        {
            ams::Result rc;
            switch (port_index)
            {
            case 0:
                rc = this->AcceptImpl(server, ams::sf::CreateSharedObjectEmplaced<IUsbGcInterface, UsbGcInterfaceImpl>());
                break;
                AMS_UNREACHABLE_DEFAULT_CASE();
            };

            return rc;
        }

        void UsbGcInterfaceThreadFunction(void*)
        {
            R_ABORT_UNLESS(g_ServerManager.RegisterServer(0, ams::sm::ServiceName::Encode("usb:gc"), 1));
            g_ServerManager.LoopProcess();
            ams::os::YieldThread();
        }
    }

    ams::Result UsbGcInterfaceImpl::GetAdapterPacketState(sf::Out<GameCubePacket>& packet1, sf::Out<GameCubePacket>& packet2, sf::Out<u8>& adapterMask)
    {
        u32 mask = 0;
        if (g_GameCubeDriver1.IsInUse())
        {
            g_GameCubeDriver1.GetPacketForBypass(packet1->packet);
            mask |= 1;
        }
        if (g_GameCubeDriver2.IsInUse())
        {
            g_GameCubeDriver2.GetPacketForBypass(packet2->packet);
            mask |= 2;
        }
        adapterMask.SetValue(mask);
        R_SUCCEED();
    }

    void Launch()
    {
        R_ABORT_UNLESS(ams::os::CreateThread(
            &g_Thread,
            UsbGcInterfaceThreadFunction,
            nullptr,
            g_ThreadStack,
            ThreadStackSize,
            ThreadPriority));

        ams::os::SetThreadNamePointer(&g_Thread, "usb::gc::UsbGcInterface");
        // ::usb::util::Log("Starting usb:hs Mitm Service\n");
        ams::os::StartThread(&g_Thread);
    }

    void WaitFinish()
    {
        ams::os::WaitThread(&g_Thread);
    }
}