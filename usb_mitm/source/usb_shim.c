/** This file contains redefinitions of the usbHs* service methods provided by libnx
 * 
 * This method redefines them so that we can have precise control over the services used. It also implements a few not provided
 * by libnx, since they are otherwise unnecessary for custom processes.
 */

#include "usb_shim.h"

Result _usbHsCmdNoIO(Service* srv, u64 cmd_id) {
    serviceAssumeDomain(srv);
    return serviceDispatch(srv, cmd_id);
}

Result _usbHsCmdInU8NoOut(Service* srv, u8 inval, u32 cmd_id) {
    serviceAssumeDomain(srv);
    return serviceDispatchIn(srv, cmd_id, inval);
}

Result _usbDsCmdNoInOutU32(Service* srv, u32 *out, u32 cmd_id) {
    serviceAssumeDomain(srv);
    return serviceDispatchOut(srv, cmd_id, *out);
}

Result _usbHsCmdRecvBufNoOut(Service* srv, void* buffer, size_t size, u32 cmd_id) {
    serviceAssumeDomain(srv);
    return serviceDispatch(srv, cmd_id,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { buffer, size } },
    );
}

Result _usbHsGetHandle(Service* srv, Handle* handle_out, u32 cmd_id) {
    serviceAssumeDomain(srv);
    return serviceDispatch(srv, cmd_id,
        .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
        .out_handles = handle_out,
    );
}

Result usbHsEpReOpenFwd(Service* srv)
{
    return _usbHsCmdNoIO(srv, 0);
}

Result usbHsEpCloseFwd(Service* srv)
{
    return _usbHsCmdNoIO(srv, 1);
}

Result usbHsEpGetCompletionEventFwd(Service* srv, Handle* handle_out) 
{
    return _usbHsGetHandle(srv, handle_out, 2);
}

Result usbHsEpPopulateRingFwd(Service* srv)
{
    return _usbHsCmdNoIO(srv, 3);
}

Result usbHsEpPostBufferAsyncFwd(Service* srv, void* buffer, u32 size, u64 id, u32* xferId)
{
    serviceAssumeDomain(srv);

    const struct {
        u32 size;
        u32 pad;
        u64 buffer;
        u64 id;
    } input = { size, 0, (u64)buffer, id };

    return serviceDispatchInOut(srv, 4, input, *xferId);
}

Result usbHsEpGetXferReportFwd(Service* srv, UsbHsXferReport* reports, u32 max_reports, u32* count)
{
    serviceAssumeDomain(srv);
    return serviceDispatchInOut(srv, 5, max_reports, *count,
        .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out },
        .buffers = { { reports, max_reports * sizeof(UsbHsXferReport) }}
    );
}

Result usbHsEpBatchBufferAsyncFwd(Service* srv, void* buffer, u32 urbCount, u32 unk1, u32 unk2, u64 id, u32* xferId)
{
    const struct {
        u32 urbCount;
        u32 unk1;
        u32 unk2;
        u32 pad;
        u64 buffer;
        u64 id;
    } input = { urbCount, unk1, unk2, 0, (u64)buffer, id };
    serviceAssumeDomain(srv);
    return serviceDispatchInOut(srv, 6, input, *xferId);
}

Result usbHsEpCreateSmmuSpaceFwd(Service* srv, void* buffer, u32 size)
{
    const struct {
        u32 size;
        u32 pad;
        u64 buffer;
    } input = { size, 0, (u64)buffer };

    serviceAssumeDomain(srv);
    return serviceDispatchIn(srv, 7, input);
}

Result usbHsEpShareReportRingFwd(Service* srv, Handle handle, u32 size)
{
    serviceAssumeDomain(srv);
    return serviceDispatchIn(srv, 8, size,
        .in_num_handles = 1,
        .in_handles = { handle }
    );
}

Result usbHsIfGetStateChangeEventFwd(Service* srv, Handle* handle)
{
    return _usbHsGetHandle(srv, handle, 0);
}

Result usbHsIfSetInterfaceFwd(Service* srv, void* info, size_t buffer_size, u8 id)
{
    serviceAssumeDomain(srv);
    return serviceDispatchIn(srv, 1, id,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { info, buffer_size }}
    );
}

Result usbHsIfGetInterfaceFwd(Service* srv, void* info, size_t buffer_size)
{
    serviceAssumeDomain(srv);
    return serviceDispatch(srv, 2, 
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { info, buffer_size }}
    );
}

Result usbHsIfGetAlternateInterfaceFwd(Service* srv, void* info, size_t buffer_size, u8 id)
{
    serviceAssumeDomain(srv);
    return serviceDispatchIn(srv, 3, id,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { info, buffer_size }}
    );
}

