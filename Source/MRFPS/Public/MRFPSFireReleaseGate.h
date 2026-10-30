#pragma once

namespace MRFPSFireRelease
{
// The game instance survives ClientTravel while the trigger used to select
// Join may still be held. Require release before permitting gameplay fire.
struct FTransitionGate
{
    bool Login=false,PhysicalDown=false;
    bool PhysicalLocked=false,AutomationLocked=false;
    void BeginLogin() { Login=true; }
    void SetPhysical(bool Down) { PhysicalDown=Down; if (!Login && !Down) PhysicalLocked=false; }
    void SetAutomation(bool Down) { if (!Login && !Down) AutomationLocked=false; }
    bool CanPhysical() const { return !Login && !PhysicalLocked; }
    bool CanAutomation() const { return !Login && !AutomationLocked; }
    void SealForTravel() { Login=false; PhysicalLocked=PhysicalDown; AutomationLocked=true; }
};
}
