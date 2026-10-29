#include "MRFPSGame.h"
#include "MRFPSVideoReceiver.h"
#include "MRFPSIpConnection.h"
#include "Engine/NetConnection.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#if !UE_SERVER
#include "MRFPSViewCaptureSubsystem.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSVideo, Log, All);
namespace { uint32 NextServerVideoStream = 0; }

bool AMRFPSPlayerController::IsVideoSessionValid(bool Publisher) const
{
    const auto* PS=GetPlayerState<AMRFPSPlayerState>();
    const auto* Connection=GetNetConnection();
    return GetNetMode()!=NM_Standalone && PS && PS->bAuthenticated &&
        PS->AuthExpiresAt>FDateTime::UtcNow().ToUnixTimestamp() && Connection && Connection->IsEncryptionEnabled() &&
        (Publisher ? (!PS->bInspector && PS->Slot>=0 && PS->Slot<2) : PS->bInspector);
}

void AMRFPSPlayerController::MRFPSShareView()
{
#if !UE_SERVER
    if (!IsLocalController() || !IsVideoSessionValid(true)) return;
    if (auto* Capture=GetGameInstance()->GetSubsystem<UMRFPSViewCaptureSubsystem>()) {
        const bool StopFreshCapture=Capture->IsCapturing();
        // Repeated input must not dismiss/reopen an outstanding OS consent UI.
        if (!StopFreshCapture && (Capture->IsCaptureRequestPending() || VideoConsentFlow.IsBusy())) return;
        const bool RetirePrevious=StopFreshCapture || bViewSharingRequested || bHadCaptureFrames;
        Capture->StopCapture(); bViewSharingRequested=false; bHadCaptureFrames=false;
        VideoPublisherQueue.Reset();
        if (RetirePrevious) ServerStopQuestVideo(VideoPublishSequence);
        // A stale/revoked session is replaced by this same explicit action;
        // only a fresh active capture treats the action as a stop toggle.
        if (!StopFreshCapture) {
#if PLATFORM_ANDROID
            if (VideoConsentFlow.Begin(FPlatformTime::Seconds())) {
                // Open no OS activity until the authenticated server has ACKed
                // its bounded pause. Otherwise the queued RPC could be paused too.
                ServerRequestVideoConsentPause(VideoConsentFlow.GetRequest());
            }
#else
            Capture->RequestCapture(); bViewSharingRequested=true;
#endif
        }
    }
#endif
}

void AMRFPSPlayerController::HandleSessionDisconnected()
{
    GetWorldTimerManager().ClearTimer(PendingConnectionTimer);
    VideoConsentFlow.Abort();
    if (auto* Connection=Cast<UMRFPSIpConnection>(GetNetConnection())) Connection->EndCaptureConsentPause();
    VideoPublisherQueue.Reset(); bViewSharingRequested=false; bHadCaptureFrames=false; bHadVideoPublisherSession=false;
#if !UE_SERVER
    if (auto* Capture=GetGameInstance()->GetSubsystem<UMRFPSViewCaptureSubsystem>()) Capture->StopCapture();
#endif
}

void AMRFPSPlayerController::ServerRequestVideoConsentPause_Implementation(uint32 Request)
{
    if (!IsVideoSessionValid(true) || Request==0) return;
    auto* Connection=Cast<UMRFPSIpConnection>(GetNetConnection());
    if (!Connection || Request<=VideoConsentServerRequest) { ClientVideoConsentPause(Request,0); return; }
    const auto* PS=GetPlayerState<AMRFPSPlayerState>();
    const double Allowed=MRFPSConsent::AllowedPauseSeconds(FDateTime::UtcNow().ToUnixTimestamp(),PS->AuthExpiresAt);
    const double Granted=Connection->BeginCaptureConsentPause(Allowed);
    if (Granted<=0) { ClientVideoConsentPause(Request,0); return; }
    VideoConsentServerRequest=Request;
    ClientVideoConsentPause(Request,static_cast<float>(Granted));
    UE_LOG(LogMRFPSVideo,Display,TEXT("MRFPS_CONSENT_PAUSE_GRANTED seconds=%.1f"),Granted);
}

