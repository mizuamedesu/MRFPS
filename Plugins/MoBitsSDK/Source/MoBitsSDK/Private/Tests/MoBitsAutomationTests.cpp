#include "MoBitsClient.h"
#include "MoBitsConnectionComponent.h"
#include "MoBitsDiscovery.h"
#include "MoBitsCore.h"
#include "MoBitsProtocol.h"
#include "MoBitsWire.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoBitsContractsTest, "MoBits.SDK.Contracts", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoBitsContractsTest::RunTest(const FString& Parameters)
{
    for (int32 Effect = 1; Effect <= 30; ++Effect) TestTrue(TEXT("numbered effect"), MoBitsCore::ValidPlayback(Effect, .2, 200));
    TestFalse(TEXT("effect zero"), MoBitsCore::ValidPlayback(0,.2,200));
    TestEqual(TEXT("Tap wire ID"), UMoBitsConnectionComponent::GetHapticEffectId(EMoBitsHapticEffect::Tap), 1);
    TestEqual(TEXT("Nudge wire ID"), UMoBitsConnectionComponent::GetHapticEffectId(EMoBitsHapticEffect::Nudge), 2);
    TestEqual(TEXT("Impact wire ID"), UMoBitsConnectionComponent::GetHapticEffectId(EMoBitsHapticEffect::Impact), 3);
    TestEqual(TEXT("None is invalid"), UMoBitsConnectionComponent::GetHapticEffectId(EMoBitsHapticEffect::None), 0);
    const UEnum* EffectEnum = StaticEnum<EMoBitsHapticEffect>();
    for (int32 Effect = 4; Effect <= 30; ++Effect)
        TestEqual(TEXT("unnamed slot keeps its ID"), EffectEnum->GetNameStringByValue(Effect), FString::Printf(TEXT("Effect%02d"), Effect));
    TestEqual(TEXT("name 1"), UMoBitsConnectionComponent::GetHapticEffectName(1), FString(TEXT("Tap")));
    TestEqual(TEXT("name 4"), UMoBitsConnectionComponent::GetHapticEffectName(4), FString(TEXT("Effect04")));
    TestTrue(TEXT("name 31"), UMoBitsConnectionComponent::GetHapticEffectName(31).IsEmpty());
    for (int32 Effect = 1; Effect <= 3; ++Effect)
        TestEqual(TEXT("enum name matches table"), EffectEnum->GetNameStringByValue(Effect), UMoBitsConnectionComponent::GetHapticEffectName(Effect));
    const UEnum* MrFpsEnum = StaticEnum<EMoBitsMrFpsPreset>();
    const TCHAR* MrFpsNames[] = {TEXT("PistolRecoil"), TEXT("HeavyRecoil"), TEXT("AutomaticKick"), TEXT("Graze"), TEXT("Hit"), TEXT("HeavyHit"), TEXT("LethalHit"), TEXT("Revive"), TEXT("AutomaticVibration"), TEXT("AutomaticShot")};
    for (int32 Index = 0; Index < 10; ++Index)
    {
        TestEqual(TEXT("MR FPS reflection matches the app registration"), MrFpsEnum->GetNameStringByValue(Index + 4), FString(MrFpsNames[Index]));
        TestEqual(TEXT("MR FPS wire ID"), UMoBitsConnectionComponent::GetMrFpsPresetId(static_cast<EMoBitsMrFpsPreset>(Index + 4)), Index + 4);
    }
    TestEqual(TEXT("Invalid MR FPS enum is refused"), UMoBitsConnectionComponent::GetMrFpsPresetId(static_cast<EMoBitsMrFpsPreset>(30)), 0);
    TestFalse(TEXT("TTL above 500"), MoBitsCore::ValidPlayback(1,.2,501));
    FMoBitsSessionOptions Session;
    const FString Token = FString::ChrN(64,TEXT('a'));
    TestTrue(TEXT("loopback credentials"), MoBitsProtocol::Session(FString::Printf(TEXT("{\"control_url\":\"ws://127.0.0.1:40000/mobits/v2/control\",\"motion_url\":\"ws://127.0.0.1:40000/mobits/v2/motion\",\"token\":\"%s\"}"), *Token),Session));
    TestFalse(TEXT("remote credentials"), MoBitsProtocol::Session(FString::Printf(TEXT("{\"control_url\":\"ws://example.com:40000/mobits/v2/control\",\"motion_url\":\"ws://example.com:40000/mobits/v2/motion\",\"token\":\"%s\"}"), *Token),Session));
    const FString Id = TEXT("12345678-1234-1234-1234-123456789abc");
    const FString Descriptor = FString::Printf(TEXT("{\"discovery_version\":1,\"protocol_version\":2,\"app_version\":\"0.1.12\",\"session_id\":\"%s\",\"control_url\":\"ws://127.0.0.1:40000/mobits/v2/control\",\"motion_url\":\"ws://127.0.0.1:40000/mobits/v2/motion\",\"token\":\"%s\"}"), *Id, *Token);
    TestTrue(TEXT("local discovery descriptor"), MoBitsProtocol::Discovery(Descriptor, Session));
    TestEqual(TEXT("discovery pins session identity"), Session.ExpectedSessionId, Id);
    TestFalse(TEXT("incompatible discovery"), MoBitsProtocol::Discovery(Descriptor.Replace(TEXT("\"discovery_version\":1"),TEXT("\"discovery_version\":2")),Session));
    TestFalse(TEXT("incompatible protocol"), MoBitsProtocol::Discovery(Descriptor.Replace(TEXT("\"protocol_version\":2"),TEXT("\"protocol_version\":99")),Session));
    TestFalse(TEXT("nonlocal discovery"), MoBitsProtocol::Discovery(Descriptor.Replace(TEXT("127.0.0.1"),TEXT("example.com")),Session));
    TestFalse(TEXT("invalid discovery identity"), MoBitsProtocol::Discovery(Descriptor.Replace(*Id,TEXT("old")),Session));
    TestFalse(TEXT("missing discovery fields"), MoBitsProtocol::Discovery(TEXT("{}"),Session));
    const FString FilePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MoBitsDiscoveryTest.json"));
    FString FileJson, FileError;
    TestTrue(TEXT("save isolated discovery fixture"), FFileHelper::SaveStringToFile(Descriptor,*FilePath));
    TestTrue(TEXT("read discovery file"), MoBitsDiscovery::ReadFile(FilePath,FileJson,FileError));
    TestTrue(TEXT("loaded discovery validates"), MoBitsProtocol::Discovery(FileJson,Session));
    FFileHelper::SaveStringToFile(FString::ChrN(4097,TEXT('x')),*FilePath);
    TestFalse(TEXT("oversized discovery rejected"), MoBitsDiscovery::ReadFile(FilePath,FileJson,FileError));
    IFileManager::Get().Delete(*FilePath);
    TestFalse(TEXT("missing app is actionable"), MoBitsDiscovery::ReadFile(FilePath,FileJson,FileError));
    TestTrue(TEXT("missing app diagnostic"), FileError.Contains(TEXT("Launch MoBits")));
    const double S = FMath::Sqrt(.5);
    const auto R = MoBitsCore::Rotation({{0,S,0,S}});
    const FVector Forward = FQuat(R[0],R[1],R[2],R[3]).RotateVector(FVector::ForwardVector);
    TestTrue(TEXT("Unity Y rotation becomes UE Z rotation"), Forward.Equals(FVector::RightVector,1e-5));
    MoBitsCore::OutputGate Gate; const auto Epoch = Gate.Epoch(); Gate.Stop(); Gate.ArmReply(Epoch,true);
    TestFalse(TEXT("late ARM cannot undo STOP"), Gate.Armed());
    TSharedPtr<FJsonObject> O;
    TestTrue(TEXT("JSON parses"), MoBitsProtocol::Parse(TEXT("{\"id\":1,\"status\":\"effect_unconfigured\",\"reason\":\"\",\"device_id\":\"\",\"applied_level\":0}"),O));
    FMoBitsReceipt Receipt;
    TestTrue(TEXT("unconfigured receipt"), MoBitsProtocol::Receipt(O,Receipt));
    TestFalse(TEXT("unconfigured is not success"), Receipt.bSucceeded);
    O->SetStringField(TEXT("id"),TEXT("1")); TestFalse(TEXT("string ID rejected"), MoBitsProtocol::Receipt(O,Receipt));
    return true;
}

