#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace MRFPSLoginPointer
{
enum class ESource : std::uint8_t { None, Aim, Grip, Hand };
enum class EAction : std::uint8_t { None, Press, Release, Cancel };
enum class EInput : std::uint8_t { Button, Axis, GloveController, GloveFlex, None };
enum class EMode : std::uint8_t { Auto, QuestButton, QuestAxis, GloveController, GloveFlex };

// Preserve transitions, rather than hundreds of identical axis samples. The
// first neutral sample keeps its timestamp so the required rest is measurable.
struct FEventQueue
{
    struct FEvent { EInput Kind; float Value; double At; bool Valid=true, Flag=false; };
    std::array<FEvent,32> Events{};
    std::size_t Count=0;
    bool Overflow=false;
    double LastAt=-1;
    static int Category(const FEvent& E)
    {
        if (!E.Valid || !std::isfinite(E.Value)) return -1;
        if (E.Kind==EInput::Button) return E.Value!=0?2:0;
        if (E.Kind==EInput::GloveController && E.Flag) return 2;
        const float Down=E.Kind==EInput::Axis?.6f:.55f;
        if (E.Kind==EInput::Axis ? E.Value>=Down : E.Value>Down) return 2;
        if (E.Kind==EInput::Axis ? E.Value<=.3f : E.Value<.3f) return 0;
        return 1;
    }
    void Push(EInput Kind,float Value,double At,bool Valid=true,bool Flag=false)
    {
        if (Overflow) return;
        if (!std::isfinite(At) || At<0 || (LastAt>=0 && At<LastAt)) { Count=0; Overflow=true; return; }
        LastAt=At;
        const FEvent New{Kind,Value,At,Valid,Flag};
        // Interleaved events from other channels do not change this channel.
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

// Only one redundant representation drives the pointer at a time. There is
// no shared operation ID across Quest and DivingStation, so ORing them can
// turn a late duplicate into another click even after an arbitrary timeout.
struct FPressState
{
    ESource Source=ESource::None;
    EMode Mode=EMode::Auto;
    EInput Selected=EInput::None;
    bool Button=false,Axis=false,AxisValid=false,ButtonArmed=false,AxisArmed=false,Sent=false,Focused=true;
    bool GloveController=false,GloveControllerValid=false,GloveControllerArmed=false;
    bool GloveFlex=false,GloveFlexValid=false,GloveFlexArmed=false;
    bool ButtonContributed=false,AxisContributed=false,GloveControllerContributed=false,GloveFlexContributed=false;
    double ButtonReleasedFor=0,AxisReleasedFor=0,GloveControllerReleasedFor=0,GloveFlexReleasedFor=0,PressedFor=0;

    void ClearArms()
    {
        ButtonArmed=AxisArmed=GloveControllerArmed=GloveFlexArmed=false;
        ButtonReleasedFor=AxisReleasedFor=GloveControllerReleasedFor=GloveFlexReleasedFor=0;
    }
    void Disarm() { ClearArms(); AxisValid=GloveControllerValid=GloveFlexValid=false; Selected=EInput::None; }
    void SetSource(ESource NewSource) { if (Source!=NewSource) { Source=NewSource; Disarm(); } }
    void SetFocused(bool Value) { if (Focused!=Value) { Focused=Value; Disarm(); } }
    void CancelActive()
    {
        Sent=false; ButtonContributed=AxisContributed=GloveControllerContributed=GloveFlexContributed=false; PressedFor=0;
    }
    void FailClosed() { CancelActive(); Button=Axis=GloveController=GloveFlex=true; Disarm(); }
    void SetButton(bool Down) { Button=Down; }
    void SetAxis(float Value)
    {
        if (!std::isfinite(Value)) { AxisValid=AxisArmed=false; AxisReleasedFor=0; return; }
        AxisValid=true;
        if (Value>=.6f) Axis=true; else if (Value<=.3f) Axis=false;
    }
    void SetGloveController(bool Valid,bool ButtonDown,float Value)
    {
        if (!Valid || !std::isfinite(Value)) { GloveControllerValid=GloveControllerArmed=false; GloveControllerReleasedFor=0; return; }
        GloveControllerValid=true;
        if (ButtonDown || Value>.55f) GloveController=true; else if (Value<.3f) GloveController=false;
    }
    void SetGloveFlex(bool Valid,float Curl)
    {
        if (!Valid || !std::isfinite(Curl)) { GloveFlexValid=GloveFlexArmed=false; GloveFlexReleasedFor=0; return; }
        GloveFlexValid=true;
        if (Curl>.55f) GloveFlex=true; else if (Curl<.3f) GloveFlex=false;
    }
    EInput Choose() const
    {
        switch (Mode) {
            case EMode::QuestButton: return EInput::Button;
            case EMode::QuestAxis: return AxisValid?EInput::Axis:EInput::None;
            case EMode::GloveController: return GloveControllerValid?EInput::GloveController:EInput::None;
            case EMode::GloveFlex: return GloveFlexValid?EInput::GloveFlex:EInput::None;
            // UE invokes mapped axis callbacks with zero even without an axis
            // device. That callback is not evidence that the axis exists.
            default: return GloveFlexValid?EInput::GloveFlex:GloveControllerValid?EInput::GloveController:EInput::Button;
        }
    }
    void Step(double DeltaSeconds)
    {
        const double Raw=std::isfinite(DeltaSeconds)?std::max(0.,DeltaSeconds):0.;
        const double Dt=std::min(Raw,.1);
        if (Sent) PressedFor+=Raw;
        if (!Focused || Source==ESource::None) { ClearArms(); return; }
        const auto Arm=[Dt](bool Down,bool Valid,bool& Armed,double& ReleasedFor) {
            if (Valid && !Down) { ReleasedFor+=Dt; if (ReleasedFor>=.1-1e-9) Armed=true; }
            else ReleasedFor=0;
        };
        Arm(Button,true,ButtonArmed,ButtonReleasedFor);
        Arm(Axis,AxisValid,AxisArmed,AxisReleasedFor);
        Arm(GloveController,GloveControllerValid,GloveControllerArmed,GloveControllerReleasedFor);
        Arm(GloveFlex,GloveFlexValid,GloveFlexArmed,GloveFlexReleasedFor);
    }
    EAction Next()
    {
        if (!Focused || Source==ESource::None) {
            const bool Cancel=Sent; CancelActive(); Selected=EInput::None; ClearArms();
            return Cancel?EAction::Cancel:EAction::None;
        }
        const auto Preferred=Choose();
        if (Preferred!=Selected) {
            const bool Cancel=Sent; CancelActive(); ClearArms(); Selected=Preferred;
            return Cancel?EAction::Cancel:EAction::None;
        }
        if (Sent && PressedFor>=3.) { CancelActive(); ClearArms(); return EAction::Cancel; }
        bool Down=false,Armed=false;
        switch (Selected) {
            case EInput::Button: Down=Button; Armed=ButtonArmed; break;
            case EInput::Axis: Down=Axis; Armed=AxisArmed; break;
            case EInput::GloveController: Down=GloveController; Armed=GloveControllerArmed; break;
            case EInput::GloveFlex: Down=GloveFlex; Armed=GloveFlexArmed; break;
            default: break;
        }
        if (Sent) {
            if (!Down) { CancelActive(); ClearArms(); return EAction::Release; }
            return EAction::None;
        }
        if (Down && Armed) {
            Sent=true; PressedFor=0;
            ButtonContributed=Selected==EInput::Button; AxisContributed=Selected==EInput::Axis;
            GloveControllerContributed=Selected==EInput::GloveController; GloveFlexContributed=Selected==EInput::GloveFlex;
            return EAction::Press;
        }
        return EAction::None;
    }
};

// Use event time, not frame time, to re-arm between distinct taps. Invalid or
// out-of-order timestamps fail closed before any pointer action is emitted.
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
    Cursor=Begin;
    for (std::size_t I=0;I<Queue.Count;++I) {
        const auto& E=Queue.Events[I];
        State.Step(E.At-Cursor);
        if (!Action(State.Next())) return true;
        if (E.Kind==EInput::Button) State.SetButton(E.Value!=0);
        else if (E.Kind==EInput::Axis) State.SetAxis(E.Value);
        else if (E.Kind==EInput::GloveController) State.SetGloveController(E.Valid,E.Flag,E.Value);
        else State.SetGloveFlex(E.Valid,E.Value);
        Input(E);
        if (!Action(State.Next())) return true;
        Cursor=E.At;
    }
    State.Step(End-Cursor); Action(State.Next());
    return true;
}

// Without a pose for each individual edge, multiple queued presses cannot
// safely be replayed against only the newest hit. Preview before interacting.
struct FBatchPreview { FPressState Final; unsigned Presses=0; bool Valid=false; };
inline FBatchPreview PreviewBatch(const FPressState& Initial,const FEventQueue& Queue,double Begin,double End)
{
    FBatchPreview Preview; Preview.Final=Initial;
    Preview.Valid=Dispatch(Preview.Final,Queue,Begin,End,
        [&Preview](EAction A) { if (A==EAction::Press) ++Preview.Presses; return true; },
        [](const FEventQueue::FEvent&) {});
    return Preview;
}

}
