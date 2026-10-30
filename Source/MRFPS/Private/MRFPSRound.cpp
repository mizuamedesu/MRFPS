#include "MRFPSGame.h"
#include "MRFPSBot.h"
#include "EngineUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSRound,Log,All);

int32 AMRFPSGameState::RoundSecondsRemaining() const
{
    if (Phase!=EMRFPSMatchPhase::Playing || RoundEndsAt<=0) return 0;
    const double Now=bCombatPaused ? RoundPausedAt : GetServerWorldTimeSeconds();
    return FMath::CeilToInt32(FMath::Clamp(RoundEndsAt-Now,0.,double(RoundDurationSeconds)));
}

FString AMRFPSGameState::RoundHeaderText() const
{
    if (Phase==EMRFPSMatchPhase::Waiting) return TEXT("対戦待ち");
    if (Phase==EMRFPSMatchPhase::BetweenRounds) {
        const double Now=bCombatPaused ? RoundPausedAt : GetServerWorldTimeSeconds();
        if (CompletedRounds>0 && RestartAt-Now>CountdownSeconds) return bCombatPaused?TEXT("ラウンド終了  一時停止"):TEXT("ラウンド終了");
        const int32 Seconds=FMath::CeilToInt32(FMath::Clamp(RestartAt-Now,0.,CountdownSeconds));
        return FString::Printf(TEXT("開始まで %d%s"),Seconds,bCombatPaused?TEXT("  一時停止"):TEXT(""));
    }
    if (Phase==EMRFPSMatchPhase::Finished) return WinnerSlot<0 ? TEXT("引き分け") : TEXT("試合終了");
    const int32 Seconds=RoundSecondsRemaining();
    return FString::Printf(TEXT("第%d / %d   %02d:%02d%s"),RoundNumber,MaximumRounds,Seconds/60,Seconds%60,bCombatPaused?TEXT("  一時停止"):TEXT(""));
}

FMRFPSRoundNotice AMRFPSGameState::RoundNotice(int32 ViewerSlot) const
{
    FMRFPSRoundNotice Result;
    if (bCombatPaused && Phase==EMRFPSMatchPhase::Playing) {
        Result.bVisible=true; Result.Main=TEXT("一時停止"); Result.Detail=TEXT("追跡・管理操作を確認"); return Result;
    }
    const double Now=bCombatPaused ? RoundPausedAt : GetServerWorldTimeSeconds();
    const double Remaining=FMath::Max(0.,RestartAt-Now);
    if (Phase==EMRFPSMatchPhase::BetweenRounds && RestartAt>0 &&
        (CompletedRounds==0 || Remaining<=CountdownSeconds)) {
        Result.bVisible=true;
        Result.Countdown=FMath::Clamp(FMath::CeilToInt32(Remaining),1,3);
        Result.Progress=float(FMath::Clamp(Remaining/CountdownSeconds,0.,1.));
        Result.Main=FString::FromInt(Result.Countdown);
        Result.Detail=bCombatPaused?TEXT("一時停止"):TEXT("ラウンド開始");
        return Result;
    }
    const bool MatchResult=Phase==EMRFPSMatchPhase::Finished;
    if (!MatchResult && !(Phase==EMRFPSMatchPhase::BetweenRounds && CompletedRounds>0 && Remaining>CountdownSeconds)) return Result;
    Result.bVisible=true;
    const int32 Winner=MatchResult?WinnerSlot:LastRoundWinnerSlot;
    const bool Player=ViewerSlot==0 || ViewerSlot==1;
    Result.bDefeat=Player && Winner>=0 && Winner!=ViewerSlot;
    Result.Main=Winner<0?TEXT("引き分け"):Player?(Winner==ViewerSlot?TEXT("勝利"):TEXT("敗北")):
        FString::Printf(TEXT("P%d 勝利"),Winner+1);
    Result.Detail=MatchResult?TEXT("試合終了"):FString::Printf(TEXT("ラウンド %d"),CompletedRounds);
    if (bCombatPaused) Result.Detail+=TEXT("  一時停止");
    return Result;
}

FString AMRFPSGameState::RoundScoreText(int32 Slot) const
{
    if (Slot==0 || Slot==1) {
        const int32 Own=Slot==0?RoundWinsSlot0:RoundWinsSlot1;
        const int32 Other=Slot==0?RoundWinsSlot1:RoundWinsSlot0;
        if (Phase==EMRFPSMatchPhase::Finished && WinnerSlot>=0)
            return FString::Printf(TEXT("%s   %d - %d"),WinnerSlot==Slot?TEXT("勝利"):TEXT("敗北"),Own,Other);
        return FString::Printf(TEXT("自分 %d  -  %d 相手"),Own,Other);
    }
    return FString::Printf(TEXT("P1 %d  -  %d P2"),RoundWinsSlot0,RoundWinsSlot1);
}

