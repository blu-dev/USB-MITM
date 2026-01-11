#pragma once
#include <stratosphere.hpp>

namespace usb::gc
{
    using InterfaceId = u32;
    using EndpointId = u32;

    struct IntfAsyncXfer
    {
        UsbHsXferReport* mpReport;
        u64 mClientBuffer;
        u8 bmRequestType;
        u8 bRequest;
        u16 wValue;
        u16 wIndex;
        u16 wLength;
    };

    struct ProxyInterface {
        InterfaceId mId;
        Handle mIfStateChangeEvent;
        Handle mCtrlXferCompletionEvent;
        Handle mReadPostBufferCompletionEvent;
        Handle mWritePostBufferCompletionEvent;
    };

    struct IntfAsyncTransferArgs {
        InterfaceId mId;
        u8* mpBuffer;
        UsbHsXferReport* mpReport;
        size_t mSize;
    };

    /* Initialize and Finalization API */
    /* Initializes the thread for polling gamecube adapters */
    void Initialize();
    /* Waits on the threads for polling gamecube adapters */
    void WaitProcess();

    /* Opens an interface to the GameCube adapter. */
    /* Opening this interface will open both of the endpoints at the same time and begin driving the gamecube adapter. */
    /* The endpoints will not be closed until CloseInterface is invoked */
    ProxyInterface OpenInterface(Handle ClientProcess, Service IfSession, const UsbHsInterface* pInterface);

    /* Closes the interface to the GameCube adapter. */
    void CloseInterface(InterfaceId id);
    
    /* Initializes an async transfer on the interface itself */
    /* This isn't necessary for the GameCube adapter, so it's possible this method is stubbed */
    void IntfAsyncTransfer(InterfaceId id, IntfAsyncXfer xfer);

    /* Writes a packet to the GameCube adapter. This will enqueue a packet to be written. */
    /* Note that this method is non-blocking, and that the "queue" of packets to the GC adapter */
    /* Is only one long. If this method is called again before the driver thread has had an opportunity */
    /* to write the previous packet, then the previous packet gets discarded. */
    void WritePacket(InterfaceId id, u64 buffer, size_t size, UsbHsXferReport* pReport);

    /* Gets the last packet that was read from the GameCube controller, and writes it to the specified pointer */
    void ReadPacket(InterfaceId id, u64 buffer, size_t size, UsbHsXferReport* pReport);

    void ReadWithTransfer(Handle ForeignProcess, uintptr_t ForeignMemory, void* LocalMemory, size_t Size);
    void WriteWithTransfer(Handle ForeignProcess, void* LocalMemory, uintptr_t ForeignMemory, size_t Size);
}