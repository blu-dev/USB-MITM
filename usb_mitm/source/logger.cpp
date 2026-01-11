#include "logger.hpp"

namespace usb::util
{

    static constinit char s_SdCardMount[] = "sd";
    static constinit char s_LogFilePath[] = "sd:/log.txt";

    static long s_LogFilePosition = 0;
    static ams::fs::FileHandle s_LogFileHandle;

    static constexpr bool s_Enabled = true;

#define CHECK_ENABLED()       \
    if constexpr (!s_Enabled) \
    {                         \
        return;               \
    }

    namespace impl
    {
        ams::os::Mutex s_LogMutex(false);
        char s_FormatBuffer[500];

        void Log(const char *text, size_t length)
        {
            CHECK_ENABLED();
            R_ABORT_UNLESS(ams::fs::OpenFile(std::addressof(s_LogFileHandle), s_LogFilePath, ams::fs::OpenMode_All));

            /* Write text to file. */
            R_ABORT_UNLESS(ams::fs::WriteFile(s_LogFileHandle, s_LogFilePosition, text, length, ams::fs::WriteOption::Flush));

            R_ABORT_UNLESS(ams::fs::FlushFile(s_LogFileHandle));

            ams::fs::CloseFile(s_LogFileHandle);
            /* Move forward position. */
            s_LogFilePosition += length;
        }
    }

    void Initialize()
    {
        CHECK_ENABLED();
        /* Mount SD card. */
        R_ABORT_UNLESS(ams::fs::MountSdCard(s_SdCardMount));

        /* Try create log file, ignore if it already exists. */
        R_TRY_CATCH(ams::fs::CreateFile(s_LogFilePath, 0))
        {
            R_CATCH(ams::fs::ResultPathAlreadyExists) {}
        }
        R_END_TRY_CATCH_WITH_ABORT_UNLESS;

        /* Open log file. */
        R_ABORT_UNLESS(ams::fs::OpenFile(std::addressof(s_LogFileHandle), s_LogFilePath, ams::fs::OpenMode_All));

        /* Get size of log file, so we can set our position to the end of it. */
        R_ABORT_UNLESS(ams::fs::GetFileSize(std::addressof(s_LogFilePosition), s_LogFileHandle));

        ams::fs::CloseFile(s_LogFileHandle);
    }

    void Finalize()
    {
        CHECK_ENABLED();

        /* Unmount SD card. */
        ams::fs::Unmount(s_SdCardMount);
    }
}