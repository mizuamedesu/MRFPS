#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace MRFPSLoginPointer
{
enum class ESource : std::uint8_t { None, Aim, Grip, Hand };
enum class EAction : std::uint8_t { None, Press, Release, Cancel };
enum class EInput : std::uint8_t { Button, Axis };

// Preserve ordered edges between widget ticks. Overflow never silently
// changes an in-progress click: the caller cancels and waits for release.
struct FEventQueue
{
    struct FEvent { EInput Kind; float Value; };
    std::array<FEvent,32> Events{};
    std::size_t Count=0;
    bool Overflow=false;
    void Push(EInput Kind,float Value)
    {
        if (Overflow) return;
        if (Count>=Events.size()) { Count=0; Overflow=true; return; }
        Events[Count++]={Kind,Value};
    }
    void Clear() { Count=0; Overflow=false; }
};

// The controller button and axis can both represent one physical trigger.
// Start disarmed and disarm on every pose switch/loss. A fresh, sustained
// release is required before a new press; tracking recovery never clicks.
struct FPressState
{
    ESource Source=ESource::None;
    bool Button=false,Axis=false,AxisValid=false,ButtonArmed=false,AxisArmed=false,Sent=false,Focused=true;
    bool ButtonContributed=false,AxisContributed=false;
    double ButtonReleasedFor=0,AxisReleasedFor=0;
    double PressedFor=0;

    void Disarm() { ButtonArmed=AxisArmed=false; AxisValid=false; ButtonReleasedFor=AxisReleasedFor=0; }

    void SetSource(ESource NewSource)
    {
        if (Source==NewSource) return;
        Source=NewSource; Disarm();
    }
    void SetFocused(bool Value)
    {
        if (Focused==Value) return;
        Focused=Value; Disarm();
    }
    void CancelActive() { Sent=false; ButtonContributed=AxisContributed=false; PressedFor=0; }
    void FailClosed()
    {
        CancelActive(); Button=true; Axis=true; AxisValid=false;
        ButtonArmed=AxisArmed=false; ButtonReleasedFor=AxisReleasedFor=0;
    }
    void SetButton(bool Down) { Button=Down; }
    void SetAxis(float Value)
    {
        if (!std::isfinite(Value)) { Axis=false; AxisValid=AxisArmed=false; AxisReleasedFor=0; return; }
        AxisValid=true;
        if (Value>=.6f) Axis=true; else if (Value<=.3f) Axis=false;
    }
    void Step(double DeltaSeconds)
    {
        const double Dt=std::clamp(std::isfinite(DeltaSeconds)?DeltaSeconds:0.,0.,.05);
        if (Sent && std::isfinite(DeltaSeconds) && DeltaSeconds>0) PressedFor+=DeltaSeconds;
        if (!Focused || Source==ESource::None) { ButtonArmed=AxisArmed=false; ButtonReleasedFor=AxisReleasedFor=0; return; }
        // Treat the two channels independently: a lost digital release must
        // not prevent a fresh neutral/press sequence on a working axis (or
        // vice versa). The stale channel itself remains disarmed.
        if (!Button) { ButtonReleasedFor+=Dt; if (ButtonReleasedFor>=.1) ButtonArmed=true; }
        else ButtonReleasedFor=0;
        if (AxisValid && !Axis) { AxisReleasedFor+=Dt; if (AxisReleasedFor>=.1) AxisArmed=true; }
        else AxisReleasedFor=0;
    }
    EAction Next()
    {
        if (!Focused || Source==ESource::None) {
            if (!Sent) return EAction::None;
            CancelActive(); return EAction::Cancel;
        }
        if (Sent && PressedFor>=3.) {
            CancelActive(); ButtonArmed=AxisArmed=false; ButtonReleasedFor=AxisReleasedFor=0;
            return EAction::Cancel;
        }
        if (Sent) {
            ButtonContributed|=ButtonArmed && Button;
            AxisContributed|=AxisArmed && AxisValid && Axis;
            if (AxisContributed && !AxisValid) {
                CancelActive(); ButtonArmed=AxisArmed=false; ButtonReleasedFor=AxisReleasedFor=0;
                return EAction::Cancel;
            }
            if ((ButtonContributed && !Button) || (AxisContributed && AxisValid && !Axis)) {
                CancelActive(); ButtonArmed=AxisArmed=false; ButtonReleasedFor=AxisReleasedFor=0;
                return EAction::Release;
            }
            return EAction::None;
        }
        if ((ButtonArmed && Button) || (AxisArmed && AxisValid && Axis)) {
            Sent=true; PressedFor=0; ButtonContributed=ButtonArmed && Button; AxisContributed=AxisArmed && AxisValid && Axis;
            return EAction::Press;
        }
        return EAction::None;
    }
};
}