void AMRFPSPlayerController::ServerFinishVideoConsentPause_Implementation(uint32 Request)
{
    UE_LOG(LogMRFPSVideo,Display,TEXT("MRFPS_CONSENT_FINISH request=%u expected=%u valid=%d"),
        Request,VideoConsentServerRequest,IsVideoSessionValid(true)?1:0);
    if (!IsVideoSessionValid(true) || Request==0 || Request!=VideoConsentServerRequest) return;
    if (auto* Connection=Cast<UMRFPSIpConnection>(GetNetConnection())) Connection->EndCaptureConsentPause();
    // This real packet refreshed the server's receive clock. The reply similarly
    // refreshes the client before it returns to ordinary timeout/publication.
    ClientVideoConsentPause(Request,0);
}

void AMRFPSPlayerController::ClientVideoConsentPause_Implementation(uint32 Request,float Seconds)
{
#if !UE_SERVER
    UE_LOG(LogMRFPSVideo,Display,TEXT("MRFPS_CONSENT_REPLY request=%u expected=%u seconds=%.2f local=%d valid=%d phase=%d"),
        Request,VideoConsentFlow.GetRequest(),Seconds,IsLocalController()?1:0,IsVideoSessionValid(true)?1:0,
        static_cast<int32>(VideoConsentFlow.GetPhase()));
    if (!IsLocalController() || !IsVideoSessionValid(true) || Request!=VideoConsentFlow.GetRequest() || !FMath::IsFinite(Seconds) || Seconds<0) return;
    auto* Connection=Cast<UMRFPSIpConnection>(GetNetConnection());
    auto* Capture=GetGameInstance()->GetSubsystem<UMRFPSViewCaptureSubsystem>();
    const auto AbortConsent=[&]() {
        if (Capture) Capture->StopCapture();
        if (Connection) Connection->EndCaptureConsentPause();
        bViewSharingRequested=false; VideoConsentFlow.Abort();
        ServerFinishVideoConsentPause(Request);
    };
    if (!Connection || !Capture) { AbortConsent(); return; }
    if (Seconds>0) {
        // A duplicated/old grant must never cancel or renew a current dialog.
        if (VideoConsentFlow.GetPhase()!=MRFPSConsent::EPhase::AwaitingGrant) return;
        const double Allowed=MRFPSConsent::AllowedPauseSeconds(FDateTime::UtcNow().ToUnixTimestamp(),GetPlayerState<AMRFPSPlayerState>()->AuthExpiresAt);
        if (!VideoConsentFlow.Grant(Request,double(Seconds),FPlatformTime::Seconds(),Allowed) ||
            Connection->BeginCaptureConsentPauseUntil(VideoConsentFlow.GetHardDeadline())<=0) {
            UE_LOG(LogMRFPSVideo,Warning,TEXT("MRFPS_CONSENT_REPLY_ABORT reason=client_lease"));
            AbortConsent(); ConnectionError=TEXT("認証の残り時間が不足しています。再接続してください"); return;
        }
        UE_LOG(LogMRFPSVideo,Display,TEXT("MRFPS_CONSENT_REQUEST_OS"));
        Capture->RequestCapture(); bViewSharingRequested=true;
    } else if (VideoConsentFlow.Resume(Request,FPlatformTime::Seconds())) {
        Connection->EndCaptureConsentPause();
        bViewSharingRequested=Capture->IsCapturing();
    } else if (VideoConsentFlow.GetPhase()==MRFPSConsent::EPhase::AwaitingGrant) {
        AbortConsent(); ConnectionError=TEXT("共有を開始できません。接続を確認してください");
    } else if (VideoConsentFlow.IsExpired(FPlatformTime::Seconds())) {
        AbortConsent(); ConnectionError=TEXT("共有の接続確認がタイムアウトしました");
    }
#endif
}
void AMRFPSPlayerController::MRFPSReconstructedViews()
{
    if (IsInspector()) { bReconstructedViews=!bReconstructedViews; LastCaptureRefresh=-100; }
}
UTexture2D* AMRFPSPlayerController::GetQuestVideoTexture(int32 Slot) const
{
    if (!IsVideoSessionValid(false)) return nullptr;
    const auto* Source=FindPlayer(Slot);
    const auto* SourceState=Source ? Source->GetPlayerState<AMRFPSPlayerState>() : nullptr;
    if (!SourceState || !SourceState->bAuthenticated || SourceState->bInspector || SourceState->Slot!=Slot ||
        SourceState->AuthExpiresAt<=FDateTime::UtcNow().ToUnixTimestamp() || SourceState->VideoStreamId==0) return nullptr;
    auto* Receiver=GetGameInstance()->GetSubsystem<UMRFPSVideoReceiver>();
    return Receiver ? Receiver->GetFreshTextureForStream(Slot,SourceState->VideoStreamId) : nullptr;
}
FString AMRFPSPlayerController::GetQuestVideoStatus(int32 Slot) const
{
    if (!IsVideoSessionValid(false)) return TEXT("Connect as an authenticated inspector");
    const auto* Source=FindPlayer(Slot);
    const auto* SourceState=Source ? Source->GetPlayerState<AMRFPSPlayerState>() : nullptr;
    if (!SourceState || !SourceState->bAuthenticated || SourceState->bInspector || SourceState->Slot!=Slot ||
        SourceState->AuthExpiresAt<=FDateTime::UtcNow().ToUnixTimestamp()) return TEXT("Video waiting for authenticated publisher");
    if (!SourceState->VideoStreamId) return TEXT("Quest view paused / resume headset sharing if needed");
    const auto* Receiver=GetGameInstance()->GetSubsystem<UMRFPSVideoReceiver>();
    return Receiver ? Receiver->GetSafeStatusForStream(Slot,SourceState->VideoStreamId) : TEXT("Player video unavailable");
}

