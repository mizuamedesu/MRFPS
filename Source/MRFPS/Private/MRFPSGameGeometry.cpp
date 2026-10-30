#include "MRFPSGame.h"
#include "MRFPSGeometryWorld.h"
#include "Engine/NetConnection.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#if !UE_SERVER
#include "GeometryCaptureSubsystem.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogMRFPSGeometry,Log,All);
namespace { uint32 NextServerGeometryStream=0; }

bool AMRFPSPlayerController::IsGeometrySessionValid(bool Publisher) const
{
    const auto* PS=GetPlayerState<AMRFPSPlayerState>(); const auto* Connection=GetNetConnection();
    if (GetNetMode()==NM_Standalone || !PS || !PS->bAuthenticated ||
        PS->AuthExpiresAt<=FDateTime::UtcNow().ToUnixTimestamp() || !Connection || !Connection->IsEncryptionEnabled()) return false;
    if (!Publisher) return PS->bInspector || (PS->Slot>=0 && PS->Slot<2);
    const auto* GeometryPawn=GetPawn<AMRFPSPawn>();
    const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
    const bool Shared=GS && GS->Layout.bCalibrated && GS->bSharedFloorConfirmed && GS->SharedFloorRevision &&
        GS->Layout.RegistrationRevision==GS->SharedFloorRevision;
    const bool Solo=GS && GeometryPawn && GS->bSoloBotSession && GS->SoloHumanSlot==PS->Slot &&
        GS->SoloHumanCalibration && GeometryPawn->GeometryCalibrationGeneration==GS->SoloHumanCalibration;
    return !PS->bInspector && PS->Slot>=0 && PS->Slot<2 && GeometryPawn && GeometryPawn->GetPlayerState<AMRFPSPlayerState>()==PS &&
        FindPlayer(PS->Slot)==GeometryPawn && (Shared || Solo) && GeometryPawn->bStageFloorReferenced &&
        GeometryPawn->bCalibrated && GeometryPawn->GeometryCalibrationGeneration!=0 &&
        !GeometryPawn->Pose.bDesktop && GeometryPawn->Pose.IsSane() && GeometryPawn->IsInsideArena() &&
        (!HasAuthority() || GeometryPawn->IsPoseFresh());
}
void AMRFPSPlayerController::StopGeometryPublisher()
{
    GeometryPublisherQueue.Reset();
    if (bHadGeometryFrames && IsGeometrySessionValid(false)) ServerStopGeometry(GeometryLocalCalibration,GeometrySequence,GeometryLocalLayoutRevision);
    bHadGeometryFrames=false;
#if !UE_SERVER
    if (bGeometryCaptureRequested) if (auto* Capture=GetGameInstance()->GetSubsystem<UMRFPSGeometryCaptureSubsystem>()) Capture->StopCapture();
#endif
    bGeometryCaptureRequested=false; GeometryLocalCalibration=0; GeometryTrackingEpoch=0; GeometryNativeSequence=0; GeometryLocalLayoutRevision=0;
}
void AMRFPSPlayerController::ResetGeometryForLayout()
{
    GeometryUpload.Reset(); for (auto& Assembly:GeometryDownloads) Assembly.Reset();
    for (auto& Queue:GeometryRelayQueues) Queue.Reset();
    if (IsLocalController()) StopGeometryPublisher();
    if (HasAuthority()) if (auto* PS=GetPlayerState<AMRFPSPlayerState>()) { PS->GeometryStreamId=0; PS->ForceNetUpdate(); }
}
void AMRFPSPlayerController::TickMeasuredGeometry()
{
    if (!IsLocalController()) return;
#if !UE_SERVER
    auto* GeometryPawn=GetPawn<AMRFPSPawn>(); FTransform TrackingToArena; double Since=0;
    auto* Capture=GetGameInstance()->GetSubsystem<UMRFPSGeometryCaptureSubsystem>();
    const auto* LayoutState=GetWorld()->GetGameState<AMRFPSGameState>();
    if (LayoutState && GeometryLocalLayoutRevision!=LayoutState->Layout.Revision) {
        StopGeometryPublisher(); GeometryLocalLayoutRevision=LayoutState->Layout.Revision;
    }
    if (Capture && GeometryPawn && GeometryPawn->GetGeometryCalibrationEpoch() &&
        Capture->GetTrackingSpaceEpoch()!=GeometryPawn->GetGeometryCalibrationEpoch()) {
        UE_LOG(LogMRFPSGeometry,Warning,TEXT("MRFPS_CALIBRATION_INVALIDATED reason=geometry_epoch_changed"));
        StopGeometryPublisher(); GeometryPawn->InvalidateGeometryCalibration(); return;
    }
    if (!Capture || !IsGeometrySessionValid(true) || !GeometryPawn) {
        if (bGeometryCaptureRequested || bHadGeometryFrames) StopGeometryPublisher();
        return;
    }
    if (!GeometryPawn->GetGeometryCalibration(TrackingToArena,Since)) {
        if (bGeometryCaptureRequested || bHadGeometryFrames) StopGeometryPublisher();
        return;
    }
    if (GeometryLocalCalibration!=GeometryPawn->GeometryCalibrationGeneration) {
        StopGeometryPublisher(); GeometryLocalCalibration=GeometryPawn->GeometryCalibrationGeneration;
        GeometryLocalLayoutRevision=LayoutState?LayoutState->Layout.Revision:0;
    }
    if (!GeometryTrackingEpoch) GeometryTrackingEpoch=GeometryPawn->GetGeometryCalibrationEpoch();
    if (!bGeometryCaptureRequested) { Capture->RequestCapture(); bGeometryCaptureRequested=true; }
    const double Now=FPlatformTime::Seconds();
    FMRFPSEnvironmentGeometryFrame Frame;
    if (Capture->DequeueLatestFrame(Frame)) {
        if (GeometryTrackingEpoch && Frame.TrackingSpaceEpoch!=GeometryTrackingEpoch) {
            StopGeometryPublisher(); GeometryPawn->InvalidateGeometryCalibration(); return;
        }
        // The render thread may have produced this frame after the tick began.
        // Sample time after dequeue so a fresh concurrent frame is not future-dated.
        const double Age=FPlatformTime::Seconds()-Frame.CaptureMonotonicTime;
        if (!Frame.bTrackingValid || !Frame.TrackingSpaceEpoch) {
            StopGeometryPublisher(); GeometryPawn->InvalidateGeometryCalibration(); return;
        }
        if (!FMath::IsFinite(Age) || Age<0 || Age>MRFPSGeometryWire::MaxCaptureAge || Frame.CaptureMonotonicTime<Since ||
            !Frame.Sequence || Frame.Sequence<=GeometryNativeSequence) return;
        GeometryTrackingEpoch=Frame.TrackingSpaceEpoch; GeometryNativeSequence=Frame.Sequence;
        if (Frame.Vertices.Num()>MRFPSGeometryWire::MaxVertices || Frame.Indices.Num()>MRFPSGeometryWire::MaxTriangles*3 || Frame.Indices.Num()%3) {
            StopGeometryPublisher(); return;
        }
        FMRFPSGeometryMesh Mesh; TArray<int32> Remap; Remap.Init(INDEX_NONE,Frame.Vertices.Num());
        // Drop triangles crossing unknown/out-of-arena samples; do not clamp
        // them into invented boundary surfaces. Quantize before degeneracy tests.
        for (int32 I=0;I<Frame.Indices.Num();I+=3) {
            FVector Points[3]; bool Valid=true;
            for (int32 J=0;J<3;++J) {
                const uint32 Index=Frame.Indices[I+J];
                if (Index>=uint32(Frame.Vertices.Num())) { Valid=false; break; }
                const FVector P=TrackingToArena.TransformPosition(FVector(Frame.Vertices[Index]));
                const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
                if (!GS || !GS->Layout.ContainsGeometry(P)) { Valid=false; break; }
                Points[J]=FVector(FMath::RoundToInt(P.X),FMath::RoundToInt(P.Y),FMath::RoundToInt(P.Z));
                if (!GS->Layout.ContainsGeometry(Points[J])) { Valid=false; break; }
            }
            if (!Valid || FVector::CrossProduct(Points[1]-Points[0],Points[2]-Points[0]).SizeSquared()<1.e-4 ||
                FVector::DistSquared(Points[0],Points[1])>FMath::Square(MRFPSGeometryWire::MaxTriangleEdge) ||
                FVector::DistSquared(Points[0],Points[2])>FMath::Square(MRFPSGeometryWire::MaxTriangleEdge) ||
                FVector::DistSquared(Points[1],Points[2])>FMath::Square(MRFPSGeometryWire::MaxTriangleEdge)) continue;
            for (int32 J=0;J<3;++J) {
                const uint32 Index=Frame.Indices[I+J];
                if (Remap[Index]==INDEX_NONE) { Remap[Index]=Mesh.Vertices.Num(); Mesh.Vertices.Add(Points[J]); }
                Mesh.Indices.Add(Remap[Index]);
            }
        }
        if (Mesh.Indices.IsEmpty()) {
            if (bHadGeometryFrames) ServerStopGeometry(GeometryLocalCalibration,GeometrySequence,GeometryLocalLayoutRevision);
            bHadGeometryFrames=false; GeometryPublisherQueue.Reset();
        } else if (GeometrySequence<MAX_uint32) {
            FMRFPSGeometryChunk Header; Header.Slot=uint8(GeometryPawn->GetSlot()); Header.StreamId=GeometryLocalCalibration;
            Header.Calibration=GeometryLocalCalibration; Header.Sequence=++GeometrySequence;
            const auto* GS=GetWorld()->GetGameState();
            if (!LayoutState) return;
            Header.LayoutRevision=LayoutState->Layout.Revision;
            const double CaptureServerSeconds=GS?GS->GetServerWorldTimeSeconds()-Age:-1;
            if (CaptureServerSeconds<0 || !FMath::IsFinite(CaptureServerSeconds)) return;
            Header.CaptureMs=uint32(uint64(CaptureServerSeconds*1000)); TArray<uint8> Bytes;
            if (Mesh.Encode(Header,Bytes)) { GeometryPublisherQueue.Push(Header,Bytes,Now); bHadGeometryFrames=true; }
        }
    }
    if (!Capture->IsCapturing()) {
        if (bHadGeometryFrames) { ServerStopGeometry(GeometryLocalCalibration,GeometrySequence,GeometryLocalLayoutRevision); bHadGeometryFrames=false; }
        GeometryPublisherQueue.Reset(); return;
    }
    for (int32 I=0;I<6 && GetNetConnection()->IsNetReady();++I) {
        FMRFPSGeometryChunk Chunk; if (!GeometryPublisherQueue.Next(Chunk,Now)) break; ServerPublishGeometry(Chunk);
    }
#endif
}
void AMRFPSPlayerController::ServerPublishGeometry_Implementation(const FMRFPSGeometryChunk& Chunk)
{
    if (!IsGeometrySessionValid(true) || !Chunk.IsSane()) return;
    auto* GeometryPawn=GetPawn<AMRFPSPawn>(); auto* PS=GetPlayerState<AMRFPSPlayerState>();
    const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
    if (!GS || Chunk.LayoutRevision!=GS->Layout.Revision) return;
    if (Chunk.Slot!=PS->Slot || Chunk.Calibration!=GeometryPawn->GeometryCalibrationGeneration || Chunk.StreamId!=Chunk.Calibration ||
        Chunk.Sequence<=GeometryClosedThrough || Chunk.Sequence<=GeometryLastAcceptedSequence ||
        !MRFPSGeometryWire::CaptureFresh(Chunk.CaptureMs,GetWorld()->GetTimeSeconds())) return;
    const double Now=FPlatformTime::Seconds(),Elapsed=FMath::Clamp(Now-GeometryRateAt,0.0,1.0); GeometryRateAt=Now;
    GeometryRateBytes=FMath::Max(0.0,GeometryRateBytes-Elapsed*140000);
    if (Chunk.Payload.Num()>60000-GeometryRateBytes) return;
    GeometryRateBytes+=Chunk.Payload.Num();
    if (!GeometryUpload.Add(Chunk,Now)) return;
    FMRFPSGeometryChunk Header; TArray<uint8> Bytes; if (!GeometryUpload.Take(Header,Bytes)) return;
    if (Now-LastGeometryAcceptedAt<.24 || !MRFPSGeometryWire::CaptureFresh(Header.CaptureMs,GetWorld()->GetTimeSeconds())) return;
    FMRFPSGeometryMesh Mesh; if (!FMRFPSGeometryMesh::Decode(Header,Bytes,Mesh)) return;
    for (const FVector& Point:Mesh.Vertices) if (!GS->Layout.ContainsGeometry(Point)) return;
    for (const FVector& Point:Mesh.Vertices) if (FVector::DistSquared(Point,GeometryPawn->Pose.Head)>650.*650.) return;
    const bool NewStream=PS->GeometryStreamId==0;
    if (NewStream) { if (NextServerGeometryStream==MAX_uint32) return; PS->GeometryStreamId=++NextServerGeometryStream; }
    Header.StreamId=PS->GeometryStreamId;
    auto* WorldGeometry=GetWorld()->GetSubsystem<UMRFPSGeometryWorld>();
    if (!WorldGeometry || !WorldGeometry->Commit(GeometryPawn,Header,MoveTemp(Mesh))) { if (NewStream) PS->GeometryStreamId=0; return; }
    if (NewStream) PS->ForceNetUpdate();
    LastGeometryAcceptedAt=Now; GeometryLastAcceptedSequence=Header.Sequence;
    int32 Recipients=0;
    for (FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It) {
        auto* Peer=Cast<AMRFPSPlayerController>(It->Get());
        if (Peer && Peer->IsGeometrySessionValid(false)) { Peer->GeometryRelayQueues[Header.Slot].Push(Header,Bytes,Now); ++Recipients; }
    }
    if (Now-LastGeometryLogAt>=1) {
        LastGeometryLogAt=Now;
        UE_LOG(LogMRFPSGeometry,Display,TEXT("MRFPS_GEOMETRY_COMMIT slot=%d stream=%u sequence=%u vertices=%d triangles=%d recipients=%d"),
            Header.Slot,Header.StreamId,Header.Sequence,Header.VertexCount,Header.TriangleCount,Recipients);
    }
}
void AMRFPSPlayerController::ServerStopGeometry_Implementation(uint32 Calibration,uint32 FinalSequence,uint32 LayoutRevision)
{
    auto* GeometryPawn=GetPawn<AMRFPSPawn>(); const auto* PS=GetPlayerState<AMRFPSPlayerState>();
    const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
    if (!GS || LayoutRevision!=GS->Layout.Revision) return;
    if (!IsGeometrySessionValid(false) || !PS || PS->bInspector || !GeometryPawn || GeometryPawn->GetPlayerState<AMRFPSPlayerState>()!=PS ||
        Calibration!=GeometryPawn->GeometryCalibrationGeneration) return;
    if (FinalSequence<GeometryLastAcceptedSequence) return; // A delayed stop cannot retire a newer accepted capture.
    GeometryClosedThrough=FMath::Max(GeometryClosedThrough,FMath::Max(FinalSequence,GeometryLastAcceptedSequence)); GeometryUpload.Reset();
    if (auto* Geometry=GetWorld()->GetSubsystem<UMRFPSGeometryWorld>()) Geometry->RemoveOwner(GeometryPawn);
}
void AMRFPSPlayerController::ServiceGeometryRelay()
{
    if (!HasAuthority()) return;
    if (!IsGeometrySessionValid(true)) GeometryUpload.Reset();
    if (!IsGeometrySessionValid(false)) { for (auto& Queue:GeometryRelayQueues) Queue.Reset(); return; }
    const double Now=FPlatformTime::Seconds();
    for (int32 I=0;I<6 && GetNetConnection()->IsNetReady();++I) {
        FMRFPSGeometryChunk Chunk; bool Found=false;
        for (int32 Attempt=0;Attempt<2;++Attempt) {
            const int32 Slot=NextGeometryRelaySlot; NextGeometryRelaySlot=(Slot+1)%2;
            const auto* Source=FindPlayer(Slot); const auto* PS=Source?Source->GetPlayerState<AMRFPSPlayerState>():nullptr;
            if (!PS || !PS->bAuthenticated || PS->bInspector || PS->AuthExpiresAt<=FDateTime::UtcNow().ToUnixTimestamp() ||
                !PS->GeometryStreamId || !Source->bCalibrated || !Source->IsInsideArena() || !Source->IsPoseFresh()) {
                GeometryRelayQueues[Slot].Reset(); continue;
            }
            if (GeometryRelayQueues[Slot].Next(Chunk,Now)) {
                if (Chunk.StreamId!=PS->GeometryStreamId || Chunk.Calibration!=Source->GeometryCalibrationGeneration) { GeometryRelayQueues[Slot].Reset(); continue; }
                Found=true; break;
            }
        }
        if (!Found) break; ClientMeasuredGeometry(Chunk);
    }
}
void AMRFPSPlayerController::ClientMeasuredGeometry_Implementation(const FMRFPSGeometryChunk& Chunk)
{
    if (!IsLocalController() || !IsGeometrySessionValid(false) || !Chunk.IsSane()) return;
    const auto* GS=GetWorld()->GetGameState<AMRFPSGameState>();
    if (!GS || Chunk.LayoutRevision!=GS->Layout.Revision) return;
    auto* Source=FindPlayer(Chunk.Slot); const auto* PS=Source?Source->GetPlayerState<AMRFPSPlayerState>():nullptr;
    if (!PS || !PS->bAuthenticated || PS->bInspector || PS->AuthExpiresAt<=FDateTime::UtcNow().ToUnixTimestamp() ||
        PS->Slot!=Chunk.Slot || PS->GeometryStreamId!=Chunk.StreamId || !Source->bCalibrated ||
        Source->GeometryCalibrationGeneration!=Chunk.Calibration) return;
    if (!GeometryDownloads[Chunk.Slot].Add(Chunk,FPlatformTime::Seconds())) return;
    FMRFPSGeometryChunk Header; TArray<uint8> Bytes; if (!GeometryDownloads[Chunk.Slot].Take(Header,Bytes)) return;
    FMRFPSGeometryMesh Mesh; if (!FMRFPSGeometryMesh::Decode(Header,Bytes,Mesh)) return;
    if (auto* Geometry=GetWorld()->GetSubsystem<UMRFPSGeometryWorld>()) Geometry->Commit(Source,Header,MoveTemp(Mesh));
}
