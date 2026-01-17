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
    bool g_IsPatchableSysversion = true;

    namespace
    {
        /* Metadata information about the patch that we are going to perform */
        struct PatchInformation {
            /* Min/Max Supported Versions of the 1000hz patch */
            hos::Version mMinSupportedVersion;
            hos::Version mMaxSupportedVersion;
            /* The offset into the USB sysmodule that we want to patch */
            /* This is used to construct a `bl` instruction to the start of the module */
            int32_t mInstructionPatchOffset;

            /* The beginning/end of our patch code, this should be included in assembly */
            uintptr_t mPatchBegin;
            uintptr_t mPatchEnd;
        };

        /* All supported versions of the 1000hz patch */
        static PatchInformation g_Patches[] = {
            { 
                .mMinSupportedVersion = hos::Version_21_1_0,
                .mMaxSupportedVersion = hos::Version_21_2_0,
                .mInstructionPatchOffset = 0x3EA04,
                .mPatchBegin = reinterpret_cast<uintptr_t>(UsbServicePatchBegin),
                .mPatchEnd = reinterpret_cast<uintptr_t>(UsbServicePatchEnd)
            }
        };

        static constexpr ams::ncm::ProgramId g_UsbProgramId = ams::ncm::SystemProgramId::Usb;
    }

    void PatchUsbService() {
        /* Get the patch for this sysversion */
        const PatchInformation* pPatch = nullptr;

        for (size_t i = 0; i < (sizeof(g_Patches) / sizeof(PatchInformation)); i++)
        {
            const PatchInformation* pInfo = &g_Patches[i];
            if (pInfo->mMinSupportedVersion <= ATMOSPHERE_TARGET_FIRMWARE_CURRENT && ATMOSPHERE_TARGET_FIRMWARE_CURRENT <= pInfo->mMaxSupportedVersion)
            {
                pPatch = pInfo;
                break;
            }
        }

        /* If there is no patch for this sysversion, then set our flag and leave */
        if (!pPatch)
        {
            g_IsPatchableSysversion = false;
            return;
        }

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

        AMS_ABORT_UNLESS((pPatch->mInstructionPatchOffset > 0) && (pPatch->mInstructionPatchOffset % 4 == 0));

        int32_t imm26 = pPatch->mInstructionPatchOffset / 4;
        /* imm26 is a signed number, so here we are checking that it is both unsigned and that it does not overlap */
        /* with the opcode region */
        AMS_ABORT_UNLESS((imm26 & 0xFE000000) != 0, "Instruction patch offset is too large");

        uint32_t instruction = ((uint32_t)-imm26) | 0x94000000;

        /* Step 6: Patch the process memory */
        /* NOTE: This is an extremely unsafe implementation that patches the region of .text used for the entrypoint */
        /* If, for some reason, the USB service were to crash at any point, this has very undefined behavior */
        /* I Would much rather us have a better code-cave carved out, but for now this one will do. */
        R_ABORT_UNLESS(ams::svc::WriteDebugProcessMemory(
            hUsbDebugProcess,
            pPatch->mPatchBegin,
            UsbMemInfo.base_address,
            pPatch->mPatchEnd - pPatch->mPatchBegin
        ));

        R_ABORT_UNLESS(ams::svc::WriteDebugProcessMemory(
            hUsbDebugProcess,
            reinterpret_cast<uintptr_t>(&instruction),
            UsbMemInfo.base_address + pPatch->mInstructionPatchOffset,
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