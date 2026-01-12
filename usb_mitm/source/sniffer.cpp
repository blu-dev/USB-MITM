#include "sniffer.hpp"
#include "usb_shim.h"

namespace ams::usb::sniffer
{
    namespace
    {
        constexpr size_t g_HeapMemorySize = 64_KB;
        alignas(os::MemoryPageSize) u8 g_HeapMemory[g_HeapMemorySize];

        constinit lmem::HeapHandle g_HeapHandle;
        constinit sf::ExpHeapAllocator g_SfAllocator = {};
    }

    struct Sniffer
    {
        enum class SnifferPacket : u64
        {
            RegisterInterface = 0,
            RegisterReadEndpoint = 1,
            RegisterWriteEndpoint = 2,
            FunctionCall = 3
        };

        template<typename T>
        struct FunctionPacket
        {
            u32 ObjectId;
            u32 FunctionId;
            T Value;
        };

        template<typename T>
        struct Packet
        {
            SnifferPacket PacketId;
            ams::os::Tick Tick;
            T Value;
        };

        uint8_t* mpBuffer;
        s64 mFilePos;
        fs::FileHandle mFileHandle;
        u32 mBufferSize;
        u32 mCursor;
        os::MutexType mLock;

        Sniffer(uint32_t id)
        {
            os::InitializeMutex(&mLock, false, 1);
            char PathBuffer[0x100];
            snprintf(PathBuffer, 0x100, "sd:/sniffer_%04u.bin", id);

            /* We don't care about the result here in case it doesn't exist */
            fs::DeleteFile(PathBuffer);
            R_ABORT_UNLESS(fs::CreateFile(PathBuffer, 0));
            R_ABORT_UNLESS(fs::OpenFile(&mFileHandle, PathBuffer, fs::OpenMode_Write | fs::OpenMode_AllowAppend));

            mpBuffer = reinterpret_cast<u8*>(malloc(os::MemoryPageSize));
            mFilePos = 0;
            mBufferSize = os::MemoryPageSize;
            mCursor = 0;
        }

        ~Sniffer()
        {
            if (mCursor > 0)
            {
                R_ABORT_UNLESS(fs::WriteFile(
                    mFileHandle,
                    mFilePos,
                    mpBuffer,
                    mCursor,
                    fs::WriteOption::Flush
                ));
            }

            free(mpBuffer);

            fs::CloseFile(mFileHandle);
            os::FinalizeMutex(&mLock);
        }

        template<typename T>
        void WritePacket(Packet<T> packet)
        {
            size_t PacketCursor = 0;
            size_t RemainingSize = sizeof(Packet<T>);
            os::LockMutex(&mLock);
            while (RemainingSize > (mBufferSize - mCursor))
            {
                size_t WriteSize = mBufferSize - mCursor;
                memcpy(
                    mpBuffer + mCursor,
                    reinterpret_cast<const uint8_t*>(&packet) + PacketCursor,
                    WriteSize
                );

                R_ABORT_UNLESS(fs::WriteFile(
                    mFileHandle,
                    mFilePos,
                    mpBuffer,
                    mBufferSize,
                    fs::WriteOption::Flush
                ));

                mFilePos += (s64)mBufferSize;
                PacketCursor += WriteSize;
                RemainingSize -= WriteSize;
                mCursor = 0;
            }

            memcpy(
                mpBuffer + mCursor,
                reinterpret_cast<const uint8_t*>(&packet) + PacketCursor,
                RemainingSize
            );
            mCursor += RemainingSize;
            os::UnlockMutex(&mLock);
        }

        void RegisterInterface(u32 id)
        {
            this->WritePacket<u32>(Packet<u32> {
                .PacketId = SnifferPacket::RegisterInterface,
                .Tick = os::GetSystemTick(),
                .Value = id
            });
        }

        void RegisterReadEndpoint(u32 id)
        {
            this->WritePacket<u32>(Packet<u32> {
                .PacketId = SnifferPacket::RegisterReadEndpoint,
                .Tick = os::GetSystemTick(),
                .Value = id
            });
        }

        void RegisterWriteEndpoint(u32 id)
        {
            this->WritePacket<u32>(Packet<u32> {
                .PacketId = SnifferPacket::RegisterWriteEndpoint,
                .Tick = os::GetSystemTick(),
                .Value = id
            });
        }

