#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace MRFPSLoginPointer
{
enum class ESource : std::uint8_t { None, Aim, Grip, Hand, AimGrip };
enum class EAction : std::uint8_t { None, Press, Release, Cancel };
enum class EInput : std::uint8_t { Button, Axis, None };
enum class EMode : std::uint8_t { Auto, QuestButton, QuestAxis };

struct FEventQueue
{
    struct FEvent {
        EInput Kind; float Value; double At; bool Valid=true, Flag=false;
        std::uintptr_t Target=0; bool TargetCaptured=false;
    };
    std::array<FEvent,32> Events{};
    std::size_t Count=0;
    bool Overflow=false;
    double LastAt=-1;
    static int Category(const FEvent& E)
    {
        if (!E.Valid || !std::isfinite(E.Value)) return -1;
        if (E.Kind==EInput::Button) return E.Value!=0?2:0;
        return E.Value>=.6f?2:E.Value<=.3f?0:1;
    }
    void Push(EInput Kind,float Value,double At,bool Valid=true,bool Flag=false,std::uintptr_t Target=0,bool TargetCaptured=false)
    {
        if (Overflow) return;
        if (!std::isfinite(At) || At<0 || (LastAt>=0 && At<LastAt)) { Count=0; Overflow=true; return; }
        LastAt=At;
        const FEvent New{Kind,Value,At,Valid,Flag,Target,TargetCaptured};
        for (std::size_t I=Count;I>0;--I) if (Events[I-1].Kind==Kind) {
            if (Category(Events[I-1])==Category(New)) return;
            break;
        }
        if (Count>=Events.size()) { Count=0; Overflow=true; return; }
        Events[Count++]=New;
    }
    void Clear() { Count=0; Overflow=false; LastAt=-1; }
};

inline bool SameHit(const void* Previous,const void* Current) { return Previous && Previous==Current; }
inline bool CapturedTargetMatches(bool Captured,std::uintptr_t Target,const void* Current)
{ return Captured && Target!=0 && Target==reinterpret_cast<std::uintptr_t>(Current); }
inline bool ButtonTargetsMatch(const FEventQueue& Queue,const void* Current)
{
    for (std::size_t I=0;I<Queue.Count;++I) {
        const auto& E=Queue.Events[I];
        if (E.Kind==EInput::Button && !CapturedTargetMatches(E.TargetCaptured,E.Target,Current)) return false;
    }
    return true;
}
inline bool CancelForHitChange(bool Pressed,const void* Previous,const void* Current)
{ return Pressed && !SameHit(Previous,Current); }

struct FPressState
{
    ESource Source=ESource::None;
    EMode Mode=EMode::Auto;
    EInput Selected=EInput::None, Previous=EInput::None;
    bool Button=false,Axis=false,AxisValid=false,ButtonArmed=false,AxisArmed=false,Sent=false,Focused=true;
    double ButtonReleasedFor=0,AxisReleasedFor=0,PressedFor=0,SinceRelease=0;
    void ClearArms() { ButtonArmed=AxisArmed=false; ButtonReleasedFor=AxisReleasedFor=0; }
    void ClearArm(EInput Kind)
    {
        if (Kind==EInput::Button) { ButtonArmed=false; ButtonReleasedFor=0; }
        else if (Kind==EInput::Axis) { AxisArmed=false; AxisReleasedFor=0; }
    }
    void Disarm() { ClearArms(); AxisValid=false; Selected=Previous=EInput::None; }
    void SetSource(ESource Value) { if (Source!=Value) { Source=Value; Disarm(); } }
    void SetFocused(bool Value) { if (Focused!=Value) { Focused=Value; Disarm(); } }
    void CancelActive() { Sent=false; Selected=EInput::None; PressedFor=0; }
    void FailClosed() { CancelActive(); Button=Axis=true; Disarm(); }
    void SetButton(bool Down) { Button=Down; }
    void SetAxis(float Value)
    {
        if (!std::isfinite(Value)) { AxisValid=AxisArmed=false; AxisReleasedFor=0; return; }
        AxisValid=true;
        if (Value>=.6f) Axis=true; else if (Value<=.3f) Axis=false;
    }
    bool Allowed(EInput Kind) const
    { return Mode==EMode::QuestAxis ? Kind==EInput::Axis : Kind==EInput::Button; }
    void Step(double DeltaSeconds)
    {
        const double Raw=std::isfinite(DeltaSeconds)?std::max(0.,DeltaSeconds):0.;
        const double Dt=std::min(Raw,.1);
        if (Sent) PressedFor+=Raw;
        if (Previous!=EInput::None) SinceRelease+=Raw;
        if (!Focused || Source==ESource::None) { ClearArms(); return; }
        const auto Arm=[Dt](bool Down,bool Valid,bool& Armed,double& ReleasedFor) {
            if (Valid && !Down) { ReleasedFor+=Dt; if (ReleasedFor>=.1-1e-9) Armed=true; }
            else ReleasedFor=0;
        };
        Arm(Button,true,ButtonArmed,ButtonReleasedFor);
        Arm(Axis,AxisValid,AxisArmed,AxisReleasedFor);
    }
    EAction Next()
    {
        if (!Focused || Source==ESource::None) {
            const bool Cancel=Sent; CancelActive(); ClearArms(); return Cancel?EAction::Cancel:EAction::None;
        }
        if (Sent && PressedFor>=3.) { CancelActive(); ClearArms(); return EAction::Cancel; }
        if (Sent) {
            const bool Valid=Allowed(Selected) && (Selected==EInput::Button || (Selected==EInput::Axis && AxisValid));
            if (!Valid) { const auto Old=Selected; CancelActive(); ClearArm(Old); return EAction::Cancel; }
            const bool Down=Selected==EInput::Button?Button:Axis;
            if (!Down) { Previous=Selected; SinceRelease=0; CancelActive(); ClearArms(); return EAction::Release; }
            return EAction::None;
        }
        const bool B=Allowed(EInput::Button) && ButtonArmed && Button;
        const bool A=Allowed(EInput::Axis) && AxisValid && AxisArmed && Axis;
        if (B || A) { Selected=B?EInput::Button:EInput::Axis; Sent=true; PressedFor=0; return EAction::Press; }
        return EAction::None;
    }
};

template<typename FAction,typename FInput>
bool Dispatch(FPressState& State,const FEventQueue& Queue,double Begin,double End,FAction Action,FInput Input)
{
    if (Queue.Overflow || !std::isfinite(Begin) || !std::isfinite(End) || Begin>End) return false;
    double Cursor=Begin;
    for (std::size_t I=0;I<Queue.Count;++I) {
        const auto& E=Queue.Events[I];
        if (!std::isfinite(E.At) || E.At<Cursor || E.At>End) return false;
        Cursor=E.At;
    }
    const auto Drain=[&]() {
        for (int I=0;I<3;++I) { const auto Next=State.Next(); if (Next==EAction::None) return true; if (!Action(Next)) return false; }
        return true;
    };
    Cursor=Begin;
    for (std::size_t I=0;I<Queue.Count;++I) {
        const auto& E=Queue.Events[I]; State.Step(E.At-Cursor);
        if (!Drain()) return true;
        if (E.Kind==EInput::Button) State.SetButton(E.Value!=0); else State.SetAxis(E.Value);
        Input(E); if (!Drain()) return true; Cursor=E.At;
    }
    State.Step(End-Cursor); Drain(); return true;
}

struct FBatchPreview
{
    FPressState Final; unsigned Presses=0,Releases=0; bool Valid=false;
    bool Ambiguous() const { return Releases+(Final.Sent?1u:0u)>1u; }
};
inline FBatchPreview PreviewBatch(const FPressState& Initial,const FEventQueue& Queue,double Begin,double End)
{
    FBatchPreview Preview; Preview.Final=Initial;
    Preview.Valid=Dispatch(Preview.Final,Queue,Begin,End,
        [&Preview](EAction A) { if (A==EAction::Press) ++Preview.Presses; else if (A==EAction::Release) ++Preview.Releases; return true; },
        [](const FEventQueue::FEvent&) {});
    return Preview;
}
}
