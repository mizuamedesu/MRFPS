#pragma once

#include "MRFPSConsentPolicy.h"

// This policy uses local session/lease state, not frame freshness, to decide
// whether capture must be retired. It never grants or dismisses OS consent.
namespace MRFPSVideoControl
{
struct FState
{
    bool Session = false;
    bool Error = false;
    bool ConsentPending = false;
    bool FlowBusy = false;
    bool Fresh = false;
    bool Requested = false;
    bool HadFrames = false;
    bool Queued = false;
    bool ServerStream = false;
    bool LocalPause = false;

    bool NeedsStop() const
    { return Session || Error || ConsentPending || FlowBusy || Requested || HadFrames || Queued || ServerStream || LocalPause; }
    bool NeedsServerStop() const
    { return Session || Requested || HadFrames || Queued || ServerStream; }
};

enum class EAction { AlreadyStopped, Stop, AwaitConsent, AlreadyLive, AwaitFrame, Start };
inline EAction Decide(bool Enabled, const FState& State)
{
    if (!Enabled) return State.NeedsStop() ? EAction::Stop : EAction::AlreadyStopped;
    if (State.ConsentPending || State.FlowBusy) return EAction::AwaitConsent;
    if (State.Session && !State.Error) return State.Fresh ? EAction::AlreadyLive : EAction::AwaitFrame;
    return EAction::Start;
}

enum class EStopEffect { RetireCapture, ResetLocalQueue, EndLocalPause, FinishServerPause, StopServerStream };

// One game-thread transition. Effects are requests; their asynchronous OS and
// server completion must be observed separately. This coordinator is shared
// by the live controller and a portable effects/lease test.
template<typename FApplyEffect>
bool ApplyStop(const FState& State, MRFPSConsent::FClientFlow& Flow, double Now, FApplyEffect&& ApplyEffect)
{
    if (!State.NeedsStop()) return false;
    const auto Plan=Flow.Stop(Now,State.LocalPause);
    ApplyEffect(EStopEffect::RetireCapture,0);
    ApplyEffect(EStopEffect::ResetLocalQueue,0);
    if (Plan.EndLocalPause) ApplyEffect(EStopEffect::EndLocalPause,0);
    if (Plan.SendFinish) ApplyEffect(EStopEffect::FinishServerPause,Plan.Request);
    if (State.NeedsServerStop()) ApplyEffect(EStopEffect::StopServerStream,0);
    return true;
}
}
