#pragma once

#include "MRFPSGame.h"
#include "MRFPSRosterPolicy.h"
#include "Engine/NetConnection.h"
#include "Engine/World.h"

namespace MRFPSRoster
{
struct FRow
{
    FString Name;
    MRFPSRosterPolicy::ERole Role=MRFPSRosterPolicy::ERole::Hidden;
    int32 Id=-1,Slot=-1,Ping=-1;
    bool Self=false;
};

inline bool CanView(const AMRFPSPlayerController* PC)
{
    const auto* PS=PC?PC->GetPlayerState<AMRFPSPlayerState>():nullptr;
    const auto* Connection=PC?PC->GetNetConnection():nullptr;
    const int64 Now=FDateTime::UtcNow().ToUnixTimestamp();
    const bool Connected=PC && PC->GetNetMode()==NM_Client && Connection && Connection->GetConnectionState()==USOCK_Open;
    return PC && PS && MRFPSRosterPolicy::CanView(PC->IsLocalController(),Connected,
        Connection && Connection->IsEncryptionEnabled(),PS->bAuthenticated,PS->bInspector,PS->IsABot(),PS->Slot,
        !PS->AuthSubject.IsEmpty(),PS->AuthExpiresAt,Now);
}

inline TArray<FRow> Collect(const AMRFPSPlayerController* PC)
{
    TArray<FRow> Rows;
    if (!CanView(PC)) return Rows;
    const UWorld* World=PC->GetWorld();
    const auto* GS=World?World->GetGameState<AMRFPSGameState>():nullptr;
    if (!GS) return Rows;
    const auto* Self=PC->GetPlayerState<AMRFPSPlayerState>();
    const int64 Now=FDateTime::UtcNow().ToUnixTimestamp();
    const double ServerTime=GS->GetServerWorldTimeSeconds();
    for (APlayerState* Base:GS->PlayerArray) {
        const auto* PS=Cast<AMRFPSPlayerState>(Base);
        if (!IsValid(PS) || PS->IsActorBeingDestroyed()) continue;
        const bool Bot=PS->IsABot();
        const FString Name=Bot?TEXT("BOT"):PS->GetPlayerName().TrimStartAndEnd();
        // The server currently sets the display name to the verified subject.
        // Wait for both replicated properties, rather than showing a default
        // anonymous name while admission state is incomplete.
        const bool HasIdentity=Bot || (!PS->AuthSubject.IsEmpty() && Name==PS->AuthSubject);
        const auto Role=MRFPSRosterPolicy::Classify(Bot,PS->bAuthenticated,PS->bInspector,PS->bAdmin,PS->Slot,
            HasIdentity,PS->AuthExpiresAt,Now);
        if (Role==MRFPSRosterPolicy::ERole::Hidden) continue;
        FRow Row; Row.Name=Name.Left(64); Row.Role=Role; Row.Id=PS->GetPlayerId(); Row.Slot=PS->Slot; Row.Self=PS==Self;
        Row.Ping=MRFPSRosterPolicy::PingMs(Bot,PS->RosterPing.Milliseconds,PS->RosterPing.SampleServerTime,ServerTime);
        Rows.Add(MoveTemp(Row));
    }
    Rows.Sort([](const FRow& A,const FRow& B) {
        if (A.Role!=B.Role) return int(A.Role)<int(B.Role);
        if (A.Slot!=B.Slot) return A.Slot<B.Slot;
        if (A.Id!=B.Id) return A.Id<B.Id;
        return A.Name<B.Name;
    });
    return Rows;
}
}