// Activated only by an explicitly invoked automation test and a test-session
// environment variable supplied by the app's FakeBackend integration runner.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoBitsInteropTest, "MoBits.SDK.RustInterop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoBitsInteropTest::RunTest(const FString& Parameters)
{
    FString Path = FPlatformMisc::GetEnvironmentVariable(TEXT("MOBITS_UE_TEST_DISCOVERY"));
    if (Path.IsEmpty()) { AddInfo(TEXT("RustInterop requires the FakeBackend runner; no connection opened.")); return true; }
    FMoBitsClient Client; FString Error;
    FString Session;
    if (!TestTrue(TEXT("discover app"),MoBitsDiscovery::ReadFile(Path,Session,Error))) return false;
    if (!TestTrue(TEXT("connect discovered session"),Client.Connect(Session,Error,true))) return false;
    Session.Empty();
    TArray<FMoBitsClient::FEvent> Captured;
    auto Pump = [&]()
    {
        Client.Tick(); TArray<FMoBitsClient::FEvent> E; Client.Drain(E);
        for (const auto& Event : E)
            if (Event.Kind == FMoBitsClient::EEventKind::Error) AddInfo(TEXT("SDK diagnostic: ") + Event.Error);
        Captured.Append(MoveTemp(E));
        FPlatformProcess::SleepNoStats(.001f);
    };
    auto Wait = [&](TFunction<bool()> Done)
    {
        const double Deadline = FPlatformTime::Seconds() + 5;
        while (!Done() && FPlatformTime::Seconds() < Deadline && Client.GetState() != EMoBitsConnectionState::Faulted) Pump();
        return Done();
    };
    auto Result = [&](int64 Id, const FString& Expected)
    {
        if (Id == 0) return false;
        return Wait([&]()
        {
            for (const auto& E : Captured)
                if (E.Kind == FMoBitsClient::EEventKind::Receipt && E.Receipt.Id == Id) return E.Receipt.Status == Expected;
            return false;
        });
    };
    FMoBitsMotionFrame Frame;
    if (!TestTrue(TEXT("two HELLO channels and fresh frame"),Wait([&]() { return Client.Frame(Frame); }))) return false;
    TestFalse(TEXT("connection does not arm"),Client.Armed());
    FMoBitsDevicePose Pose;
    TestTrue(TEXT("device pose"), Client.DevicePose(TEXT("mock-device-001"),Pose));
    TestTrue(TEXT("UE position conversion"),Pose.PositionCm.Equals(FVector(-300,100,200),1e-4));
    TestEqual(TEXT("20 bones"),Frame.Bones.Num(),20);
    const int64 DevicesId = Client.Devices();
    TestTrue(TEXT("GET_DEVICES"),Wait([&]()
    {
        for (const auto& E : Captured) if (E.Kind == FMoBitsClient::EEventKind::Devices) return E.Devices.Num() == 1 && E.Devices[0].BoneId == 14;
        return false;
    }));
    TestTrue(TEXT("positive device request"), DevicesId > 0);
    TestTrue(TEXT("explicit ARM confirmed"),Result(Client.Arm(),TEXT("armed")));
    TestTrue(TEXT("local effect acknowledged"),Result(Client.Play(14,1,.2f,200),TEXT("device_acknowledged")));
    for (int32 Effect = 1; Effect <= 30; ++Effect)
    {
        TestTrue(FString::Printf(TEXT("global slot %d"),Effect),Result(Client.PlayGlobal(Effect,.2f,200),TEXT("effect_unconfigured")));
        TestTrue(TEXT("unconfigured global retains arm"),Client.Armed());
    }
    TestEqual(TEXT("invalid effect rejected locally"),Client.PlayGlobal(31,.2f,200),static_cast<int64>(0));
    TestTrue(TEXT("STOP confirmed"),Result(Client.Stop(),TEXT("stopped_confirmed")));
    TestFalse(TEXT("STOP clears arm"),Client.Armed());
    const int64 ArmId = Client.Arm(), StopId = Client.Stop();
    TestTrue(TEXT("ARM and priority STOP submitted"),ArmId > 0 && StopId > ArmId);
    TestTrue(TEXT("priority STOP confirmed"),Result(StopId,TEXT("stopped_confirmed")));
    TestFalse(TEXT("late ARM does not re-arm"),Client.Armed());
    // After all previous replies have arrived, explicitly arm once again.
    TestTrue(TEXT("pending ARM completes"),Wait([&]()
    { for (const auto& E : Captured) if (E.Kind == FMoBitsClient::EEventKind::Receipt && E.Receipt.Id == ArmId) return true; return false; }));
    TestTrue(TEXT("re-arm explicitly"),Result(Client.Arm(),TEXT("armed")));
    if (FPlatformMisc::GetEnvironmentVariable(TEXT("MOBITS_UE_QUEUE_PRESSURE")) == TEXT("1"))
    {
        TArray<FMoBitsClient::FEvent> Previous; Client.Drain(Previous);
        // Keep game/network progress active while the notification consumer falls behind.
        for (int32 I = 0; I < 55; ++I)
        {
            FPlatformProcess::SleepNoStats(.02f); Client.Tick();
            if (!TestEqual(TEXT("transport stays healthy before pressure injection"), Client.GetState(), EMoBitsConnectionState::Connected)) return false;
        }
        for (int32 I = 0; I < 17; ++I) Client.Devices();
        TestEqual(TEXT("saturated pending requests fault cleanly"), Client.GetState(), EMoBitsConnectionState::Faulted);
        TArray<FMoBitsClient::FEvent> Ended; Client.Drain(Ended);
        int32 Completed = 0;
        for (const auto& E : Ended)
        {
            TestFalse(TEXT("no stale motion after pressure fault"), E.Kind == FMoBitsClient::EEventKind::Motion);
            if (E.Kind == FMoBitsClient::EEventKind::Receipt && E.Receipt.Status == TEXT("rejected")) ++Completed;
        }
        TestEqual(TEXT("every pending caller receives a terminal result"), Completed, 16);
        Client.ForceClose(); AddInfo(TEXT("MOBITS_UE_RUST_INTEROP_COMPLETED")); return true;
    }
    FPlatformProcess::SleepNoStats(.025f); Client.Tick(); // deliberately leave one motion callback queued
    FPlatformProcess::SleepNoStats(.65f);
    TestFalse(TEXT("stale data immediately invalidates output permission before the next tick"), Client.Armed());
    Client.Tick();
    TestEqual(TEXT("game stall faults session"),Client.GetState(),EMoBitsConnectionState::Faulted);
    TestFalse(TEXT("stall clears arm"),Client.Armed());
    TArray<FMoBitsClient::FEvent> Ended; Client.Drain(Ended);
    for (const auto& E : Ended)
        TestFalse(TEXT("fault discards queued motion/device callbacks"), E.Kind == FMoBitsClient::EEventKind::Motion || E.Kind == FMoBitsClient::EEventKind::Devices);
    TestFalse(TEXT("stale pose unavailable"),Client.DevicePose(TEXT("mock-device-001"),Pose));
    AddInfo(TEXT("MOBITS_UE_RUST_INTEROP_COMPLETED"));
    Client.ForceClose();
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoBitsRecoveryTest, "MoBits.SDK.RecoveryInterop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoBitsRecoveryTest::RunTest(const FString& Parameters)
{
    const FString Path = FPlatformMisc::GetEnvironmentVariable(TEXT("MOBITS_RECOVERY_DISCOVERY"));
    const FString Phase = FPlatformMisc::GetEnvironmentVariable(TEXT("MOBITS_RECOVERY_PHASE"));
    if (Path.IsEmpty() || Phase.IsEmpty()) { AddInfo(TEXT("RecoveryInterop requires the isolated Rust FakeBackend runner.")); return true; }
    FMoBitsClient Client; FString Error;
    TArray<FMoBitsClient::FEvent> Captured;
    auto Pump = [&]() {
        Client.Tick(); TArray<FMoBitsClient::FEvent> Events; Client.Drain(Events);
        for (auto& E : Events) if (E.Kind == FMoBitsClient::EEventKind::Receipt || E.Kind == FMoBitsClient::EEventKind::Devices) Captured.Add(MoveTemp(E));
        FPlatformProcess::SleepNoStats(.005f);
    };
    auto Wait = [&](TFunction<bool()> Done) {
        const double End = FPlatformTime::Seconds() + 25;
        while (!Done() && FPlatformTime::Seconds() < End) Pump();
        return Done();
    };
    auto Mark = [&](const FString& Value) { return FFileHelper::SaveStringToFile(Value, *Phase); };
    auto Server = [&](const FString& Value) { return Wait([&]() { FString Reply; return FFileHelper::LoadFileToString(Reply, *(Phase + TEXT(".server"))) && Reply == Value; }); };
    FMoBitsMotionFrame Frame;
    auto Fresh = [&]() { return Client.Frame(Frame) && Frame.Devices.Num() == 1; };
    auto Result = [&](int64 Id, const TCHAR* Expected) {
        const uint64 Generation = Client.GetGeneration();
        return Id > 0 && Wait([&]() { for (const auto& E : Captured) if (E.Generation == Generation && E.Kind == FMoBitsClient::EEventKind::Receipt && E.Receipt.Id == Id) return E.Receipt.Status == Expected; return false; });
    };
    if (!TestFalse(TEXT("app absent at startup"), Client.ConnectLocal(Error,Path)) || !TestTrue(TEXT("startup schedules recovery"), Client.IsReconnecting())) return false;
    Mark(TEXT("waiting"));
    if (!TestTrue(TEXT("app appears and fresh data recovers"), Wait(Fresh))) return false;
    TestFalse(TEXT("startup does not arm"), Client.Armed());
    if (!TestTrue(TEXT("initial explicit ARM"), Result(Client.Arm(),TEXT("armed"))) || !TestTrue(TEXT("initial explicit PLAY"), Result(Client.Play(14,1,.2f,200),TEXT("device_acknowledged")))) return false;
    uint64 Generation = Client.GetGeneration();
    Mark(TEXT("initial-output")); if (!Server(TEXT("block-ready"))) return false;
    Captured.Reset();
    const int64 Lost = Client.Play(14,2,.2f,200);
    Mark(TEXT("pending"));
    if (!TestTrue(TEXT("lost receipt faults within command deadline"), Lost > 0 && Wait([&]() { return Client.GetState() == EMoBitsConnectionState::Faulted; }))) return false;
    TestFalse(TEXT("lost receipt disarms"), Client.Armed());
    TestFalse(TEXT("old pose cleared"), Client.Frame(Frame));
    Mark(TEXT("receipt-timeout"));
    if (!TestTrue(TEXT("lost receipt connection recovers"), Wait([&]() { return Client.GetGeneration() > Generation && Fresh(); }))) return false;
    TestFalse(TEXT("receipt recovery does not arm"), Client.Armed());
    TestEqual(TEXT("PLAY blocked until explicit re-arm"), Client.Play(14,1,.2f,200),static_cast<int64>(0));
    for (int32 I=1; I<=3; ++I) {
        Generation = Client.GetGeneration(); Captured.Reset();
        Mark(FString::Printf(TEXT("cut-%d"),I));
        if (I==1) {
            Client.Suspend();
            const double PauseEnd=FPlatformTime::Seconds()+.3; while (FPlatformTime::Seconds()<PauseEnd) Pump();
            TestEqual(TEXT("pause inhibits retries"),Client.GetGeneration(),Generation);
            Client.Resume();
        }
        if (!TestTrue(TEXT("rotated session recovers fresh tracking"), Wait([&]() { return Client.GetGeneration() > Generation && Fresh(); }))) return false;
        TestFalse(TEXT("restart never arms"), Client.Armed());
        Mark(FString::Printf(TEXT("recovered-%d"),I)); if (!Server(FString::Printf(TEXT("continue-%d"),I))) return false;
    }
    Captured.Reset();
    if (!TestTrue(TEXT("explicit ARM after restart"), Result(Client.Arm(),TEXT("armed"))) || !TestTrue(TEXT("explicit output after restart"), Result(Client.Play(14,1,.2f,200),TEXT("device_acknowledged")))) return false;
    Generation = Client.GetGeneration();
    Mark(TEXT("stop-failure")); if (!Server(TEXT("stop-block-ready"))) return false;
    if (!Wait([&]() { return Client.GetState() == EMoBitsConnectionState::Faulted; })) return false;
    const double End = FPlatformTime::Seconds() + 1.2; while (FPlatformTime::Seconds() < End) Pump();
    TestTrue(TEXT("unconfirmed OFF keeps connection unavailable"), Client.GetState() != EMoBitsConnectionState::Connected);
    TestFalse(TEXT("unconfirmed OFF clears output"),Client.Armed());
    Mark(TEXT("stop-blocked"));
    if (!TestTrue(TEXT("confirmed OFF retry permits recovery"),Wait([&]() { return Client.GetGeneration() > Generation && Fresh(); }))) return false;
    TestFalse(TEXT("OFF retry never re-arms"),Client.Armed());
    Captured.Reset();
    if (!TestTrue(TEXT("explicit re-arm after OFF"),Result(Client.Arm(),TEXT("armed"))) || !TestTrue(TEXT("explicit output after OFF"),Result(Client.Play(14,1,.2f,200),TEXT("device_acknowledged")))) return false;
    Client.Disconnect(); if (!TestTrue(TEXT("manual disconnect finishes"),Wait([&]() { return Client.GetState() == EMoBitsConnectionState::Disconnected; }))) return false;
    Generation = Client.GetGeneration();
    const double ManualEnd = FPlatformTime::Seconds() + 2.4; while (FPlatformTime::Seconds() < ManualEnd) Pump();
    TestEqual(TEXT("disconnect inhibits retry"),Client.GetGeneration(),Generation);
    TestFalse(TEXT("no retry after disconnect"),Client.IsReconnecting());
    Mark(TEXT("complete")); AddInfo(TEXT("MOBITS_SDK_RECOVERY_PASSED")); return true;
}

#endif