Result usbHsIfGetCurrentFrameFwd(Service* srv, u32* out)
{
    serviceAssumeDomain(srv);
    return serviceDispatchOut(srv, 4, *out);
}

Result usbHsIfCtrlXferAsyncFwd(Service* srv, u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u64 buffer)
{
    const struct {
        u8 bmRequestType;
        u8 bRequest;
        u16 wValue;
        u16 wIndex;
        u16 wLength;
        u64 buffer;
    } in = { bmRequestType, bRequest, wValue, wIndex, wLength, (u64)buffer };

    serviceAssumeDomain(srv);
    return serviceDispatchIn(srv, 5, in);
}

Result usbHsIfGetCtrlXferCompletionEventFwd(Service* srv, Handle* handle)
{
    return _usbHsGetHandle(srv, handle, 6);
}

Result usbHsIfGetCtrlXferReportFwd(Service* srv, void* buffer, size_t buffer_size)
{
    serviceAssumeDomain(srv);
    return serviceDispatch(srv, 7,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { buffer, buffer_size } }
    );
}

Result usbHsIfResetDeviceFwd(Service* srv)
{
    return _usbHsCmdNoIO(srv, 8);
}

Result usbHsIfOpenUsbEpFwd(
    Service *s, Service* outService,
    u16 maxUrbCount, u32 epType, u32 epNumber, u32 epDirection, u32 maxXferSize,
    struct usb_endpoint_descriptor* out
) {
    const struct {
        u16 maxUrbCount;
        u16 pad;
        u32 epType;
        u32 epNumber;
        u32 epDirection;
        u32 maxXferSize;
    } in = { maxUrbCount, 0, epType, epNumber, epDirection, maxXferSize };

    return serviceDispatchInOut(s, 9, in, *out,
            .out_num_objects = 1,
            .out_objects = outService
    );
}

Result usbHsQueryAllInterfacesFwd(Service *s, const UsbHsInterfaceFilter *filter, UsbHsInterface *out, size_t count, s32 *total_out)
{
    s32 tmp = 0;
    Result rc = serviceDispatchInOut(s, 1, *filter, tmp,
                                     .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                     .buffers = {{out, count * sizeof(UsbHsInterface)}});
    if (R_SUCCEEDED(rc) && total_out != NULL)
    {
        *total_out = tmp;
    }
    return rc;
}

Result usbHsQueryAvailableInterfacesFwd(Service *s, const UsbHsInterfaceFilter *filter, UsbHsInterface *out, size_t count, s32 *total_out)
{
    s32 tmp = 0;
    Result rc = serviceDispatchInOut(s, 2, *filter, tmp,
                                     .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                     .buffers = {{out, count * sizeof(UsbHsInterface)}});
    if (R_SUCCEEDED(rc) && total_out != NULL)
    {
        *total_out = tmp;
    }
    return rc;
}

Result usbHsQueryAcquiredInterfacesFwd(Service *s, UsbHsInterface *out, size_t count, s32 *total_out)
{
    s32 tmp = 0;
    Result rc = serviceDispatchOut(s, 3, tmp,
                                     .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                     .buffers = {{out, count * sizeof(UsbHsInterface)}});
    if (R_SUCCEEDED(rc) && total_out != NULL)
    {
        *total_out = tmp;
    }
    return rc;
}

Result usbHsCreateInterfaceAvailableEventFwd(Service *s, const UsbHsInterfaceFilter *filter, u8 id, Handle *h)
{
    const struct {
        u8 id;
        u8 pad;
        UsbHsInterfaceFilter filter;
    } in = { id, 0, *filter };
    return serviceDispatchIn(s, 4, in, .out_handle_attrs = {SfOutHandleAttr_HipcCopy }, .out_handles = h );
}
Result usbHsDestroyInterfaceAvailableEventFwd(Service *s, u8 id)
{
    return serviceDispatchIn(s, 5, id);
}
Result usbHsGetInterfaceStateChangeEventFwd(Service *s, Handle *h)
{
    return serviceDispatch(s, 6, .out_handle_attrs = { SfOutHandleAttr_HipcCopy}, .out_handles = h);
}

Result usbHsAcquireUsbIfFwd(Service *s, Service* outService, void* out1, size_t count1, void* out2, size_t count2, u32 interfaceId) {
    return serviceDispatchIn(s, 7, interfaceId,
            .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out, SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
            .buffers = {{out1, count1}, {out2, count2}},
            .out_num_objects = 1,
            .out_objects = outService
    );
}