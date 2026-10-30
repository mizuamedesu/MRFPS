#include "MRFPSGame.h"
#include "MRFPSBot.h"
#include "MRFPSAdminPanel.h"
#include "MRFPSLoginMenu.h"
#include "MRFPSLocalPoseStatus.h"
#include "MRFPSMenu.h"
#include "MRFPSRoster.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "MRFPSAuthClientSubsystem.h"
#include "MRFPSHapticsComponent.h"
#include "MoBitsQuestStandaloneComponent.h"
#include "MRFPSIpConnection.h"
#if !UE_SERVER
#include "MRFPSViewCaptureSubsystem.h"
#include "GeometryCaptureSubsystem.h"
#endif

namespace
{
FString IdText(const FGuid& Id) { return Id.ToString(EGuidFormats::DigitsWithHyphens).ToLower(); }
FGuid NewLocalId()
{
    FGuid Id=FGuid::NewGuid();
    // FGuid's string order differs from native Windows GUID field layout.
    // Normalize the text's UUID version/variant bits on every platform.
    Id.B=(Id.B&0xffff0fffU)|0x00004000U; Id.C=(Id.C&0x3fffffffU)|0x80000000U;
    return Id;
}
bool ParseId(const FString& Text,FGuid& Id)
{
    return Text.Len()==36 && Text[14]=='4' && (Text[19]=='8'||Text[19]=='9'||Text[19]=='a'||Text[19]=='b') &&
        FGuid::Parse(Text,Id) && IdText(Id)==Text;
}
#if !UE_SERVER
bool ReadPair(const TSharedPtr<FJsonObject>& Params,const TCHAR* Name,FVector2D& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Items=nullptr;
    if (!Params->TryGetArrayField(Name,Items) || !Items || Items->Num()!=2 ||
        !(*Items)[0].IsValid() || !(*Items)[1].IsValid()) return false;
    double X=0,Y=0;
    if (!(*Items)[0]->TryGetNumber(X) || !(*Items)[1]->TryGetNumber(Y)) return false;
    Out=FVector2D(X,Y);
    return FMath::IsFinite(Out.X) && FMath::IsFinite(Out.Y);
}
#endif
TSharedPtr<FJsonObject> ParseObject(const FString& Text)
{
    TSharedPtr<FJsonObject> Object;
    const auto Reader=TJsonReaderFactory<>::Create(Text);
    if(!FJsonSerializer::Deserialize(Reader,Object) || !Object.IsValid()) return nullptr;
    return Object;
}
FString JsonText(const TSharedPtr<FJsonObject>& Object)
{
    FString Text; const auto Writer=TJsonWriterFactory<>::Create(&Text);
    return FJsonSerializer::Serialize(Object.ToSharedRef(),Writer)?Text:FString();
}
bool ReadVector(const TSharedPtr<FJsonObject>& Object,const TCHAR* Key,FVector& Value)
{
    const TArray<TSharedPtr<FJsonValue>>* A=nullptr;
    if(!Object.IsValid() || !Object->TryGetArrayField(Key,A) || !A || A->Num()!=3) return false;
    double X=0,Y=0,Z=0;
    if(!(*A)[0]->TryGetNumber(X)||!(*A)[1]->TryGetNumber(Y)||!(*A)[2]->TryGetNumber(Z) ||
       !FMath::IsFinite(X)||!FMath::IsFinite(Y)||!FMath::IsFinite(Z)) return false;
    Value=FVector(X,Y,Z); return true;
}
bool ReadShape(const TSharedPtr<FJsonObject>& Object,const TCHAR* Key,FMRFPSWallShape& Shape)
{
    const TSharedPtr<FJsonObject>* Value=nullptr;
    if(!Object.IsValid() || !Object->TryGetObjectField(Key,Value) || !Value || !Value->IsValid()) return false;
    double Height=0,Mask=0;
    if(!(*Value)->TryGetNumberField(TEXT("height"),Height)||!(*Value)->TryGetNumberField(TEXT("mask"),Mask)||
       !FMath::IsFinite(Height)||!FMath::IsFinite(Mask)||Height<1||Height>8||Mask<0||Mask>0xffffff ||
       FMath::FloorToDouble(Height)!=Height||FMath::FloorToDouble(Mask)!=Mask) return false;
    Shape.HeightRows=uint8(Height); Shape.SolidCells=uint32(Mask); return Shape.IsSane();
}
bool ReadRevision(const TSharedPtr<FJsonObject>& Object,const TCHAR* Key,uint32& Revision)
{
    double Value=0;
    if(!Object.IsValid()||!Object->TryGetNumberField(Key,Value)||!FMath::IsFinite(Value)||Value<1||Value>MAX_uint32||FMath::FloorToDouble(Value)!=Value) return false;
    Revision=uint32(Value); return true;
}
TSharedPtr<FJsonObject> ShapeJson(const FMRFPSWallShape& Shape)
{
    auto Out=MakeShared<FJsonObject>(); Out->SetNumberField(TEXT("height"),Shape.HeightRows); Out->SetNumberField(TEXT("mask"),Shape.SolidCells); return Out;
}
TArray<TSharedPtr<FJsonValue>> VectorJson(const FVector& Value)
{
    return {MakeShared<FJsonValueNumber>(Value.X),MakeShared<FJsonValueNumber>(Value.Y),MakeShared<FJsonValueNumber>(Value.Z)};
}
}

FString AMRFPSPlayerController::AutomationDirectory() const
{
#if UE_SERVER
    return FString();
#else
    FString Name=TEXT("MRFPS-Client"); FParse::Value(FCommandLine::Get(),TEXT("MRFPSWindowName="),Name);
    if(Name.IsEmpty()||Name.Len()>64) return FString();
    for(TCHAR C:Name) if(!((C>='A'&&C<='Z')||(C>='a'&&C<='z')||(C>='0'&&C<='9')||C=='_'||C=='-')) return FString();
    FString Base;
#if PLATFORM_ANDROID
    Base=UMRFPSAuthClientSubsystem::PrivateDirectory();
    return Base.IsEmpty()||FPaths::IsRelative(Base)?FString():FPaths::Combine(Base,TEXT("Automation"),Name);
#else
#if PLATFORM_WINDOWS
    Base=FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
#elif PLATFORM_MAC
    Base=FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
    if(!Base.IsEmpty()) Base=FPaths::Combine(Base,TEXT("Library/Application Support"));
#else
    Base=FPlatformMisc::GetEnvironmentVariable(TEXT("XDG_STATE_HOME"));
    if(Base.IsEmpty()) { Base=FPlatformMisc::GetEnvironmentVariable(TEXT("HOME")); if(!Base.IsEmpty()) Base=FPaths::Combine(Base,TEXT(".local/state")); }
#endif
    return Base.IsEmpty()||FPaths::IsRelative(Base)?FString():FPaths::Combine(Base,TEXT("MRFPS/Automation"),Name);
#endif
#endif
}

