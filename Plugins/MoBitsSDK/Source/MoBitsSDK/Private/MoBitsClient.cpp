#include "MoBitsClient.h"
#include "HAL/PlatformTime.h"

namespace
{
TSharedRef<FJsonObject> Command(const TCHAR* Type)
{ auto O = MakeShared<FJsonObject>(); O->SetStringField(TEXT("type"), Type); return O; }
}
bool FMoBitsClient::Fresh() const
{
    const double Now = FPlatformTime::Seconds();
    return State == EMoBitsConnectionState::Connected && bHasFrame &&
        Now - LastTick <= MoBitsCore::TimeoutSeconds && Now - LastPong <= MoBitsCore::TimeoutSeconds &&
        Now - LastMotion <= MoBitsCore::TimeoutSeconds && Clock.AgeMs(Latest.ServerTimeMs, Now) <= 500;
}
bool FMoBitsClient::Frame(FMoBitsMotionFrame& Out) const
{ if (!Fresh()) return false; Out = Latest; return true; }
bool FMoBitsClient::DevicePose(const FString& Id, FMoBitsDevicePose& Out) const
{
    if (!Fresh() || !Latest.bTrackingActive) return false;
    for (const auto& D : Latest.Devices)
        if (D.DeviceId == Id && D.SampleAgeMs + Clock.AgeMs(Latest.ServerTimeMs, FPlatformTime::Seconds()) <= 500)
        { Out = D; Out.SampleAgeMs += Clock.AgeMs(Latest.ServerTimeMs, FPlatformTime::Seconds()); return true; }
    return false;
}
int64 FMoBitsClient::Request(ECommand Kind, const TSharedRef<FJsonObject>& Object, int32 TtlMs)
{
    check(IsInGameThread());
    const bool bStop = Kind == ECommand::Stop;
    if (State != EMoBitsConnectionState::Connected && !(bStop && State == EMoBitsConnectionState::Disconnecting))
    { Report(TEXT("MoBits session is not connected.")); return 0; }
    int32 Stops = 0, Ordinary = 0;
    for (const auto& P : Pending) { if (P.Value.Kind == ECommand::Stop) ++Stops; else ++Ordinary; }
    if ((!bStop && Ordinary >= MoBitsCore::MaxPending) || (bStop && Stops >= 8))
    { Fault(TEXT("MoBits pending command limit was reached.")); return 0; }
    if (NextId >= 4294967295LL) { Fault(TEXT("MoBits command ID exhausted. Open a new session.")); return 0; }
    const int64 Id = ++NextId;
    Object->SetNumberField(TEXT("id"), static_cast<double>(Id));
    const double Now = FPlatformTime::Seconds();
    FString Error;
    if (!Control || !Control->Send(MoBitsProtocol::Encode(Object), Now + (TtlMs > 0 ? TtlMs / 1000.0 : 5), Error))
    { Fault(Error.IsEmpty() ? TEXT("MoBits transport is unavailable.") : Error); return 0; }
    Pending.Add(Id, {Kind, Gate.Epoch(), Now + 5});
    return Id;
}
int64 FMoBitsClient::Devices() { return Request(ECommand::Devices, Command(TEXT("GET_DEVICES"))); }
int64 FMoBitsClient::Arm()
{
    if (bStopUnconfirmed) { Report(TEXT("Confirm STOP before arming again.")); return 0; }
    for (const auto& P : Pending) if (P.Value.Kind == ECommand::Stop) { Report(TEXT("Wait for STOP confirmation before arming.")); return 0; }
    if (!Fresh()) { Report(TEXT("Fresh tracking frames are required before arming.")); return 0; }
    for (const auto& P : Pending)
        if (P.Value.Kind == ECommand::Arm) { Report(TEXT("An ARM request is already pending.")); return 0; }
    if (Armed()) { Report(TEXT("MoBits haptics are already armed.")); return 0; }
    return Request(ECommand::Arm, Command(TEXT("ARM_HAPTICS")));
}
int64 FMoBitsClient::Play(int32 TargetBone, int32 Effect, float Level, int32 TtlMs)
{
    const char* Target = MoBitsCore::TargetName(TargetBone);
    if (!Target || !MoBitsCore::ValidPlayback(Effect, Level, TtlMs))
    { Report(TEXT("PLAY requires a body target, effect 1..30, finite level (0,1], and ttlMs 1..500.")); return 0; }
    if (!Armed() || !Fresh()) { Report(TEXT("Arm haptics and consume fresh tracking before PLAY.")); return 0; }
    auto O = Command(TEXT("PLAY")); O->SetStringField(TEXT("target"), UTF8_TO_TCHAR(Target));
    O->SetNumberField(TEXT("effect"), Effect); O->SetNumberField(TEXT("level"), Level); O->SetNumberField(TEXT("ttl_ms"), TtlMs);
    return Request(ECommand::Play, O, TtlMs);
}
int64 FMoBitsClient::PlayGlobal(int32 Effect, float Level, int32 TtlMs)
{
    if (!MoBitsCore::ValidPlayback(Effect, Level, TtlMs))
    { Report(TEXT("PLAY_GLOBAL requires effect 1..30, finite level (0,1], and ttlMs 1..500.")); return 0; }
    if (!Armed() || !Fresh()) { Report(TEXT("Arm haptics and consume fresh tracking before PLAY_GLOBAL.")); return 0; }
    auto O = Command(TEXT("PLAY_GLOBAL"));
    O->SetNumberField(TEXT("effect"), Effect); O->SetNumberField(TEXT("level"), Level); O->SetNumberField(TEXT("ttl_ms"), TtlMs);
    return Request(ECommand::Global, O, TtlMs);
}
int64 FMoBitsClient::Stop()
{
    Gate.Stop(); // invalidates every older ARM/PLAY response immediately
    return Request(ECommand::Stop, Command(TEXT("STOP_ALL")));
}
bool FMoBitsClient::Receive(bool bMotion, const FString& Text)
{
    TSharedPtr<FJsonObject> O;
    FString Type;
    if (!MoBitsProtocol::Parse(Text, O) || !MoBitsProtocol::String(O, TEXT("type"), Type)) return false;
    const double Now = FPlatformTime::Seconds();
    if (Type == TEXT("HELLO"))
    {
        FString Id; double Time = 0;
        if ((bMotion ? bMotionHello : bControlHello) || !MoBitsProtocol::Hello(O, bMotion, Id, Time)) return false;
        if (bMotion)
        {
            if (Id != SessionId || !bControlHello) return false;
            bMotionHello = true; LastMotion = Now;
        }
        else
        {
            if (!Options.ExpectedSessionId.IsEmpty() && Id != Options.ExpectedSessionId) return false;
            SessionId = Id; bControlHello = true; LastPong = Now;
            MotionSocket = MakeUnique<FMoBitsSocket>(); FString Error;
            if (!MotionSocket->Open(Options.Port, true, Options.Token, SessionId, Error)) { Report(Error); return false; }
            Options.Token.Empty(); // no need to keep credentials after both upgrades were queued
        }
        Clock.Observe(Time, Now);
        if (bControlHello && bMotionHello)
        {
            ConnectedAt = Now;
            SetState(EMoBitsConnectionState::Connected);
            Devices(); // refreshed on every new generation, including automatic recovery
        }
        return true;
    }
    if (!(bMotion ? bMotionHello : bControlHello)) return false;
    if (bMotion)
    {
        FMoBitsMotionFrame F;
        if (!MoBitsProtocol::Motion(O, SessionId, F) || F.Sequence <= Latest.Sequence ||
            (bHasFrame && F.OriginRevision < Latest.OriginRevision)) return false;
        Latest = MoveTemp(F); bHasFrame = true; LastMotion = Now;
        return true;
    }
    if (Type == TEXT("PONG"))
    {
        double Time = 0;
        if (!MoBitsProtocol::Number(O, TEXT("server_time_ms"), Time) || Time < 0) return false;
        Clock.Observe(Time, Now); LastPong = Now; return true;
    }
    int64 Id = 0;
    if (!MoBitsProtocol::Integer(O, TEXT("id"), Id, 1, 4294967295LL)) return false;
    const FPending* Found = Pending.Find(Id);
    if (!Found) return false;
    const FPending P = *Found;
    FEvent E;
    if (Type == TEXT("DEVICES"))
    {
        if (P.Kind != ECommand::Devices || !MoBitsProtocol::Devices(O, E.Devices)) return false;
        E.Kind = EEventKind::Devices;
    }
    else if (Type == TEXT("RESULT"))
    {
        E.Kind = EEventKind::Receipt;
        if (P.Kind == ECommand::Devices || !MoBitsProtocol::Receipt(O, E.Receipt)) return false;
        const FString& Status = E.Receipt.Status;
        const bool bFailure = Status == TEXT("rejected") || Status == TEXT("expired");
        if (P.Kind == ECommand::Arm)
        {
            if (!bFailure && Status != TEXT("armed")) return false;
            Gate.ArmReply(P.Epoch, E.Receipt.bSucceeded && State == EMoBitsConnectionState::Connected);
        }
        else if (P.Kind == ECommand::Stop)
        { if (!bFailure && Status != TEXT("stopped_confirmed") && Status != TEXT("stop_unconfirmed")) return false;
            if (P.Epoch == Gate.Epoch()) bStopUnconfirmed = Status != TEXT("stopped_confirmed"); }
        else
        {
            if (!bFailure && Status != TEXT("device_acknowledged") && Status != TEXT("effect_unconfigured")) return false;
            // App-global slots currently have no pattern definitions. Any future
            // acknowledgement must still never re-arm an old output epoch.
            Gate.PlayReply(P.Epoch, TCHAR_TO_UTF8(*Status));
        }
    }
    else return false;
    Pending.Remove(Id); Emit(MoveTemp(E));
    if (Id == DisconnectId)
    {
        // Closing after an unconfirmed OFF is not a confirmation. Preserve the
        // receipt and let the app retry its independent session-close STOP.
        CloseTransports(); CompletePending(TEXT("Session disconnected.")); SetState(EMoBitsConnectionState::Disconnected);
    }
    return true;
}
void FMoBitsClient::Tick()
{
    check(IsInGameThread());
    if (bSuspended) return;
    const double Now = FPlatformTime::Seconds();
    if (State == EMoBitsConnectionState::Disconnected) return;
    if (State == EMoBitsConnectionState::Faulted)
    {
        if (bConnectionWanted && bAutoReconnect && Now >= RetryAt)
        { FString Error; ResolveAndStart(Error); }
        return;
    }
    if (State == EMoBitsConnectionState::Connected && Fresh() && Now - ConnectedAt >= 1) RetryCount = 0;
    if (Now - LastTick > MoBitsCore::TimeoutSeconds)
    { Fault(TEXT("Game-thread progress stopped for more than 500 ms. Connection will recover; arm explicitly afterwards.")); return; }
    LastTick = Now;
    TArray<FString> Messages; FString Error;
    if (Control && !Control->Poll(Messages, Error)) { Fault(Error); return; }
    for (const auto& M : Messages)
    { if (!Receive(false, M)) { Fault(TEXT("Invalid MoBits control message.")); return; } if (!Control) return; }
    Messages.Reset();
    if (MotionSocket && !MotionSocket->Poll(Messages, Error)) { Fault(Error); return; }
    for (const auto& M : Messages)
        if (!Receive(true, M)) { Fault(TEXT("Invalid MoBits motion message.")); return; }
    if (State == EMoBitsConnectionState::Connecting && Now - Began > 5)
    { Fault(TEXT("MoBits HELLO timed out.")); return; }
    if (State == EMoBitsConnectionState::Connected &&
        (Now - LastPong > MoBitsCore::TimeoutSeconds || Now - LastMotion > MoBitsCore::TimeoutSeconds))
    { Fault(TEXT("MoBits heartbeat or motion stream stopped for more than 500 ms.")); return; }
    if (bHasFrame && Latest.Sequence > ConsumedSequence)
    {
        if (Clock.AgeMs(Latest.ServerTimeMs, Now) > 500)
        { Fault(TEXT("Buffered MoBits motion frame is stale.")); return; }
        // Acknowledge ONLY the latest frame actually consumed by this game tick.
        // Receiving bytes in a network callback is not game progress.
        ConsumedSequence = Latest.Sequence;
        FEvent E; E.Kind = EEventKind::Motion; E.Frame = Latest; Emit(MoveTemp(E));
    }
    if (bControlHello && Now - LastPing >= MoBitsCore::HeartbeatSeconds)
    {
        auto O = Command(TEXT("PING")); O->SetNumberField(TEXT("last_frame_sequence"), static_cast<double>(ConsumedSequence));
        if (!Control->Send(MoBitsProtocol::Encode(O), Now + 0.5, Error)) { Fault(Error); return; }
        LastPing = Now;
    }
    for (const auto& P : Pending)
        if (Now > P.Value.Deadline) { Fault(TEXT("MoBits command result timed out; STOP is unconfirmed.")); return; }
}
