#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/CriticalSection.h"

class FRunnableThread;
class FSocket;

// The network reader timestamps input independently of the render/game tick.
// Only the owning game thread parses packets and updates glove state.
class FMRFPSGloveReceiver final : public FRunnable
{
public:
    struct FPacket { TArray<uint8> Data; double ReceivedAt=0; };
    FMRFPSGloveReceiver(FSocket* InSocket,const FString& InSender,float InStaleAfter);
    virtual ~FMRFPSGloveReceiver() override;
    bool Start();
    virtual uint32 Run() override;
    virtual void Stop() override;
    void TakePackets(TArray<FPacket>& Out);

private:
    FSocket* Socket;
    FString AllowedSender;
    float StaleAfter;
    FRunnableThread* Thread=nullptr;
    TAtomic<bool> bStop{false};
    FCriticalSection Mutex;
    TArray<FPacket> Packets;
};
