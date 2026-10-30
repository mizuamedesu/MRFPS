#pragma once
#include <cmath>

// Remember a glove input held during a menu even if its stream goes stale.
// Only a fresh neutral reading can release that channel's latch.
namespace MRFPSFireRelease
{
struct FGloveNeutral
{
    bool FlexHeld=false,ControllerHeld=false;
    void Observe(bool FlexValid,float Curl,bool ControllerValid,bool Button,float Trigger)
    {
        if (FlexValid && std::isfinite(Curl)) FlexHeld=Curl>=.30f;
        if (ControllerValid && std::isfinite(Trigger)) ControllerHeld=Button || Trigger>=.30f;
    }
    bool CanRearm(bool PhysicalDown,bool AutomationDown) const
    {
        return !PhysicalDown && !AutomationDown && !FlexHeld && !ControllerHeld;
    }
};

// Owned by the game instance: the login pawn and controller can be destroyed
// by ClientTravel while the trigger that selected Join is still held.
struct FTransitionGate
{
    bool Login=false,PhysicalDown=false;
    bool PhysicalLocked=false,FlexLocked=false,ControllerLocked=false,AutomationLocked=false;
    double LastFlexAt=-1,LastControllerAt=-1,FlexFence=-1,ControllerFence=-1;
    void BeginLogin() { Login=true; }
    void SetPhysical(bool Down) { PhysicalDown=Down; if (!Login && !Down) PhysicalLocked=false; }
    void SetAutomation(bool Down) { if (!Login && !Down) AutomationLocked=false; }
    bool CanPhysical() const { return !Login && !PhysicalLocked; }
    bool CanFlex() const { return !Login && !FlexLocked; }
    bool CanController() const { return !Login && !ControllerLocked; }
    bool CanAutomation() const { return !Login && !AutomationLocked; }
    void Observe(bool FlexValid,float Curl,double FlexAt,bool ControllerValid,bool Button,float Trigger,double ControllerAt)
    {
        if (FlexValid && std::isfinite(Curl) && std::isfinite(FlexAt) && FlexAt>=0) {
            if (!Login && FlexLocked && FlexAt>FlexFence && Curl<.30f) FlexLocked=false;
            if (FlexAt>LastFlexAt) LastFlexAt=FlexAt;
        } else if (!Login) { FlexLocked=true; FlexFence=LastFlexAt; }
        if (ControllerValid && std::isfinite(Trigger) && std::isfinite(ControllerAt) && ControllerAt>=0) {
            if (!Login && ControllerLocked && ControllerAt>ControllerFence && !Button && Trigger<.30f) ControllerLocked=false;
            if (ControllerAt>LastControllerAt) LastControllerAt=ControllerAt;
        } else if (!Login) { ControllerLocked=true; ControllerFence=LastControllerAt; }
    }
    void SealForTravel()
    {
        Login=false; PhysicalLocked=PhysicalDown; FlexLocked=ControllerLocked=AutomationLocked=true;
        FlexFence=LastFlexAt; ControllerFence=LastControllerAt;
    }
};
}
