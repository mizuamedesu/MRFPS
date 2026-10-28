#include "MRFPSAuthenticatedLocalPlayer.h"

#include "MRFPSAuthClientSubsystem.h"
#include "MRFPSAuthLogPrivacy.h"
#include "MRFPSAuthLoginOptions.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/PendingNetGame.h"

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSAuthLogin, Log, All);

FString MRFPSAuthLogin::BuildForCurrentPendingConnection(const UGameInstance* Instance, const UMRFPSAuthClientSubsystem* Credentials)
{
    if (!Instance || !Credentials || Credentials->GetOuter() != Instance || !Credentials->HasAccessToken()) return FString();
    const FWorldContext* Context = Instance->GetWorldContext();
    const UPendingNetGame* Pending = Context && Context->OwningGameInstance == Instance ? Context->PendingNetGame.Get() : nullptr;
    const UNetConnection* Connection = Pending && Pending->NetDriver ? Pending->NetDriver->ServerConnection.Get() : nullptr;
    // A rogue server can send NMT_Challenge before EncryptionAck. Never fall
    // back to a previous world's driver or emit a bearer token at that point.
    if (!Connection || !Connection->IsEncryptionEnabled())
    {
        UE_LOG(LogMRFPSAuthLogin, Display, TEXT("AUTH_ENCRYPTION_REQUIRED"));
        return FString();
    }
    return TEXT("AuthToken=") + Credentials->GetAccessToken();
}

FString UMRFPSAuthenticatedLocalPlayer::GetGameLoginOptions() const
{
    FMRFPSAuthLogPrivacy::SuppressNetworkTokenLogs();
    if (const auto* Instance = GetGameInstance())
    {
        return MRFPSAuthLogin::BuildForCurrentPendingConnection(Instance, Instance->GetSubsystem<UMRFPSAuthClientSubsystem>());
    }
    return FString();
}
