#include "MRFPSRosterPublisher.h"
#include "MRFPSGame.h"
#include "MRFPSBot.h"
#include "Engine/NetConnection.h"
#include "Engine/World.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ConfigCacheIni.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

THIRD_PARTY_INCLUDES_START
#define UI UI_ST
#include <openssl/evp.h>
#include <openssl/hmac.h>
#undef UI
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSRoster, Log, All);

FMRFPSRosterPublisher::~FMRFPSRosterPublisher()
{
    if (Pending.IsValid() && Pending->GetStatus()==EHttpRequestStatus::Processing) Pending->CancelRequest();
}

FString FMRFPSRosterPublisher::HmacSha256(const FString& Key, const FString& Body)
{
    const FTCHARToUTF8 KeyBytes(*Key), BodyBytes(*Body);
    uint8 Output[EVP_MAX_MD_SIZE]; unsigned int Size=0;
    if (!HMAC(EVP_sha256(),KeyBytes.Get(),KeyBytes.Length(),reinterpret_cast<const uint8*>(BodyBytes.Get()),
        BodyBytes.Length(),Output,&Size) || Size!=32) return FString();
    FString Result; Result.Reserve(64);
    for (uint32 I=0;I<Size;++I) Result+=FString::Printf(TEXT("%02x"),uint32(Output[I]));
    return Result;
}

bool FMRFPSRosterPublisher::Start()
{
    Secret=FPlatformMisc::GetEnvironmentVariable(TEXT("MRFPS_ROSTER_HMAC_KEY"));
    bool bVerifyPeer=false;
    if (Secret.Len()!=64 || !GConfig || !GConfig->GetBool(TEXT("/Script/Engine.NetworkSettings"),TEXT("n.VerifyPeer"),bVerifyPeer,GEngineIni) || !bVerifyPeer) return false;
    for (TCHAR C:Secret) if (!((C>=TEXT('0') && C<=TEXT('9')) || (C>=TEXT('a') && C<=TEXT('f')))) return false;
    Instance=FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    return true;
}

