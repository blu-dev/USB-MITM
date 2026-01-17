#include "logger.hpp"
#include <switch.h>
#include <stratosphere.hpp>
#include "sniffer.hpp"
#include "driver_thread.hpp"
#include "usbmitm_module.hpp"
#include "usb_gc_service.hpp"
#include "usb_sysmodule_patch.hpp"

namespace ams::init
{
    namespace
    {
        constexpr size_t g_MallocBufferSize = 4_MB;
        alignas(os::MemoryPageSize) constinit u8 g_MallocBuffer[g_MallocBufferSize];
    }

    void InitializeSystemModule()
    {

        /* Initialize the global malloc allocator. */
        init::InitializeAllocator(g_MallocBuffer, sizeof(g_MallocBuffer));
        /* Initialize stratosphere. */
        hos::InitializeForStratosphere();
        ::usb::util::Initialize();
    }

    void FinalizeSystemModule()
    {
        ::usb::util::Finalize();
    }

    void Startup() {}
}

namespace ams
{
    void Main()
    {
        R_ABORT_UNLESS(smInitialize());
        mitm::usb::sysmodule_patch::PatchUsbService();

        usb::sniffer::Initialize();       
        mitm::usb::Launch();
        mitm::usb::Initialize();
        usb::gc::Launch();

        ams::usb::gc::g_GameCubeDriver1.Initialize(ams::usb::gc::GameCubeDriverId::One);
        ams::usb::gc::g_GameCubeDriver2.Initialize(ams::usb::gc::GameCubeDriverId::Two);

        ams::usb::gc::g_GameCubeDriver1.Finalize();
        ams::usb::gc::g_GameCubeDriver2.Finalize();

        usb::gc::WaitFinish();
        mitm::usb::WaitFinished();


    }
}
