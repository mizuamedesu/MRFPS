#pragma once

#include "CoreMinimal.h"
#include "MoBitsWire.h"
#include <deque>

class FSocket;
class ISocketSubsystem;

/** Nonblocking IPv4 loopback only. All operations run on the game thread. */
class FMoBitsSocket
{
public:
    ~FMoBitsSocket();
    bool Open(int32 Port, bool bMotion, const FString& Token, const FString& Session, FString& Error);
    bool Poll(TArray<FString>& Messages, FString& Error);
    bool Send(const FString& Text, double DeadlineSeconds, FString& Error);
    void Close();
    bool IsUpgraded() const { return bUpgraded; }
private:
    struct FSend { std::vector<uint8> Bytes; std::size_t Offset = 0; double Deadline = 0; };
    bool Queue(std::vector<uint8>&& Bytes, double Deadline, FString& Error);
    bool Flush(FString& Error);
    bool Consume(const uint8* Bytes, int32 Count, TArray<FString>& Messages, FString& Error);
    bool WouldBlock() const;
    ISocketSubsystem* Subsystem = nullptr;
    FSocket* Socket = nullptr;
    bool bUpgraded = false;
    bool bTcpConnected = false;
    double OpenedAt = 0;
    std::string ExpectedAccept;
    std::string Headers;
    MoBitsWire::Decoder Decoder;
    std::deque<FSend> Sends;
    std::size_t QueuedBytes = 0;
};
