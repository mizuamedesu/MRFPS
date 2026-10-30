#include "../../Source/MRFPS/Public/MRFPSLoginPressState.h"
#include "../../Source/MRFPS/Public/MRFPSFireReleaseGate.h"
#include "../../Plugins/MRFPSGloves/Source/MRFPSGloves/Public/MRFPSGloveTriggerJournal.h"
#include <cassert>
#include <limits>
#include <iostream>
using namespace MRFPSLoginPointer;
static void SelectAndArm(FPressState& S) { assert(S.Next()==EAction::None); S.Step(.1); }
int main()
{
    using MRFPSGloveTrigger::EStream;
    MRFPSGloveTrigger::FJournal J;
    J.Sample(EStream::Flex,1.,true,0,false,.15);
    std::uint64_t Cursor=J.Latest();
    J.Sample(EStream::Flex,1.01,true,.8f,false,.15); J.Sample(EStream::Flex,1.02,true,0,false,.15);
    std::array<MRFPSGloveTrigger::FEvent,MRFPSGloveTrigger::FJournal::Capacity> Samples;
    std::size_t N=0; assert(J.Read(Cursor,Samples,N) && N==2 && Samples[0].Value==.8f && Samples[1].Value==0);
    auto Reader2=std::uint64_t(1); assert(J.Read(Reader2,Samples,N) && N==2);
    J.Sample(EStream::Flex,1.4,true,.8f,false,.15);
    assert(J.Read(Cursor,Samples,N) && N==2 && !Samples[0].Valid && Samples[1].Valid);
    auto Old=Cursor; for(int I=0;I<70;++I) J.Sample(EStream::Flex,1.41+I*.001,true,I%2?0.f:1.f,false,.15);
    assert(!J.Read(Old,Samples,N) && N==0);
    J.Sample(EStream::Controller,2.,true,0,false,.15); Cursor=J.Latest();
    J.Sample(EStream::Controller,2.01,true,0,true,.15); J.Sample(EStream::Controller,2.02,true,0,false,.15);
    assert(J.Read(Cursor,Samples,N) && N==2 && Samples[0].Button && !Samples[1].Button);

    // The physical Fire trigger remains usable even with fresh glove flex.
    FPressState S; S.SetSource(ESource::AimGrip); S.SetAxis(0); S.SetGloveController(true,false,0); S.SetGloveFlex(true,0);
    SelectAndArm(S);
    S.SetButton(true); assert(S.Next()==EAction::Press && S.Selected==EInput::Button);
    S.SetButton(false); assert(S.Next()==EAction::Release);

    FPressState LateButton; LateButton.SetSource(ESource::AimGrip); LateButton.SetGloveFlex(true,0); LateButton.Step(.1);
    LateButton.SetGloveFlex(true,.8f); assert(LateButton.Next()==EAction::Press);
    LateButton.SetGloveFlex(true,0); assert(LateButton.Next()==EAction::Release);
    LateButton.Step(.11); LateButton.SetButton(true); assert(LateButton.Next()==EAction::None);
    LateButton.Step(.3); assert(LateButton.Next()==EAction::None); // held suppressed input never fires later
    LateButton.SetButton(false); LateButton.Step(.1); LateButton.SetButton(true); assert(LateButton.Next()==EAction::Press);
    LateButton.SetButton(false); assert(LateButton.Next()==EAction::Release);
    // A late duplicate glove representation is suppressed, without disabling
    // a fresh physical trigger or fresh glove input after its own release.
    S.Step(.11); S.SetGloveFlex(true,.8f); assert(S.Next()==EAction::None);
    S.Step(.15); assert(S.Next()==EAction::None);
    S.SetGloveFlex(true,0); S.Step(.1); S.SetGloveFlex(true,.8f);
    assert(S.Next()==EAction::Press && S.Selected==EInput::GloveFlex);
    S.SetGloveFlex(true,0); assert(S.Next()==EAction::Release);

    // Glove alone is accepted while the controller is tracked; a physical
    // trigger takes over a held glove press without firing the old capture.
    S.Step(.25); S.SetGloveFlex(true,.8f); assert(S.Next()==EAction::Press);
    S.SetButton(true); assert(S.Next()==EAction::Cancel); assert(S.Next()==EAction::Press && S.Selected==EInput::Button);
    S.SetGloveFlex(false,0); assert(S.Next()==EAction::None); // unrelated stale glove cannot cancel controller
    S.SetButton(false); assert(S.Next()==EAction::Release);
    S.SetGloveFlex(true,.8f); S.Step(.2); assert(S.Next()==EAction::None); // returned held glove is disarmed
    S.SetGloveFlex(true,0); S.Step(.1); S.SetGloveFlex(true,.8f); assert(S.Next()==EAction::Press);
    S.SetGloveFlex(false,0); assert(S.Next()==EAction::Cancel); // selected stale input cancels
    S.SetButton(false); S.Step(.1); S.SetButton(true); assert(S.Next()==EAction::Press);
    S.SetButton(false); assert(S.Next()==EAction::Release);

    // Explicit modes remain usable; loss cancels rather than clicks.
    FPressState Forced; Forced.Mode=EMode::GloveController; Forced.SetSource(ESource::Hand);
    Forced.SetGloveController(true,false,0); Forced.SetGloveFlex(true,0); Forced.SetAxis(0);
    SelectAndArm(Forced);
    Forced.SetGloveController(true,true,0); assert(Forced.Next()==EAction::Press);
    Forced.SetGloveController(false,false,0); assert(Forced.Next()==EAction::Cancel);
    Forced.SetGloveController(true,true,0); Forced.Step(.2); assert(Forced.Next()==EAction::None);
    Forced.SetGloveController(true,false,0); Forced.Step(.1); Forced.SetGloveController(true,true,0); assert(Forced.Next()==EAction::Press);
    Forced.Mode=EMode::QuestButton; assert(Forced.Next()==EAction::Cancel);
    Forced.SetButton(false); Forced.Step(.1); Forced.SetButton(true); assert(Forced.Next()==EAction::Press);
    Forced.SetButton(false); assert(Forced.Next()==EAction::Release);

    Forced.Step(.1); Forced.SetButton(true); assert(Forced.Next()==EAction::Press);
    Forced.SetFocused(false); assert(Forced.Next()==EAction::Cancel);
    Forced.SetFocused(true); Forced.Step(.2); assert(Forced.Next()==EAction::None);
    Forced.SetButton(false); Forced.Step(.1); Forced.SetButton(true); assert(Forced.Next()==EAction::Press);
    Forced.Step(3.); assert(Forced.Next()==EAction::Cancel);
    Forced.SetButton(false); Forced.Step(.1); Forced.SetButton(true); assert(Forced.Next()==EAction::Press);
    Forced.SetSource(ESource::None); assert(Forced.Next()==EAction::Cancel);

    // Closing the menu cannot re-arm firing while either glove input remains
    // held, and loss of that stream cannot masquerade as a neutral release.
    MRFPSFireRelease::FGloveNeutral Gate;
    Gate.Observe(true,.8f,true,true,.8f);
    assert(!Gate.CanRearm(false,false));
    Gate.Observe(false,0,true,false,0); assert(!Gate.CanRearm(false,false));
    Gate.Observe(true,0,true,false,0); assert(!Gate.CanRearm(true,false));
    assert(!Gate.CanRearm(false,true) && Gate.CanRearm(false,false));

    // The game-instance gates survive replacing the login pawn. Each source
    // independently waits for a genuine neutral after the transition.
    MRFPSFireRelease::FTransitionGate Travel;
    Travel.BeginLogin(); Travel.SetPhysical(true);
    Travel.Observe(true,.8f,10,true,true,.8f,10);
    Travel.SealForTravel();
    assert(!Travel.CanPhysical() && !Travel.CanFlex() && !Travel.CanController());
    Travel.SetPhysical(true); assert(!Travel.CanPhysical()); // held through ClientTravel
    Travel.SetPhysical(false);
    Travel.Observe(false,0,11,false,false,0,11);
    assert(Travel.CanPhysical() && !Travel.CanFlex() && !Travel.CanController()); // stale only blocks its own source
    Travel.SetPhysical(true); assert(Travel.CanPhysical()); // independent physical shot
    Travel.Observe(true,0,10,true,false,0,10);
    assert(!Travel.CanFlex() && !Travel.CanController()); // cached samples do not count
    Travel.Observe(true,0,11,true,true,.8f,11);
    assert(Travel.CanFlex() && !Travel.CanController());
    Travel.Observe(true,0,12,true,false,0,12);
    assert(Travel.CanFlex() && Travel.CanController());
    Travel.SetAutomation(true); assert(!Travel.CanAutomation());
    Travel.SetAutomation(false); assert(Travel.CanAutomation());
    Travel.Observe(false,0,13,false,false,0,13);
    assert(!Travel.CanFlex() && !Travel.CanController() && Travel.CanPhysical());
    Travel.Observe(true,.8f,14,true,true,.8f,14);
    assert(!Travel.CanFlex() && !Travel.CanController()); // reconnect while held
    Travel.Observe(true,0,15,true,false,0,15);
    assert(Travel.CanFlex() && Travel.CanController());
    MRFPSFireRelease::FTransitionGate NoGlove;
    NoGlove.BeginLogin(); NoGlove.SetPhysical(true); NoGlove.SealForTravel();
    assert(!NoGlove.CanPhysical() && !NoGlove.CanFlex() && !NoGlove.CanController());
    NoGlove.SetPhysical(false); assert(NoGlove.CanPhysical());
    NoGlove.Observe(true,.8f,20,true,true,.8f,20);
    assert(!NoGlove.CanFlex() && !NoGlove.CanController()); // first appearance held
    NoGlove.Observe(true,0,21,true,false,0,21);
    assert(NoGlove.CanFlex() && NoGlove.CanController() && NoGlove.CanPhysical());

    MRFPSFireRelease::FGloveFiring FireStreams;
    FireStreams.Update(true,.8f,true,false,.4f); assert(FireStreams.FlexDown && FireStreams.Down());
    FireStreams.Update(false,0,true,false,.4f); assert(!FireStreams.Down()); // stale flex cannot leave a phantom press
    FireStreams.Update(false,0,true,true,.8f); assert(FireStreams.ControllerDown && FireStreams.Down());
    FireStreams.Update(true,0,false,false,0); assert(!FireStreams.Down());
    FireStreams.Update(true,.8f,true,false,0); assert(FireStreams.Down());
    FireStreams.Reset(); assert(!FireStreams.Down());

    FEventQueue Q; for(int I=0;I<1000;++I) Q.Push(EInput::Axis,0,10.+I*.001);
    assert(!Q.Overflow && Q.Count==1 && Q.Events[0].At==10.);
    Q.Push(EInput::Axis,.7f,11.01); Q.Push(EInput::Axis,.8f,11.02); Q.Push(EInput::Axis,0,11.03); assert(Q.Count==3);
    Q.Clear(); for(int I=0;I<33;++I) Q.Push(EInput::Button,float(I%2),0); assert(Q.Overflow);
    Q.Clear(); Q.Push(EInput::Button,1,3); Q.Push(EInput::Button,0,2); assert(Q.Overflow);
    Q.Clear(); Q.Push(EInput::Axis,0,10); Q.Push(EInput::Axis,0,20); Q.Push(EInput::Button,1,15); assert(Q.Overflow);

    // The production dispatcher cancels a glove-owned capture and transfers
    // it to the physical trigger before that trigger's release.
    FPressState Combined; Combined.SetSource(ESource::AimGrip); Combined.SetGloveFlex(true,0); Combined.Step(.1);
    Q.Clear(); Q.Push(EInput::GloveFlex,.8f,4.01); Q.Push(EInput::Button,1,4.02);
    Q.Push(EInput::Button,0,4.03); Q.Push(EInput::GloveFlex,0,4.04);
    int CombinedPress=0,CombinedRelease=0,CombinedCancel=0;
    auto CombinedAction=[&](EAction A){CombinedPress+=A==EAction::Press;CombinedRelease+=A==EAction::Release;CombinedCancel+=A==EAction::Cancel;return true;};
    auto Ignore=[](const FEventQueue::FEvent&){};
    const auto TransferPreview=PreviewBatch(Combined,Q,4.,4.1);
    assert(TransferPreview.Valid && TransferPreview.Presses==2 && TransferPreview.Releases==1 && !TransferPreview.Ambiguous());
    assert(Dispatch(Combined,Q,4.,4.1,CombinedAction,Ignore));
    assert(CombinedPress==2 && CombinedCancel==1 && CombinedRelease==1);

    // The same journal event type goes through the production dispatcher.
    FPressState Fast; Fast.Mode=EMode::GloveFlex; Fast.SetSource(ESource::Hand); Fast.SetGloveFlex(true,0); SelectAndArm(Fast);
    Q.Clear(); Q.Push(EInput::GloveFlex,.8f,5.01); Q.Push(EInput::GloveFlex,0,5.02);
    Q.Push(EInput::GloveFlex,.8f,5.13); Q.Push(EInput::GloveFlex,0,5.14);
    int P=0,R=0; auto Act=[&](EAction A){P+=A==EAction::Press;R+=A==EAction::Release;return true;};
    const auto Preview=PreviewBatch(Fast,Q,5.,5.2);
    assert(Preview.Valid && Preview.Presses==2 && Preview.Releases==2 && Preview.Ambiguous());
    int TargetA=0,TargetB=0;
    assert(SameHit(&TargetA,&TargetA));
    assert(!SameHit(&TargetA,&TargetB) && !SameHit(nullptr,&TargetA)); // never replay A's edge on B
    FEventQueue Targeted;
    Targeted.Push(EInput::Button,1,1,true,false,reinterpret_cast<std::uintptr_t>(&TargetA),true);
    Targeted.Push(EInput::Button,0,1.01,true,false,reinterpret_cast<std::uintptr_t>(&TargetA),true);
    assert(ButtonTargetsMatch(Targeted,&TargetA));
    assert(!ButtonTargetsMatch(Targeted,&TargetB)); // tap A, move to B before frame
    assert(!ButtonTargetsMatch(Targeted,nullptr));
    Targeted.Clear();
    Targeted.Push(EInput::Button,1,2,true,false,reinterpret_cast<std::uintptr_t>(&TargetB),true);
    assert(ButtonTargetsMatch(Targeted,&TargetB)); // press immediately after entering B
    Targeted.Push(EInput::Button,0,2.01,true,false,reinterpret_cast<std::uintptr_t>(&TargetA),true);
    assert(!ButtonTargetsMatch(Targeted,&TargetB)); // release moved away from press
    Targeted.Clear(); Targeted.Push(EInput::Button,1,3,true,false,0,true);
    assert(!ButtonTargetsMatch(Targeted,&TargetB)); // no hit at input time
    FPressState Entry; Entry.SetSource(ESource::AimGrip); Entry.Step(.1);
    assert(!CancelForHitChange(Entry.Sent,nullptr,&TargetA));
    Entry.SetButton(true); assert(Entry.Next()==EAction::Press);
    assert(CancelForHitChange(Entry.Sent,&TargetA,&TargetB));
    Entry.CancelActive(); Entry.ClearArms(); Entry.SetButton(false); assert(Entry.Next()==EAction::None);
    assert(Dispatch(Fast,Q,5.,5.2,Act,Ignore) && P==2 && R==2);
    std::cout << "PASS: controller and glove input, firing release guard, journal and ordered edges\n";
}
