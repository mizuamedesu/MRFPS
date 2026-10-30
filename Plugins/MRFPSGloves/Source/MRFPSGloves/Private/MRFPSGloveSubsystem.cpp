#include "MRFPSGloveSubsystem.h"
#include "MRFPSGloveSettings.h"
#include "MRFPSGloveReceiver.h"
#include "MRFPSGloveConnection.h"
#include "MRFPSOSC.h"
#include "Common/UdpSocketBuilder.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSGloves, Log, All);

UMRFPSGloveSubsystem::UMRFPSGloveSubsystem() : FTickableGameObject(ETickableTickType::Never) {}
UMRFPSGloveSubsystem::~UMRFPSGloveSubsystem() { StopReceiver(); }

bool UMRFPSGloveSubsystem::StartReceiver()
{
    if (Receiver) return false;
    Receiver=new FMRFPSGloveReceiver(Socket,AllowedSender,StaleAfter);
    if (Receiver->Start()) return true;
    StopReceiver();
    return false;
}

void UMRFPSGloveSubsystem::StopReceiver() { delete Receiver;Receiver=nullptr; }

bool UMRFPSGloveSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
    return Super::ShouldCreateSubsystem(Outer) && !IsRunningDedicatedServer()
        && !FParse::Param(FCommandLine::Get(), TEXT("MRFPSDesktop"))
        && !FParse::Param(FCommandLine::Get(), TEXT("MRFPSInspector"))
        && !FParse::Param(FCommandLine::Get(), TEXT("Inspector"));
}

void UMRFPSGloveSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    const UMRFPSGloveSettings* Settings = GetDefault<UMRFPSGloveSettings>();
    if (!Settings->bEnabled) return;
#if PLATFORM_ANDROID
    const MRFPSGloveConnection::FPin Defaults{Settings->AllowedSender, Settings->ListenPort, Settings->DeviceId};
    MRFPSGloveConnection::FPin Pin;
    bool bSaveExplicitPin = false, bInvalidSavedPin = false;
    if (!MRFPSGloveConnection::Select(GConfig, GGameUserSettingsIni, FCommandLine::Get(), Defaults,
                                      Pin, bSaveExplicitPin, bInvalidSavedPin))
    {
        UE_LOG(LogMRFPSGloves, Error, TEXT("ContactGlove input disabled: invalid Quest relay pin"));
        return;
    }
    if (bInvalidSavedPin)
        UE_LOG(LogMRFPSGloves, Warning, TEXT("Invalid saved Quest relay pin; accepting loopback only"));
    AllowedSender = Pin.Sender;
    ReceiverPort = Pin.Port;
    PinnedDeviceId = Pin.Device;
#else
    AllowedSender = Settings->AllowedSender;
    PinnedDeviceId = Settings->DeviceId;
    FParse::Value(FCommandLine::Get(), TEXT("MRFPSGloveSender="), AllowedSender);
    FParse::Value(FCommandLine::Get(), TEXT("MRFPSGloveDevice="), PinnedDeviceId);
    ReceiverPort = Settings->ListenPort;
    FParse::Value(FCommandLine::Get(), TEXT("MRFPSGlovePort="), ReceiverPort);
#endif
    StaleAfter = FMath::Clamp(Settings->StaleAfterSeconds, 0.02f, 1.0f);
    DeviceStatusStaleAfter = FMath::Clamp(Settings->DeviceStatusStaleAfterSeconds, FMath::Max(0.2f, StaleAfter), 5.0f);
    TrackerMounts[0] = Settings->LeftTrackerToHand;
    TrackerMounts[1] = Settings->RightTrackerToHand;
    for (int32 I = 0; I < 2; ++I)
    {
        const FTransform& Mount = TrackerMounts[I];
        bMountValid[I] = !Mount.ContainsNaN() && Mount.GetScale3D().Equals(FVector::OneVector, 0.001)
            && Mount.GetTranslation().GetAbsMax() <= 100.0 && Mount.GetRotation().IsNormalized();
        if (!bMountValid[I]) UE_LOG(LogMRFPSGloves, Error, TEXT("Invalid %s TrackerToHand calibration: external positional tracking disabled for this hand"), I == 0 ? TEXT("left") : TEXT("right"));
    }
    FIPv4Address Bind;
    FIPv4Address Sender;
    if (ReceiverPort < 1024 || ReceiverPort > 65535 || !FIPv4Address::Parse(Settings->BindAddress, Bind)
        || !FIPv4Address::Parse(AllowedSender, Sender))
    {
        UE_LOG(LogMRFPSGloves, Error, TEXT("ContactGlove input disabled: invalid port or sender IPv4 address"));
        return;
    }
    Socket = FUdpSocketBuilder(TEXT("MRFPS ContactGlove OSC")).AsNonBlocking().BoundToAddress(Bind)
        .BoundToPort(uint16(ReceiverPort)).WithReceiveBufferSize(262144);
    if (!Socket)
    {
        UE_LOG(LogMRFPSGloves, Error, TEXT("Could not bind ContactGlove OSC port %d"), ReceiverPort);
        return;
    }
    if (!StartReceiver())
    {
        UE_LOG(LogMRFPSGloves, Error, TEXT("Could not start ContactGlove receiver thread"));
        Socket->Close();ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);Socket=nullptr;
        return;
    }
