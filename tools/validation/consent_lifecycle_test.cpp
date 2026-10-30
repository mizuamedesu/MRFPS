#include "../../Source/MRFPS/Public/MRFPSConsentPolicy.h"
#include "../../Source/MRFPS/Public/MRFPSVideoControlPolicy.h"
#include "../../Plugins/MRFPSAuth/Source/MRFPSAuth/Public/MRFPSAuthClientUsePolicy.h"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
int Checks=0;
void Check(bool Value,const char* Name) { ++Checks; if(!Value) throw std::runtime_error(Name); }
}
int main()
{
    try {
        using namespace MRFPSConsent;
        Check(AllowedPauseSeconds(100,100)==0,"expired authorization cannot grant a pause");
        Check(AllowedPauseSeconds(100,99)==0,"past authorization cannot grant a pause");
        Check(AllowedPauseSeconds(100,120)==20,"pause is bounded by signed authorization expiry");
        Check(AllowedPauseSeconds(100,1000)==180,"pause has a hard 180 second cap");
        FPauseLease Lease;
        Check(Lease.Timeout(100,100,30)==30,"ordinary connections retain ordinary timeout");
        Check(Lease.Begin(100,10000)==180,"oversized lease cannot exceed cap");
        Check(60<Lease.Timeout(160,100,30),"a real 60 second permission pause survives the normal 30 second cutoff");
        for(int I=1;I<=100;++I) Lease.Begin(100+I*.1,180);
        Check(Lease.Timeout(200,100,30)==180,"repeated pause requests never extend the original deadline");
        Check(Lease.IsActive(279.9),"lease remains active just before deadline");
        Check(!Lease.IsActive(280),"lease expires at the hard deadline");
        Check(Lease.Timeout(280,100,30)==30,"expired lease restores ordinary timeout");
        Lease.End(); Lease.Begin(100,180);
        Check(Lease.Begin(150,10)==10,"updated authorization may shorten but never lengthen a lease");
        Check(!Lease.IsActive(160),"shortened authorization deadline is retained");
        Lease.End();
        Check(Lease.Timeout(151,150,30)==30,"resume with a real received packet needs no timeout extension");
        Check(Lease.Begin(-1,180)==0,"invalid negative clock cannot grant a lease");
        Check(Lease.Begin(1,std::numeric_limits<double>::quiet_NaN())==0,"nonfinite duration cannot grant a lease");

        FClientFlow Flow;
        Check(Flow.Begin(100),"explicit request begins one control exchange");
        const auto First=Flow.GetRequest();
        Check(Flow.IsBusy() && Flow.GetPhase()==EPhase::AwaitingGrant,"publication is blocked before server ACK");
        Check(!Flow.Begin(101),"repeated input cannot spam pending consent control");
        Check(!Flow.Grant(First+1,180,100.1,1000),"unmatched server ACK cannot open consent");
        Check(!Flow.Finish(100.1),"consent cannot finish before its grant");
        Check(Flow.Grant(First,180,100.1,1000),"matching timely server ACK permits the OS request");
        Check(Flow.IsBusy() && !Flow.IsExpired(160),"a suspended game thread preserves the bounded consent state");
        Check(!Flow.Grant(First,180,160,1000),"duplicate grant cannot renew or reopen OS consent");
        Check(Flow.Finish(160),"a resolved OS result requests a real resume round trip");
        Check(Flow.IsBusy() && Flow.GetPhase()==EPhase::AwaitingResumeAck,"frames remain blocked until resume ACK");
        Check(!Flow.Resume(First+1,160.1),"old or mismatched resume cannot release the frame gate");
        Check(Flow.Resume(First,160.1),"current real resume ACK releases the frame gate");
        Check(!Flow.IsBusy(),"resume completed");
        Check(!Flow.Grant(First,180,160.2,1000),"late grant cannot resurrect retired OS consent");
        Check(Flow.Begin(170),"a later explicit request is independent");
        const auto Second=Flow.GetRequest();
        Check(Second>First,"request generations are monotonic");
        Check(!Flow.Grant(First,180,170.1,1000),"previous request cannot affect current consent");
        Flow.Abort();
        Check(!Flow.Grant(Second,180,170.2,1000),"disconnect abort fences a delayed consent grant");
        Check(Flow.Begin(180),"fresh request after abort is possible");
        const auto Third=Flow.GetRequest();
        Check(Flow.IsExpired(185.1),"missing server ACK is bounded");
        Check(!Flow.Grant(Third,180,185.1,1000),"late grant cannot open a timed-out dialog");
        Flow.Abort(); Flow.Begin(200); Flow.Grant(Flow.GetRequest(),180,200.1,1000); Flow.Finish(210);
        Check(!Flow.Resume(Flow.GetRequest(),215.1),"late resume ACK cannot silently bypass its control deadline");
        Flow.Abort(); Flow.Begin(300); Flow.Grant(Flow.GetRequest(),10000,300.1,1000);
        Check(Flow.IsExpired(480.2),"client also caps a server-provided duration");

        // Model asymmetric network delays, rather than treating grant receipt
        // as the lease's start. The old implementation overran every downlink.
        for (double Delay : {1.0,2.0,3.0,4.0,4.999}) {
            FClientFlow Delayed;
            Check(Delayed.Begin(100),"delayed exchange starts");
            Check(Delayed.Grant(Delayed.GetRequest(),180,100+Delay,1000),"a grant within five seconds may open consent");
            Check(std::abs(Delayed.GetHardDeadline()-279)<1e-8,"full RTT and UTC margin are removed from the hard deadline");
            for (double Uplink : {0.0,Delay*.5,Delay}) {
                const double ServerDeadline=100+Uplink+180;
                Check(Delayed.GetHardDeadline()<ServerDeadline,"client hard deadline precedes server deadline for either delay direction");
            }
            Check(Delayed.IsExpired(274),"consent stops five seconds before the hard deadline");
            Check(!Delayed.IsExpired(273.999),"consent still has its remaining bounded time");
        }
        FClientFlow ExactControlLimit;
        ExactControlLimit.Begin(100);
        Check(!ExactControlLimit.Grant(ExactControlLimit.GetRequest(),180,105,1000),"grant at the exact control deadline fails closed");

        FClientFlow ShortTtl;
        ShortTtl.Begin(100);
        Check(ShortTtl.Grant(ShortTtl.GetRequest(),180,101,10),"local signed TTL can shorten the server grant");
        Check(ShortTtl.GetHardDeadline()==110,"UTC rounding margin precedes the ten-second signed TTL boundary");
        Check(ShortTtl.IsExpired(105),"TTL-limited consent reserves five seconds for resume ACK");
        FPauseLease ClientLease;
        Check(ClientLease.BeginUntil(101.25,ShortTtl.GetHardDeadline())==8.75,"connection receives the absolute hard deadline without restarting its duration");
        Check(ClientLease.IsActive(109.999),"connection remains protected while the resumed app awaits its ACK");
        Check(!ClientLease.IsActive(110),"connection lease cannot outlive the conservative TTL boundary");
        Check(ShortTtl.Finish(107),"late consent within hard deadline can release the server lease");
        FClientFlow BeforeBoundary=ShortTtl;
        Check(BeforeBoundary.Resume(BeforeBoundary.GetRequest(),109.999),"a timely real resume ACK releases the frame gate");
        Check(!ShortTtl.Resume(ShortTtl.GetRequest(),110),"resume timeout is clamped to hard deadline instead of now plus five");
        Check(ShortTtl.IsBusy(),"a rejected late ACK cannot release the publication gate");

        for (double Ttl : {0.0,1.0,5.0,6.0}) {
            FClientFlow Insufficient;
            Insufficient.Begin(100);
            Check(!Insufficient.Grant(Insufficient.GetRequest(),180,101,Ttl),"insufficient signed TTL cannot open OS consent");
        }
        FClientFlow ShortServerGrant;
        ShortServerGrant.Begin(100);
        Check(!ShortServerGrant.Grant(ShortServerGrant.GetRequest(),7,101,1000),"server grant must cover RTT plus UTC margin plus ACK reserve");
        Check(!ShortServerGrant.Grant(ShortServerGrant.GetRequest(),180,99,1000),"backwards monotonic clock cannot create a lease");
        Check(!ShortServerGrant.Grant(ShortServerGrant.GetRequest(),180,101,std::numeric_limits<double>::quiet_NaN()),"nonfinite signed TTL fails closed");

        FClientFlow ExpiredConsent;
        ExpiredConsent.Begin(100);
        Check(ExpiredConsent.Grant(ExpiredConsent.GetRequest(),180,101,10),"late-consent regression is initially authorized");
        const auto ExpiredId=ExpiredConsent.GetRequest();
        Check(!ExpiredConsent.Finish(110),"consent at hard deadline cannot create a fresh resume window");
        Check(!ExpiredConsent.Finish(150),"OS suspension beyond lease cannot extend connection recovery");
        Check(!ExpiredConsent.Resume(ExpiredId,150),"late consent never releases the frame gate");
        ExpiredConsent.Abort();
        Check(ExpiredConsent.GetHardDeadline()==0,"disconnect abort clears the old hard deadline");
        Check(!ExpiredConsent.Grant(ExpiredId,180,151,1000),"late OS/grant callback after abort cannot resurrect capture");
        Check(!ExpiredConsent.Resume(ExpiredId,151),"late resume callback after abort cannot resurrect publication");
        Check(ExpiredConsent.Begin(160),"later explicit action can start a new independent exchange");
        Check(!ExpiredConsent.Grant(ExpiredId,180,161,1000),"retired generation cannot affect the new exchange");

        using namespace MRFPSVideoControl;
        FState Capture;
        Check(Decide(false,Capture)==EAction::AlreadyStopped,"an empty capture is already stopped");
        Capture.Session=true;
        Check(Decide(false,Capture)==EAction::Stop,"silent capture session must be stopped even with no fresh frame");
        Check(Decide(true,Capture)==EAction::AwaitFrame,"silent live session waits for its existing stream instead of re-prompting");
        Capture.Fresh=true;
        Check(Decide(true,Capture)==EAction::AlreadyLive,"a recovered frame makes the same session live again");
        Capture.Fresh=false; Capture.Error=true;
        Check(Decide(false,Capture)==EAction::Stop,"a failed but retained session still needs retirement");
        Check(Decide(true,Capture)==EAction::Start,"an errored session requires an explicit restart");
        Capture={}; Capture.FlowBusy=true;
        Check(Decide(false,Capture)==EAction::Stop,"a pending grant can be cancelled before capture begins");
        Check(Decide(true,Capture)==EAction::AwaitConsent,"duplicate enable does not open another dialog while pending");
        Capture={}; Capture.Queued=true;
        Check(Decide(false,Capture)==EAction::Stop && Capture.NeedsServerStop(),"queued frames are retired on stop");
        Capture={}; Capture.ServerStream=true;
        Check(Decide(false,Capture)==EAction::Stop && Capture.NeedsServerStop(),"an open replicated stream snapshot is retired");
        Capture={}; Capture.LocalPause=true;
        Check(Decide(false,Capture)==EAction::Stop,"a lingering local consent lease is retired");

        FClientFlow StopGrant;
        Check(StopGrant.Begin(500),"stop-grant flow starts");
        const auto GrantId=StopGrant.GetRequest();
        const FStopPlan GrantStop=StopGrant.Stop(500.5,false);
        Check(GrantStop.SendFinish && GrantStop.EndLocalPause && !GrantStop.WaitForAck && GrantStop.Request==GrantId,
              "stop during grant retires server request and immediately ends any local pause");
        Check(!StopGrant.IsBusy() && !StopGrant.Grant(GrantId,180,501,1000),"late grant cannot reopen stopped consent");

        FClientFlow StopConsent; FPauseLease StopClientLease,StopServerLease;
        Check(StopConsent.Begin(600),"stop-consent flow starts");
        const auto ConsentId=StopConsent.GetRequest();
        Check(StopConsent.Grant(ConsentId,180,600.1,1000),"server grant permits a real consent request");
        Check(StopClientLease.BeginUntil(600.1,StopConsent.GetHardDeadline())>0 && StopServerLease.Begin(600.1,180)>0,
              "both peers have bounded timeout protection");
        const FStopPlan ConsentStop=StopConsent.Stop(601,true);
        Check(ConsentStop.SendFinish && ConsentStop.WaitForAck && !ConsentStop.EndLocalPause &&
              StopConsent.GetPhase()==EPhase::AwaitingResumeAck,"stop during OS consent waits for the real server resume ACK");
        StopServerLease.End();
        Check(StopClientLease.IsActive(601.1) && !StopServerLease.IsActive(601.1),"local timeout protection survives until reply");
        const FStopPlan RetryBeforeAck=StopConsent.Stop(601.2,true);
        Check(RetryBeforeAck.SendFinish && RetryBeforeAck.WaitForAck && !RetryBeforeAck.EndLocalPause,
              "repeated desired stop cannot open another consent request or release the client lease early");
        Check(!StopConsent.Grant(ConsentId,180,601.3,1000),"late duplicate grant cannot recreate the stopped session");
        Check(StopConsent.Resume(ConsentId,601.4),"matching resume ACK completes the stopped consent flow");
        StopClientLease.End();
        Check(!StopConsent.IsBusy() && !StopClientLease.IsActive(601.4),"both consent leases are retired after ACK");

        FClientFlow StopExpired;
        Check(StopExpired.Begin(700) && StopExpired.Grant(StopExpired.GetRequest(),180,700.1,1000),"deadline fixture starts");
        const auto DeadlineId=StopExpired.GetRequest();
        const auto ExpiredStop=StopExpired.Stop(StopExpired.GetHardDeadline(),true);
        Check(ExpiredStop.SendFinish && ExpiredStop.EndLocalPause && !ExpiredStop.WaitForAck && !StopExpired.IsBusy(),
              "an expired consent lease is ended without extending its original deadline");
        Check(!StopExpired.Resume(DeadlineId,1000),"an expired ACK cannot revive the stopped flow");

        // Execute the same effects coordinator used by the controller, with
        // fake local/server effects so stale capture and lease ordering are observable.
        FClientFlow ApplyFlow;
        FState Live; Live.Session=true; Live.Requested=true; Live.HadFrames=true; Live.Queued=true; Live.ServerStream=true;
        std::vector<EStopEffect> Applied;
        auto Record=[&](EStopEffect Effect,std::uint32_t) { Applied.push_back(Effect); };
        Check(ApplyStop(Live,ApplyFlow,800,Record),"silent active session executes the stop coordinator");
        Check(Applied==std::vector<EStopEffect>({EStopEffect::RetireCapture,EStopEffect::ResetLocalQueue,
              EStopEffect::EndLocalPause,EStopEffect::StopServerStream}),"stop retires capture, queue and server stream in order");
        Applied.clear(); Live={};
        Check(!ApplyStop(Live,ApplyFlow,801,Record) && Applied.empty(),"a completed stop has no repeated effects");
        Check(ApplyFlow.Begin(810) && ApplyFlow.Grant(ApplyFlow.GetRequest(),180,810.1,1000),"pending stop fixture obtains grant");
        const auto ApplyId=ApplyFlow.GetRequest();
        FState Pending; Pending.Session=true; Pending.FlowBusy=true; Pending.LocalPause=true; Pending.ServerStream=true;
        std::uint32_t FinishedId=0;
        auto PendingEffects=[&](EStopEffect Effect,std::uint32_t Id) { Applied.push_back(Effect); if(Effect==EStopEffect::FinishServerPause) FinishedId=Id; };
        Check(ApplyStop(Pending,ApplyFlow,811,PendingEffects),"pending consent can be stopped");
        Check(Applied==std::vector<EStopEffect>({EStopEffect::RetireCapture,EStopEffect::ResetLocalQueue,
              EStopEffect::FinishServerPause,EStopEffect::StopServerStream}) && FinishedId==ApplyId,
              "pending stop sends the matching finish without releasing the local lease before ACK");
        Check(ApplyFlow.GetPhase()==EPhase::AwaitingResumeAck && ApplyFlow.Resume(ApplyId,811.1),
              "the stopped pending flow completes only after its real ACK");

        using namespace MRFPSAuthClientPolicy;
        FConnectionUses Uses(2);
        const std::string A="00000000-0000-4000-8000-000000000001";
        const std::string B="00000000-0000-4000-8000-000000000002";
        const std::string C="00000000-0000-4000-8000-000000000003";
        Check(Uses.Check(A,200,100)==EUse::Fresh,"verified unattempted jti may connect");
        Check(Uses.Begin(A,200,100)==EUse::Fresh,"first connection attempt retires jti locally");
        Check(Uses.Check(A,200,101)==EUse::AlreadyAttempted,"old jti cannot be automatically retried");
        Check(Uses.Check(A,300,101)==EUse::AlreadyAttempted,"reloading or resigning the same jti cannot reset history");
        Check(Uses.Begin(B,200,101)==EUse::Fresh,"a distinct signed jti permits an explicit reconnect");
        Check(Uses.Begin(C,300,101)==EUse::Capacity,"full history fails closed without evicting live entries");
        Check(Uses.Check(A,200,201)==EUse::Invalid,"expired bearer remains unusable");
        Check(Uses.Check(C,300,206)==EUse::Fresh,"expired history is reclaimed after replay retention");
        Check(Uses.Begin(C,300,206)==EUse::Fresh,"fresh credential remains usable after bounded cleanup");
        Check(Uses.Check("",300,206)==EUse::Invalid,"missing jti cannot enter the connection history");
        std::cout << "CONSENT_LIFECYCLE_PASS checks=" << Checks << '\n';
        return 0;
    } catch(const std::exception& Error) {
        std::cerr << "CONSENT_LIFECYCLE_FAIL check=" << Error.what() << '\n';
        return 1;
    }
}