void FMRFPSRosterPublisher::Tick(UWorld* World, double Now)
{
    if (!World || Secret.IsEmpty() || Instance.IsEmpty() || Now<NextPublishAt) return;
    NextPublishAt=Now+2.0;
    if (Pending.IsValid()) {
        if (Pending->GetStatus()==EHttpRequestStatus::Processing) return;
        const auto Response=Pending->GetResponse();
        if (Pending->GetStatus()!=EHttpRequestStatus::Succeeded || !Response.IsValid() || Response->GetResponseCode()!=200) {
            UE_LOG(LogMRFPSRoster,Warning,TEXT("MRFPS_ROSTER_PUBLISH_FAILED http=%d"),Response.IsValid()?Response->GetResponseCode():0);
        } else if (!bLoggedSuccess) {
            bLoggedSuccess=true;
            UE_LOG(LogMRFPSRoster,Display,TEXT("MRFPS_ROSTER_PUBLISH_OK instance=%s pid=%u"),*Instance,FPlatformProcess::GetCurrentProcessId());
        }
        Pending.Reset();
    }
    const int64 CapturedAt=FDateTime::UtcNow().ToUnixTimestamp()*1000;
    const double ServerTime=World->GetTimeSeconds();
    const int64 Utc=CapturedAt/1000;
    TArray<TSharedPtr<FJsonValue>> Participants;
    for (FConstPlayerControllerIterator It=World->GetPlayerControllerIterator();It;++It) {
        const auto* PC=It->Get();
        const auto* PS=PC?PC->GetPlayerState<AMRFPSPlayerState>():nullptr;
        const auto* Connection=PC?PC->GetNetConnection():nullptr;
        if (!PC || !Connection || Connection->GetConnectionState()!=USOCK_Open || !PS || PS->IsABot() ||
            !PS->bAuthenticated || PS->AuthExpiresAt<=Utc || PS->GetPlayerId()<0) continue;
        FString Name;
        for (TCHAR C:PS->GetPlayerName()) if (Name.Len()<64 && C>=32 && C!=127) Name.AppendChar(C);
        auto Row=MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("id"),PS->GetPlayerId());
        Row->SetStringField(TEXT("name"),Name);
        Row->SetStringField(TEXT("role"),PS->bAdmin?TEXT("admin"):PS->bInspector?TEXT("inspector"):TEXT("player"));
        const double Age=ServerTime-PS->RosterPing.SampleServerTime;
        if (PS->RosterPing.Milliseconds>=0 && PS->RosterPing.SampleServerTime>=0 && FMath::IsFinite(Age) && Age>=0 && Age<=5.0) {
            Row->SetNumberField(TEXT("ping_ms"),PS->RosterPing.Milliseconds);
            Row->SetNumberField(TEXT("ping_at_ms"),double(CapturedAt-FMath::CeilToInt64(Age*1000.0)));
        } else {
            Row->SetField(TEXT("ping_ms"),MakeShared<FJsonValueNull>());
            Row->SetField(TEXT("ping_at_ms"),MakeShared<FJsonValueNull>());
        }
        Participants.Add(MakeShared<FJsonValueObject>(Row));
    }
    if (const auto* State=World->GetGameState<AMRFPSGameState>(); State && State->bSoloBotSession) {
        for (const APlayerState* Base:State->PlayerArray) {
            const auto* PS=Cast<AMRFPSPlayerState>(Base);
            const auto* Pawn=PS?PS->GetPawn<AMRFPSPawn>():nullptr;
            if (!PS || !PS->IsABot() || PS->Slot<0 || PS->Slot>1 || !MRFPSBot::IsServerBot(Pawn)) continue;
            auto Row=MakeShared<FJsonObject>();
            // Human engine PlayerIds start at 256; 0/1 are reserved for bots.
            Row->SetNumberField(TEXT("id"),PS->Slot);
            Row->SetStringField(TEXT("name"),TEXT("BOT"));
            Row->SetStringField(TEXT("role"),TEXT("bot"));
            Row->SetField(TEXT("ping_ms"),MakeShared<FJsonValueNull>());
            Row->SetField(TEXT("ping_at_ms"),MakeShared<FJsonValueNull>());
            Participants.Add(MakeShared<FJsonValueObject>(Row));
        }
    }
    if (Participants.Num()>4096) {
        UE_LOG(LogMRFPSRoster,Error,TEXT("MRFPS_ROSTER_SNAPSHOT_LIMIT participants=%d"),Participants.Num());
        return;
    }
    const TSharedPtr<FJsonObject> Root=MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("instance"),Instance);
    Root->SetNumberField(TEXT("seq"),double(++Sequence));
    Root->SetNumberField(TEXT("captured_at_ms"),double(CapturedAt));
    Root->SetArrayField(TEXT("participants"),MoveTemp(Participants));
    FString Body;
    if (!FJsonSerializer::Serialize(Root,TJsonWriterFactory<>::Create(&Body))) return;
    const FTCHARToUTF8 BodyBytes(*Body);
    if (BodyBytes.Length()>524288) {
        UE_LOG(LogMRFPSRoster,Error,TEXT("MRFPS_ROSTER_SNAPSHOT_LIMIT bytes=%d"),BodyBytes.Length());
        return;
    }
    const FString Signature=HmacSha256(Secret,Body);
    if (Signature.Len()!=64) return;
    Pending=FHttpModule::Get().CreateRequest();
    Pending->SetURL(TEXT("https://mrfps-admin.mizuame.app/api/server/roster"));
    Pending->SetVerb(TEXT("POST"));
    Pending->SetHeader(TEXT("Content-Type"),TEXT("application/json"));
    Pending->SetHeader(TEXT("X-MRFPS-Signature"),Signature);
    Pending->SetTimeout(6.0f);
    Pending->SetContentAsString(Body);
    if (!Pending->ProcessRequest()) { Pending.Reset(); UE_LOG(LogMRFPSRoster,Warning,TEXT("MRFPS_ROSTER_PUBLISH_FAILED start")); }
}