void AMRFPSPlayerController::TickQuestVideo()
{
    if (!IsLocalController()) return;
    const double Now=FPlatformTime::Seconds();
    const bool Inspector=IsVideoSessionValid(false), Publisher=IsVideoSessionValid(true);
    bool CaptureFresh=false;
    if (!Inspector && bHadVideoInspectorSession) {
        if (auto* Receiver=GetGameInstance()->GetSubsystem<UMRFPSVideoReceiver>()) Receiver->ResetAll();
        for (auto& Assembly:VideoDownloads) Assembly.Reset();
    }
    bHadVideoInspectorSession=Inspector;
#if !UE_SERVER
    auto* Capture=GetGameInstance()->GetSubsystem<UMRFPSViewCaptureSubsystem>();
    if (!Publisher && bHadVideoPublisherSession && Capture) {
        Capture->StopCapture(); bViewSharingRequested=false; VideoConsentFlow.Abort();
        if (auto* Connection=Cast<UMRFPSIpConnection>(GetNetConnection())) Connection->EndCaptureConsentPause();
    }
    bool AutoCapture=FParse::Param(FCommandLine::Get(),TEXT("MRFPSVideoFromAdb")) ||
        FParse::Param(FCommandLine::Get(),TEXT("MRFPSRequestCapture")) ||
        FParse::Param(FCommandLine::Get(),TEXT("MRFPSCaptureDesktopView"));
#if PLATFORM_ANDROID
    // A player session requests OS consent once when it connects. Consent is
    // still granted or denied by the headset user, never bypassed.
    AutoCapture=AutoCapture || !FParse::Param(FCommandLine::Get(),TEXT("MRFPSNoAutoCapture"));
#endif
    if (Publisher && !bHadVideoPublisherSession && Capture && AutoCapture) {
        MRFPSShareView();
    }
    if (Publisher && Capture && VideoConsentFlow.IsBusy()) {
        // MRFPSShareView above may have started this flow after the tick's
        // timestamp was sampled. A fresh clock avoids treating that earlier
        // timestamp as a clock rollback and immediately aborting the grant.
        const double ConsentNow=FPlatformTime::Seconds();
        if (VideoConsentFlow.GetPhase()==MRFPSConsent::EPhase::AwaitingConsent &&
            (!Capture->IsCaptureRequestPending() || VideoConsentFlow.IsExpired(ConsentNow))) {
            if (VideoConsentFlow.IsExpired(ConsentNow)) { Capture->StopCapture(); bViewSharingRequested=false; }
            // Even a late OS result releases the server lease, but cannot create
            // a new ACK deadline beyond the original conservative hard limit.
            const bool AwaitingAck=VideoConsentFlow.Finish(ConsentNow);
            ServerFinishVideoConsentPause(VideoConsentFlow.GetRequest());
            if (!AwaitingAck) {
                Capture->StopCapture(); bViewSharingRequested=false; VideoConsentFlow.Abort();
                if (auto* Connection=Cast<UMRFPSIpConnection>(GetNetConnection())) Connection->EndCaptureConsentPause();
                ConnectionError=TEXT("共有の待機時間が過ぎました。再接続してください");
            }
        } else if (VideoConsentFlow.IsExpired(ConsentNow)) {
            ServerFinishVideoConsentPause(VideoConsentFlow.GetRequest());
            Capture->StopCapture(); bViewSharingRequested=false; VideoConsentFlow.Abort();
            if (auto* Connection=Cast<UMRFPSIpConnection>(GetNetConnection())) Connection->EndCaptureConsentPause();
            ConnectionError=TEXT("共有の接続確認がタイムアウトしました");
        }
    }
    if (Publisher && Capture) {
        TArray<uint8> Jpeg; int32 Width=0,Height=0;
        if (!VideoConsentFlow.IsBusy() && Capture->DequeueLatestFrame(Jpeg,Width,Height) && Jpeg.Num()>=4 && Jpeg.Num()<=MRFPSVideo::MaxFrameBytes &&
            Width>0 && Width<=MRFPSVideo::MaxWidth && Height>0 && Height<=MRFPSVideo::MaxHeight && VideoPublishSequence<MAX_uint32) {
            FMRFPSVideoChunk Header; Header.Slot=static_cast<uint8>(GetPlayerState<AMRFPSPlayerState>()->Slot);
            Header.StreamId=1; Header.Sequence=++VideoPublishSequence;
            Header.Width=Width; Header.Height=Height; Header.TotalBytes=Jpeg.Num();
            VideoPublisherQueue.Push(Header,Jpeg,Now);
        }
        CaptureFresh=!VideoConsentFlow.IsBusy() && Capture->IsCapturing();
        if (!CaptureFresh && bHadCaptureFrames) { VideoPublisherQueue.Reset(); ServerStopQuestVideo(VideoPublishSequence); }
        bHadCaptureFrames=CaptureFresh;
    }
#endif
    bHadVideoPublisherSession=Publisher;
    if (!Publisher || !CaptureFresh) VideoPublisherQueue.Reset();
    else for (int32 I=0;I<MRFPSVideo::PublisherChunkBudgetPerTick && GetNetConnection()->IsNetReady();++I) {
        FMRFPSVideoChunk Chunk; if (!VideoPublisherQueue.Next(Chunk,Now)) break; ServerPublishQuestVideo(Chunk);
    }
    if (Publisher && CaptureFresh && VideoPublisherQueue.HasData() && !GetNetConnection()->IsNetReady()) ++VideoBackpressureTicks;
    if (Inspector && Now>=NextVideoSubscriptionAt) {
        NextVideoSubscriptionAt=Now+.25;
        const uint8 Desired=MRFPSVideo::ObserverSubscription(bReconstructedViews,bMultiView,ViewSlot,PlayerVideoWindowMask());
        if (Desired!=LastVideoSubscriptionSent) { ServerSubscribeQuestVideo(Desired); LastVideoSubscriptionSent=Desired; }
    }
    if (Publisher || Inspector) LogVideoDiagnostics(Publisher?TEXT("publisher"):TEXT("viewer"));
}