void AMRFPSGameMode::BeginRound()
{
    auto* GS=GetGameState<AMRFPSGameState>();
    if (!GS || !GameState || GS->bCombatPaused || GS->RoundNumber>=AMRFPSGameState::MaximumRounds) return;
    ++GS->RoundNumber;
    for (APlayerState* Base:GameState->PlayerArray) if (auto* PS=Cast<AMRFPSPlayerState>(Base)) {
        PS->Kills=PS->Deaths=0; PS->ForceNetUpdate();
    }
    for (TActorIterator<AMRFPSPawn> It(GetWorld());It;++It) It->Respawn();
    for (TActorIterator<AMRFPSBarrier> It(GetWorld());It;++It) if (!It->bAdminPlaced) It->Destroy();
    GS->RoundEndsAt=GetWorld()->GetTimeSeconds()+AMRFPSGameState::RoundDurationSeconds;
    GS->RoundPausedAt=GS->RestartAt=0; GS->Phase=EMRFPSMatchPhase::Playing; GS->ForceNetUpdate();
    UE_LOG(LogMRFPSRound,Display,TEXT("MRFPS_ROUND_START round=%d seconds=%d"),GS->RoundNumber,AMRFPSGameState::RoundDurationSeconds);
}

void AMRFPSGameMode::FinishRound(bool bElimination)
{
    auto* GS=GetGameState<AMRFPSGameState>();
    if (!GS || !GameState || GS->Phase!=EMRFPSMatchPhase::Playing || GS->bCombatPaused ||
        GS->RoundNumber<1 || GS->RoundNumber>AMRFPSGameState::MaximumRounds ||
        GS->RoundEndsAt<=0 || (!bElimination && GetWorld()->GetTimeSeconds()<GS->RoundEndsAt)) return;
    int32 Kills[2]={0,0};
    for (APlayerState* Base:GameState->PlayerArray) if (const auto* PS=Cast<AMRFPSPlayerState>(Base);
        PS && !PS->bInspector && PS->Slot>=0 && PS->Slot<2) Kills[PS->Slot]=PS->Kills;
    const int32 Winner=Kills[0]==Kills[1] ? -1 : Kills[0]>Kills[1] ? 0 : 1;
    if (Winner==0) ++GS->RoundWinsSlot0; else if (Winner==1) ++GS->RoundWinsSlot1;
    GS->CompletedRounds=GS->RoundNumber; GS->LastRoundWinnerSlot=Winner;
    GS->RoundEndsAt=GS->RoundPausedAt=0;
    const bool MatchOver=GS->RoundWinsSlot0>=AMRFPSGameState::RoundsToWin ||
        GS->RoundWinsSlot1>=AMRFPSGameState::RoundsToWin || GS->RoundNumber>=AMRFPSGameState::MaximumRounds;
    GS->WinnerSlot=GS->RoundWinsSlot0>=AMRFPSGameState::RoundsToWin ? 0 :
        GS->RoundWinsSlot1>=AMRFPSGameState::RoundsToWin ? 1 : -1;
    GS->Phase=MatchOver ? EMRFPSMatchPhase::Finished : EMRFPSMatchPhase::BetweenRounds;
    GS->RestartAt=MatchOver?0.:GetWorld()->GetTimeSeconds()+AMRFPSGameState::ResultSeconds+AMRFPSGameState::CountdownSeconds;
    for (TActorIterator<AMRFPSPawn> It(GetWorld());It;++It) { It->bShieldActive=false; It->ClientCombatStopped(); }
    GS->ForceNetUpdate();
    UE_LOG(LogMRFPSRound,Display,TEXT("MRFPS_ROUND_END round=%d winner=%d kills0=%d kills1=%d wins0=%d wins1=%d"),
        GS->RoundNumber,Winner,Kills[0],Kills[1],GS->RoundWinsSlot0,GS->RoundWinsSlot1);
    if (MatchOver) UE_LOG(LogMRFPSRound,Display,TEXT("MRFPS_MATCH winner=%d"),GS->WinnerSlot);
}

void AMRFPSGameMode::TickRounds()
{
    auto* GS=GetGameState<AMRFPSGameState>(); if (!GS || GS->bCombatPaused) return;
    if (GS->Phase==EMRFPSMatchPhase::Playing) FinishRound();
    else if (GS->Phase==EMRFPSMatchPhase::BetweenRounds && GS->RestartAt>0 && GetWorld()->GetTimeSeconds()>=GS->RestartAt) BeginRound();
}

void AMRFPSGameMode::ResetForFighterChange()
{
    ResetToWaiting();
}

bool AMRFPSGameMode::StartPreparedSoloMatch()
{
    auto* GS=GetGameState<AMRFPSGameState>();
    if (!HasAuthority() || !GS || GS->Phase!=EMRFPSMatchPhase::Waiting || GS->bCombatPaused || !MRFPSBot::IsPrepared(this)) return false;
    ResetMatch(); return GS->Phase==EMRFPSMatchPhase::BetweenRounds;
}
