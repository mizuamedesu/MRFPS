#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace MRFPSLoginPointer
{
enum class ESource : std::uint8_t { None, Aim, Grip, Hand };

// The controller button and axis can both represent one physical trigger.
// Start disarmed and disarm on every pose switch/loss. A fresh, sustained
// release is required before a new press; tracking recovery never clicks.
struct FPressState
{
    ESource Source=ESource::None;
    bool Button=false,Axis=false,AxisValid=false,ButtonArmed=false,AxisArmed=false,Sent=false;
    double ButtonReleasedFor=0,AxisReleasedFor=0;

    void SetSource(ESource NewSource)
    {
        if (Source==NewSource) return;
        Source=NewSource; ButtonArmed=AxisArmed=false; AxisValid=false;
        ButtonReleasedFor=AxisReleasedFor=0;
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
        if (Source==ESource::None) { ButtonArmed=AxisArmed=false; ButtonReleasedFor=AxisReleasedFor=0; return; }
        // Treat the two channels independently: a lost digital release must
        // not prevent a fresh neutral/press sequence on a working axis (or
        // vice versa). The stale channel itself remains disarmed.
        if (!Button) { ButtonReleasedFor+=Dt; if (ButtonReleasedFor>=.1) ButtonArmed=true; }
        else ButtonReleasedFor=0;
        if (AxisValid && !Axis) { AxisReleasedFor+=Dt; if (AxisReleasedFor>=.1) AxisArmed=true; }
        else AxisReleasedFor=0;
    }
    bool Desired() const
    {
        return Source!=ESource::None && ((ButtonArmed && Button) || (AxisArmed && Axis));
    }
};
}