        template<typename T>
        void WriteFunction(u32 object, u32 function_id, T function)
        {
            this->WritePacket<FunctionPacket<T>>(Packet<FunctionPacket<T>> {
                .PacketId = SnifferPacket::FunctionCall,
                .Tick = os::GetSystemTick(),
                .Value = {
                    .ObjectId = object,
                    .FunctionId = function_id,
                    .Value = function
                }
            });
        }
    };

    namespace
    {
        static std::atomic_uint32_t g_IfSession;
        static std::atomic_uint32_t g_EpSession;
    }

    UsbIfSessionSniffer::UsbIfSessionSniffer(os::NativeHandle client, ::Service forward_service)
        : mForwardService(forward_service), mClientProcess(client), pSniffer(nullptr), mId(g_IfSession.fetch_add(1))
    {
        pSniffer = (Sniffer*)malloc(sizeof(Sniffer));
        new(pSniffer) Sniffer(mId);

        pSniffer->RegisterInterface(mId);
        pSniffer->WriteFunction(mId, (u32)FunctionCall::Constructor, ConstructorCall {});
    }

    UsbIfSessionSniffer::~UsbIfSessionSniffer()
    {
        pSniffer->WriteFunction(mId, (u32)FunctionCall::Destructor, DestructorCall {});
        pSniffer->~Sniffer();
        free(pSniffer);
        serviceClose(&mForwardService);
    }

    Result UsbIfSessionSniffer::GetStateChangeEvent(sf::OutCopyHandle out)
    {
        Handle h;
        Result r = usbHsIfGetStateChangeEventFwd(&mForwardService, &h);
        pSniffer->WriteFunction(
            mId,
            (u32)FunctionCall::GetStateChangeEvent,
            GetStateChangeEventCall { .out_Result = r }
        );
        if (R_SUCCEEDED(r))
        {
            out.SetValue(h, true);
        }
        return r;
    }
    Result UsbIfSessionSniffer::SetInterface(const sf::OutBuffer &out, u8 id)
    {
        Result r = usbHsIfSetInterfaceFwd(&mForwardService, out.GetPointer(), out.GetSize(), id);
        pSniffer->WriteFunction(
            mId,
            (u32)FunctionCall::SetInterface,
            SetInterfaceCall {
                .in_InterfaceId = id,
                .out_Result = r,
                .out_Interface = *reinterpret_cast<UsbHsInterface*>(out.GetPointer())
            }
        );
        return r;
    }
    Result UsbIfSessionSniffer::GetInterface(const sf::OutBuffer &out)
    {
        Result r = usbHsIfGetInterfaceFwd(&mForwardService, out.GetPointer(), out.GetSize());
        pSniffer->WriteFunction(
            mId,
            (u32)FunctionCall::GetInterface,
            GetInterfaceCall {
                .out_Result = r,
                .out_Interface = *reinterpret_cast<UsbHsInterface*>(out.GetPointer())
            }
        );
        return r;
    }
    Result UsbIfSessionSniffer::GetAlternateInterface(const sf::OutBuffer &out, u8 id)
    {
        Result r = usbHsIfGetAlternateInterfaceFwd(&mForwardService, out.GetPointer(), out.GetSize(), id);
        pSniffer->WriteFunction(
            mId,
            (u32)FunctionCall::GetAlternateInterface,
            GetAlternateInterfaceCall {
                .in_InterfaceId = id,
                .out_Result = r,
                .out_Interface = *reinterpret_cast<UsbHsInterface*>(out.GetPointer())
            }
        );
        return r;
    }
    Result UsbIfSessionSniffer::GetCurrentFrame(sf::Out<u32> current_frame)
    {
        u32 tmp;
        Result r = usbHsIfGetCurrentFrameFwd(&mForwardService, &tmp);
        pSniffer->WriteFunction(
            mId,
            (u32)FunctionCall::GetCurrentFrame,
            GetCurrentFrameCall {
                .out_Result = r,
                .out_FrameId = tmp
            }
        );
        if (R_SUCCEEDED(r))
        {
            current_frame.SetValue(tmp);
        }

        return r;
    }
    Result UsbIfSessionSniffer::CtrlXferAsync(u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u64 buffer)
    {
        Result r = usbHsIfCtrlXferAsyncFwd(&mForwardService, bmRequestType, bRequest, wValue, wIndex, wLength, buffer);
        pSniffer->WriteFunction(
            mId,
            (u32)FunctionCall::CtrlXferAsync,
            CtrlXferAsyncCall {
                .in_bmRequestType = bmRequestType,
                .in_bRequest = bRequest,
                .in_wValue = wValue,
                .in_wIndex = wIndex,
                .in_wLength = wLength,
                .in_Buffer = buffer,
                .out_Result = r
            }
        );

        return r;
    }
    Result UsbIfSessionSniffer::GetCtrlXferCompletionEvent(sf::OutCopyHandle out)
    {
        Handle h;
        Result r = usbHsIfGetCtrlXferCompletionEventFwd(&mForwardService, &h);
        pSniffer->WriteFunction(
            mId,
            (u32)FunctionCall::GetCtrlXferCompletionEvent,
            GetCtrlXferCompletionEventCall {
                .out_Result = r
            }
        );
        if (R_SUCCEEDED(r))
        {
            out.SetValue(h, true);
        }
        return r;
    }
    Result UsbIfSessionSniffer::GetCtrlXferReport(const sf::OutBuffer &out)
    {
        Result r = usbHsIfGetCtrlXferReportFwd(&mForwardService, out.GetPointer(), out.GetSize());
        pSniffer->WriteFunction(
            mId,
            (u32)FunctionCall::GetCtrlXferReport,
            GetCtrlXferReportCall {
                .out_Result = r,
                .out_Report = *reinterpret_cast<UsbHsXferReport*>(out.GetPointer())
            }
        );
        return r;
    }
    Result UsbIfSessionSniffer::ResetDevice()
    {
        Result r = usbHsIfResetDeviceFwd(&mForwardService);
        pSniffer->WriteFunction(mId, (u32)FunctionCall::ResetDevice, ResetDeviceCall { .out_Result = r });
        return r;
    }
    Result UsbIfSessionSniffer::OpenUsbEp(sf::Out<sf::SharedPointer<::ams::usb::IClientEpSession>> out_session, sf::Out<usb_endpoint_descriptor> out_desc, u16 maxUrbCount, u32 epType, u32 epNumber, u32 epDirection, u32 maxXferSize)
    {
        Service EpSession;
        Result r = usbHsIfOpenUsbEpFwd(
            &mForwardService,
            &EpSession,
            maxUrbCount,
            epType,
            epNumber,
            epDirection,
            maxXferSize,
            out_desc.GetPointer()
        );

        pSniffer->WriteFunction(
            mId,
            (u32)FunctionCall::OpenUsbEp,
            OpenUsbEpCall {
                .in_maxUrbCount = maxUrbCount,
                .in_epType = epType,
                .in_epNumber = epNumber,
                .in_epDirection = epDirection,
                .in_maxXferSize = maxXferSize,
                .out_Result = r,
                .out_Descriptor = out_desc.GetValue()
            }
        );

        if (R_SUCCEEDED(r))
        {
            out_session.SetValue(sf::ObjectFactory<sf::ExpHeapAllocator::Policy>::CreateSharedEmplaced<ams::usb::IClientEpSession, UsbEpSessionSniffer>(
                std::addressof(g_SfAllocator),
                mClientProcess, EpSession, pSniffer, epDirection == 2
            ));
        }

        return r;
    }


