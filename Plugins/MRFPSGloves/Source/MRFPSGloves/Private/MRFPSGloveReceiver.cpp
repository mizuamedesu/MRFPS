#include "MRFPSGloveReceiver.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

FMRFPSGloveReceiver::FMRFPSGloveReceiver(FSocket* InSocket,const FString& InSender,float InStaleAfter)
    : Socket(InSocket),AllowedSender(InSender),StaleAfter(InStaleAfter) {}

FMRFPSGloveReceiver::~FMRFPSGloveReceiver()
{
    Stop();
    if (Thread) { Thread->WaitForCompletion(); delete Thread; Thread=nullptr; }
}

bool FMRFPSGloveReceiver::Start()
{
    if (!Socket || Thread) return false;
    Thread=FRunnableThread::Create(this,TEXT("MRFPSGloveReceiver"),0,TPri_Normal);
    return Thread!=nullptr;
}

void FMRFPSGloveReceiver::Stop() { bStop.Store(true); }

void FMRFPSGloveReceiver::TakePackets(TArray<FPacket>& Out)
{
    FScopeLock Lock(&Mutex);
    Swap(Out,Packets);
}

uint32 FMRFPSGloveReceiver::Run()
{
    double LastPoll=-1;
    bool DiscardBacklog=false;
    uint8 Data[8192];
    const TSharedRef<FInternetAddr> Sender=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
    while (!bStop.Load()) {
        Socket->Wait(ESocketWaitConditions::WaitForRead,FTimespan::FromMilliseconds(10));
        if (bStop.Load()) break;
        const double Now=FPlatformTime::Seconds();
        if (LastPoll>=0 && (Now<LastPoll || Now-LastPoll>StaleAfter)) {
            // A suspended receiver cannot distinguish packets queued before
            // resume from live input. Drain them and wait for new arrivals.
            DiscardBacklog=true;
            FScopeLock Lock(&Mutex);Packets.Reset();
        }
        LastPoll=Now;
        uint32 Pending=0;
        for (int32 Count=0;Count<512 && Socket->HasPendingData(Pending);++Count) {
            int32 Read=0;
            if (!Socket->RecvFrom(Data,sizeof(Data),Read,*Sender)) break;
            if (DiscardBacklog || Pending>sizeof(Data) || Sender->ToString(false)!=AllowedSender) continue;
            FPacket Packet;Packet.ReceivedAt=FPlatformTime::Seconds();Packet.Data.Append(Data,Read);
            FScopeLock Lock(&Mutex);
            // Keep recent input if the game stalls; stale samples are never
            // made fresh just because the game thread eventually reads them.
            if (Packets.Num()>=512) Packets.RemoveAt(0,1,EAllowShrinking::No);
            Packets.Add(MoveTemp(Packet));
        }
        if (DiscardBacklog && !Socket->HasPendingData(Pending)) DiscardBacklog=false;
    }
    return 0;
}
