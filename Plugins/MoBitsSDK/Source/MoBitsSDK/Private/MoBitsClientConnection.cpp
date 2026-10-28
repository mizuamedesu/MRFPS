#include "MoBitsClient.h"
#include "MoBitsDiscovery.h"
#include "HAL/PlatformTime.h"

// Session ownership, discovery and bounded recovery. All entry points run on the game thread.
void FMoBitsClient::CloseTransports()
{
    DiscardTrackingEvents();
    Control.Reset(); MotionSocket.Reset(); Options.Token.Empty(); SessionId.Empty();
    bControlHello = bMotionHello = bHasFrame = false; Latest = FMoBitsMotionFrame();
    Clock.Reset(); Gate.Stop(); bStopUnconfirmed = false; ConsumedSequence = 0; DisconnectId = 0;
}
void FMoBitsClient::Fault(const FString& Reason)
{
    const bool bDisconnecting = State == EMoBitsConnectionState::Disconnecting;
    CloseTransports(); CompletePending(Reason); Report(Reason);
    SetState(bDisconnecting ? EMoBitsConnectionState::Disconnected : EMoBitsConnectionState::Faulted);
    if (!bDisconnecting) ScheduleRetry();
}
void FMoBitsClient::ForceClose()
{
    check(IsInGameThread());
    bConnectionWanted = false; ManualOptions = FMoBitsSessionOptions(); DiscoveryFile.Empty(); RetryCount = 0;
    // Closing either channel causes the app's independent cancellation/OFF path.
    CloseTransports(); CompletePending(TEXT("Session ended; app STOP confirmation was not received."));
    ++Generation; Events.Reset(); SetState(EMoBitsConnectionState::Disconnected);
}
void FMoBitsClient::ScheduleRetry()
{
    RetryAt = FPlatformTime::Seconds() + FMath::Min(2.0, .25 * (1 << FMath::Min(RetryCount, 3)));
    RetryCount = FMath::Min(RetryCount + 1, MAX_int32 - 1);
}
bool FMoBitsClient::ResolveAndStart(FString& Error)
{
    FMoBitsSessionOptions NewOptions = ManualOptions;
    if (bDiscover)
    {
        FString Json;
        if (!(DiscoveryFile.IsEmpty() ? MoBitsDiscovery::Read(Json, Error) : MoBitsDiscovery::ReadFile(DiscoveryFile, Json, Error)) ||
            !MoBitsProtocol::Discovery(Json, NewOptions))
        {
            if (Error.IsEmpty()) Error = TEXT("Invalid MoBits discovery information. Waiting for the local app.");
            Json.Empty(); Fault(Error); return false;
        }
        Json.Empty();
    }
    return Start(NewOptions, Error);
}
bool FMoBitsClient::ConnectLocal(FString& Error, const FString& DiscoveryPath)
{
    check(IsInGameThread());
    if (State == EMoBitsConnectionState::Connected) return true;
    if (State != EMoBitsConnectionState::Disconnected && State != EMoBitsConnectionState::Faulted)
    { Error = TEXT("MoBits connection is already changing."); return false; }
    bConnectionWanted = true; bDiscover = true; DiscoveryFile = DiscoveryPath;
    ManualOptions = FMoBitsSessionOptions(); RetryCount = 0;
    return ResolveAndStart(Error);
}
bool FMoBitsClient::Connect(const FString& Json, FString& Error, bool bDiscovery)
{
    check(IsInGameThread());
    if (State != EMoBitsConnectionState::Disconnected && State != EMoBitsConnectionState::Faulted)
    { Error = TEXT("Disconnect the current MoBits session first."); return false; }
    FMoBitsSessionOptions NewOptions;
    if (!(bDiscovery ? MoBitsProtocol::Discovery(Json, NewOptions) : MoBitsProtocol::Session(Json, NewOptions)))
    { Error = TEXT("Invalid MoBits session JSON. Use the app's current loopback connection information."); return false; }
    bConnectionWanted = true; bDiscover = false; DiscoveryFile.Empty(); RetryCount = 0;
    ManualOptions = NewOptions; // private memory only; fixed-session callers cannot discover rotated credentials
    return Start(NewOptions, Error);
}
bool FMoBitsClient::Start(const FMoBitsSessionOptions& NewOptions, FString& Error)
{
    CloseTransports(); Pending.Reset(); Events.Reset(); ++Generation; Options = NewOptions; NextId = 0;
    Began = LastTick = LastPing = LastPong = LastMotion = FPlatformTime::Seconds();
    Control = MakeUnique<FMoBitsSocket>();
    SetState(EMoBitsConnectionState::Connecting);
    if (!Control->Open(Options.Port, false, Options.Token, FString(), Error))
    { Fault(Error); return false; }
    return true;
}
void FMoBitsClient::Disconnect()
{
    check(IsInGameThread());
    bConnectionWanted = false; ManualOptions = FMoBitsSessionOptions(); DiscoveryFile.Empty(); RetryCount = 0;
    DiscardTrackingEvents();
    if (State == EMoBitsConnectionState::Disconnecting) return;
    if (State != EMoBitsConnectionState::Connected) { ForceClose(); return; }
    DisconnectId = Stop();
    if (DisconnectId != 0) SetState(EMoBitsConnectionState::Disconnecting);
}
void FMoBitsClient::Suspend()
{
    check(IsInGameThread());
    if (bSuspended) return;
    bSuspended = true;
    if (State == EMoBitsConnectionState::Connecting || State == EMoBitsConnectionState::Connected)
        Fault(TEXT("MoBits paused; tracking will recover after resume."));
}
void FMoBitsClient::Resume()
{
    check(IsInGameThread());
    if (!bSuspended) return;
    bSuspended = false; LastTick = RetryAt = FPlatformTime::Seconds();
}