void AMRFPSPlayerController::LogVideoDiagnostics(const TCHAR* Leg)
{
    const double Now=FPlatformTime::Seconds();
    if (Now-LastVideoDiagnosticsAt<2) return;
    LastVideoDiagnosticsAt=Now;
    const auto* C=GetNetConnection(); if (!C) return;
    const auto* PS=GetPlayerState<AMRFPSPlayerState>();
    UE_LOG(LogMRFPSVideo,Display,TEXT("MRFPS_VIDEO_PATH leg=%s slot=%d netspeed=%d netready=%d inloss=%d outloss=%d source=%u pushed=%llu sentchunks=%llu sentframes=%llu replaced=%llu expired=%llu backpressure=%llu inchunks=%llu assemblyreject=%llu ratedrop=%llu framedrop=%llu accepted=%llu assembled=%llu superseded=%llu assemblyexpired=%llu recovered=%llu relay0=%llu relay1=%llu download0=%llu download1=%llu"),
        Leg,PS?PS->Slot:-1,C->CurrentNetSpeed,C->IsNetReady(),C->InPacketsLost,C->OutPacketsLost,VideoPublishSequence,
        VideoPublisherQueue.PushedFrames,VideoPublisherQueue.SentChunks,VideoPublisherQueue.FinishedFrames,
        VideoPublisherQueue.ReplacedFrames,VideoPublisherQueue.ExpiredFrames,VideoBackpressureTicks,
        VideoInboundChunks,VideoAssemblyRejected,VideoRateRejected,VideoFrameRejected,VideoAcceptedFrames,
        VideoUpload.CompletedFrames,VideoUpload.SupersededFrames,VideoUpload.ExpiredFrames,VideoUpload.RecoveredChunks,
        VideoRelayQueues[0].FinishedFrames,VideoRelayQueues[1].FinishedFrames,VideoDownloads[0].CompletedFrames,VideoDownloads[1].CompletedFrames);
}