    UsbEpSessionSniffer::UsbEpSessionSniffer(
        os::NativeHandle client,
        ::Service forward_service,
        Sniffer* pSniffer,
        bool is_write
    )
        : mForwardService(forward_service), mClientProcess(client), pParentSniffer(pSniffer), mId(g_EpSession.fetch_add(1)), mIsWrite(is_write)
    {
        if (mIsWrite)
        {
            pParentSniffer->RegisterWriteEndpoint(mId);
        }
        else
        {
            pParentSniffer->RegisterReadEndpoint(mId);
        }
        pParentSniffer->WriteFunction(mId, (u32)FunctionCall::Constructor, ConstructorCall {});
    }

    UsbEpSessionSniffer::~UsbEpSessionSniffer()
    {
        pParentSniffer->WriteFunction(mId, (u32)FunctionCall::Destructor, DestructorCall {});
        serviceClose(&mForwardService);
    }

    Result UsbEpSessionSniffer::ReOpen()
    {
        Result r = usbHsEpReOpenFwd(&mForwardService);
        pParentSniffer->WriteFunction(mId, (u32)FunctionCall::ReOpen, ReOpenCall { .out_Result = r });
        return r;
    }

    Result UsbEpSessionSniffer::Close()
    {
        Result r = usbHsEpCloseFwd(&mForwardService);
        pParentSniffer->WriteFunction(mId, (u32)FunctionCall::Close, CloseCall { .out_Result = r });
        return r;
    }