bool AMRFPSPlayerController::WriteAutomationResult(const FString& Folder,const FGuid& Id,const FString& Status,const FString& Code,const TSharedPtr<FJsonObject>& Data) const
{
    auto Result=MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("id"),IdText(Id)); Result->SetStringField(TEXT("status"),Status); Result->SetStringField(TEXT("code"),Code);
    if(Data.IsValid()) Result->SetObjectField(TEXT("data"),Data);
    const FString Path=FPaths::Combine(Folder,TEXT("result.json"));
    const FString Temporary=Path+TEXT(".")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".tmp");
    if(!FFileHelper::SaveStringToFile(JsonText(Result),*Temporary,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
    if(!IFileManager::Get().Move(*Path,*Temporary,true,true,false,true)) { IFileManager::Get().Delete(*Temporary); return false; }
    return true;
}

bool AMRFPSPlayerController::DesktopAutomationAllowed() const
{
#if UE_SERVER
    return false;
#else
    const auto* Fighter=GetPawn<AMRFPSPawn>();
    const auto* AuthenticatedState=GetPlayerState<AMRFPSPlayerState>();
    const auto* State=GetWorld()?GetWorld()->GetGameState<AMRFPSGameState>():nullptr;
    return IsLocalController() && GetNetMode()==NM_Client && Fighter && Fighter->IsLocallyControlled() &&
        !Fighter->IsXR() && Fighter->Pose.bDesktop && Fighter->Pose.bHeadTracked && Fighter->Health>0 &&
        Fighter->LastLocalPoseSampleAt>=0 && FPlatformTime::Seconds()-Fighter->LastLocalPoseSampleAt<=.3 &&
        AuthenticatedState && AuthenticatedState->bAuthenticated && !AuthenticatedState->bInspector && !AuthenticatedState->bAdmin && AuthenticatedState->Slot>=0 && AuthenticatedState->Slot<2 &&
        AuthenticatedState->AuthExpiresAt>FDateTime::UtcNow().ToUnixTimestamp() && State && !State->bCombatPaused &&
        State->Phase!=EMRFPSMatchPhase::Finished && !IsMoveInputIgnored() && !IsLookInputIgnored() &&
        !(SessionMenu && SessionMenu->IsInViewport()) && !(AdminPanel && AdminPanel->IsInViewport()) &&
        !(DeviceLogin && DeviceLogin->IsOpen());
#endif
}

void AMRFPSPlayerController::ClearAutomationInput()
{
#if !UE_SERVER
    AutomationInput.Clear();
    AutomationInputPawn.Reset();
    AutomationMoveSeconds=0;
#endif
}

void AMRFPSPlayerController::TickAutomationInput(float DeltaSeconds)
{
#if !UE_SERVER
    AutomationMoveSeconds=0;
    if (!AutomationInputPawn.IsValid()) { AutomationInput.Clear(); return; }
    auto* Fighter=AutomationInputPawn.Get();
    if (Fighter!=GetPawn<AMRFPSPawn>() || !DesktopAutomationAllowed() || !AutomationInput.Active(FPlatformTime::Seconds())) {
        ClearAutomationInput(); return;
    }
    // Only integrate elapsed time after this lease was accepted. A stalled
    // frame cannot retroactively apply input or produce a large camera jump.
    const double Now=FPlatformTime::Seconds();
    const float Step=float(AutomationInput.ConsumeStep(Now,DeltaSeconds));
    AutomationMoveSeconds=Step;
    Fighter->DesktopLook(float(AutomationInput.LookDegreesPerSecond.X)*Step,float(AutomationInput.LookDegreesPerSecond.Y)*Step);
#endif
}

void AMRFPSPlayerController::TickAutomation()
{
#if !UE_SERVER
    const double Now=FPlatformTime::Seconds(); if(Now<NextAutomationAt) return; NextAutomationAt=Now+.25;
    const FString Directory=AutomationDirectory(); if(Directory.IsEmpty() || !IFileManager::Get().DirectoryExists(*Directory)) return;
    TArray<FString> Folders; IFileManager::Get().FindFiles(Folders,*FPaths::Combine(Directory,TEXT("*")),false,true); Folders.Sort();
    for(const FString& Name:Folders) {
        FGuid Id; if(!ParseId(Name,Id)) continue;
        const FString Folder=FPaths::Combine(Directory,Name), RequestFile=FPaths::Combine(Folder,TEXT("request.json"));
        const FString ResultFile=FPaths::Combine(Folder,TEXT("result.json")), Started=FPaths::Combine(Folder,TEXT("started"));
        if(IFileManager::Get().FileExists(*ResultFile)||!IFileManager::Get().FileExists(*RequestFile)) continue;
        if(IFileManager::Get().FileExists(*Started)) {
            if(!AutomationPending.Contains(Id)) WriteAutomationResult(Folder,Id,TEXT("unknown"),TEXT("CLIENT_RESTARTED_BEFORE_REPLY"));
            continue;
        }
        const int64 Size=IFileManager::Get().FileSize(*RequestFile); FString Text;
        if(Size<1||Size>8192||!FFileHelper::LoadFileToString(Text,*RequestFile)) { WriteAutomationResult(Folder,Id,TEXT("error"),TEXT("INVALID_REQUEST")); continue; }
        const auto Request=ParseObject(Text); FString InId,Operation;
        const TSharedPtr<FJsonObject>* ParamsPtr=nullptr; double Version=0;
        if(!Request.IsValid()||!Request->TryGetStringField(TEXT("id"),InId)||InId!=Name||
           !Request->TryGetNumberField(TEXT("version"),Version)||Version!=1||
           !Request->TryGetStringField(TEXT("operation"),Operation)||!Request->TryGetObjectField(TEXT("parameters"),ParamsPtr)||!ParamsPtr||!ParamsPtr->IsValid()) {
            WriteAutomationResult(Folder,Id,TEXT("error"),TEXT("INVALID_REQUEST")); continue;
        }
        const TSharedPtr<FJsonObject> Params=*ParamsPtr;
        // Durable marker before any effect. If the process dies mid-operation,
        // the next instance reports unknown instead of silently executing twice.
        if(!FFileHelper::SaveStringToFile(TEXT("started"),*Started,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { WriteAutomationResult(Folder,Id,TEXT("error"),TEXT("JOURNAL_WRITE_FAILED")); continue; }
        FString Code=TEXT("AUTOMATION_OK"); bool Ok=true,Already=false,Submitted=false;
        auto Data=MakeShared<FJsonObject>();
        const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
        if(Operation==TEXT("status")) {
            Data->SetBoolField(TEXT("inspector"),IsInspector()); Data->SetBoolField(TEXT("admin"),IsAdmin());
            Data->SetBoolField(TEXT("connected"),GetNetMode()!=NM_Standalone); Data->SetNumberField(TEXT("view_slot"),ViewSlot);
            // Nonsecret local identity for correlating repeated status snapshots.
            // The pin is captured for this connection during EncryptionAck, not
            // fetched from a potentially replaced credential after it connects.
            auto Identity=MakeShared<FJsonObject>();
            Identity->SetNumberField(TEXT("process_id"),FPlatformProcess::GetCurrentProcessId());
            const bool InspectorSession=IsVideoSessionValid(false);
            Identity->SetBoolField(TEXT("inspector_session_valid"),InspectorSession);
            if (InspectorSession) if (const auto* Connection=Cast<UMRFPSIpConnection>(GetNetConnection())) {
                const FGuid ConnectionId=Connection->GetDiagnosticConnectionId();
                const FString Pin=Connection->GetDiagnosticServerPin();
                if (ConnectionId.IsValid() && !Pin.IsEmpty()) {
                    Identity->SetStringField(TEXT("connection_id"),ConnectionId.ToString(EGuidFormats::Digits).ToLower());
                    Identity->SetStringField(TEXT("server_certificate_sha256"),Pin);
                }
            }
            Data->SetObjectField(TEXT("video_measurement_identity"),Identity);
            Data->SetBoolField(TEXT("multiview"),bMultiView); Data->SetBoolField(TEXT("reconstructed"),bReconstructedViews);
            Data->SetBoolField(TEXT("mouse_released"),bInspectorMouseReleased); Data->SetBoolField(TEXT("build_mode"),bAdminWallPlacement);
            Data->SetNumberField(TEXT("player_window_mask"),PlayerVideoWindowMask()); Data->SetBoolField(TEXT("inspector_window"),InspectorWindowCamera!=nullptr);
            Data->SetBoolField(TEXT("session_menu"),SessionMenu&&SessionMenu->IsInViewport());
            if (SessionMenu && SessionMenu->IsInViewport())
                Data->SetStringField(TEXT("session_menu_tab"),SessionMenu->IsRosterVisible()?TEXT("participants"):TEXT("settings"));
            Data->SetBoolField(TEXT("admin_panel"),AdminPanel&&AdminPanel->IsInViewport());
            Data->SetBoolField(TEXT("login_panel"),DeviceLogin&&DeviceLogin->IsOpen());
            if (const auto* Instance=GetGameInstance<UMRFPSGameInstance>()) {
                using MRFPSConnectionStatus::EPhase;
                const auto Phase=Instance->ConnectionStatus.Phase;
                Data->SetStringField(TEXT("connection_status"),Phase==EPhase::Connecting?TEXT("connecting"):
                    Phase==EPhase::Joined?TEXT("joined"):Phase==EPhase::Failure?TEXT("failed"):TEXT("idle"));
                Data->SetBoolField(TEXT("join_notice_pending"),Instance->ConnectionStatus.ShowSuccess());
                if (Phase==EPhase::Failure) Data->SetStringField(TEXT("connection_message"),Instance->ConnectionFailureMessage());
            }
            if(const auto* Fighter=GetPawn<AMRFPSPawn>(); Fighter && Fighter->IsLocallyControlled()) {
                auto PlayerInfo=MakeShared<FJsonObject>();
                PlayerInfo->SetStringField(TEXT("combat_source"),TEXT("replicated_snapshot_age_unknown"));
                PlayerInfo->SetNumberField(TEXT("health"),Fighter->Health);
                PlayerInfo->SetNumberField(TEXT("magazine_rounds"),Fighter->MagazineRounds);
                PlayerInfo->SetNumberField(TEXT("magazine_capacity"),AMRFPSPawn::MagazineCapacity);
                PlayerInfo->SetNumberField(TEXT("shield_energy"),Fighter->ShieldEnergy);
                PlayerInfo->SetBoolField(TEXT("shield_active"),Fighter->bShieldActive);
                PlayerInfo->SetBoolField(TEXT("calibrated"),Fighter->bCalibrated);
                PlayerInfo->SetBoolField(TEXT("xr"),Fighter->IsXR());
                const double SampleAge=Fighter->LastLocalPoseSampleAt<0?-1:FMath::Max(0.,FPlatformTime::Seconds()-Fighter->LastLocalPoseSampleAt);
                PlayerInfo->SetNumberField(TEXT("local_pose_sample_age_seconds"),SampleAge);
                const bool SampleFresh=SampleAge>=0 && SampleAge<=.3;
                PlayerInfo->SetBoolField(TEXT("local_pose_sample_fresh"),SampleFresh);
                PlayerInfo->SetBoolField(TEXT("head_tracked"),SampleFresh && Fighter->Pose.bHeadTracked);
                MRFPSLocalPoseStatus::Write(*PlayerInfo,Fighter->Pose.Head,Fighter->Pose.HeadRotation,SampleFresh,Fighter->Pose.bHeadTracked);
                PlayerInfo->SetBoolField(TEXT("left_tracked"),SampleFresh && Fighter->Pose.bLeftTracked);
                PlayerInfo->SetBoolField(TEXT("right_tracked"),SampleFresh && Fighter->Pose.bRightTracked);
                PlayerInfo->SetBoolField(TEXT("physical_fire_down"),Fighter->IsPhysicalFireDown());
                PlayerInfo->SetBoolField(TEXT("reload_held"),IsReloadHeld());
                PlayerInfo->SetNumberField(TEXT("reload_progress"),ReloadProgress());
                PlayerInfo->SetBoolField(TEXT("grenade_held"),Fighter->IsGrenadeHeld());
                PlayerInfo->SetNumberField(TEXT("flash_intensity"),Fighter->FlashIntensity());
                PlayerInfo->SetBoolField(TEXT("fire_release_required"),Fighter->IsFireReleaseRequired());
                const double FireTime=Fighter->LocalFireTime();
                PlayerInfo->SetNumberField(TEXT("last_fire_request_age_seconds"),FireTime<0?-1:FMath::Max(0.,GetWorld()->GetTimeSeconds()-FireTime));
                if (const auto* GI=Cast<UMRFPSGameInstance>(GetGameInstance())) {
                    PlayerInfo->SetBoolField(TEXT("fire_menu_locked"),GI->FireTransition.Login);
                    PlayerInfo->SetBoolField(TEXT("physical_fire_locked"),!GI->FireTransition.CanPhysical());
                }
                const bool InputAllowed=DesktopAutomationAllowed();
                PlayerInfo->SetBoolField(TEXT("desktop_input_allowed"),InputAllowed);
                const bool LeaseActive=InputAllowed && AutomationInputPawn.Get()==Fighter && AutomationInput.Active(FPlatformTime::Seconds());
                PlayerInfo->SetBoolField(TEXT("automation_input_active"),LeaseActive);
                PlayerInfo->SetBoolField(TEXT("automation_fire_requested"),LeaseActive && AutomationInput.Fire);
                PlayerInfo->SetBoolField(TEXT("automation_shield_requested"),LeaseActive && AutomationInput.Shield);
                Data->SetObjectField(TEXT("player"),PlayerInfo);
            }
            if (const auto* Capture=GetGameInstance()->GetSubsystem<UMRFPSGeometryCaptureSubsystem>()) {
                auto Geometry=MakeShared<FJsonObject>();
                const auto* Fighter=GetPawn<AMRFPSPawn>(); FTransform Transform; double Since=0;
                Geometry->SetBoolField(TEXT("publisher_session_valid"),IsGeometrySessionValid(true));
                Geometry->SetBoolField(TEXT("capture_requested"),bGeometryCaptureRequested);
                Geometry->SetBoolField(TEXT("capture_fresh"),Capture->IsCapturing());
                Geometry->SetBoolField(TEXT("calibration_acknowledged"),Fighter && Fighter->GetGeometryCalibration(Transform,Since));
                Geometry->SetNumberField(TEXT("capture_tracking_epoch"),double(Capture->GetTrackingSpaceEpoch()));
                Geometry->SetNumberField(TEXT("calibrated_tracking_epoch"),Fighter?double(Fighter->GetGeometryCalibrationEpoch()):0);
                Geometry->SetNumberField(TEXT("native_sequence"),GeometryNativeSequence);
                Geometry->SetNumberField(TEXT("published_sequence"),GeometrySequence);
                Geometry->SetStringField(TEXT("status"),Capture->GetSafeStatus());
                Data->SetObjectField(TEXT("geometry_capture"),Geometry);
            }
            if (auto* Capture=GetGameInstance()->GetSubsystem<UMRFPSViewCaptureSubsystem>()) {
                auto Video=MakeShared<FJsonObject>();
                const auto CaptureState=CaptureControlState(Capture,FPlatformTime::Seconds(),true);
                Video->SetBoolField(TEXT("publisher_session_valid"),IsVideoSessionValid(true));
                Video->SetBoolField(TEXT("capture_requested"),CaptureState.Requested);
                Video->SetBoolField(TEXT("capture_session_active"),CaptureState.Session);
                Video->SetBoolField(TEXT("capture_error"),CaptureState.Error);
                Video->SetBoolField(TEXT("consent_pending"),CaptureState.ConsentPending || CaptureState.FlowBusy);
                Video->SetBoolField(TEXT("client_consent_pause_active"),CaptureState.LocalPause);
                Video->SetBoolField(TEXT("server_stream_snapshot_open"),CaptureState.ServerStream);
                Video->SetBoolField(TEXT("capture_fresh"),CaptureState.Fresh);
                Video->SetStringField(TEXT("capture_status"),Capture->GetSafeStatus());
                Video->SetNumberField(TEXT("publisher_frames_queued_lifetime"),double(VideoPublisherQueue.PushedFrames));
                Video->SetNumberField(TEXT("publisher_frames_sent_lifetime"),double(VideoPublisherQueue.FinishedFrames));
                Video->SetNumberField(TEXT("publisher_frames_replaced_lifetime"),double(VideoPublisherQueue.ReplacedFrames));
                Data->SetObjectField(TEXT("video_capture"),Video);
            }
            if(IsInspector()) {
                FVector Position; FRotator Rotation; GetPlayerViewPoint(Position,Rotation);
                Data->SetArrayField(TEXT("camera_position"),VectorJson(Position));
                Data->SetArrayField(TEXT("camera_rotation"),VectorJson(FVector(Rotation.Pitch,Rotation.Yaw,Rotation.Roll)));
                if(InspectorWindowCamera) {
                    const FRotator Extra=InspectorWindowCamera->GetComponentRotation();
                    Data->SetArrayField(TEXT("inspector_camera_position"),VectorJson(InspectorWindowCamera->GetComponentLocation()));
                    Data->SetArrayField(TEXT("inspector_camera_rotation"),VectorJson(FVector(Extra.Pitch,Extra.Yaw,Extra.Roll)));
                }
                TArray<TSharedPtr<FJsonValue>> Streams;
                for(int32 Slot=0;Slot<2;++Slot) {
                    auto Item=MakeShared<FJsonObject>();
                    Item->SetNumberField(TEXT("slot"),Slot);
                    const auto* Source=FindPlayer(Slot);
                    const auto* SourceState=Source?Source->GetPlayerState<AMRFPSPlayerState>():nullptr;
                    Item->SetNumberField(TEXT("stream_id"),SourceState?SourceState->VideoStreamId:0);
                    Item->SetStringField(TEXT("status"),GetQuestVideoStatus(Slot));
                    Item->SetBoolField(TEXT("current_stream_texture_fresh"),GetQuestVideoTexture(Slot)!=nullptr);
                    Item->SetNumberField(TEXT("completed_frames_lifetime"),double(VideoDownloads[Slot].CompletedFrames));
                    Item->SetNumberField(TEXT("expired_frames_lifetime"),double(VideoDownloads[Slot].ExpiredFrames));
                    Item->SetNumberField(TEXT("recovered_chunks_lifetime"),double(VideoDownloads[Slot].RecoveredChunks));
                    Streams.Add(MakeShared<FJsonValueObject>(Item));
                }
                Data->SetArrayField(TEXT("video_streams"),Streams);
            }
            if(const auto* Fighter=GetPawn<AMRFPSPawn>(); Fighter && Fighter->Haptics) {
                Data->SetNumberField(TEXT("haptics_connection"),int32(Fighter->Haptics->GetConnectionState())); Data->SetBoolField(TEXT("haptics_armed"),Fighter->Haptics->IsHapticsArmed());
                if (Fighter->QuestHaptics) {
                    const auto State=Fighter->QuestHaptics->GetStatus();
                    Data->SetBoolField(TEXT("haptics_armed"),State.Phase==TEXT("armed"));
                    Data->SetStringField(TEXT("quest_mobits_phase"),State.Phase);
                    Data->SetNumberField(TEXT("quest_mobits_code"),State.Code);
                    Data->SetStringField(TEXT("quest_mobits_message"),State.Message);
                    Data->SetBoolField(TEXT("quest_mobits_has_session"),State.bHasSession);
                    Data->SetNumberField(TEXT("quest_mobits_stop_unconfirmed"),State.StopUnconfirmedMask);
                    TArray<TSharedPtr<FJsonValue>> Devices;
                    for (const auto& Device:State.Devices) {
                        auto Row=MakeShared<FJsonObject>(); Row->SetStringField(TEXT("id"),Device.Id);
                        Row->SetNumberField(TEXT("model"),Device.Model); Row->SetBoolField(TEXT("connected"),Device.bConnected);
                        Row->SetBoolField(TEXT("paired"),Device.bPaired); Row->SetBoolField(TEXT("pairing"),Device.bPairing);
                        Devices.Add(MakeShared<FJsonValueObject>(Row));
                    }
                    Data->SetArrayField(TEXT("quest_mobits_devices"),Devices);
                }
            }
            if(GS) { Data->SetNumberField(TEXT("phase"),int32(GS->Phase)); Data->SetBoolField(TEXT("paused"),GS->bCombatPaused);
                Data->SetBoolField(TEXT("solo_bot"),GS->bSoloBotSession); Data->SetBoolField(TEXT("solo_geometry_ready"),GS->bSoloGeometryReady);
                Data->SetBoolField(TEXT("solo_measured_geometry_received"),GS->bSoloGeometryReady);
                Data->SetStringField(TEXT("solo_obstacle_data"),!GS->bSoloBotSession?TEXT("not_solo"):GS->bSoloGeometryReady?TEXT("received"):TEXT("not_received"));
                Data->SetNumberField(TEXT("round"),GS->RoundNumber); Data->SetNumberField(TEXT("completed_rounds"),GS->CompletedRounds);
                Data->SetNumberField(TEXT("max_rounds"),AMRFPSGameState::MaximumRounds); Data->SetNumberField(TEXT("round_seconds_remaining"),GS->RoundSecondsRemaining());
                Data->SetNumberField(TEXT("round_wins_0"),GS->RoundWinsSlot0); Data->SetNumberField(TEXT("round_wins_1"),GS->RoundWinsSlot1);
                Data->SetNumberField(TEXT("last_round_winner_slot"),GS->LastRoundWinnerSlot); Data->SetBoolField(TEXT("last_round_draw"),GS->CompletedRounds>0 && GS->LastRoundWinnerSlot<0);
                Data->SetNumberField(TEXT("match_winner_slot"),GS->WinnerSlot); Data->SetBoolField(TEXT("match_draw"),GS->Phase==EMRFPSMatchPhase::Finished && GS->WinnerSlot<0);
                Data->SetBoolField(TEXT("round_ready_waiting"),GS->IsAwaitingRoundReady());
                Data->SetNumberField(TEXT("round_ready_window"),GS->RoundReadyWindow);
                Data->SetNumberField(TEXT("round_ready_mask"),GS->RoundReadyMask);
                Data->SetNumberField(TEXT("round_ready_required_mask"),GS->RequiredRoundReadyMask());
                const auto* NoticePawn=GetPawn<AMRFPSPawn>(); const auto Notice=GS->RoundNotice(NoticePawn?NoticePawn->GetSlot():-1);
                auto RoundNotice=MakeShared<FJsonObject>(); RoundNotice->SetBoolField(TEXT("visible"),Notice.bVisible);
                RoundNotice->SetNumberField(TEXT("countdown"),Notice.Countdown); RoundNotice->SetNumberField(TEXT("progress"),Notice.Progress);
                RoundNotice->SetStringField(TEXT("main"),Notice.Main); RoundNotice->SetStringField(TEXT("detail"),Notice.Detail);
                Data->SetObjectField(TEXT("round_notice"),RoundNotice);
                Data->SetBoolField(TEXT("shared_floor_confirmed"),GS->bSharedFloorConfirmed); Data->SetBoolField(TEXT("layout_calibrated"),GS->Layout.bCalibrated);
                Data->SetNumberField(TEXT("layout_revision"),GS->Layout.Revision); Data->SetNumberField(TEXT("control_revision"),GS->ControlRevision);
                Data->SetNumberField(TEXT("width_cm"),GS->Layout.WidthCm); Data->SetNumberField(TEXT("length_cm"),GS->Layout.LengthCm); }
        } else if(Operation==TEXT("roster.list")) {
            if (!MRFPSRoster::CanView(this)) { Ok=false; Code=TEXT("INSPECTOR_REQUIRED"); }
            else {
                TArray<TSharedPtr<FJsonValue>> Participants;
                for (const auto& Row:MRFPSRoster::Collect(this)) {
                    auto Item=MakeShared<FJsonObject>();
                    const TCHAR* RoleName=Row.Role==MRFPSRosterPolicy::ERole::Admin?TEXT("admin"):
                        Row.Role==MRFPSRosterPolicy::ERole::Inspector?TEXT("inspector"):
                        Row.Role==MRFPSRosterPolicy::ERole::Bot?TEXT("bot"):TEXT("player");
                    Item->SetStringField(TEXT("name"),Row.Name); Item->SetStringField(TEXT("role"),RoleName);
                    Item->SetNumberField(TEXT("id"),Row.Id); Item->SetNumberField(TEXT("slot"),Row.Slot);
                    Item->SetBoolField(TEXT("self"),Row.Self);
                    if (Row.Ping>=0) Item->SetNumberField(TEXT("ping_ms"),Row.Ping);
                    else Item->SetField(TEXT("ping_ms"),MakeShared<FJsonValueNull>());
                    Participants.Add(MakeShared<FJsonValueObject>(Item));
                }
                Data->SetArrayField(TEXT("participants"),Participants);
            }
        } else if(Operation==TEXT("objects.list")) {
            if(!IsInspector()) { Ok=false; Code=TEXT("INSPECTOR_REQUIRED"); }
            else {
                TArray<TSharedPtr<FJsonValue>> Objects;
                for(TActorIterator<AMRFPSBarrier> It(GetWorld());It;++It) if(It->bAdminPlaced&&!It->bPlacementPreview) {
                    AMRFPSBarrier* Actor=*It; FGuid TargetId;
                    for(const auto& Entry:AutomationTargets) if(Entry.Value.Get()==Actor) { TargetId=Entry.Key; break; }
                    if(!TargetId.IsValid()) { TargetId=NewLocalId(); AutomationTargets.Add(TargetId,Actor); }
                    auto Item=MakeShared<FJsonObject>(); Item->SetStringField(TEXT("target"),IdText(TargetId));
                    Item->SetStringField(TEXT("kind"),Actor->BuildKind==EMRFPSBuildKind::Crate?TEXT("crate"):TEXT("wall"));
                    Item->SetArrayField(TEXT("location"),VectorJson(Actor->GetActorLocation())); Item->SetNumberField(TEXT("yaw"),Actor->GetActorRotation().Yaw);
                    Item->SetObjectField(TEXT("shape"),ShapeJson(Actor->WallShape)); Objects.Add(MakeShared<FJsonValueObject>(Item));
                }
                Data->SetArrayField(TEXT("objects"),Objects);
                if(GS) { Data->SetNumberField(TEXT("layout_revision"),GS->Layout.Revision); Data->SetNumberField(TEXT("control_revision"),GS->ControlRevision); }
            }
        } else if(Operation==TEXT("view.set")) {
            FString View; if(!IsInspector()||!Params->TryGetStringField(TEXT("view"),View)) { Ok=false; Code=TEXT("INVALID_VIEW"); }
            else if(View==TEXT("free")) { Already=ViewSlot<0; if(!Already) FreeView(); }
            else if(View==TEXT("player1")||View==TEXT("player2")) {
                const int32 Slot=View==TEXT("player1")?0:1;
                if(!FindPlayer(Slot)) { Ok=false; Code=TEXT("PLAYER_UNAVAILABLE"); }
                else { Already=ViewSlot==Slot; if(!Already) MRFPSView(Slot); }
            } else { Ok=false; Code=TEXT("INVALID_VIEW"); }
            if(Ok && SessionMenu && SessionMenu->IsInViewport()) SessionMenu->Close();
        } else if(Operation==TEXT("multiview.set")||Operation==TEXT("reconstructed.set")||Operation==TEXT("focus.set")||Operation==TEXT("build-mode.set")) {
            bool Enabled=false;
            if(!Params->TryGetBoolField(TEXT("enabled"),Enabled)||!IsInspector()) { Ok=false; Code=TEXT("INVALID_STATE"); }
            else if(Operation==TEXT("multiview.set")) {
                // Building owns the camera and forces tiles off. Leave build mode
                // through the same transition as the GUI before changing tiles.
                if(bAdminWallPlacement) SetAdminWallPlacement(false);
                Already=bMultiView==Enabled; if(!Already) ToggleMultiView();
            }
            else if(Operation==TEXT("reconstructed.set")) { Already=bReconstructedViews==Enabled; if(!Already) MRFPSReconstructedViews(); }
            else if(Operation==TEXT("focus.set")) {
                if((SessionMenu&&SessionMenu->IsInViewport())||(AdminPanel&&AdminPanel->IsInViewport())||(DeviceLogin&&DeviceLogin->IsOpen())) { Ok=false; Code=TEXT("UI_INPUT_BUSY"); }
                else { Already=bInspectorMouseReleased==Enabled; if(!Already) SetInspectorMouseReleased(Enabled); if(bInspectorMouseReleased!=Enabled) { Ok=false; Code=TEXT("UI_INPUT_BUSY"); } }
            }
            else if(!IsAdmin()) { Ok=false; Code=TEXT("ADMIN_REQUIRED"); }
            else { Already=bAdminWallPlacement==Enabled; if(!Already) SetAdminWallPlacement(Enabled); }
        } else if(Operation==TEXT("window.set")) {
            FString Window; bool Open=false;
            if(!IsInspector()||!Params->TryGetStringField(TEXT("window"),Window)||!Params->TryGetBoolField(TEXT("open"),Open)) { Ok=false; Code=TEXT("INVALID_WINDOW"); }
            else if(Window==TEXT("inspector")) { const bool Exists=InspectorWindowCamera!=nullptr; Already=Exists==Open; if(!Already) { if(Open) OpenInspectorViewWindow(); else CloseInspectorViewWindow(); } if(Open&&!InspectorWindowCamera) { Ok=false; Code=TEXT("WINDOW_UNAVAILABLE"); } }
            else if(Window==TEXT("player1")||Window==TEXT("player2")) { const int32 Slot=Window==TEXT("player1")?0:1; const bool Exists=(PlayerVideoWindowMask()&(1u<<Slot))!=0; Already=Exists==Open;
                if(!Already) { if(Open) OpenPlayerVideoWindow(Slot); else ClosePlayerVideoWindow(Slot); }
                if(Open&&!(PlayerVideoWindowMask()&(1u<<Slot))) { Ok=false; Code=TEXT("WINDOW_UNAVAILABLE"); }
            } else { Ok=false; Code=TEXT("INVALID_WINDOW"); }
        } else if(Operation==TEXT("menu.set")) {
            FString Menu,Tab; bool Open=false;
            const bool HasTab=Params->HasField(TEXT("tab"));
            const bool TabValid=!HasTab || (Params->TryGetStringField(TEXT("tab"),Tab) && (Tab==TEXT("settings") || Tab==TEXT("participants")));
            if(!Params->TryGetStringField(TEXT("menu"),Menu)||!Params->TryGetBoolField(TEXT("open"),Open)||!TabValid||
                (HasTab && (Menu!=TEXT("session") || !Open))) { Ok=false; Code=TEXT("INVALID_MENU"); }
            else if(Menu==TEXT("session")) {
                const bool WantRoster=HasTab && Tab==TEXT("participants");
                if (WantRoster && !MRFPSRoster::CanView(this)) { Ok=false; Code=TEXT("INSPECTOR_REQUIRED"); }
                const bool Exists=SessionMenu&&SessionMenu->IsInViewport();
                Already=Exists==Open && (!HasTab || (SessionMenu && SessionMenu->IsRosterVisible()==WantRoster));
                if(Ok && Exists!=Open) ToggleMenu();
                if(Ok && (SessionMenu&&SessionMenu->IsInViewport())!=Open) { Ok=false; Code=TEXT("MENU_UNAVAILABLE"); }
                if (Ok && HasTab && (!SessionMenu || !SessionMenu->SetRosterVisible(WantRoster))) { Ok=false; Code=TEXT("MENU_UNAVAILABLE"); }
            } else if(Menu==TEXT("admin")) {
                if(!IsAdmin()) { Ok=false; Code=TEXT("ADMIN_REQUIRED"); }
                else { const bool Exists=AdminPanel&&AdminPanel->IsInViewport(); Already=Exists==Open; if(!Already) ToggleAdminPanel();
                    if((AdminPanel&&AdminPanel->IsInViewport())!=Open) { Ok=false; Code=TEXT("MENU_UNAVAILABLE"); } }
            } else if(Menu==TEXT("login")) {
                const bool Exists=DeviceLogin&&DeviceLogin->IsOpen(); Already=Exists==Open;
#if PLATFORM_ANDROID
                // Quest can show the login overlay without leaving a session.
                // Call even for an already-closed panel to cancel startup open.
                SetQuestDeviceLoginVisible(Open);
#else
                if(!Already && Open && GetNetMode()!=NM_Standalone) {
                    WriteAutomationResult(Folder,Id,TEXT("submitted"),TEXT("LOGIN_SCREEN_REQUESTED"),Data); OpenDeviceLogin(); return;
                }
                if(!Already) { if(Open) OpenDeviceLogin(); else if(DeviceLogin) DeviceLogin->CloseLogin(); }
#endif
                if((DeviceLogin&&DeviceLogin->IsOpen())!=Open) { Ok=false; Code=TEXT("MENU_UNAVAILABLE"); }
            } else if(Menu==TEXT("mobits")) {
#if PLATFORM_ANDROID
                const bool Exists=DeviceLogin && DeviceLogin->IsMoBitsOpen(); Already=Exists==Open;
                if (!Already && Open) { SetQuestDeviceLoginVisible(true); if (DeviceLogin) DeviceLogin->ShowMoBits(); }
                else if (!Already && DeviceLogin) DeviceLogin->CloseLogin();
                if ((DeviceLogin && DeviceLogin->IsMoBitsOpen())!=Open) { Ok=false; Code=TEXT("MENU_UNAVAILABLE"); }
#else
                Ok=false; Code=TEXT("MENU_UNAVAILABLE");
#endif
            } else { Ok=false; Code=TEXT("INVALID_MENU"); }
        } else if(Operation==TEXT("connection.set")) {
            bool Connected=false;
            if(!Params->TryGetBoolField(TEXT("connected"),Connected)) { Ok=false; Code=TEXT("INVALID_STATE"); }
            else if(Connected) { Ok=false; Code=TEXT("USE_DEVICE_LOGIN"); }
            else {
                switch (RequestDisconnectToLogin()) {
                case EMRFPSDisconnectResult::AlreadyDisconnected: Already=true; Code=TEXT("DISCONNECTED"); break;
                case EMRFPSDisconnectResult::Pending: Submitted=true; Code=TEXT("DISCONNECT_PENDING"); break;
                case EMRFPSDisconnectResult::Submitted: Submitted=true; Code=TEXT("DISCONNECT_REQUESTED"); break;
                default: Ok=false; Code=TEXT("DISCONNECT_UNAVAILABLE"); break;
                }
            }
        } else if(Operation==TEXT("camera.set")) {
            FString CameraTarget; FVector Position,Angles;
            if(!IsInspector()||!Params->TryGetStringField(TEXT("camera"),CameraTarget)||!ReadVector(Params,TEXT("position"),Position)||
               !ReadVector(Params,TEXT("rotation"),Angles)||Position.SizeSquared()>FMath::Square(25000.)||Angles.GetAbsMax()>360000) { Ok=false; Code=TEXT("INVALID_CAMERA"); }
            else {
                const FRotator Wanted=FRotator(Angles.X,Angles.Y,Angles.Z).GetNormalized();
                if(CameraTarget==TEXT("main")) {
                    auto* Observer=GetPawn<AMRFPSInspectorPawn>();
                    if(!Observer) { Ok=false; Code=TEXT("INSPECTOR_REQUIRED"); }
                    else {
                        Already=ViewSlot<0&&!bAdminWallPlacement&&Observer->GetActorLocation().Equals(Position,.1)&&GetControlRotation().Equals(Wanted,.01);
                        if(!Already) { SetAdminWallPlacement(false); FreeView(); Observer->SetActorLocation(Position,false,nullptr,ETeleportType::TeleportPhysics); SetControlRotation(Wanted); }
                    }
                } else if(CameraTarget==TEXT("inspector")) {
                    if(!InspectorWindowCamera) { Ok=false; Code=TEXT("WINDOW_UNAVAILABLE"); }
                    else { Already=InspectorWindowCamera->GetComponentLocation().Equals(Position,.1)&&InspectorWindowCamera->GetComponentRotation().Equals(Wanted,.01);
                        if(!Already) InspectorWindowCamera->SetWorldLocationAndRotation(Position,Wanted); }
                } else { Ok=false; Code=TEXT("INVALID_CAMERA"); }
            }
        } else if(Operation==TEXT("player.input.clear")) {
            Already=!AutomationInput.Active(FPlatformTime::Seconds());
            ClearAutomationInput();
        } else if(Operation==TEXT("player.input.set")) {
            bool Fire=false,Shield=false; FVector2D Move,Look; double Duration=0;
            FMRFPSLocalInputLease Requested;
            if(!DesktopAutomationAllowed()) { Ok=false; Code=TEXT("DESKTOP_PLAYER_UNAVAILABLE"); }
            else if(!Params->TryGetBoolField(TEXT("fire"),Fire)||!Params->TryGetBoolField(TEXT("shield"),Shield)||
                !ReadPair(Params,TEXT("move"),Move)||!ReadPair(Params,TEXT("look_degrees_per_second"),Look)||
                !Params->TryGetNumberField(TEXT("duration_seconds"),Duration)||
                !Requested.Set(Fire,Shield,Move,Look,Duration,FPlatformTime::Seconds())) { Ok=false; Code=TEXT("INVALID_INPUT"); }
            else {
                ClearAutomationInput(); AutomationInput=Requested; AutomationInputPawn=GetPawn<AMRFPSPawn>();
                Code=TEXT("INPUT_LEASE_SET");
            }
        } else if(Operation==TEXT("player.reload")||Operation==TEXT("player.wall")) {
            if(!DesktopAutomationAllowed()) { Ok=false; Code=TEXT("DESKTOP_PLAYER_UNAVAILABLE"); }
            else {
                if(Operation==TEXT("player.reload")) { AutomationReloadUntil=FPlatformTime::Seconds()+2.5; TickReload(); }
                else WallPressed();
                Submitted=true; Code=TEXT("INPUT_SUBMITTED");
            }
        } else if(Operation==TEXT("player.calibrate")) {
            if(IsInspector()||!GetPawn<AMRFPSPawn>()) { Ok=false; Code=TEXT("PLAYER_REQUIRED"); }
            else { Calibrate(); Submitted=true; Code=TEXT("CALIBRATION_SUBMITTED"); }
        } else if(Operation==TEXT("player.round_ready")) {
            auto* Fighter=GetPawn<AMRFPSPawn>(); const auto* ReadyState=GetWorld()->GetGameState<AMRFPSGameState>();
            if(IsInspector()||!Fighter||Fighter->GetSlot()<0||Fighter->GetSlot()>1) { Ok=false; Code=TEXT("PLAYER_REQUIRED"); }
            else if(!ReadyState || !ReadyState->IsAwaitingRoundReady()) { Ok=false; Code=TEXT("ROUND_READY_NOT_WAITING"); }
            else if(!Fighter->bCalibrated) { Ok=false; Code=TEXT("CALIBRATION_REQUIRED"); }
            else if(ReadyState->RoundReadyMask&(1u<<Fighter->GetSlot())) { Already=true; Code=TEXT("ROUND_READY_ALREADY_SET"); }
            else { Fighter->ServerRoundReady(ReadyState->RoundReadyWindow); Submitted=true; Code=TEXT("ROUND_READY_SUBMITTED"); }
        } else if(Operation==TEXT("haptics.set")) {
            FString State; auto* Fighter=GetPawn<AMRFPSPawn>(); auto* Haptics=Fighter?Fighter->Haptics.Get():nullptr;
            if(IsInspector()||!Haptics||!Params->TryGetStringField(TEXT("state"),State)) { Ok=false; Code=TEXT("HAPTICS_UNAVAILABLE"); }
            else if (Fighter->QuestHaptics) {
                if (State==TEXT("stopped")) { Fighter->QuestHaptics->StopAll(); Submitted=true; Code=TEXT("HAPTICS_STOP_SUBMITTED"); }
                else if (State==TEXT("disconnected")) {
                    if (!Fighter->QuestHaptics->GetStatus().bHasSession) Already=true;
                    else if (Fighter->QuestHaptics->CloseSession()) { Submitted=true; Code=TEXT("HAPTICS_DISCONNECT_SUBMITTED"); }
                    else { Ok=false; Code=TEXT("HAPTICS_DISCONNECT_FAILED"); }
                } else { Ok=false; Code=TEXT("USE_MOBITS_SETUP"); }
            }
            else if(State==TEXT("connected")) {
                if(Haptics->GetConnectionState()==EMoBitsConnectionState::Connected) Already=true;
                else if(Haptics->GetConnectionState()==EMoBitsConnectionState::Connecting) { Submitted=true; Code=TEXT("HAPTICS_CONNECTING"); }
                else { FString Error; if(Haptics->Connect(Error)) { Submitted=true; Code=TEXT("HAPTICS_CONNECTING"); } else { Ok=false; Code=TEXT("HAPTICS_CONNECT_FAILED"); } }
            } else if(State==TEXT("armed")) {
                if(Haptics->IsHapticsArmed()) Already=true;
                else if(Haptics->GetConnectionState()!=EMoBitsConnectionState::Connected) { Ok=false; Code=TEXT("HAPTICS_NOT_CONNECTED"); }
                else if(Haptics->ArmHaptics()>0) { Submitted=true; Code=TEXT("HAPTICS_ARM_SUBMITTED"); }
                else { Ok=false; Code=TEXT("HAPTICS_ARM_REJECTED"); }
            } else if(State==TEXT("stopped")) {
                // Always issue STOP_ALL, even if the local armed flag is false:
                // an earlier arm request could still be in flight.
                Haptics->StopHaptics(); Submitted=true; Code=TEXT("HAPTICS_STOP_SUBMITTED");
            } else if(State==TEXT("disconnected")) {
                if(Haptics->GetConnectionState()==EMoBitsConnectionState::Disconnected) Already=true;
                else { Haptics->Disconnect(); Submitted=true; Code=TEXT("HAPTICS_DISCONNECT_SUBMITTED"); }
            } else { Ok=false; Code=TEXT("INVALID_STATE"); }
        } else if(Operation==TEXT("capture.set")) {
            bool Enabled=false; auto* Capture=GetGameInstance()->GetSubsystem<UMRFPSViewCaptureSubsystem>();
            if(!Params->TryGetBoolField(TEXT("enabled"),Enabled)||!Capture||!IsVideoSessionValid(true)) { Ok=false; Code=TEXT("CAPTURE_UNAVAILABLE"); }
            else {
                const auto State=CaptureControlState(Capture,Now,Enabled);
                switch (MRFPSVideoControl::Decide(Enabled,State)) {
                case MRFPSVideoControl::EAction::AlreadyStopped:
                case MRFPSVideoControl::EAction::AlreadyLive: Already=true; break;
                case MRFPSVideoControl::EAction::Stop:
                    if (StopVideoSharing(Capture,Now)) { Submitted=true; Code=TEXT("CAPTURE_STOP_SUBMITTED"); }
                    else Already=true;
                    break;
                case MRFPSVideoControl::EAction::AwaitConsent: Submitted=true; Code=TEXT("CAPTURE_CONSENT_PENDING"); break;
                case MRFPSVideoControl::EAction::AwaitFrame: Submitted=true; Code=TEXT("CAPTURE_WAITING_FRAME"); break;
                case MRFPSVideoControl::EAction::Start:
                    MRFPSShareView();
                    if (VideoConsentFlow.IsBusy() || Capture->HasCaptureSession()) { Submitted=true; Code=TEXT("CAPTURE_SUBMITTED"); }
                    else { Ok=false; Code=TEXT("CAPTURE_START_FAILED"); }
                    break;
                }
            }
        } else if(Operation==TEXT("admin.action")||Operation==TEXT("object.ensure")||Operation==TEXT("object.remove")||Operation==TEXT("wall.set")) {
            if(!IsAdmin()||!GS) { Ok=false; Code=TEXT("ADMIN_REQUIRED"); }
            else {
                AMRFPSBarrier* Target=nullptr;
                uint32 ExpectedLayout=0,ExpectedControl=0;
                if(!ReadRevision(Params,TEXT("layout_revision"),ExpectedLayout)||!ReadRevision(Params,TEXT("control_revision"),ExpectedControl)) { Ok=false; Code=TEXT("REVISION_REQUIRED"); }
                if(Operation==TEXT("object.remove")||Operation==TEXT("wall.set")) {
                    FString TargetText; FGuid TargetId;
                    if(!Params->TryGetStringField(TEXT("target"),TargetText)||!ParseId(TargetText,TargetId)) { Ok=false; Code=TEXT("INVALID_TARGET"); }
                    else if(const TWeakObjectPtr<AMRFPSBarrier>* Found=AutomationTargets.Find(TargetId)) Target=Found->Get();
                    if(Ok&&!IsValid(Target)) { Ok=false; Code=TEXT("TARGET_UNAVAILABLE"); }
                }
                if(Ok) {
                    AutomationPending.Add(Id,Folder);
                    ServerAutomationCommand(Id,Operation,JsonText(Params),Target,ExpectedLayout,ExpectedControl);
                    return;
                }
            }
        } else { Ok=false; Code=TEXT("UNSUPPORTED_OPERATION"); }
        WriteAutomationResult(Folder,Id,Ok?(Submitted?TEXT("submitted"):Already?TEXT("already_applied"):TEXT("ok")):TEXT("error"),Code,Data);
        return;
    }
#endif
}

void AMRFPSPlayerController::ClientAutomationResult_Implementation(FGuid Id,const FString& Code)
{
#if !UE_SERVER
    if(FString* Folder=AutomationPending.Find(Id)) {
        const bool Ok=Code==TEXT("AUTOMATION_OK")||Code==TEXT("AUTOMATION_ALREADY");
        if(WriteAutomationResult(*Folder,Id,Ok?(Code==TEXT("AUTOMATION_ALREADY")?TEXT("already_applied"):TEXT("ok")):TEXT("error"),Code)) AutomationPending.Remove(Id);
    }
#endif
}

void AMRFPSPlayerController::ServerAutomationCommand_Implementation(FGuid Id,const FString& Operation,const FString& Parameters,AMRFPSBarrier* Target,uint32 LayoutRevision,uint32 ControlRevision)
{
    auto* GS=GetWorld()->GetGameState<AMRFPSGameState>(); auto* GM=GetWorld()->GetAuthGameMode<AMRFPSGameMode>();
    if(!IsAdmin()||!GS||!GM||!Id.IsValid()||Operation.Len()>32||Parameters.Len()>4096) { ClientAutomationResult(Id,TEXT("ADMIN_REQUIRED")); return; }
    const FString Identity=Operation+TEXT("\n")+Parameters;
    if(const auto* Previous=GM->AutomationReceipts.Find(Id)) {
        ClientAutomationResult(Id,Previous->Key==Identity?Previous->Value:TEXT("AUTOMATION_ID_CONFLICT")); return;
    }
    if(GM->AutomationReceipts.Num()>=8192) { ClientAutomationResult(Id,TEXT("AUTOMATION_CAPACITY")); return; }
    FString Code=TEXT("ADMIN_STALE_STATE"); bool Accepted=false;
    const auto Params=ParseObject(Parameters);
    if(!Params.IsValid()) Code=TEXT("INVALID_REQUEST");
    else if(LayoutRevision!=GS->Layout.Revision||ControlRevision!=GS->ControlRevision) Code=TEXT("ADMIN_STALE_STATE");
    else {
        const double Now=GetWorld()->GetTimeSeconds();
        const bool Build=Operation!=TEXT("admin.action");
        double& Last=Build?LastAdminWallAt:LastAdminActionAt;
        if(Now-Last<.2) Code=TEXT("ADMIN_RATE_LIMIT");
        else {
            Last=Now;
            if(Operation==TEXT("admin.action")) {
                FString Action; FVector2D Dimensions=FVector2D::ZeroVector; EMRFPSAdminAction Type=EMRFPSAdminAction::Start; bool Valid=Params->TryGetStringField(TEXT("action"),Action);
                if(Action==TEXT("start")) Type=EMRFPSAdminAction::Start;
                else if(Action==TEXT("pause")) Type=EMRFPSAdminAction::Pause;
                else if(Action==TEXT("resume")) Type=EMRFPSAdminAction::Resume;
                else if(Action==TEXT("reset")) Type=EMRFPSAdminAction::Reset;
                else if(Action==TEXT("confirm_floor")) Type=EMRFPSAdminAction::ConfirmSharedFloor;
                else if(Action==TEXT("calibrate_floor")) Type=EMRFPSAdminAction::CalibrateFloor;
                else if(Action==TEXT("clear_objects")) Type=EMRFPSAdminAction::ClearAdminWalls;
                else if(Action==TEXT("reset_buildings")) Type=EMRFPSAdminAction::ResetBuildings;
                else if(Action==TEXT("set_dimensions")) { Type=EMRFPSAdminAction::SetDimensions; double W=0,L=0; Valid=Valid&&Params->TryGetNumberField(TEXT("width_cm"),W)&&Params->TryGetNumberField(TEXT("length_cm"),L)&&FMath::IsFinite(W)&&FMath::IsFinite(L); Dimensions=FVector2D(W,L); }
                else Valid=false;
                if(!Valid) Code=TEXT("INVALID_REQUEST");
                else if((Action==TEXT("pause")&&(GS->Phase==EMRFPSMatchPhase::Playing||GS->Phase==EMRFPSMatchPhase::BetweenRounds)&&GS->bCombatPaused)||
                        (Action==TEXT("resume")&&(GS->Phase==EMRFPSMatchPhase::Playing||GS->Phase==EMRFPSMatchPhase::BetweenRounds)&&!GS->bCombatPaused)||
                        (Action==TEXT("start")&&(GS->Phase==EMRFPSMatchPhase::Playing||GS->Phase==EMRFPSMatchPhase::BetweenRounds)&&!GS->bCombatPaused)||
                        (Action==TEXT("set_dimensions")&&FMath::IsNearlyEqual(GS->Layout.WidthCm,Dimensions.X)&&FMath::IsNearlyEqual(GS->Layout.LengthCm,Dimensions.Y))) {
                    Accepted=true; Code=TEXT("AUTOMATION_ALREADY");
                } else Accepted=GM->HandleAdminAction(this,Type,Dimensions,Code);
            } else if(Operation==TEXT("object.ensure")) {
                FString Kind; FVector Surface,Origin,Direction; double Yaw=0;
                const bool Valid=Params->TryGetStringField(TEXT("kind"),Kind)&&(Kind==TEXT("wall")||Kind==TEXT("crate"))&&
                    ReadVector(Params,TEXT("surface"),Surface)&&ReadVector(Params,TEXT("origin"),Origin)&&ReadVector(Params,TEXT("direction"),Direction)&&Params->TryGetNumberField(TEXT("yaw"),Yaw)&&FMath::IsFinite(Yaw);
                if(!Valid) Code=TEXT("INVALID_REQUEST"); else Accepted=GM->EnsureAutomationObject(this,Kind==TEXT("wall")?EMRFPSBuildKind::Wall:EMRFPSBuildKind::Crate,Surface,Origin,Direction,float(Yaw),Code);
            } else if(Operation==TEXT("wall.set")) {
                FMRFPSWallShape Expected,Desired;
                if(!ReadShape(Params,TEXT("expected"),Expected)||!ReadShape(Params,TEXT("desired"),Desired)) Code=TEXT("INVALID_REQUEST");
                else Accepted=GM->SetAutomationWall(this,Target,Expected,Desired,Code);
            } else if(Operation==TEXT("object.remove")) {
                FMRFPSWallShape Expected;
                if(!ReadShape(Params,TEXT("expected"),Expected)) Code=TEXT("INVALID_REQUEST"); else Accepted=GM->RemoveAutomationObject(this,Target,Expected,Code);
            } else Code=TEXT("UNSUPPORTED_OPERATION");
        }
    }
    if(Accepted&&Code!=TEXT("AUTOMATION_ALREADY")) Code=TEXT("AUTOMATION_OK");
    GM->AutomationReceipts.Add(Id,TPair<FString,FString>(Identity,Code));
    ClientAutomationResult(Id,Code);
}
