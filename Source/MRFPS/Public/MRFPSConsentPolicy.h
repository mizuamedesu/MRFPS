#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// Pure state used by the live connection/controller and the portable regressions.
// It grants neither authentication nor OS capture permission.
namespace MRFPSConsent
{
constexpr double MaximumPauseSeconds = 180.0;
constexpr double AcknowledgementSeconds = 5.0;
// AllowedPauseSeconds consumes integer UTC seconds. Subtract a whole second so
// the client's deadline also precedes the signed expiry within that UTC second.
constexpr double ExpiryRoundingMarginSeconds = 1.0;

inline double AllowedPauseSeconds(std::int64_t NowUtc,std::int64_t AuthExpiresUtc)
{
    if (NowUtc<0 || AuthExpiresUtc<=NowUtc) return 0;
    return std::min(MaximumPauseSeconds,double(AuthExpiresUtc-NowUtc));
}

class FPauseLease
{
public:
    double Begin(double Now,double RequestedSeconds)
    {
        if (!std::isfinite(Now) || Now<0 || !std::isfinite(RequestedSeconds) || RequestedSeconds<=0) return 0;
        return BeginUntil(Now,Now+std::min(RequestedSeconds,MaximumPauseSeconds));
    }
    double BeginUntil(double Now,double RequestedDeadline)
    {
        if (!std::isfinite(Now) || Now<0 || !std::isfinite(RequestedDeadline) || RequestedDeadline<=Now) return 0;
        const double BoundedDeadline=std::min(RequestedDeadline,Now+MaximumPauseSeconds);
        // Repeated requests cannot renew an already outstanding pause.
        if (IsActive(Now)) Deadline=std::min(Deadline,BoundedDeadline);
        else Deadline=BoundedDeadline;
        return Deadline-Now;
    }
    void End() { Deadline=0; }
    bool IsActive(double Now) const { return std::isfinite(Now) && Now>=0 && Deadline>Now; }
    double Timeout(double Now,double LastReceive,double OrdinaryTimeout) const
    {
        if (!IsActive(Now) || !std::isfinite(LastReceive)) return OrdinaryTimeout;
        return std::max(OrdinaryTimeout,Deadline-LastReceive);
    }
private:
    double Deadline=0;
};

enum class EPhase : std::uint8_t { Idle, AwaitingGrant, AwaitingConsent, AwaitingResumeAck };
struct FStopPlan
{
    std::uint32_t Request = 0;
    bool SendFinish = false;
    bool EndLocalPause = false;
    bool WaitForAck = false;
};
class FClientFlow
{
public:
    bool Begin(double Now)
    {
        if (!std::isfinite(Now) || Now<0 || Phase!=EPhase::Idle || Request==std::numeric_limits<std::uint32_t>::max()) return false;
        ++Request; Phase=EPhase::AwaitingGrant; RequestAt=Now; HardDeadline=0; Deadline=Now+AcknowledgementSeconds; return true;
    }
    bool Grant(std::uint32_t Id,double Seconds,double Now,double AuthRemainingSeconds)
    {
        if (Phase!=EPhase::AwaitingGrant || Id!=Request || !std::isfinite(Now) || Now<RequestAt || !std::isfinite(Seconds) ||
            Seconds<=0 || !std::isfinite(AuthRemainingSeconds) || AuthRemainingSeconds<=0 || Now>=Deadline) return false;
        // The full request round trip is conservative: its start precedes the
        // server lease's start, without requiring synchronized platform clocks.
        const double Remaining=std::min(std::min(Seconds,MaximumPauseSeconds)-(Now-RequestAt),AuthRemainingSeconds)-ExpiryRoundingMarginSeconds;
        if (Remaining<=AcknowledgementSeconds) return false;
        HardDeadline=Now+Remaining;
        Phase=EPhase::AwaitingConsent; Deadline=HardDeadline-AcknowledgementSeconds; return true;
    }
    bool Finish(double Now)
    {
        if (Phase!=EPhase::AwaitingConsent || !std::isfinite(Now) || Now<RequestAt || Now>=HardDeadline) return false;
        Phase=EPhase::AwaitingResumeAck; Deadline=std::min(Now+AcknowledgementSeconds,HardDeadline); return true;
    }
    bool Resume(std::uint32_t Id,double Now)
    {
        if (Phase!=EPhase::AwaitingResumeAck || Id!=Request || !std::isfinite(Now) || Now<RequestAt || Now>=Deadline) return false;
        Abort(); return true;
    }
    // Stop never opens or dismisses OS consent. A granted lease remains active
    // locally until a real resume ACK, unless the bounded deadline has expired.
    FStopPlan Stop(double Now, bool LocalPauseActive)
    {
        FStopPlan Plan; Plan.Request=Request;
        if (Phase==EPhase::Idle) {
            Plan.SendFinish=LocalPauseActive && Request!=0;
            Plan.EndLocalPause=true;
            return Plan;
        }
        Plan.SendFinish=Request!=0;
        if (Phase==EPhase::AwaitingConsent && Finish(Now)) { Plan.WaitForAck=true; return Plan; }
        if (Phase==EPhase::AwaitingResumeAck && !IsExpired(Now)) { Plan.WaitForAck=true; return Plan; }
        Abort(); Plan.EndLocalPause=true; return Plan;
    }
    void Abort() { Phase=EPhase::Idle; Deadline=HardDeadline=RequestAt=0; }
    bool IsExpired(double Now) const { return Phase!=EPhase::Idle && (!std::isfinite(Now) || Now<RequestAt || Now>=Deadline); }
    bool IsBusy() const { return Phase!=EPhase::Idle; }
    EPhase GetPhase() const { return Phase; }
    std::uint32_t GetRequest() const { return Request; }
    double GetHardDeadline() const { return HardDeadline; }
private:
    EPhase Phase=EPhase::Idle;
    std::uint32_t Request=0;
    double Deadline=0,HardDeadline=0,RequestAt=0;
};
}
