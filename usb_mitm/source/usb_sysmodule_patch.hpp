#pragma once

namespace ams::mitm::usb::sysmodule_patch
{
    extern bool g_IsPatchableSysversion;
    void PatchUsbService();
}