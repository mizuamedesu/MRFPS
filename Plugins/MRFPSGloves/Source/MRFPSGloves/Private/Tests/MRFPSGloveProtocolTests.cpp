#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "MRFPSOSC.h"
#include "MRFPSGloveSubsystem.h"
#include "MRFPSGloveConnection.h"
#include "Engine/GameInstance.h"
#include "Common/UdpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "SocketSubsystem.h"
#include "HAL/PlatformProcess.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSGloveOSCValidationTest, "MRFPS.Gloves.OSCValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMRFPSGloveOSCValidationTest::RunTest(const FString& Parameters)
{
    FMRFPSGloveHandFrame OpenHand;
    OpenHand.FingerCurls={.1818f,.1218f,.0734f,.0993f,.3730f};
    TestFalse(TEXT("Stale fingers cannot activate a shield"),OpenHand.IsOpenPalm());
    OpenHand.bFlexValid=true;
    TestTrue(TEXT("Measured open hand tolerates a slightly bent little finger"),OpenHand.IsOpenPalm());
    OpenHand.FingerCurls={.7f,.8f,.8f,.8f,.8f};
    TestFalse(TEXT("A fist is not an open palm"),OpenHand.IsOpenPalm());
    // /a, types ,if: 1 and 1.0f encoded in network byte order.
    const uint8 Valid[] = { '/', 'a', 0, 0, ',', 'i', 'f', 0, 0, 0, 0, 1, 0x3f, 0x80, 0, 0 };
    MRFPSOSC::FMessage Message;
    TestTrue(TEXT("Valid OSC packet"), MRFPSOSC::Parse(Valid, sizeof(Valid), Message));
    TestEqual(TEXT("Integer byte order"), Message.Arguments[0].Integer, 1);
    TestEqual(TEXT("Float byte order"), Message.Arguments[1].Number, 1.0f);
    TestTrue(TEXT("Typed schema"), MRFPSOSC::Match(Message, TEXT("if")));
    TestFalse(TEXT("Wrong typed schema"), MRFPSOSC::Match(Message, TEXT("fi")));
    TestFalse(TEXT("Truncated packet"), MRFPSOSC::Parse(Valid, sizeof(Valid) - 1, Message));
    uint8 NonFinite[sizeof(Valid)];
    FMemory::Memcpy(NonFinite, Valid, sizeof(Valid));
    NonFinite[12] = 0x7f;
    NonFinite[13] = 0xc0;
    TestFalse(TEXT("NaN cannot become tracked input"), MRFPSOSC::Parse(NonFinite, sizeof(NonFinite), Message));
    TestFalse(TEXT("Never-received data is invalid"), MRFPSOSC::IsFresh(-1.0, 10.0, 0.15));
    TestFalse(TEXT("Stale data is invalid"), MRFPSOSC::IsFresh(1.0, 1.2, 0.15));
    TestFalse(TEXT("Future receipt is invalid"), MRFPSOSC::IsFresh(1.0, 0.9, 0.15));
    TestTrue(TEXT("Fresh data"), MRFPSOSC::IsFresh(1.0, 1.1, 0.15));
    TestFalse(TEXT("Fresh default samples from a disconnected glove are invalid"), MRFPSOSC::IsConnectedSample(false, 1.0, 1.0, 1.1, 1.0, 0.15));
    TestFalse(TEXT("Unknown connection state cannot validate hand data"), MRFPSOSC::IsConnectedSample(true, -1.0, 1.0, 1.1, 1.0, 0.15));
    TestFalse(TEXT("A continuing hand stream cannot refresh a dead connection heartbeat"), MRFPSOSC::IsConnectedSample(true, 0.0, 1.0, 1.1, 1.0, 0.15));
    TestTrue(TEXT("Fresh sample survives a slower connection heartbeat"), MRFPSOSC::IsConnectedSample(true, 0.8, 1.0, 1.1, 1.0, 0.15));
    TestFalse(TEXT("A stale hand sample remains invalid while status is fresh"), MRFPSOSC::IsConnectedSample(true, 1.0, 0.8, 1.1, 1.0, 0.15));
    TestTrue(TEXT("Fresh connected hand data is valid"), MRFPSOSC::IsConnectedSample(true, 1.0, 1.0, 1.1, 1.0, 0.15));
    const FTransform Mount(FQuat::Identity, FVector(10, 0, 0));
    const FTransform Tracker(FQuat(FRotator(0, 90, 0)), FVector(100, 0, 0));
    const FTransform Hand = Mount * Tracker;
    TestTrue(TEXT("Mount offset rotates in tracker local space before translation"), Hand.GetLocation().Equals(FVector(100, 10, 0), 0.001));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSQuestGlovePinTest, "MRFPS.Gloves.QuestRelayPin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMRFPSQuestGlovePinTest::RunTest(const FString& Parameters)
{
    using namespace MRFPSGloveConnection;
    const FString Filename=TEXT("MRFPSQuestRelayPinAutomation.ini");
    FConfigCacheIni Config(EConfigCacheType::Temporary);
    Config.DisableFileOperations(); Config.Add(Filename,FConfigFile());
    const FPin Defaults{TEXT("127.0.0.1"),25788,TEXT("configured")};
    FPin Result; bool bSave=false, bBadSaved=false;
    TestTrue(TEXT("Unconfigured launch uses default"),Select(&Config,Filename,TEXT(""),Defaults,Result,bSave,bBadSaved));
    TestEqual(TEXT("Default sender"),Result.Sender,FString(TEXT("127.0.0.1")));
    TestFalse(TEXT("Default launch does not save"),bSave);
    TestTrue(TEXT("Explicit valid pin"),Select(&Config,Filename,TEXT("-MRFPSGloveSender=192.168.1.10 -MRFPSGlovePort=25789 -MRFPSGloveDevice=CG2-1"),Defaults,Result,bSave,bBadSaved));
    TestTrue(TEXT("Explicit pin will be saved after receiver startup"),bSave);
    Store(&Config,Filename,Result);
    TestTrue(TEXT("Next ordinary launch loads pin"),Select(&Config,Filename,TEXT(""),Defaults,Result,bSave,bBadSaved));
    TestEqual(TEXT("Loaded sender"),Result.Sender,FString(TEXT("192.168.1.10")));
    TestEqual(TEXT("Loaded port"),Result.Port,25789);
    TestEqual(TEXT("Loaded device"),Result.Device,FString(TEXT("CG2-1")));
    TestFalse(TEXT("Ordinary launch does not overwrite pin"),bSave);
    TestTrue(TEXT("Explicit new sender overrides entire old tuple"),Select(&Config,Filename,TEXT("-MRFPSGloveSender=192.168.1.11"),Defaults,Result,bSave,bBadSaved));
    TestEqual(TEXT("Default port applies to the new sender"),Result.Port,25788);
    TestTrue(TEXT("Old device is cleared"),Result.Device.IsEmpty());
    Store(&Config,Filename,Result);
    TestTrue(TEXT("Empty device survives round trip"),Select(&Config,Filename,TEXT(""),Defaults,Result,bSave,bBadSaved));
    TestTrue(TEXT("Empty device remains empty"),Result.Device.IsEmpty());
    for (const TCHAR* Bad : {TEXT("v1|0.0.0.0|25788|*"),TEXT("v1|255.255.255.255|25788|*"),
                             TEXT("v1|224.0.0.1|25788|*"),TEXT("v1|192.168.1.10|25788x|*"),
                             TEXT("v1|192.168.1.10|25788|has space"),TEXT("v1|192.168.1.10|25788")}) {
        Config.SetString(Section,Key,Bad,Filename);
        TestTrue(TEXT("Invalid saved value falls back safely"),Select(&Config,Filename,TEXT(""),Defaults,Result,bSave,bBadSaved));
        TestTrue(TEXT("Invalid saved value is reported"),bBadSaved);
        TestEqual(TEXT("Invalid saved value allows loopback only"),Result.Sender,FString(TEXT("127.0.0.1")));
    }
    TestFalse(TEXT("Invalid explicit IP disables input"),Select(&Config,Filename,TEXT("-MRFPSGloveSender=0.0.0.0"),Defaults,Result,bSave,bBadSaved));
    TestFalse(TEXT("Malformed explicit port disables input"),Select(&Config,Filename,TEXT("-MRFPSGloveSender=192.168.1.10 -MRFPSGlovePort=25788x"),Defaults,Result,bSave,bBadSaved));
    TestFalse(TEXT("Port alone cannot modify a saved tuple"),Select(&Config,Filename,TEXT("-MRFPSGlovePort=25789"),Defaults,Result,bSave,bBadSaved));
    TestFalse(TEXT("Device alone cannot modify a saved tuple"),Select(&Config,Filename,TEXT("-MRFPSGloveDevice=CG2-1"),Defaults,Result,bSave,bBadSaved));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSGloveBacklogTest,"MRFPS.Gloves.ResumeDiscardsOldInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSGloveBacklogTest::RunTest(const FString& Parameters)
{
    auto* Instance=NewObject<UGameInstance>();
    auto* Gloves=NewObject<UMRFPSGloveSubsystem>(Instance);
    FSocket* Receiver=FUdpSocketBuilder(TEXT("GloveBacklogTest")).AsNonBlocking().BoundToAddress(FIPv4Address::InternalLoopback).BoundToPort(0);
    FSocket* Sender=FUdpSocketBuilder(TEXT("GloveBacklogSender")).AsNonBlocking().BoundToAddress(FIPv4Address::InternalLoopback).BoundToPort(0);
    if (!TestNotNull(TEXT("UDP receiver"),Receiver) || !TestNotNull(TEXT("UDP sender"),Sender)) {
        if(Receiver) ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Receiver);
        if(Sender) ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Sender);
        return false;
    }
    Gloves->Socket=Receiver;Gloves->AllowedSender=TEXT("127.0.0.1");Gloves->StaleAfter=.15f;
    const auto Destination=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();Receiver->GetAddress(*Destination);
    const auto String=[](TArray<uint8>& Packet,const ANSICHAR* Value) {
        const int32 N=FCStringAnsi::Strlen(Value);Packet.Append(reinterpret_cast<const uint8*>(Value),N);Packet.Add(0);
        while(Packet.Num()%4) Packet.Add(0);
    };
    const auto Int=[](TArray<uint8>& Packet,uint32 Value) {
        Packet.Add(uint8(Value>>24));Packet.Add(uint8(Value>>16));Packet.Add(uint8(Value>>8));Packet.Add(uint8(Value));
    };
    const auto Float=[&](TArray<uint8>& Packet,float Value) { uint32 Bits;FMemory::Memcpy(&Bits,&Value,4);Int(Packet,Bits); };
    TArray<uint8> Device,Hand,RightHand;
    String(Device,"/DS/HC/Device");String(Device,",isiisifiifiif");
    Int(Device,1);String(Device,"test-glove");Int(Device,1);Int(Device,2);String(Device,"ContactGlove2");
    Int(Device,6);Float(Device,0);Int(Device,1);Int(Device,90);Float(Device,0);Int(Device,1);Int(Device,80);Float(Device,0);
    String(Hand,"/DS/HC/Hand");String(Hand,",isiffffffffffffffffffff");
    Int(Hand,1);String(Hand,"test-glove");Int(Hand,1);for(int32 I=0;I<20;++I) Float(Hand,.8f);
    String(RightHand,"/DS/HC/Hand");String(RightHand,",isiffffffffffffffffffff");
    Int(RightHand,1);String(RightHand,"test-glove");Int(RightHand,0);for(int32 I=0;I<20;++I) Float(RightHand,.3f);
    const auto Send=[&](const TArray<uint8>& Packet) { int32 Sent=0;return Sender->SendTo(Packet.GetData(),Packet.Num(),Sent,*Destination) && Sent==Packet.Num(); };
    TestTrue(TEXT("Queue a connected glove status"),Send(Device));TestTrue(TEXT("Queue a real-schema hand sample"),Send(Hand));
    Gloves->Tick(.016f);FMRFPSGloveHandFrame Frame;
    TestTrue(TEXT("Fresh connected input is accepted"),Gloves->GetLatestHand(EMRFPSGloveHand::Left,Frame));
    TestTrue(TEXT("Queue samples before suspension"),Send(Device) && Send(Hand));
    FPlatformProcess::Sleep(.18f);
    Gloves->Tick(.016f);
    TestFalse(TEXT("Queued samples do not become fresh after resume"),Gloves->GetLatestHand(EMRFPSGloveHand::Left,Frame));
    TestTrue(TEXT("New samples arrive after resume"),Send(Device) && Send(Hand));Gloves->Tick(.016f);
    TestTrue(TEXT("Fresh input works again after the backlog is discarded"),Gloves->GetLatestHand(EMRFPSGloveHand::Left,Frame));
    TestTrue(TEXT("The independent UDP reader starts"),Gloves->StartReceiver());
    TestTrue(TEXT("Queue input while the render thread is stalled"),Send(Device) && Send(Hand));
    FPlatformProcess::Sleep(.18f);Gloves->Tick(.18f);
    TestFalse(TEXT("The independent reader preserves the true age of old input"),Gloves->GetLatestHand(EMRFPSGloveHand::Left,Frame));
    TestTrue(TEXT("New input follows the stale sample"),Send(Device) && Send(Hand) && Send(RightHand));
    FPlatformProcess::Sleep(.04f);Gloves->Tick(.04f);
    TestTrue(TEXT("Fresh input is accepted after the stall"),Gloves->GetLatestHand(EMRFPSGloveHand::Left,Frame));
    TestTrue(TEXT("The right glove is received independently"),Gloves->GetLatestHand(EMRFPSGloveHand::Right,Frame));
    TestTrue(TEXT("Right and left flex values are not mixed"),Frame.Flexion.Num()==15 && FMath::IsNearlyEqual(Frame.Flexion[0],.3f));
    for (int32 SlowFrame=0;SlowFrame<3;++SlowFrame) {
        FPlatformProcess::Sleep(.12f);
        TestTrue(TEXT("A live sample arrives between slow render frames"),Send(Device) && Send(Hand) && Send(RightHand));
        FPlatformProcess::Sleep(.04f);Gloves->Tick(.16f);
        TestTrue(TEXT("Slow rendering does not permanently discard live glove input"),Gloves->GetLatestHand(EMRFPSGloveHand::Left,Frame));
        TestTrue(TEXT("Slow rendering keeps the other glove live too"),Gloves->GetLatestHand(EMRFPSGloveHand::Right,Frame));
    }
    TArray<uint8> RightOpen,RightClosed;
    for (auto* Packet:{&RightOpen,&RightClosed}) {
        String(*Packet,"/DS/HC/Hand");String(*Packet,",isiffffffffffffffffffff");
        Int(*Packet,1);String(*Packet,"test-glove");Int(*Packet,0);
        for (int32 I=0;I<20;++I) Float(*Packet,Packet==&RightClosed && I>=4 && I<=6?.8f:0.f);
    }
    TestTrue(TEXT("Send neutral right hand"),Send(Device) && Send(RightOpen));
    FPlatformProcess::Sleep(.04f); Gloves->Tick(.04f);
    auto Cursor=Gloves->GetRightTriggerJournal().Latest();
    TestTrue(TEXT("Send a full right-hand tap between game frames"),Send(Device) && Send(RightClosed) && Send(RightOpen));
    FPlatformProcess::Sleep(.04f); Gloves->Tick(.04f);
    std::array<MRFPSGloveTrigger::FEvent,MRFPSGloveTrigger::FJournal::Capacity> Edges;
    std::size_t Count=0;
    TestTrue(TEXT("Journal cursor remains complete"),Gloves->GetRightTriggerJournal().Read(Cursor,Edges,Count));
    TestTrue(TEXT("Both real OSC tap edges are retained"),Count==2 && Edges[0].Valid && Edges[0].Value>.55f && Edges[1].Valid && Edges[1].Value<.3f);
    TestTrue(TEXT("Gameplay still sees latest right-hand frame"),Gloves->GetLatestHand(EMRFPSGloveHand::Right,Frame) && Frame.FingerCurls[1]<.3f);
    Gloves->StopReceiver();
    const double HeartbeatAt=FPlatformTime::Seconds()-.25;
    Gloves->ProcessPacket(Device.GetData(),Device.Num(),HeartbeatAt);
    Gloves->ProcessPacket(RightClosed.GetData(),RightClosed.Num(),FPlatformTime::Seconds());
    TestTrue(TEXT("A fresh trigger remains valid with a 250 ms old device heartbeat"),Gloves->GetLatestHand(EMRFPSGloveHand::Right,Frame));
    Cursor=Gloves->GetRightTriggerJournal().Latest();
    TArray<uint8> Disconnected;
    String(Disconnected,"/DS/HC/Device");String(Disconnected,",isiisifiifiif");
    Int(Disconnected,1);String(Disconnected,"test-glove");Int(Disconnected,1);Int(Disconnected,2);String(Disconnected,"ContactGlove2");
    Int(Disconnected,6);Float(Disconnected,0);Int(Disconnected,1);Int(Disconnected,90);Float(Disconnected,0);Int(Disconnected,0);Int(Disconnected,0);Float(Disconnected,0);
    Gloves->ProcessPacket(Disconnected.GetData(),Disconnected.Num(),FPlatformTime::Seconds());
    TestFalse(TEXT("Explicit disconnect immediately invalidates the fresh hand"),Gloves->GetLatestHand(EMRFPSGloveHand::Right,Frame));
    Count=0;
    TestTrue(TEXT("Disconnect is recorded for trigger consumers"),Gloves->GetRightTriggerJournal().Read(Cursor,Edges,Count) && Count>0 && !Edges[0].Valid);
    Gloves->Socket=nullptr;Receiver->Close();Sender->Close();
    ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Receiver);
    ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Sender);
    return true;
}
#endif