    Result UsbEpSessionSniffer::GetCompletionEvent(sf::OutCopyHandle out)
    {
        Handle h;
        Result r = usbHsEpGetCompletionEventFwd(&mForwardService, &h);
        pParentSniffer->WriteFunction(mId, (u32)FunctionCall::GetCompletionEvent, GetCompletionEventCall { .out_Result = r });
        if (R_SUCCEEDED(r))
        {
            out.SetValue(h, true);
        }
        return r;
    }

    Result UsbEpSessionSniffer::PopulateRing()
    {
        Result r = usbHsEpPopulateRingFwd(&mForwardService);
        pParentSniffer->WriteFunction(mId, (u32)FunctionCall::PopulateRing, PopulateRingCall { .out_Result = r });
        return r;
    }

    Result UsbEpSessionSniffer::PostBufferAsync(sf::Out<u32> xferId, u32 size, u64 buffer, u64 id)
    {
        u32 tmp;
        Result r = usbHsEpPostBufferAsyncFwd(&mForwardService, (void*)buffer, size, id, &tmp);
        pParentSniffer->WriteFunction(mId, (u32)FunctionCall::PostBufferAsync, PostBufferAsyncCall { .in_Size = size, .in_Buffer = buffer, .in_Id = id, .out_Result = r, .out_XferId = tmp });
        if (R_SUCCEEDED(r))
        {
            xferId.SetValue(tmp);
        }

        return r;
    }

    Result UsbEpSessionSniffer::GetXferReport(const sf::OutAutoSelectBuffer &out, sf::Out<u32> count, u32 max)
    {
        u32 tmp;
        Result r = usbHsEpGetXferReportFwd(&mForwardService, reinterpret_cast<UsbHsXferReport*>(out.GetPointer()), max, &tmp);
        pParentSniffer->WriteFunction(
            mId,
            (u32)FunctionCall::GetXferReport,
            GetXferReportCall {
                .in_Max = max,
                .out_Result = r,
                .out_Report = *reinterpret_cast<UsbHsXferReport*>(out.GetPointer()),
                .out_Count = tmp
            }
        );
        if (R_SUCCEEDED(r))
        {
            count.SetValue(tmp);
        }

        return r;
    }
    Result UsbEpSessionSniffer::BatchBufferAsync(sf::Out<u32> xferId, u32 urbCount, u32 unk1, u32 unk2, u64 buffer, u64 id)
    {
        u32 tmp;
        Result r = usbHsEpBatchBufferAsyncFwd(&mForwardService, (void*)buffer, urbCount, unk1, unk2, id, &tmp);
        pParentSniffer->WriteFunction(
            mId,
            (u32)FunctionCall::BatchBufferAsync,
            BatchBufferAsyncCall {
                .in_urbCount = urbCount,
                .in_unk1 = unk1,
                .in_unk2 = unk2,
                .in_buffer = buffer,
                .out_Result = r,
                .out_xferId = tmp
            }
        );
        if (R_SUCCEEDED(r))
        {
            xferId.SetValue(tmp);
        }

        return r;
    }
    Result UsbEpSessionSniffer::CreateSmmuSpace(u32 size, u64 buffer)
    {
        Result r = usbHsEpCreateSmmuSpaceFwd(&mForwardService, (void*)buffer, size);
        pParentSniffer->WriteFunction(
            mId,
            (u32)FunctionCall::CreateSmmuSpace,
            CreateSmmuSpaceCall {
                .in_Size = size,
                .in_Buffer = buffer,
                .out_Result = r
            }
        );
        return r;
    }
    Result UsbEpSessionSniffer::ShareReportRing(sf::CopyHandle &&xfer_mem, u32 size)
    {
        Result r = usbHsEpShareReportRingFwd(&mForwardService, xfer_mem.GetOsHandle(), size);
        pParentSniffer->WriteFunction(
            mId,
            (u32)FunctionCall::ShareReportRing,
            ShareReportRingCall {
                .in_Handle = xfer_mem.GetOsHandle(),
                .in_Size = size,
                .out_Result = r
            }
        );
        return r;
    }

    void Initialize()
    {
        g_HeapHandle = lmem::CreateExpHeap(g_HeapMemory, g_HeapMemorySize, lmem::CreateOption_ThreadSafe);
        AMS_ABORT_UNLESS(g_HeapHandle != nullptr);
        g_SfAllocator.Attach(g_HeapHandle);
    }
}