void AMRFPSPlayerController::ServerSubscribeQuestVideo_Implementation(uint8 Mask)
{
    if (!IsVideoSessionValid(false)) return;
    VideoSubscription=Mask&3;
    for (int32 Slot=0;Slot<2;++Slot) if (!(VideoSubscription&(1<<Slot))) VideoRelayQueues[Slot].Reset();
}

void AMRFPSPlayerController::ServerStopQuestVideo_Implementation(uint32 FinalSequence)
{
    if (!IsVideoSessionValid(true)) return;
    auto* PS=GetPlayerState<AMRFPSPlayerState>();
    VideoClosedThrough=FMath::Max(VideoClosedThrough,FMath::Max(FinalSequence,VideoLastAcceptedSequence));
    VideoUpload.Reset();
    const uint32 ClosedStream=PS->VideoStreamId;
    PS->VideoStreamId=0; PS->ForceNetUpdate();
    for (FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It) {
        auto* Peer=Cast<AMRFPSPlayerController>(It->Get());
        if (!Peer) continue;
        Peer->VideoRelayQueues[PS->Slot].Reset();
        if (ClosedStream && Peer->IsVideoSessionValid(false)) Peer->ClientQuestVideoStopped(PS->Slot,ClosedStream);
    }
    UE_LOG(LogMRFPSVideo,Display,TEXT("MRFPS_VIDEO_STOP slot=%d stream=%u"),PS->Slot,ClosedStream);
}

void AMRFPSPlayerController::ServerPublishQuestVideo_Implementation(const FMRFPSVideoChunk& Chunk)
{
    if (!IsVideoSessionValid(true) || !Chunk.IsSane()) return;
    ++VideoInboundChunks;
    auto* PS=GetPlayerState<AMRFPSPlayerState>();
    if (Chunk.Slot!=PS->Slot || Chunk.Sequence<=VideoClosedThrough) return;
    const double Now=FPlatformTime::Seconds();
    const auto Rate=MRFPSVideo::CheckPublisherRate(VideoRateBytes,Now-VideoRateAt,Chunk.Payload.Num());
    VideoRateAt=Now; VideoRateBytes=Rate.DecayedBytes;
    if (!Rate.bAllowed) { ++VideoRateRejected; return; }
    if (!VideoUpload.Add(Chunk,Now)) { ++VideoAssemblyRejected; return; }
    VideoRateBytes=Rate.ChargedBytes;
    FMRFPSVideoChunk Header; TArray<uint8> Jpeg;
    if (!VideoUpload.Take(Header,Jpeg)) return;
    if (Now-LastVideoAcceptedAt<MRFPSVideo::MinimumAcceptedFrameInterval || Jpeg[0]!=0xff || Jpeg[1]!=0xd8 || Jpeg[2]!=0xff ||
        Jpeg[Jpeg.Num()-2]!=0xff || Jpeg.Last()!=0xd9) { ++VideoFrameRejected; return; }
    ++VideoAcceptedFrames;
    LastVideoAcceptedAt=Now;
    VideoLastAcceptedSequence=Header.Sequence;
    if (PS->VideoStreamId==0) {
        if (NextServerVideoStream==MAX_uint32) return;
        PS->VideoStreamId=++NextServerVideoStream; PS->ForceNetUpdate();
    }
    Header.StreamId=PS->VideoStreamId; Header.Slot=PS->Slot;
    int32 Recipients=0;
    for (FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It) {
        auto* Peer=Cast<AMRFPSPlayerController>(It->Get());
        if (Peer && Peer->IsVideoSessionValid(false) && (Peer->VideoSubscription&(1<<Header.Slot))) {
            Peer->VideoRelayQueues[Header.Slot].Push(Header,Jpeg,Now); ++Recipients;
        }
    }
    if (Now-LastVideoLogAt>=1) {
        LastVideoLogAt=Now;
        UE_LOG(LogMRFPSVideo,Display,TEXT("MRFPS_VIDEO_RELAY slot=%d stream=%u sequence=%u bytes=%d subscribers=%d"),Header.Slot,Header.StreamId,Header.Sequence,Jpeg.Num(),Recipients);
    }
}

