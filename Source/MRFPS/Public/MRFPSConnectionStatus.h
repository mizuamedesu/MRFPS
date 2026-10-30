#pragma once

#include <algorithm>
#include <cmath>

// Process-local UI state: it contains neither tokens nor remote error strings.
// A successful code exchange only starts a connection; an authenticated,
// possessed client with a live connection is the evidence of actual join.
namespace MRFPSConnectionStatus
{
enum class ERole { Unknown, Player, Inspector, Admin };
enum class EPhase { Idle, Connecting, Joined, Failure };
enum class EFailure { None, Connection, Timeout, Version, Capacity, Authentication };

inline bool FocusAllowsVisibility(bool AppFocused,bool UsesVRFocus,bool HasVRFocus)
{
    return AppFocused && (!UsesVRFocus || HasVRFocus);
}

struct FState
{
    EPhase Phase=EPhase::Idle;
    ERole ExpectedRole=ERole::Unknown;
    EFailure Failure=EFailure::None;
    double VisibleSeconds=0;
    static constexpr double SuccessSeconds=5.;

    void Begin(ERole Role) { Phase=EPhase::Connecting; ExpectedRole=Role; Failure=EFailure::None; VisibleSeconds=0; }
    void Fail(EFailure Reason) { Phase=EPhase::Failure; Failure=Reason==EFailure::None?EFailure::Connection:Reason; VisibleSeconds=0; }
    void ClearFailure() { if (Phase==EPhase::Failure) { Phase=EPhase::Idle; Failure=EFailure::None; } }
    bool ObserveJoin(bool ClientWorld,bool OpenConnection,bool Authenticated,bool HasPawn,bool Admin,bool Inspector,int Slot)
    {
        const bool RoleMatches=ExpectedRole==ERole::Player ? !Admin && !Inspector && Slot>=0 && Slot<2 :
            ExpectedRole==ERole::Inspector ? !Admin && Inspector : ExpectedRole==ERole::Admin ? Admin && Inspector : false;
        if (Phase!=EPhase::Connecting || !ClientWorld || !OpenConnection || !Authenticated || !HasPawn || !RoleMatches) return false;
        Phase=EPhase::Joined; VisibleSeconds=0; return true;
    }
    bool ShowSuccess() const { return Phase==EPhase::Joined && VisibleSeconds<SuccessSeconds; }
    void AdvanceVisible(double DeltaSeconds,bool Focused,bool HudReady)
    {
        if (!ShowSuccess() || !Focused || !HudReady || !std::isfinite(DeltaSeconds) || DeltaSeconds<=0) return;
        // A suspended frame cannot consume the message while it was invisible.
        VisibleSeconds+=std::min(DeltaSeconds,.1);
    }
};
}