#if PLATFORM_ANDROID
    // Persist only an explicitly requested, validated pin after the receiver
    // starts successfully. Ordinary launches only read the previously saved pin.
    if (bSaveExplicitPin) {
        if (GConfig && !GGameUserSettingsIni.IsEmpty()) {
            MRFPSGloveConnection::Store(GConfig, GGameUserSettingsIni, Pin);
            GConfig->Flush(false, GGameUserSettingsIni);
        } else {
            UE_LOG(LogMRFPSGloves, Warning, TEXT("Quest relay pin is active for this session but settings are unavailable"));
        }
    }
#endif
    Station = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
    bool bStationValid = false;
    Station->SetIp(*Settings->StationAddress, bStationValid);
    Station->SetPort(Settings->StationPort);
    bStationConnection = Settings->bConnectToStation && bStationValid && Settings->StationPort > 0 && Settings->StationPort <= 65535;
    SetTickableTickType(ETickableTickType::Conditional);
    UE_LOG(LogMRFPSGloves, Display, TEXT("ContactGlove OSC listening on %s:%d; accepting %s"),
        *Settings->BindAddress, ReceiverPort, *AllowedSender);
}

void UMRFPSGloveSubsystem::Deinitialize()
{
    SetTickableTickType(ETickableTickType::Never);
    StopReceiver();
    if (Socket)
    {
        if (bStationConnection) SendStationControl("/DS/HC/Disconnect");
        Socket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
        Socket = nullptr;
    }
    Station.Reset();
    Super::Deinitialize();
}

void UMRFPSGloveSubsystem::BeginDestroy()
{
    SetTickableTickType(ETickableTickType::Never);
    StopReceiver();
    Super::BeginDestroy();
}

TStatId UMRFPSGloveSubsystem::GetStatId() const { RETURN_QUICK_DECLARE_CYCLE_STAT(UMRFPSGloveSubsystem, STATGROUP_Tickables); }

void UMRFPSGloveSubsystem::SendStationControl(const ANSICHAR* Address)
{
    if (!Socket || !Station.IsValid()) return;
    TArray<uint8> Packet;
    const int32 Length = FCStringAnsi::Strlen(Address);
    Packet.Append(reinterpret_cast<const uint8*>(Address), Length);
    Packet.AddZeroed(4 - Length % 4);
    const uint8 TypeTag[] = { ',', 'i', 0, 0 };
    Packet.Append(TypeTag, 4);
    Packet.Add(uint8(uint32(ReceiverPort) >> 24));
    Packet.Add(uint8(uint32(ReceiverPort) >> 16));
    Packet.Add(uint8(uint32(ReceiverPort) >> 8));
    Packet.Add(uint8(ReceiverPort));
    int32 Sent = 0;
    Socket->SendTo(Packet.GetData(), Packet.Num(), Sent, *Station);
}

