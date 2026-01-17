#include <stratosphere.hpp>
#include "logger.hpp"
#include "usb_sysmodule_patch.hpp"

extern "C" {
    /* The UsbServicePatch subroutine is defined in the gamecube_patch.s file */
    void UsbServicePatchBegin();
    void UsbServicePatchEnd();
}

namespace ams::mitm::usb::sysmodule_patch
{
    namespace
    {
        static constexpr ams::ncm::ProgramId g_UsbProgramId = ams::ncm::SystemProgramId::Usb;

        /* NOTE: The below values are unused in C++ code, they are hardcoded into the assembly patch */
        /* These might be useful in future versions where we generate the assembly patch(es) at runtime */
        static constexpr uintptr_t PatchUsbEndpointDescriptor_offset = 0x3EA04;

        static constexpr uint16_t VendorId_GameCubeAdapter = 0x057E;
        static constexpr uint16_t ProductId_GameCubeAdapter = 0x0337;
        static constexpr uint8_t  PatchedInterval = 1;

        struct VendorAndDeviceId {
            uint16_t VendorId;
            uint16_t ProductId;
        }; // PACKED: This is unaligned in the USB service
    }

    void PatchUsbService() {
        /* Step 1: Initialize the sm: service (now done by main function) */
        // R_ABORT_UNLESS(smInitialize());
        /* Step 2: Initialize the pm:dmnt service */
        R_ABORT_UNLESS(pmdmntInitialize());
        /* Step 3: Acquire the ProcessId for the USB service */
        ams::os::ProcessId ProcessId;
        R_ABORT_UNLESS(ams::pm::dmnt::GetProcessId(&ProcessId, g_UsbProgramId));

        /* Close out our services now that we don't need them anymore */
        pmdmntExit();

        ::usb::util::Log("Acquired process ID for USB process: %X\n", ProcessId);

        ams::os::NativeHandle hUsbDebugProcess;
        R_ABORT_UNLESS(ams::svc::DebugActiveProcess(&hUsbDebugProcess, (u64)ProcessId));

        ::usb::util::Log("Began debugging USB process\n");

        /* Step 4: Flush the debug event queue and then continue */
        /* This is based on Atmosphere's GDB stub implementation */
        s32 NumThreads = 0;
        bool IsAttached = false;

        ams::svc::DebugEventInfo DbgEventInfo;
        while (NumThreads == 0 || !IsAttached) 
        {
            s32 DummyIndex;
            R_ABORT_UNLESS(ams::svc::WaitSynchronization(&DummyIndex, &hUsbDebugProcess, 1, UINT64_MAX));

            R_ABORT_UNLESS(ams::svc::GetDebugEvent(&DbgEventInfo, hUsbDebugProcess));

            switch (DbgEventInfo.type) {
                case ams::svc::DebugEvent_CreateThread: NumThreads++; break;
                case ams::svc::DebugEvent_ExitThread: NumThreads--; break;
                case ams::svc::DebugEvent_Exception: IsAttached = true; break;
                default: break;
            }
        }

        /* Step 5: Find the code memory region that belongs to the USB process */
        ams::svc::MemoryInfo UsbMemInfo;
        ams::svc::PageInfo UsbPageInfo;

        R_ABORT_UNLESS(ams::svc::QueryDebugProcessMemory(&UsbMemInfo, &UsbPageInfo, hUsbDebugProcess, 0));

        /* TODO: We should probably perform some safety checks on the code region, but the first code region of the USB process
            appears to always be the main text region */
        while (
            (UsbMemInfo.state != ams::svc::MemoryState_Code)
                && (UsbMemInfo.permission != ams::svc::MemoryPermission_ReadExecute)
        )
        {
            R_ABORT_UNLESS(ams::svc::QueryDebugProcessMemory(&UsbMemInfo, &UsbPageInfo, hUsbDebugProcess, UsbMemInfo.base_address + UsbMemInfo.size));
            AMS_ASSERT(UsbMemInfo.base_address != 0, "Looped memory space searching for code");
        }

        /* Step 6: Patch the process memory */
        /* NOTE: This is an extremely unsafe implementation that patches the region of .text used for the entrypoint */
        /* If, for some reason, the USB service were to crash at any point, this has very undefined behavior */
        /* I Would much rather us have a better code-cave carved out, but for now this one will do. */
        R_ABORT_UNLESS(ams::svc::WriteDebugProcessMemory(
            hUsbDebugProcess,
            reinterpret_cast<uintptr_t>(UsbServicePatchBegin),
            UsbMemInfo.base_address,
            reinterpret_cast<size_t>(reinterpret_cast<uintptr_t>(UsbServicePatchEnd) - reinterpret_cast<uintptr_t>(UsbServicePatchBegin)))
        );

        const uint32_t JumpToPatchInstr = 0x97FF057F; // bl -0x3EA04

        R_ABORT_UNLESS(ams::svc::WriteDebugProcessMemory(
            hUsbDebugProcess,
            reinterpret_cast<uintptr_t>(&JumpToPatchInstr),
            UsbMemInfo.base_address + PatchUsbEndpointDescriptor_offset,
            sizeof(uint32_t)
        ));

        /* Step 7: Continue the debugged process */
        u64 ThreadIds[] = { 0 };

        R_ABORT_UNLESS(ams::svc::ContinueDebugEvent(hUsbDebugProcess, ams::svc::ContinueFlag_ContinueAll | ams::svc::ContinueFlag_EnableExceptionEvent | ams::svc::ContinueFlag_ExceptionHandled, ThreadIds, 1));
        ::usb::util::Log("Continuing USB process\n");

        /* Step 8: Close our debug process handle */
        /* This should allow us to crack open USB with GDB at runtime if we need to */
        svcCloseHandle(hUsbDebugProcess);
    }
}