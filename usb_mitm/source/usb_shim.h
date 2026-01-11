#pragma once
#include <switch.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* USB Endpoint API */
    Result usbHsEpReOpenFwd(Service* srv);
    Result usbHsEpCloseFwd(Service* srv);
    Result usbHsEpGetCompletionEventFwd(Service* srv, Handle* handle_out);
    Result usbHsEpPopulateRingFwd(Service* srv);
    Result usbHsEpPostBufferAsyncFwd(Service* srv, void* buffer, u32 size, u64 id, u32* xferId);
    Result usbHsEpGetXferReportFwd(Service* srv, UsbHsXferReport* reports, u32 max_reports, u32* count);
    Result usbHsEpBatchBufferAsyncFwd(Service* srv, void* buffer, u32 urbCount, u32 unk1, u32 unk2, u64 id, u32* xferId);
    Result usbHsEpCreateSmmuSpaceFwd(Service* srv, void* buffer, u32 size);
    Result usbHsEpShareReportRingFwd(Service* srv, Handle handle, u32 size);

    /* USB Interface API */
    Result usbHsIfGetStateChangeEventFwd(Service* srv, Handle* handle);
    Result usbHsIfSetInterfaceFwd(Service* srv, void* info, size_t buffer_size, u8 id);
    Result usbHsIfGetInterfaceFwd(Service* srv, void* info, size_t buffer_size);
    Result usbHsIfGetAlternateInterfaceFwd(Service* srv, void* info, size_t buffer_size, u8 id);
    Result usbHsIfGetCurrentFrameFwd(Service* srv, u32* out);
    Result usbHsIfCtrlXferAsyncFwd(Service* srv, u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u64 buffer);
    Result usbHsIfGetCtrlXferCompletionEventFwd(Service* srv, Handle* handle);
    Result usbHsIfGetCtrlXferReportFwd(Service* srv, void* buffer, size_t buffer_size);
    Result usbHsIfResetDeviceFwd(Service* srv);
    Result usbHsIfOpenUsbEpFwd(
        Service *s, Service* outService,
        u16 maxUrbCount, u32 epType, u32 epNumber, u32 epDirection, u32 maxXferSize,
        struct usb_endpoint_descriptor* out
    );

    /* USB Service API */
    Result usbHsQueryAllInterfacesFwd(Service *s, const UsbHsInterfaceFilter *filter, UsbHsInterface *out, size_t count, s32 *total_out);
    Result usbHsQueryAvailableInterfacesFwd(Service *s, const UsbHsInterfaceFilter *filter, UsbHsInterface *out, size_t count, s32 *total_out);
    Result usbHsAcquireUsbIfFwd(Service *s, Service* outService, void* out1, size_t count1, void* out2, size_t count2, u32 interfaceId);

#ifdef __cplusplus
}
#endif