void UMRFPSGloveSubsystem::Tick(float DeltaTime)
{
    if (!Socket) return;
    const double Now = FPlatformTime::Seconds();
    const double LastStatus = FMath::Max(Hands[0].DeviceStatusReceivedAt, Hands[1].DeviceStatusReceivedAt);
    if (bStationConnection && Now >= NextConnectAt && (LastStatus < 0.0 || Now - LastStatus > 0.5))
    {
        SendStationControl("/DS/HC/Connect");
        NextConnectAt = Now + 2.0;
    }
    if (Receiver) {
        TArray<FMRFPSGloveReceiver::FPacket> Captured;
        Receiver->TakePackets(Captured);
        for (const auto& Packet:Captured) ProcessPacket(Packet.Data.GetData(),Packet.Data.Num(),Packet.ReceivedAt);
        return;
    }
    // The direct reader is kept for deterministic socket tests. Runtime always
    // uses the receiver thread above, even when the render frame rate is low.
    // On headset suspension packets can accumulate in the OS socket queue.
    // Discard that backlog before accepting fresh sensor samples, so an old
    // held trigger or grip cannot become fresh merely because the app resumed.
    if (LastTickAt>=0 && (Now<LastTickAt || Now-LastTickAt>StaleAfter)) bDiscardBacklog=true;
    LastTickAt=Now;
    uint32 Pending = 0;
    uint8 Data[8192];
    const TSharedRef<FInternetAddr> Sender = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
    if (bDiscardBacklog) {
        for (int32 Count=0;Count<512 && Socket->HasPendingData(Pending);++Count) {
            int32 Read=0;
            if (!Socket->RecvFrom(Data,sizeof(Data),Read,*Sender)) break;
        }
        if (!Socket->HasPendingData(Pending)) bDiscardBacklog=false;
        return;
    }
    for (int32 Count = 0; Count < 512 && Socket->HasPendingData(Pending); ++Count)
    {
        int32 Read = 0;
        if (!Socket->RecvFrom(Data, sizeof(Data), Read, *Sender)) break;
        if (Pending > sizeof(Data) || Sender->ToString(false) != AllowedSender) continue;
        ProcessPacket(Data, Read, Now);
    }
}

void UMRFPSGloveSubsystem::ProcessPacket(const uint8* Data, int32 Size, double Now, int32 Depth)
{
    if (Depth > 4 || Size < 4 || Size > 8192 || Size % 4 != 0) return;
    if (Size >= 16 && FMemory::Memcmp(Data, "#bundle\0", 8) == 0)
    {
        int32 Cursor = 16;
        while (Cursor <= Size - 4)
        {
            const uint32 Length = MRFPSOSC::ReadU32(Data + Cursor);
            Cursor += 4;
            if (Length < 4 || Length > uint32(Size - Cursor)) return;
            ProcessPacket(Data + Cursor, int32(Length), Now, Depth + 1);
            Cursor += int32(Length);
        }
        return;
    }
    ProcessMessage(Data, Size, Now);
}

