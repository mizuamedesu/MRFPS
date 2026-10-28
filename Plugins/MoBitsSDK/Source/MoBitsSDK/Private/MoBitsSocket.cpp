#include "MoBitsSocket.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "Misc/Base64.h"
#include "Misc/Guid.h"
#include "Misc/SecureHash.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

namespace
{
std::string Utf8String(const FString& Value)
{ const FTCHARToUTF8 V(*Value); return std::string(V.Get(), V.Length()); }
FString UnrealString(const std::string& Value)
{ const FUTF8ToTCHAR V(Value.data(), static_cast<int32>(Value.size())); return FString(V.Length(), V.Get()); }
}
FMoBitsSocket::~FMoBitsSocket() { Close(); }
bool FMoBitsSocket::WouldBlock() const
{
    const ESocketErrors E = Subsystem->GetLastErrorCode();
    return E == SE_EWOULDBLOCK || E == SE_EINPROGRESS || E == SE_EALREADY || E == SE_EINTR;
}
bool FMoBitsSocket::Open(int32 Port, bool bMotion, const FString& Token, const FString& Session, FString& Error)
{
    check(IsInGameThread());
    Close();
    Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!Subsystem) { Error = TEXT("Socket subsystem is unavailable."); return false; }
    Socket = Subsystem->CreateSocket(NAME_Stream, TEXT("MoBits authenticated loopback"), FNetworkProtocolTypes::IPv4);
    if (!Socket) { Error = TEXT("Cannot create MoBits socket."); return false; }
    Socket->SetNonBlocking(true);
    Socket->SetNoDelay(true);
    int32 ActualSize = 0;
    Socket->SetReceiveBufferSize(256 * 1024, ActualSize);
    TSharedRef<FInternetAddr> Addr = Subsystem->CreateInternetAddr();
    bool bValid = false;
    Addr->SetIp(TEXT("127.0.0.1"), bValid);
    Addr->SetPort(Port);
    if (!bValid || (!Socket->Connect(*Addr) && !WouldBlock()))
    { Error = TEXT("Cannot connect to the MoBits app."); Close(); return false; }
    OpenedAt = FPlatformTime::Seconds();
    const FGuid KeyGuid = FGuid::NewGuid();
    const FString Key = FBase64::Encode(reinterpret_cast<const uint8*>(&KeyGuid), sizeof(KeyGuid));
    const FTCHARToUTF8 HashInput(*(Key + TEXT("258EAFA5-E914-47DA-95CA-C5AB0DC85B11")));
    uint8 Hash[20];
    FSHA1::HashBuffer(HashInput.Get(), HashInput.Length(), Hash);
    ExpectedAccept = Utf8String(FBase64::Encode(Hash, sizeof(Hash)));
    const std::string Request = MoBitsWire::UpgradeRequest(Port, bMotion, Utf8String(Token), Utf8String(Key), Utf8String(Session));
    return Queue(std::vector<uint8>(Request.begin(), Request.end()), OpenedAt + 2, Error);
}
bool FMoBitsSocket::Queue(std::vector<uint8>&& Bytes, double Deadline, FString& Error)
{
    if (Sends.size() >= 32 || QueuedBytes + Bytes.size() > 65536)
    { Error = TEXT("MoBits transport queue is full."); return false; }
    QueuedBytes += Bytes.size();
    Sends.push_back({std::move(Bytes), 0, Deadline});
    return true;
}
bool FMoBitsSocket::Flush(FString& Error)
{
    while (!Sends.empty())
    {
        FSend& Next = Sends.front();
        if (FPlatformTime::Seconds() > Next.Deadline)
        { Error = TEXT("MoBits command expired before transport send completed."); return false; }
        int32 Written = 0;
        const bool bSent = Socket->Send(Next.Bytes.data() + Next.Offset, static_cast<int32>(Next.Bytes.size() - Next.Offset), Written);
        if (!bSent)
        {
            if (WouldBlock()) return true;
            Error = FString::Printf(TEXT("MoBits transport send failed (socket error %d)."), static_cast<int32>(Subsystem->GetLastErrorCode()));
            return false;
        }
        if (Written <= 0) return true;
        Next.Offset += Written;
        if (Next.Offset == Next.Bytes.size()) { QueuedBytes -= Next.Bytes.size(); Sends.pop_front(); }
    }
    return true;
}
bool FMoBitsSocket::Send(const FString& Text, double DeadlineSeconds, FString& Error)
{
    check(IsInGameThread());
    if (!Socket || !bUpgraded) { Error = TEXT("MoBits transport is not ready."); return false; }
    const auto Utf8 = Utf8String(Text);
    if (Utf8.size() > 2048) { Error = TEXT("MoBits command exceeds 2048 bytes."); return false; }
    if (!Queue(MoBitsWire::Encode(1, Utf8, FGuid::NewGuid().A), DeadlineSeconds, Error)) return false;
    return Flush(Error);
}
bool FMoBitsSocket::Consume(const uint8* Bytes, int32 Count, TArray<FString>& Messages, FString& Error)
{
    if (!bUpgraded)
    {
        Headers.append(reinterpret_cast<const char*>(Bytes), Count);
        const auto End = Headers.find("\r\n\r\n");
        if (End == std::string::npos)
        { if (Headers.size() > MoBitsWire::MaxHeaderBytes) { Error = TEXT("MoBits upgrade headers are too large."); return false; } return true; }
        if (!MoBitsWire::ValidateUpgrade(Headers.substr(0, End + 4), ExpectedAccept))
        { Error = TEXT("MoBits WebSocket upgrade was rejected or invalid."); return false; }
        bUpgraded = true;
        std::string Remainder = Headers.substr(End + 4);
        Headers.clear(); ExpectedAccept.clear();
        return Consume(reinterpret_cast<const uint8*>(Remainder.data()), static_cast<int32>(Remainder.size()), Messages, Error);
    }
    std::vector<MoBitsWire::Message> Decoded;
    if (!Decoder.Feed(Bytes, Count, Decoded)) { Error = TEXT("Invalid MoBits WebSocket frame."); return false; }
    for (const auto& M : Decoded)
    {
        if (M.Opcode == 8) { Error = TEXT("MoBits app closed the connection."); return false; }
        if (M.Opcode == 9)
        { if (!Queue(MoBitsWire::Encode(10, M.Payload, FGuid::NewGuid().A), FPlatformTime::Seconds() + 0.5, Error)) return false; }
        else if (M.Opcode == 1)
        {
            if (Messages.Num() >= 64) { Error = TEXT("Too many MoBits messages in one game tick."); return false; }
            Messages.Add(UnrealString(M.Payload));
        }
    }
    return true;
}
bool FMoBitsSocket::Poll(TArray<FString>& Messages, FString& Error)
{
    check(IsInGameThread());
    if (!Socket) { Error = TEXT("MoBits transport is closed."); return false; }
    if (!bUpgraded && FPlatformTime::Seconds() - OpenedAt > 2) { Error = TEXT("MoBits connection timed out."); return false; }
    if (!bTcpConnected)
    {
        // A successful nonblocking Connect can still be in progress. Do not
        // write the HTTP upgrade until the socket becomes writable.
        if (!Socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::Zero())) return true;
        if (Socket->GetConnectionState() == SCS_ConnectionError)
        { Error = TEXT("MoBits TCP connection failed."); return false; }
        bTcpConnected = true;
    }
    if (!Flush(Error)) return false;
    uint8 Bytes[16384];
    int32 Total = 0;
    while (Total < 256 * 1024)
    {
        int32 Count = 0;
        if (!Socket->Recv(Bytes, sizeof(Bytes), Count))
        { if (WouldBlock()) return true; Error = TEXT("MoBits transport receive failed."); return false; }
        // FSocketBSD reports would-block as true with zero bytes. A graceful
        // TCP close reports false and is handled above.
        if (Count == 0) return true;
        Total += Count;
        if (!Consume(Bytes, Count, Messages, Error)) return false;
    }
    return true;
}
void FMoBitsSocket::Close()
{
    if (Socket)
    { Socket->Shutdown(ESocketShutdownMode::ReadWrite); Socket->Close(); Subsystem->DestroySocket(Socket); Socket = nullptr; }
    Sends.clear(); QueuedBytes = 0; Headers.clear(); ExpectedAccept.clear(); Decoder = MoBitsWire::Decoder(); bUpgraded = bTcpConnected = false;
}