void AMRFPSPlayerController::ServiceVideoRelay()
{
    if (!HasAuthority()) return;
    if (IsVideoSessionValid(true) || IsVideoSessionValid(false)) LogVideoDiagnostics(IsVideoSessionValid(true)?TEXT("server-upload"):TEXT("server-relay"));
    if (!IsVideoSessionValid(false)) { for (auto& Queue:VideoRelayQueues) Queue.Reset(); VideoSubscription=0; return; }
    const double Now=FPlatformTime::Seconds();
    for (int32 I=0;I<MRFPSVideo::RelayChunkBudgetPerTick && GetNetConnection()->IsNetReady();++I) {
        FMRFPSVideoChunk Chunk; bool Found=false;
        for (int32 Try=0;Try<2;++Try) {
            const int32 Slot=NextRelaySlot; NextRelaySlot=(NextRelaySlot+1)%2;
            const auto* Source=FindPlayer(Slot);
            const auto* SourceState=Source ? Source->GetPlayerState<AMRFPSPlayerState>() : nullptr;
            if (!SourceState || !SourceState->bAuthenticated || SourceState->bInspector ||
                SourceState->AuthExpiresAt<=FDateTime::UtcNow().ToUnixTimestamp() || SourceState->VideoStreamId==0) {
                VideoRelayQueues[Slot].Reset(); continue;
            }
            if ((VideoSubscription&(1<<Slot)) && VideoRelayQueues[Slot].Next(Chunk,Now)) {
                if (Chunk.StreamId!=SourceState->VideoStreamId) { VideoRelayQueues[Slot].Reset(); continue; }
                Found=true; break;
            }
        }
        if (!Found) break;
        ClientQuestVideo(Chunk);
    }
}

void AMRFPSPlayerController::ClientQuestVideo_Implementation(const FMRFPSVideoChunk& Chunk)
{
    if (!IsLocalController() || !IsVideoSessionValid(false) || !Chunk.IsSane()) return;
    const auto* SourcePawn=FindPlayer(Chunk.Slot);
    const auto* PS=SourcePawn ? SourcePawn->GetPlayerState<AMRFPSPlayerState>() : nullptr;
    if (!PS || !PS->bAuthenticated || PS->bInspector || PS->Slot!=Chunk.Slot ||
        PS->AuthExpiresAt<=FDateTime::UtcNow().ToUnixTimestamp() || PS->VideoStreamId!=Chunk.StreamId) return;
    if (!VideoDownloads[Chunk.Slot].Add(Chunk,FPlatformTime::Seconds())) return;
    FMRFPSVideoChunk Header; TArray<uint8> Jpeg;
    if (!VideoDownloads[Chunk.Slot].Take(Header,Jpeg)) return;
    if (auto* Receiver=GetGameInstance()->GetSubsystem<UMRFPSVideoReceiver>())
        Receiver->SubmitFrame(Header.Slot,Header.StreamId,Header.Sequence,Header.Width,Header.Height,MoveTemp(Jpeg));
}

void AMRFPSPlayerController::ClientQuestVideoStopped_Implementation(uint8 Slot,uint32 StreamId)
{
    if (!IsLocalController() || Slot>=2 || StreamId==0) return;
    const auto* Source=FindPlayer(Slot);
    const auto* PS=Source ? Source->GetPlayerState<AMRFPSPlayerState>() : nullptr;
    if (PS && PS->VideoStreamId>StreamId) return; // An older stop cannot close a newer publisher.
    // Do not reset the bounded assembler here: it may already hold a newer
    // partial frame while publisher replication/stop delivery is reordered.
    // Old completed frames are rejected by the receiver's stopped-generation
    // watermark; a higher stream naturally replaces the assembler's old work.
    if (auto* Receiver=GetGameInstance()->GetSubsystem<UMRFPSVideoReceiver>()) Receiver->StopStream(Slot,StreamId);
}