bool UMRFPSGloveSubsystem::ProcessMessage(const uint8* Data, int32 Size, double Now)
{
    MRFPSOSC::FMessage Message;
    if (!MRFPSOSC::Parse(Data, Size, Message)) return false;
    const auto& Args = Message.Arguments;
    if (Message.Address == TEXT("/DS/HC/Device"))
    {
        // DivingStation emits default Hand/Controller packets even when a glove is
        // disconnected. Only this authoritative per-hand status establishes connection.
        if (Args.Num() < 1 || Args[0].Type != TEXT('i') || Args[0].Integer != 1 || (Args.Num() - 1) % 12 != 0) return false;
        const TCHAR* UnitTypes = TEXT("siisifiifiif");
        int32 SelectedOffset = INDEX_NONE;
        int32 FirstOffset = INDEX_NONE;
        for (int32 Offset = 1; Offset < Args.Num(); Offset += 12)
        {
            for (int32 I = 0; I < 12; ++I) if (Args[Offset + I].Type != UnitTypes[I]) return false;
            if (Args[Offset].Text.IsEmpty() || (Args[Offset + 1].Integer != 0 && Args[Offset + 1].Integer != 1)
                || (Args[Offset + 6].Integer != 0 && Args[Offset + 6].Integer != 1)
                || (Args[Offset + 9].Integer != 0 && Args[Offset + 9].Integer != 1)) return false;
            const FString& Id = Args[Offset].Text;
            if (!PinnedDeviceId.IsEmpty() && Id != PinnedDeviceId) continue;
            if (!SelectedDeviceId.IsEmpty())
            {
                if (Id == SelectedDeviceId) SelectedOffset = Offset;
            }
            else
            {
                if (FirstOffset == INDEX_NONE) FirstOffset = Offset;
                if (Args[Offset + 1].Integer == 1) SelectedOffset = Offset;
            }
        }
        if (SelectedOffset == INDEX_NONE && SelectedDeviceId.IsEmpty()) SelectedOffset = FirstOffset;
        if (SelectedOffset != INDEX_NONE) SelectedDeviceId = Args[SelectedOffset].Text;
        for (int32 I = 0; I < 2; ++I)
        {
            FMRFPSGloveHandFrame& Hand = Hands[I];
            const bool bWasConnected = Hand.bConnected;
            Hand.DeviceId = SelectedDeviceId;
            Hand.DeviceStatusReceivedAt = Now;
            Hand.bDeviceStatusValid = true;
            Hand.bConnected = SelectedOffset != INDEX_NONE && Args[SelectedOffset + (I == 0 ? 6 : 9)].Integer == 1;
            Hand.BatteryPercent = SelectedOffset != INDEX_NONE ? Args[SelectedOffset + (I == 0 ? 7 : 10)].Integer : -1;
            if (Hand.bConnected && !bWasConnected)
            {
                if (I==1) {
                    RightTriggerJournal.Invalidate(MRFPSGloveTrigger::EStream::Controller,Now);
                    RightTriggerJournal.Invalidate(MRFPSGloveTrigger::EStream::Flex,Now);
                }
                // Discard pre-connection default samples and wait for real subsequent streams.
                Hand.FlexReceivedAt = Hand.ControllerReceivedAt = Hand.JointsReceivedAt = Hand.WristReceivedAt = -1.0;
                Hand.bFlexValid = Hand.bControllerValid = Hand.bJointRotationsValid = Hand.bWristImuValid = false;
            }
            if (!Hand.bConnected)
            {
                if (I==1) {
                    RightTriggerJournal.Invalidate(MRFPSGloveTrigger::EStream::Controller,Now);
                    RightTriggerJournal.Invalidate(MRFPSGloveTrigger::EStream::Flex,Now);
                }
                Hand.bFlexValid = Hand.bControllerValid = Hand.bJointRotationsValid = Hand.bWristImuValid = false;
                Hand.Trigger = Hand.Grip = 0.0f;
                Hand.bTriggerPressed = Hand.bAButtonPressed = Hand.bBButtonPressed = false;
            }
        }
        return true;
    }
    if (Args.Num() < 3 || Args[0].Type != TEXT('i') || Args[0].Integer != 1 || Args[1].Type != TEXT('s')
        || Args[1].Text.IsEmpty() || Args[2].Type != TEXT('i') || (Args[2].Integer != 0 && Args[2].Integer != 1)) return false;
    const FString& Device = Args[1].Text;
    if (!PinnedDeviceId.IsEmpty() && Device != PinnedDeviceId) return false;
    if (!SelectedDeviceId.IsEmpty() && Device != SelectedDeviceId) return false;
    const int32 HandIndex = Args[2].Integer == 1 ? 0 : 1;
    FMRFPSGloveHandFrame& Hand = Hands[HandIndex];

    if (Message.Address == TEXT("/DS/HC/Hand"))
    {
        if (!MRFPSOSC::Match(Message, TEXT("isiffffffffffffffffffff"))) return false;
        for (int32 Finger = 0; Finger < 5; ++Finger)
        {
            float Sum = 0.0f;
            for (int32 Joint = 0; Joint < 3; ++Joint)
            {
                const float Value = FMath::Clamp(Args[3 + Finger * 4 + Joint].Number, 0.0f, 1.0f);
                Hand.Flexion[Finger * 3 + Joint] = Value;
                Sum += Value;
            }
            Hand.FingerCurls[Finger] = Sum / 3.0f;
            Hand.Splay[Finger] = FMath::Clamp(Args[6 + Finger * 4].Number, -1.0f, 1.0f);
        }
        Hand.FlexReceivedAt = Now;
        Hand.bFlexValid = true;
        if (HandIndex==1) RightTriggerJournal.Sample(MRFPSGloveTrigger::EStream::Flex,Now,
            Hand.bConnected && Hand.bDeviceStatusValid && MRFPSOSC::IsFresh(Hand.DeviceStatusReceivedAt,Now,DeviceStatusStaleAfter),
            Hand.FingerCurls[1],false,StaleAfter);
    }
    else if (Message.Address == TEXT("/DS/HC/HandQuat"))
    {
        if (Args.Num() != 63) return false;
        TArray<FQuat> Joints;
        for (int32 I = 3; I < 63; I += 4)
        {
            for (int32 J = 0; J < 4; ++J) if (Args[I + J].Type != TEXT('f')) return false;
            FQuat Q(Args[I + 1].Number, Args[I + 2].Number, Args[I + 3].Number, Args[I].Number);
            if (Q.SizeSquared() < 0.5 || Q.SizeSquared() > 1.5) return false;
            Q.Normalize();
            Joints.Add(Q);
        }
        Hand.FingerLocalRotations = MoveTemp(Joints);
        Hand.JointsReceivedAt = Now;
        Hand.bJointRotationsValid = true;
    }
    else if (Message.Address == TEXT("/DS/HC/Wrist"))
    {
        if (!MRFPSOSC::Match(Message, TEXT("isiffff"))) return false;
        FQuat Q(Args[4].Number, Args[5].Number, Args[6].Number, Args[3].Number);
        if (Q.SizeSquared() < 0.5 || Q.SizeSquared() > 1.5) return false;
        Q.Normalize();
        Hand.WristImuRotation = Q;
        Hand.WristReceivedAt = Now;
        Hand.bWristImuValid = true;
    }
    else if (Message.Address == TEXT("/DS/HC/Controller"))
    {
        if (Args.Num() < 12 || Args.Num() > 16) return false;
        for (int32 I = 3; I < 9; ++I) if (Args[I].Type != TEXT('i') || Args[I].Integer < 0 || Args[I].Integer > 2) return false;
        for (int32 I = 9; I < Args.Num(); ++I) if (Args[I].Type != TEXT('f')) return false;
        Hand.bAButtonPressed = Args[3].Integer == 2;
        Hand.bBButtonPressed = Args[4].Integer == 2;
        Hand.bTriggerPressed = Args[6].Integer == 2;
        Hand.Trigger = FMath::Clamp(Args[9].Number, 0.0f, 1.0f);
        Hand.Grip = FMath::Clamp(Args[10].Number, 0.0f, 1.0f);
        Hand.ControllerReceivedAt = Now;
        Hand.bControllerValid = true;
        if (HandIndex==1) RightTriggerJournal.Sample(MRFPSGloveTrigger::EStream::Controller,Now,
            Hand.bConnected && Hand.bDeviceStatusValid && MRFPSOSC::IsFresh(Hand.DeviceStatusReceivedAt,Now,DeviceStatusStaleAfter),
            Hand.Trigger,Hand.bTriggerPressed,StaleAfter);
    }
    else if (Message.Address == TEXT("/MRFPS/Tracker"))
    {
        if (!MRFPSOSC::Match(Message, TEXT("isifffffffi"))) return false;
        if (Args[10].Integer != 0 && Args[10].Integer != 1) return false;
        const FVector Position(Args[3].Number, Args[4].Number, Args[5].Number);
        FQuat Rotation(Args[6].Number, Args[7].Number, Args[8].Number, Args[9].Number);
        if (Position.GetAbsMax() > 100000.0 || Rotation.SizeSquared() < 0.5 || Rotation.SizeSquared() > 1.5) return false;
        Rotation.Normalize();
        // UE composition: local child transform * parent transform.
        // A mount offset never establishes tracking; only the live tracked flag does.
        Hand.TrackingTransform = TrackerMounts[HandIndex] * FTransform(Rotation, Position);
        Hand.PoseReceivedAt = Now;
        Hand.bPoseValid = Args[10].Integer == 1 && bMountValid[HandIndex];
    }
    else return false;

    SelectedDeviceId = Device;
    Hand.DeviceId = Device;
    return true;
}

