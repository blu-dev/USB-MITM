#include "usbmitm_module.hpp"
#include "usb_mitm_service.hpp"
#include "logger.hpp"

namespace ams::mitm::usb
{

    namespace
    {
        const size_t ThreadStackSize = 0x4000;
        const s32 ThreadPriority = -11;
        alignas(os::ThreadStackAlignment) u8 g_ThreadStack[ThreadStackSize];
        os::ThreadType g_Thread;

        struct UsbMitmServerManagerOptions
        {
            static constexpr size_t PointerBufferSize = 0x1000;
            static constexpr size_t MaxDomains = 0x10;
            static constexpr size_t MaxDomainObjects = 0x100;
            static constexpr bool CanDeferInvokeRequest = false;
            static constexpr bool CanManageMitmServers = true;
        };

        class ServerManager final : public sf::hipc::ServerManager<1, UsbMitmServerManagerOptions>
        {
        private:
            virtual Result OnNeedsToAccept(int port_index, Server *server) override;
        };

        ServerManager g_ServerManager;

        Result ServerManager::OnNeedsToAccept(int port_index, Server *server)
        {
            std::shared_ptr<::Service> fsrv;
            sm::MitmProcessInfo client_info;
            ::usb::util::Log("Received \'OnNeedsToAccept\' Request: %d\n", port_index);
            server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(client_info));
            ::usb::util::Log("Acknowledged \'OnNeedsToAccept\' Request: %d\n", port_index);

            Result rc;
            switch (port_index)
            {
            case 0:
                rc = this->AcceptMitmImpl(server, sf::CreateSharedObjectEmplaced<IUsbMitmInterface, UsbMitmService>(decltype(fsrv)(fsrv), client_info), fsrv);
                break;
                AMS_UNREACHABLE_DEFAULT_CASE();
            };

            ::usb::util::Log("Processed request and accepted MITM\n");
            return rc;
        }

        void UsbHsMitmThreadFunction(void *)
        {
            R_ABORT_UNLESS((g_ServerManager.RegisterMitmServer<UsbMitmService>(0, sm::ServiceName::Encode("usb:hs"))));
            ::usb::util::Log("Registered usb:hs MITM Server\n");
            g_ServerManager.LoopProcess();
            ams::os::YieldThread();
        }
    }

    void Launch()
    {
        R_ABORT_UNLESS(os::CreateThread(
            &g_Thread,
            UsbHsMitmThreadFunction,
            nullptr,
            g_ThreadStack,
            ThreadStackSize,
            ThreadPriority));

        os::SetThreadNamePointer(&g_Thread, "usbhs::UsbMitmThread");
        ::usb::util::Log("Starting usb:hs Mitm Service\n");
        os::StartThread(&g_Thread);
    }

    void WaitFinished()
    {
        os::WaitThread(&g_Thread);
    }

}
