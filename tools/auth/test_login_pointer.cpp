#include "../../Source/MRFPS/Public/MRFPSLoginPressState.h"
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

    // Auto selection prefers fresh glove flex; late duplicates from every
    // other stream cannot generate a second click, however long they arrive.
    FPressState S; S.SetSource(ESource::Aim); S.SetAxis(0); S.SetGloveController(true,false,0); S.SetGloveFlex(true,0);
    SelectAndArm(S); assert(S.Selected==EInput::GloveFlex);
    S.SetGloveFlex(true,.56f); assert(S.Next()==EAction::Press);
    S.SetGloveFlex(true,.29f); assert(S.Next()==EAction::Release); S.Step(1.);
    S.SetButton(true); assert(S.Next()==EAction::None); S.SetButton(false); assert(S.Next()==EAction::None);
    S.SetAxis(1); assert(S.Next()==EAction::None); S.SetAxis(0); assert(S.Next()==EAction::None);
    S.SetGloveController(true,true,1); assert(S.Next()==EAction::None);
    S.SetGloveController(true,false,0); assert(S.Next()==EAction::None);
    S.SetGloveFlex(true,.55f); assert(S.Next()==EAction::None);
    S.SetGloveFlex(true,.56f); assert(S.Next()==EAction::Press);
    S.SetGloveFlex(true,.3f); assert(S.Next()==EAction::None);
    S.SetGloveFlex(true,.29f); assert(S.Next()==EAction::Release);

    // Losing the selected stream cancels, and an already-held fallback cannot
    // click until a fresh neutral and press. A non-selected loss has no effect.
    S.Step(.1); S.SetGloveFlex(true,.8f); assert(S.Next()==EAction::Press);
    S.SetGloveController(true,true,1); assert(S.Next()==EAction::None);
    S.SetGloveFlex(false,0); assert(S.Next()==EAction::Cancel && S.Selected==EInput::GloveController);
    S.Step(.2); assert(S.Next()==EAction::None);
    S.SetGloveController(true,false,0); S.Step(.1); S.SetGloveController(true,true,1); assert(S.Next()==EAction::Press);
    S.SetGloveController(false,false,0); assert(S.Next()==EAction::Cancel && S.Selected==EInput::Button);
    S.SetButton(false); S.Step(.1); S.SetButton(true); assert(S.Next()==EAction::Press);
    S.SetGloveFlex(false,0); assert(S.Next()==EAction::None);
    // Reconnection while held cancels Quest and cannot trigger another click.
    S.SetGloveFlex(true,.8f); assert(S.Next()==EAction::Cancel && S.Selected==EInput::GloveFlex);
    S.Step(.2); assert(S.Next()==EAction::None);
    S.SetGloveFlex(true,0); S.Step(.1); S.SetGloveFlex(true,.8f); assert(S.Next()==EAction::Press);
    S.SetGloveFlex(true,0); assert(S.Next()==EAction::Release);

    // An explicit mode can choose glove controller or Quest digital even when
    // redundant streams are fresh. Losing a forced source selects nothing.
    FPressState Forced; Forced.Mode=EMode::GloveController; Forced.SetSource(ESource::Hand);
    Forced.SetGloveController(true,false,0); Forced.SetGloveFlex(true,0); Forced.SetAxis(0);
    SelectAndArm(Forced); assert(Forced.Selected==EInput::GloveController);
    Forced.SetGloveController(true,true,0); assert(Forced.Next()==EAction::Press);
    Forced.SetGloveController(false,false,0); assert(Forced.Next()==EAction::Cancel && Forced.Selected==EInput::None);
    Forced.SetGloveController(true,true,0); assert(Forced.Next()==EAction::None);
    Forced.Step(.2); assert(Forced.Next()==EAction::None);
    Forced.SetGloveController(true,false,0); Forced.Step(.1); Forced.SetGloveController(true,true,0); assert(Forced.Next()==EAction::Press);
    Forced.Mode=EMode::QuestButton; assert(Forced.Next()==EAction::Cancel && Forced.Selected==EInput::Button);
    Forced.SetButton(false); Forced.Step(.1); Forced.SetButton(true); assert(Forced.Next()==EAction::Press);
    Forced.SetButton(false); assert(Forced.Next()==EAction::Release);

    // Focus/pose loss and a missing release cancel, never synthesize a click.
    Forced.SetButton(false); Forced.Step(.1); Forced.SetButton(true); assert(Forced.Next()==EAction::Press);
    Forced.SetFocused(false); assert(Forced.Next()==EAction::Cancel);
    Forced.SetFocused(true); assert(Forced.Next()==EAction::None); Forced.Step(.2); assert(Forced.Next()==EAction::None);
    Forced.SetButton(false); Forced.Step(.1); Forced.SetButton(true); assert(Forced.Next()==EAction::Press);
    Forced.Step(3.); assert(Forced.Next()==EAction::Cancel);
    Forced.SetButton(false); Forced.Step(.1); Forced.SetButton(true); assert(Forced.Next()==EAction::Press);
    Forced.SetSource(ESource::None); assert(Forced.Next()==EAction::Cancel);

    FEventQueue Q; for(int I=0;I<1000;++I) Q.Push(EInput::Axis,0,10.+I*.001);
    assert(!Q.Overflow && Q.Count==1 && Q.Events[0].At==10.);
    Q.Push(EInput::Axis,.7f,11.01); Q.Push(EInput::Axis,.8f,11.02); Q.Push(EInput::Axis,0,11.03); assert(Q.Count==3);
    Q.Clear(); for(int I=0;I<33;++I) Q.Push(EInput::Button,float(I%2),0); assert(Q.Overflow);
    Q.Clear(); Q.Push(EInput::Button,1,3); Q.Push(EInput::Button,0,2); assert(Q.Overflow);
    Q.Clear(); Q.Push(EInput::Axis,0,10); Q.Push(EInput::Axis,0,20); Q.Push(EInput::Button,1,15); assert(Q.Overflow);

    // The same journal event type goes through the production dispatcher.
    FPressState Fast; Fast.Mode=EMode::GloveFlex; Fast.SetSource(ESource::Hand); Fast.SetGloveFlex(true,0); SelectAndArm(Fast);
    Q.Clear(); Q.Push(EInput::GloveFlex,.8f,5.01); Q.Push(EInput::GloveFlex,0,5.02);
    Q.Push(EInput::GloveFlex,.8f,5.13); Q.Push(EInput::GloveFlex,0,5.14);
    int P=0,R=0; auto Act=[&](EAction A){P+=A==EAction::Press;R+=A==EAction::Release;return true;};
    auto Ignore=[](const FEventQueue::FEvent&){};
    const auto Preview=PreviewBatch(Fast,Q,5.,5.2);
    assert(Preview.Valid && Preview.Presses==2); // UI cancels this ambiguous batch before any click.
    int TargetA=0,TargetB=0;
    assert(SameHit(&TargetA,&TargetA));
    assert(!SameHit(&TargetA,&TargetB) && !SameHit(nullptr,&TargetA)); // never replay A's edge on B
    assert(Dispatch(Fast,Q,5.,5.2,Act,Ignore) && P==2 && R==2);
    std::cout << "PASS: bounded glove journal, single-source arbitration, cancellation and ordered edges\n";
}
