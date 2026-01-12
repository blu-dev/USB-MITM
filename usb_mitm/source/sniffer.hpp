#include "usb_mitm_service.hpp"

namespace ams::usb::sniffer
{
    struct Sniffer;   

    class UsbIfSessionSniffer
    {
    public:
        enum class FunctionCall : u32
        {
            Constructor = 0,
            Destructor = 1,
            GetStateChangeEvent = 2,
            SetInterface = 3,
            GetInterface = 4,
            GetAlternateInterface = 5,
            GetCurrentFrame = 6,
            CtrlXferAsync = 7,
            GetCtrlXferCompletionEvent = 8,
            GetCtrlXferReport = 9,
            ResetDevice = 10,
            OpenUsbEp = 11,
        };

        struct ConstructorCall
        {
        };

        struct DestructorCall
        {
        };

        struct GetStateChangeEventCall
        {
            Result out_Result;
        };

        struct SetInterfaceCall
        {
            u32 in_InterfaceId;
            Result out_Result;
            UsbHsInterface out_Interface;
        };

        struct GetInterfaceCall
        {
            Result out_Result;
            UsbHsInterface out_Interface;
        };

        struct GetAlternateInterfaceCall
        {
            u32 in_InterfaceId;
            Result out_Result;
            UsbHsInterface out_Interface;
        };

        struct GetCurrentFrameCall
        {
            Result out_Result;
            u32 out_FrameId;
        };

        struct CtrlXferAsyncCall
        {
            u8 in_bmRequestType;
            u8 in_bRequest;
            u16 in_wValue;
            u16 in_wIndex;
            u16 in_wLength;
            u64 in_Buffer;
            Result out_Result;
        };

        struct GetCtrlXferCompletionEventCall
        {
            Result out_Result;
        };

        struct GetCtrlXferReportCall
        {
            Result out_Result;
            UsbHsXferReport out_Report;
        };

        struct ResetDeviceCall
        {
            Result out_Result;
        };

        struct OpenUsbEpCall
        {
            u16 in_maxUrbCount;
            u32 in_epType;
            u32 in_epNumber;
            u32 in_epDirection;
            u32 in_maxXferSize;
            Result out_Result;
            usb_endpoint_descriptor out_Descriptor;
        };

    private:
        ::Service mForwardService;
        os::NativeHandle mClientProcess;
        Sniffer* pSniffer;
        uint32_t mId;

    public:
        UsbIfSessionSniffer(os::NativeHandle client, ::Service forward_service);
        ~UsbIfSessionSniffer();

        Result GetStateChangeEvent(sf::OutCopyHandle out);
        Result SetInterface(const sf::OutBuffer &out, u8 id);
        Result GetInterface(const sf::OutBuffer &out);
        Result GetAlternateInterface(const sf::OutBuffer &out, u8 id);
        Result GetCurrentFrame(sf::Out<u32> current_frame);
        Result CtrlXferAsync(u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u64 buffer);
        Result GetCtrlXferCompletionEvent(sf::OutCopyHandle out);
        Result GetCtrlXferReport(const sf::OutBuffer &out);
        Result ResetDevice();
        Result OpenUsbEp(sf::Out<sf::SharedPointer<::ams::usb::IClientEpSession>> out_session, sf::Out<usb_endpoint_descriptor> out_desc, u16 maxUrbCount, u32 epType, u32 epNumber, u32 epDirection, u32 maxXferSize);
    };

    class UsbEpSessionSniffer
    {
    public:
        enum class FunctionCall : u32
        {
            Constructor = 12,
            Destructor = 13,
            ReOpen = 14,
            Close = 15,
            GetCompletionEvent = 16,
            PopulateRing = 17,
            PostBufferAsync = 18,
            GetXferReport = 19,
            BatchBufferAsync = 20,
            CreateSmmuSpace = 21,
            ShareReportRing = 22
        };

        struct ConstructorCall
        {
        };

        struct DestructorCall
        {
        };

        struct ReOpenCall
        {
            Result out_Result;
        };

        struct CloseCall
        {
            Result out_Result;
        };

        struct GetCompletionEventCall
        {
            Result out_Result;
        };

        struct PopulateRingCall
        {
            Result out_Result;
        };

        struct PostBufferAsyncCall
        {
            u32 in_Size;
            u64 in_Buffer;
            u64 in_Id;
            Result out_Result;
            u32 out_XferId;
        };

        struct GetXferReportCall
        {
            u32 in_Max;
            Result out_Result;
            UsbHsXferReport out_Report;
            u32 out_Count;
        };

        struct BatchBufferAsyncCall
        {
            u32 in_urbCount;
            u32 in_unk1;
            u32 in_unk2;
            u64 in_buffer;
            Result out_Result;
            u32 out_xferId;
        };

        struct CreateSmmuSpaceCall
        {
            u32 in_Size;
            u64 in_Buffer;
            Result out_Result;
        };

        struct ShareReportRingCall
        {
            u32 in_Handle;
            u32 in_Size;
            Result out_Result;
        };

    private:
        ::Service mForwardService;
        os::NativeHandle mClientProcess;
        Sniffer* pParentSniffer;
        uint32_t mId;
        bool mIsWrite;

    public:
        UsbEpSessionSniffer(os::NativeHandle client, ::Service forward_service, Sniffer* pSniffer, bool is_write);
        ~UsbEpSessionSniffer();

        Result ReOpen();
        Result Close();
        Result GetCompletionEvent(sf::OutCopyHandle out);
        Result PopulateRing();
        Result PostBufferAsync(sf::Out<u32> xferId, u32 size, u64 buffer, u64 id);
        Result GetXferReport(const sf::OutAutoSelectBuffer &out, sf::Out<u32> count, u32 max);
        Result BatchBufferAsync(sf::Out<u32> xferId, u32 urbCount, u32 unk1, u32 unk2, u64 buffer, u64 id);
        Result CreateSmmuSpace(u32 size, u64 buffer);
        Result ShareReportRing(sf::CopyHandle &&xfer_mem, u32 size);
    };

    void Initialize();
}