#pragma once
#include <stratosphere.hpp>

/* Namespace for all things usb::gc */
namespace ams::usb::gc
{
    enum class GameCubeDriverId
    {
        One,
        Two
    };

    enum class GameCubeDriverState
    {
        Unacquired,
        Initializing,
        SteadyState,
        Finalizing
    };

    class GameCubeDriver
    {
    private:
        struct AsyncCtrlXferCached
        {
            AsyncCtrlXferCached* pNext;
            u8 bmRequestType;
            u8 bRequest;
            u16 wValue;
            u16 wIndex;
            u16 wLength;
            u32 expectedOutput;
            u8 output[0];
        };
        
        alignas(os::MemoryPageSize) u8 mAsyncCtrlXferPage[os::MemoryPageSize];
        alignas(os::MemoryPageSize) u8 mReadXferPage[os::MemoryPageSize];
        alignas(os::MemoryPageSize) u8 mWriteXferPage[os::MemoryPageSize];
        alignas(os::MemoryPageSize) u8 mThreadStack[os::MemoryPageSize];
        AsyncCtrlXferCached* pCachedXfers;
        Service* mpInterface;
        Service mReadEndpoint;
        Service mWriteEndpoint;
        Handle mClientProcess;
        uintptr_t mInterfacePage;
        uintptr_t mReadEpPage;
        uintptr_t mWriteEpPage;
        uintptr_t mInterfaceMappee;
        uintptr_t mReadEpMappee;
        uintptr_t mWriteEpMappee;
        os::ThreadType mThread;
        os::EventType mAcquireEvent;
        Event mInterfaceStateChangeFwd;
        Event mInterfaceXferCompleted;
        Event mInterfaceXferCompletedFwd;
        Event mReadCompletedEvent;
        Event mReadCompletedEventFwd;
        Event mWriteCompletedEvent;
        Event mWriteCompletedEventFwd;
        GameCubeDriverId mId;
        GameCubeDriverState mState;

        AsyncCtrlXferCached* DoCtrlXferAsync(
            u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u32 expectedSize
        );

        usb_endpoint_descriptor OpenEndpoint(
            Service* pOutService, u8 endpointId, bool isInput, u32 maxXferSize
        );

        void SetupDevice();
        void SteadyState();
        void WaitSteady();

        void DriverThreadImpl();
        static void DriverThread(void*);
    public:
        void Initialize(GameCubeDriverId id);

        void Acquire(os::NativeHandle clientProcess, Service* pInterface);
        os::NativeHandle InterfaceChangeEvent();
        os::NativeHandle InterfaceEvent();
        os::NativeHandle ReadEvent();
        os::NativeHandle WriteEvent();
        void ControlTransfer(u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u64 buffer, UsbHsXferReport* pReport);
        void MapInterfacePage(uintptr_t page);
        void MapReadPage(uintptr_t page);
        void MapWritePage(uintptr_t page);
        void ReadPacket(u64 clientBuffer, u32 clientSize, UsbHsXferReport* pReport);
        void WritePacket(u64 clietnBuffer, u32 clientSize, UsbHsXferReport* pReport);
        void GetPacketForBypass(uint8_t* pOutBuffer);
        Result CloseRead();
        Result CloseWrite();
        void Release();

        bool IsInUse();

        void Finalize();
    };

    extern GameCubeDriver g_GameCubeDriver1;
    extern GameCubeDriver g_GameCubeDriver2;
}