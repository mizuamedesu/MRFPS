#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "MRFPSVideoProtocol.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include <limits>

namespace
{
FMRFPSVideoChunk VideoPart(int32 Index,uint32 Sequence=1,uint32 Stream=1)
{
    FMRFPSVideoChunk C; C.Slot=0; C.StreamId=Stream; C.Sequence=Sequence;
    C.Width=640; C.Height=360; C.TotalBytes=1200; C.Index=Index;
    C.Payload.Init(static_cast<uint8>(Index+1),FMath::Min(512,1200-Index*512)); return C;
}
TArray<uint8> VideoData(int32 Size)
{
    TArray<uint8> Data; Data.SetNumUninitialized(Size);
    for (int32 I=0;I<Size;++I) Data[I]=static_cast<uint8>((I*73+I/13+29)&255);
    return Data;
}
TArray<FMRFPSVideoChunk> VideoPackets(const TArray<uint8>& Data,uint32 Sequence=1,uint32 Stream=1)
{
    FMRFPSVideoChunk Header; Header.Slot=0; Header.StreamId=Stream; Header.Sequence=Sequence;
    Header.Width=640; Header.Height=360; Header.TotalBytes=static_cast<uint16>(Data.Num());
    FMRFPSVideoSendQueue Queue; Queue.Push(Header,Data,1);
    TArray<FMRFPSVideoChunk> Packets;
    for (int32 I=0;I<=MRFPSVideo::MaxDataChunks+MRFPSVideo::MaxParityChunks;++I)
    {
        FMRFPSVideoChunk Packet; if (!Queue.Next(Packet,1.01)) break;
        Packets.Add(MoveTemp(Packet));
    }
    return Packets;
}

struct FSlowVideoTicks
{
    int32 ServerFrames[2] = {};
    int32 ViewerFrames[2] = {};
    int32 MaximumPublisherPackets = 0;
    int32 MaximumRelayPackets = 0;
    bool bExactFreshPayloads = true;
};
FSlowVideoTicks SimulateSlowVideoTicks(double TickInterval,int32 PublisherBudget,int32 RelayBudget)
{
    FSlowVideoTicks Result;
    const auto Data=VideoData(MRFPSVideo::MaxFrameBytes);
    FMRFPSVideoSendQueue Publishers[2],Relays[2];
    FMRFPSVideoAssembler Server[2],Viewers[2];
    int32 NextSlot=0;
    for (int32 Tick=0;Tick<12;++Tick)
    {
        const double Now=10+Tick*TickInterval;
        for (int32 Slot=0;Slot<2;++Slot)
        {
            FMRFPSVideoChunk Header; Header.Slot=static_cast<uint8>(Slot); Header.StreamId=1; Header.Sequence=static_cast<uint32>(Tick+1);
            Header.Width=640; Header.Height=360; Header.TotalBytes=static_cast<uint16>(Data.Num());
            Publishers[Slot].Push(Header,Data,Now);
            int32 Sent=0;
            for (;Sent<PublisherBudget;++Sent)
            {
                FMRFPSVideoChunk Part;
                if (!Publishers[Slot].Next(Part,Now)) break;
                Server[Slot].Add(Part,Now);
                TArray<uint8> Complete;
                if (Server[Slot].Take(Header,Complete))
                {
                    ++Result.ServerFrames[Slot];
                    Result.bExactFreshPayloads &= Complete==Data && Header.Slot==Slot &&
                        Now-(10+(Header.Sequence-1)*TickInterval)<MRFPSVideo::FrameLifetime;
                    Header.StreamId=static_cast<uint32>(Slot+11);
                    Relays[Slot].Push(Header,Complete,Now);
                }
            }
            Result.MaximumPublisherPackets=FMath::Max(Result.MaximumPublisherPackets,Sent);
        }
        int32 Sent=0;
        for (;Sent<RelayBudget;++Sent)
        {
            FMRFPSVideoChunk Part; bool Found=false;
            for (int32 Try=0;Try<2;++Try)
            {
                const int32 Slot=NextSlot; NextSlot=(NextSlot+1)%2;
                if (Relays[Slot].Next(Part,Now)) { Found=true; break; }
            }
            if (!Found) break;
            Viewers[Part.Slot].Add(Part,Now);
            FMRFPSVideoChunk Header; TArray<uint8> Complete;
            if (Viewers[Part.Slot].Take(Header,Complete))
            {
                ++Result.ViewerFrames[Part.Slot];
                Result.bExactFreshPayloads &= Complete==Data &&
                    Now-(10+(Header.Sequence-1)*TickInterval)<MRFPSVideo::FrameLifetime;
            }
        }
        Result.MaximumRelayPackets=FMath::Max(Result.MaximumRelayPackets,Sent);
    }
    return Result;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSVideoWireTest,"MRFPS.Video.BoundedWire",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSVideoWireTest::RunTest(const FString&)
{
    auto Original=VideoPart(0); TArray<uint8> Wire; FMemoryWriter Writer(Wire); bool Success=false;
    TestTrue(TEXT("Valid chunk serializes"),Original.NetSerialize(Writer,nullptr,Success)&&Success);
    TestTrue(TEXT("Chunk leaves room for native DTLS and UE headers in a 1024-byte packet"),Wire.Num()<700);
    FMRFPSVideoChunk Copy; FMemoryReader Reader(Wire);
    TestTrue(TEXT("Wire round trip succeeds"),Copy.NetSerialize(Reader,nullptr,Success)&&Success);
    TestTrue(TEXT("Payload is preserved"),Copy.Payload==Original.Payload);
    // The last header uint16 is payload length. A hostile length is rejected
    // before a TArray allocation, even if the remaining packet is truncated.
    Wire[16]=1; Wire[17]=2;
    FMRFPSVideoChunk Hostile; FMemoryReader Bad(Wire);
    TestFalse(TEXT("Oversized remote length is rejected"),Hostile.NetSerialize(Bad,nullptr,Success));
    TestEqual(TEXT("Rejected remote length allocated no payload"),Hostile.Payload.Num(),0);
    auto Invalid=VideoPart(0); Invalid.TotalBytes=65535; TestFalse(TEXT("Frame cap is enforced"),Invalid.IsSane());

    auto Parity=VideoPart(0); Parity.Index=MRFPSVideo::ParityFlag; Parity.Payload.Init(0x5a,512);
    TArray<uint8> ParityWire; FMemoryWriter ParityWriter(ParityWire);
    TestTrue(TEXT("Padded parity serializes through the production wire path"),Parity.NetSerialize(ParityWriter,nullptr,Success)&&Success);
    TestTrue(TEXT("Parity RPC also stays below 700 bytes"),ParityWire.Num()<700);
    FMRFPSVideoChunk ParityCopy; FMemoryReader ParityReader(ParityWire);
    TestTrue(TEXT("Parity wire round trip"),ParityCopy.NetSerialize(ParityReader,nullptr,Success)&&Success);
    TestTrue(TEXT("Parity payload and flag are preserved"),ParityCopy.Payload==Parity.Payload && ParityCopy.Index==Parity.Index);
    auto ShortParity=Parity; ShortParity.Payload.Pop();
    TestFalse(TEXT("Parity must contain all 512 padded bytes"),ShortParity.IsSane());
    auto BadParityWire=ParityWire; BadParityWire[15]=0x81; // 1200 bytes has only group zero.
    FMRFPSVideoChunk BadGroup; FMemoryReader BadGroupReader(BadParityWire);
    TestFalse(TEXT("Remote parity group outside the frame is rejected"),BadGroup.NetSerialize(BadGroupReader,nullptr,Success));
    TestEqual(TEXT("Bad parity group allocates no payload"),BadGroup.Payload.Num(),0);
    BadParityWire=ParityWire; BadParityWire[16]=0xff; BadParityWire[17]=1;
    FMRFPSVideoChunk BadLength; FMemoryReader BadLengthReader(BadParityWire);
    TestFalse(TEXT("Remote parity shorter than 512 bytes is rejected"),BadLength.NetSerialize(BadLengthReader,nullptr,Success));
    TestEqual(TEXT("Bad parity length allocates no payload"),BadLength.Payload.Num(),0);
    auto Boundary=Parity; Boundary.TotalBytes=MRFPSVideo::MaxFrameBytes; Boundary.Index=0x8e;
    TestTrue(TEXT("Final group fourteen is valid at the frame cap"),Boundary.IsSane());
    Boundary.Index=0x8f; TestFalse(TEXT("There is never a sixteenth parity group"),Boundary.IsSane());
    Boundary.Index=119; TestTrue(TEXT("Final data index 119 fits the frame cap"),Boundary.IsSane());
    Boundary.Index=120; TestFalse(TEXT("Data index 120 cannot allocate a larger frame"),Boundary.IsSane());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSVideoAssemblyTest,"MRFPS.Video.LossOrderingAndExpiry",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSVideoAssemblyTest::RunTest(const FString&)
{
    FMRFPSVideoAssembler A; FMRFPSVideoChunk H; TArray<uint8> Frame;
    TestTrue(TEXT("Last chunk may arrive first"),A.Add(VideoPart(2),1));
    TestFalse(TEXT("A partial frame is never exposed"),A.Take(H,Frame));
    A.Add(VideoPart(0),1.01); A.Add(VideoPart(0),1.02);
    TestFalse(TEXT("Duplicate does not complete a frame"),A.Take(H,Frame));
    auto Conflict=VideoPart(1); Conflict.Width=800;
    TestFalse(TEXT("Conflicting metadata is rejected"),A.Add(Conflict,1.03));
    A.Add(VideoPart(1),1.04);
    TestTrue(TEXT("Out-of-order complete frame is exposed once"),A.Take(H,Frame));
    TestEqual(TEXT("Exact bounded payload size"),Frame.Num(),1200);
    TestFalse(TEXT("Completed-frame replay is rejected"),A.Add(VideoPart(0),1.05));
    A.Add(VideoPart(0,2),2);
    TestFalse(TEXT("Expired partial sequence cannot be resurrected"),A.Add(VideoPart(1,2),3));
    TestFalse(TEXT("Expired frame is never exposed"),A.Take(H,Frame));
    TestTrue(TEXT("New generation restarts its sequence"),A.Add(VideoPart(0,1,2),3.1));
    TestFalse(TEXT("Older publisher generation is rejected"),A.Add(VideoPart(1,5,1),3.11));
    TestTrue(TEXT("Assembly stays under one frame plus bounded parity"),A.AllocatedBytes()<=MRFPSVideo::MaxFrameBytes+MRFPSVideo::MaxParityBytes);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSVideoQueueTest,"MRFPS.Video.LatestQueueAndBackpressure",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSVideoQueueTest::RunTest(const FString&)
{
    FMRFPSVideoSendQueue Q; TArray<uint8> Data; Data.Init(7,1200);
    auto Header=VideoPart(0); Header.Payload.Reset();
    Q.Push(Header,Data,1); Header.Sequence=2; Q.Push(Header,Data,1.01); Header.Sequence=3; Q.Push(Header,Data,1.02);
    FMRFPSVideoChunk C; for(int32 I=0;I<4;++I) { TestTrue(TEXT("Current frame drains under a tick budget"),Q.Next(C,1.03)); TestEqual(TEXT("Current is not overwritten mid-frame"),C.Sequence,1u); }
    TestEqual(TEXT("Current ends only after its final parity packet"),C.Index,MRFPSVideo::ParityFlag);
    TestTrue(TEXT("Only latest waiting frame survives"),Q.Next(C,1.04)); TestEqual(TEXT("Intermediate frame was dropped"),C.Sequence,3u);
    TestFalse(TEXT("Expired backlog is dropped"),Q.Next(C,3));

    Q.Reset(); Header.Sequence=1; Q.Push(Header,Data,4); Header.Sequence=2; Q.Push(Header,Data,4.01);
    for (int32 I=0;I<4;++I) TestTrue(TEXT("Drain current before pending promotion"),Q.Next(C,4.02));
    Header.Sequence=3; Q.Push(Header,Data,4.03);
    for (int32 I=0;I<4;++I) {
        TestTrue(TEXT("New push after drain is emitted"),Q.Next(C,4.04));
        TestEqual(TEXT("New current never exposes the older waiting sequence"),C.Sequence,3u);
    }
    TestFalse(TEXT("The superseded waiting frame is not sent after the newer one"),Q.Next(C,4.05));

    Q.Reset(); Header.Sequence=10; Q.Push(Header,Data,5);
    for (int32 I=0;I<3;++I) TestTrue(TEXT("Emit all data before final parity"),Q.Next(C,5.01));
    Header.Sequence=11; Q.Push(Header,Data,5.02);
    TestTrue(TEXT("A new push cannot displace the final parity"),Q.Next(C,5.03));
    TestTrue(TEXT("Parity belongs to the intact old current"),C.Sequence==10 && C.Index==MRFPSVideo::ParityFlag);
    TestTrue(TEXT("Waiting frame follows final parity"),Q.Next(C,5.04));
    TestTrue(TEXT("New frame begins with data zero"),C.Sequence==11 && C.Index==0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSVideoSlowTickTest,"MRFPS.Video.LowTickFrameBudget",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSVideoSlowTickTest::RunTest(const FString&)
{
    for (const double Hertz:{3.0,4.0})
    {
        const auto Old=SimulateSlowVideoTicks(1/Hertz,24,24);
        const auto OldRelay=SimulateSlowVideoTicks(1/Hertz,MRFPSVideo::PublisherChunkBudgetPerTick,24);
        const auto Current=SimulateSlowVideoTicks(1/Hertz,MRFPSVideo::PublisherChunkBudgetPerTick,MRFPSVideo::RelayChunkBudgetPerTick);
        for (int32 Slot=0;Slot<2;++Slot)
        {
            TestEqual(TEXT("Old publisher budget expires every max-size frame at 3-4 Hz"),Old.ServerFrames[Slot],0);
            TestEqual(TEXT("Old relay budget still starves despite complete server uploads"),OldRelay.ViewerFrames[Slot],0);
            TestEqual(TEXT("One max-size upload completes at every slow game tick"),Current.ServerFrames[Slot],12);
            TestEqual(TEXT("Both sources complete at every slow relay tick"),Current.ViewerFrames[Slot],12);
        }
        TestTrue(TEXT("Slow-tick relay preserves exact bytes and the unchanged freshness limit"),Current.bExactFreshPayloads);
        TestTrue(TEXT("Publisher work never exceeds its finite wire-derived bound"),Current.MaximumPublisherPackets<=MRFPSVideo::PublisherChunkBudgetPerTick);
        TestTrue(TEXT("Two-source relay work never exceeds its finite bound"),Current.MaximumRelayPackets<=MRFPSVideo::RelayChunkBudgetPerTick);
    }
    // A larger per-tick budget cannot authorize transmission of an expired
    // backpressured frame or resurrect its incomplete receive state.
    const auto Data=VideoData(MRFPSVideo::MaxFrameBytes);
    FMRFPSVideoSendQueue Queue; FMRFPSVideoAssembler Assembly;
    FMRFPSVideoChunk Header; Header.Slot=0; Header.StreamId=1; Header.Sequence=1;
    Header.Width=640; Header.Height=360; Header.TotalBytes=static_cast<uint16>(Data.Num());
    Queue.Push(Header,Data,30);
    FMRFPSVideoChunk Part,First;
    for (int32 I=0;I<24;++I) { Queue.Next(Part,30); if(I==0) First=Part; Assembly.Add(Part,30); }
    TestFalse(TEXT("Network backpressure past the TTL still discards the old send queue"),Queue.Next(Part,30+MRFPSVideo::FrameLifetime+.01));
    TestFalse(TEXT("An expired receive sequence cannot be reopened by late data"),Assembly.Add(First,30+MRFPSVideo::FrameLifetime+.01));
    TArray<uint8> Complete;
    TestFalse(TEXT("No stale partial payload is exposed by the larger budget"),Assembly.Take(Header,Complete));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSVideoRateTest,"MRFPS.Video.RateAdmissionRecovery",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSVideoRateTest::RunTest(const FString&)
{
    double Level=0;
    const int32 JpegBytes=40*1024;
    const int32 DataChunks=(JpegBytes+MRFPSVideo::ChunkBytes-1)/MRFPSVideo::ChunkBytes;
    const int32 WireChunks=DataChunks+(DataChunks+MRFPSVideo::DataChunksPerParity-1)/MRFPSVideo::DataChunksPerParity;
    bool AllNormalFramesAllowed=true,AlwaysBounded=true;
    // Twenty seconds at the real producer target includes padded FEC payload.
    for (int32 Frame=0;Frame<600;++Frame) for (int32 Part=0;Part<WireChunks;++Part)
    {
        const auto Rate=MRFPSVideo::CheckPublisherRate(Level,Part==0 ? 1.0/30.0 : 0.0,MRFPSVideo::ChunkBytes);
        AllNormalFramesAllowed &= Rate.bAllowed;
        Level=Rate.bAllowed ? Rate.ChargedBytes : Rate.DecayedBytes;
        AlwaysBounded &= Level>=0 && Level<=MRFPSVideo::PublisherBurstBytes;
    }
    TestTrue(TEXT("Thirty 40-KiB JPEGs per second plus parity fit the sustained budget"),AllNormalFramesAllowed);
    TestTrue(TEXT("Sustained target traffic remains within the fixed burst bound"),AlwaysBounded);
    Level=0;
    bool FullBurstAllowed=true;
    for (int32 Part=0;Part<MRFPSVideo::PublisherBurstBytes/MRFPSVideo::ChunkBytes;++Part)
    {
        const auto Rate=MRFPSVideo::CheckPublisherRate(Level,0,MRFPSVideo::ChunkBytes);
        FullBurstAllowed &= Rate.bAllowed; Level=Rate.ChargedBytes;
    }
    TestTrue(TEXT("Exactly one full configured burst is admissible"),FullBurstAllowed);
    TestEqual(TEXT("Burst occupancy reaches its exact fixed cap"),Level,double(MRFPSVideo::PublisherBurstBytes));
    bool RejectedWithoutDebt=true;
    for (int32 Attempt=0;Attempt<20000;++Attempt)
    {
        const auto Rate=MRFPSVideo::CheckPublisherRate(Level,0,MRFPSVideo::ChunkBytes);
        RejectedWithoutDebt &= !Rate.bAllowed && Rate.ChargedBytes==Rate.DecayedBytes;
        Level=Rate.DecayedBytes;
    }
    TestTrue(TEXT("Repeated over-burst rejection adds no unbounded debt"),RejectedWithoutDebt);
    TestEqual(TEXT("Twenty thousand rejected chunks cannot raise burst occupancy"),Level,double(MRFPSVideo::PublisherBurstBytes));
    int32 AdmittedWhileOverloaded=0,RejectedWhileOverloaded=0;
    AlwaysBounded=true;
    for (int32 Attempt=0;Attempt<10000;++Attempt)
    {
        const auto Rate=MRFPSVideo::CheckPublisherRate(Level,.000001,MRFPSVideo::ChunkBytes);
        if (Rate.bAllowed) { ++AdmittedWhileOverloaded; Level=Rate.ChargedBytes; }
        else { ++RejectedWhileOverloaded; Level=Rate.DecayedBytes; }
        AlwaysBounded &= Level>=0 && Level<=MRFPSVideo::PublisherBurstBytes;
    }
    TestTrue(TEXT("Continuing overload still allows new chunks as genuine credit returns"),AdmittedWhileOverloaded>0 && RejectedWhileOverloaded>0);
    TestTrue(TEXT("Overload and recovery cannot exceed the configured burst"),AlwaysBounded);
    const auto Idle=MRFPSVideo::CheckPublisherRate(Level,1,MRFPSVideo::ChunkBytes);
    TestTrue(TEXT("A quiet interval restores admission after arbitrary rejection count"),Idle.bAllowed && Idle.DecayedBytes==0);
    const auto OldDebt=MRFPSVideo::CheckPublisherRate(double(MRFPSVideo::PublisherBurstBytes)*1000,1,MRFPSVideo::ChunkBytes);
    TestTrue(TEXT("An oversized old occupancy is capped before decay and recovers"),OldDebt.bAllowed && OldDebt.DecayedBytes==0);
    const auto Preview=MRFPSVideo::CheckPublisherRate(0,0,MRFPSVideo::ChunkBytes);
    TestTrue(TEXT("Uncommitted or assembler-rejected admission preserves the decayed level"),Preview.bAllowed && Preview.DecayedBytes==0 && Preview.ChargedBytes==MRFPSVideo::ChunkBytes);
    const auto Invalid=MRFPSVideo::CheckPublisherRate(0,std::numeric_limits<double>::quiet_NaN(),MRFPSVideo::ChunkBytes);
    TestFalse(TEXT("An invalid clock sample cannot authorize payload"),Invalid.bAllowed);
    TestFalse(TEXT("A payload larger than the wire chunk cap cannot be admitted"),MRFPSVideo::CheckPublisherRate(0,1,MRFPSVideo::ChunkBytes+1).bAllowed);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSVideoFecTest,"MRFPS.Video.XorParityRecovery",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSVideoFecTest::RunTest(const FString&)
{
    const auto Expected=VideoData(MRFPSVideo::MaxFrameBytes);
    const auto Packets=VideoPackets(Expected);
    if (!TestEqual(TEXT("Maximum frame emits 120 data and fifteen parity packets"),Packets.Num(),135)) return false;
    for (int32 Group=0;Group<MRFPSVideo::MaxParityChunks;++Group)
    {
        for (int32 I=0;I<8;++I)
            TestEqual(TEXT("Eight data packets precede each parity"),Packets[Group*9+I].Index,static_cast<uint8>(Group*8+I));
        TestEqual(TEXT("Parity flag carries the bounded group number"),Packets[Group*9+8].Index,static_cast<uint8>(MRFPSVideo::ParityFlag|Group));
        TestEqual(TEXT("Every parity row is padded to 512 bytes"),Packets[Group*9+8].Payload.Num(),512);
    }
    FMRFPSVideoAssembler A; FMRFPSVideoChunk Header; TArray<uint8> Actual;
    // Reverse delivery puts parity before its members and exercises every row.
    double Now=2;
    for (int32 I=Packets.Num()-1;I>=0;--I)
    {
        const auto& Part=Packets[I];
        if (!(Part.Index&MRFPSVideo::ParityFlag) && Part.Index%8==3) continue;
        TestTrue(TEXT("Reordered packet with one loss per group is accepted"),A.Add(Part,Now)); Now+=.001;
    }
    TestTrue(TEXT("Parity plus seven members restores each of fifteen lost data chunks"),A.Take(Header,Actual));
    TestTrue(TEXT("Recovery reproduces every original byte at the 60 KiB cap"),Actual==Expected);
    TestFalse(TEXT("A parity replay cannot reopen a completed sequence"),A.Add(Packets.Last(),Now));

    A.Reset(); Now=3;
    for (const auto& Part:Packets)
    {
        if (!(Part.Index&MRFPSVideo::ParityFlag) && (Part.Index==0 || Part.Index==1)) continue;
        A.Add(Part,Now); Now+=.001;
    }
    TestFalse(TEXT("Two unknown members in one group never become a complete frame"),A.Take(Header,Actual));
    TestEqual(TEXT("Parity storage is capped to fifteen rows"),A.AllocatedBytes(),MRFPSVideo::MaxFrameBytes+MRFPSVideo::MaxParityBytes);
    TestFalse(TEXT("Expired FEC sequence cannot be resurrected by late parity"),A.Add(Packets.Last(),4));
    TestFalse(TEXT("An expired unrecoverable frame stays invisible"),A.Take(Header,Actual));
    TestEqual(TEXT("Expiry discards data and parity payloads together"),A.AllocatedBytes(),0);

    A.Reset(); Now=5;
    for (const auto& Part:Packets) if (!(Part.Index&MRFPSVideo::ParityFlag)) { A.Add(Part,Now); Now+=.001; }
    TestTrue(TEXT("Losing every parity packet still permits complete data"),A.Take(Header,Actual));
    TestTrue(TEXT("No-parity path preserves the original bytes"),Actual==Expected);

    // Includes a one-data group, an incomplete trailing group, and a trailing
    // short member XORed with full-sized neighbors (implicit padding is zero).
    for (const int32 Size:{137,8*512+137,10*512+137})
    {
        const auto ShortExpected=VideoData(Size);
        const auto ShortPackets=VideoPackets(ShortExpected);
        const int32 LastData=(Size-1)/512;
        for (const int32 Missing:{LastData,(LastData/8)*8})
        {
            A.Reset(); Now=6;
            for (int32 I=ShortPackets.Num()-1;I>=0;--I)
            {
                const auto& Part=ShortPackets[I];
                if (!(Part.Index&MRFPSVideo::ParityFlag) && Part.Index==Missing) continue;
                A.Add(Part,Now); Now+=.001;
            }
            TestTrue(TEXT("Trailing partial group recovers its one missing member"),A.Take(Header,Actual));
            TestTrue(TEXT("Short member padding is never appended to the frame"),Actual==ShortExpected && Actual.Num()==Size);
        }
    }

    const auto SmallExpected=VideoData(1200);
    const auto Small=VideoPackets(SmallExpected);
    A.Reset();
    TestTrue(TEXT("Parity can arrive first"),A.Add(Small.Last(),7));
    TestTrue(TEXT("Identical parity duplicate is harmless"),A.Add(Small.Last(),7.01));
    auto Conflict=Small.Last(); Conflict.Payload[17]^=1;
    TestFalse(TEXT("Conflicting parity cannot overwrite its first row"),A.Add(Conflict,7.02));
    Conflict=Small.Last(); Conflict.Width=639;
    TestFalse(TEXT("Parity must share all frame metadata"),A.Add(Conflict,7.03));
    TestTrue(TEXT("First data member is accepted"),A.Add(Small[0],7.04));
    TestTrue(TEXT("Identical data duplicate is harmless"),A.Add(Small[0],7.05));
    Conflict=Small[0]; Conflict.Payload[17]^=1;
    TestFalse(TEXT("Conflicting data cannot corrupt recovery"),A.Add(Conflict,7.06));
    TestTrue(TEXT("Second member triggers recovery of the short third member"),A.Add(Small[1],7.07));
    TestTrue(TEXT("Late original agrees with its recovered bytes"),A.Add(Small[2],7.08));
    TestTrue(TEXT("Duplicates did not inflate received count"),A.Take(Header,Actual));
    TestTrue(TEXT("Conflicts did not alter the decoded payload"),Actual==SmallExpected);
    const auto NewGeneration=VideoPackets(SmallExpected,1,2);
    TestTrue(TEXT("A higher generation accepts parity with a restarted sequence"),A.Add(NewGeneration.Last(),8));
    TestFalse(TEXT("An old generation cannot inject data into new FEC state"),A.Add(Small[0],8.01));
    return true;
}
#endif
