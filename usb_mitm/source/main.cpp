#include "logger.hpp"
#include <switch.h>
#include <stratosphere.hpp>
#include "driver_thread.hpp"
#include "usbmitm_module.hpp"
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
        ::usb::util::Log("Hello World\n");
        mitm::usb::sysmodule_patch::PatchUsbService();

        mitm::usb::Initialize();
        mitm::usb::Launch();
        ::usb::gc::Initialize();
        ::usb::gc::WaitProcess();
        mitm::usb::WaitFinished();
    }
}