bool UMRFPSGloveSubsystem::GetLatestHand(EMRFPSGloveHand Hand, FMRFPSGloveHandFrame& Frame) const
{
    const int32 HandIndex = Hand == EMRFPSGloveHand::Left ? 0 : 1;
    Frame = Hands[HandIndex];
    const double Now = FPlatformTime::Seconds();
    const bool bReportedConnected = Frame.bConnected;
    Frame.bDeviceStatusValid &= MRFPSOSC::IsFresh(Frame.DeviceStatusReceivedAt, Now, DeviceStatusStaleAfter);
    Frame.bConnected &= Frame.bDeviceStatusValid;
    Frame.bFlexStreamFresh = MRFPSOSC::IsFresh(Frame.FlexReceivedAt, Now, StaleAfter);
    const auto IsValidSample = [&](double ReceivedAt)
    {
        return MRFPSOSC::IsConnectedSample(Frame.bConnected && Frame.bDeviceStatusValid, Frame.DeviceStatusReceivedAt, ReceivedAt, Now, DeviceStatusStaleAfter, StaleAfter);
    };
    Frame.bFlexValid &= IsValidSample(Frame.FlexReceivedAt);
    Frame.bPoseValid &= MRFPSOSC::IsFresh(Frame.PoseReceivedAt, Now, StaleAfter);
    Frame.bControllerValid &= IsValidSample(Frame.ControllerReceivedAt);
    Frame.bJointRotationsValid &= IsValidSample(Frame.JointsReceivedAt);
    Frame.bWristImuValid &= IsValidSample(Frame.WristReceivedAt);
    Frame.FlexAgeSeconds = Frame.FlexReceivedAt >= 0.0 ? float(Now - Frame.FlexReceivedAt) : TNumericLimits<float>::Max();
    const bool bControllerStreamFresh = MRFPSOSC::IsFresh(Frame.ControllerReceivedAt, Now, StaleAfter);
    const uint8 Diagnostic = (Frame.bDeviceStatusValid ? 1 : 0) | (bReportedConnected ? 2 : 0)
        | (Frame.bFlexStreamFresh ? 4 : 0) | (Frame.bFlexValid ? 8 : 0)
        | (bControllerStreamFresh ? 16 : 0) | (Frame.bControllerValid ? 32 : 0);
    if (LastFreshnessDiagnostic[HandIndex] != Diagnostic)
    {
        LastFreshnessDiagnostic[HandIndex] = Diagnostic;
        const auto AgeMs = [Now](double ReceivedAt) { return ReceivedAt >= 0.0 ? (Now - ReceivedAt) * 1000.0 : -1.0; };
        UE_LOG(LogMRFPSGloves, Display, TEXT("MRFPS_GLOVE_FRESH hand=%s status_fresh=%d reported_connected=%d flex_stream_fresh=%d flex_valid=%d controller_stream_fresh=%d controller_valid=%d status_age_ms=%.1f flex_age_ms=%.1f controller_age_ms=%.1f"),
            HandIndex == 0 ? TEXT("left") : TEXT("right"), int32(Frame.bDeviceStatusValid), int32(bReportedConnected),
            int32(Frame.bFlexStreamFresh), int32(Frame.bFlexValid), int32(bControllerStreamFresh), int32(Frame.bControllerValid),
            AgeMs(Frame.DeviceStatusReceivedAt), AgeMs(Frame.FlexReceivedAt), AgeMs(Frame.ControllerReceivedAt));
    }
    if (!Frame.bControllerValid)
    {
        Frame.Trigger = Frame.Grip = 0.0f;
        Frame.bTriggerPressed = Frame.bAButtonPressed = Frame.bBButtonPressed = false;
    }
    return Frame.bFlexValid;
}

bool UMRFPSGloveSubsystem::IsReceiving() const
{
    FMRFPSGloveHandFrame Frame;
    return GetLatestHand(EMRFPSGloveHand::Left, Frame) || GetLatestHand(EMRFPSGloveHand::Right, Frame);
}

void UMRFPSGloveSubsystem::SubmitTrackedPose(EMRFPSGloveHand Hand, const FTransform& TrackingPose, bool bTracked)
{
    FMRFPSGloveHandFrame& Frame = Hands[Hand == EMRFPSGloveHand::Left ? 0 : 1];
    Frame.bPoseValid = bTracked && !TrackingPose.ContainsNaN();
    if (Frame.bPoseValid)
    {
        Frame.TrackingTransform = TrackingPose;
        Frame.PoseReceivedAt = FPlatformTime::Seconds();
    }
}
