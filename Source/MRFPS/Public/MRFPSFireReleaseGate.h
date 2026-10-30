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
    bool Locked=false,PhysicalDown=false,FlexSeen=false,ControllerSeen=false;
    bool NeedFreshFlex=false,NeedFreshController=false;
    double LastFlexAt=-1,LastControllerAt=-1,FlexFence=-1,ControllerFence=-1;
    FGloveNeutral Glove;
    void BeginLogin() { Locked=true; }
    void SetPhysical(bool Down) { PhysicalDown=Down; }
    void Observe(bool FlexValid,float Curl,double FlexAt,bool ControllerValid,bool Button,float Trigger,double ControllerAt)
    {
        if (FlexValid && std::isfinite(Curl) && std::isfinite(FlexAt) && FlexAt>=0) {
            FlexSeen=true; Glove.Observe(true,Curl,false,false,0);
            if (NeedFreshFlex && FlexAt>FlexFence && Curl<.30f) NeedFreshFlex=false;
            if (FlexAt>LastFlexAt) LastFlexAt=FlexAt;
        }
        if (ControllerValid && std::isfinite(Trigger) && std::isfinite(ControllerAt) && ControllerAt>=0) {
            ControllerSeen=true; Glove.Observe(false,0,true,Button,Trigger);
            if (NeedFreshController && ControllerAt>ControllerFence && !Button && Trigger<.30f) NeedFreshController=false;
            if (ControllerAt>LastControllerAt) LastControllerAt=ControllerAt;
        }
    }
    void SealForTravel()
    {
        Locked=true; NeedFreshFlex=FlexSeen; NeedFreshController=ControllerSeen;
        FlexFence=LastFlexAt; ControllerFence=LastControllerAt;
    }
    bool TryRearm(bool AutomationDown)
    {
        if (Locked && !NeedFreshFlex && !NeedFreshController && Glove.CanRearm(PhysicalDown,AutomationDown)) Locked=false;
        return !Locked;
    }
};
}
