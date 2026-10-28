#include "MoBitsSkeletonComponent.h"
#include "MoBitsConnectionComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SceneComponent.h"

UMoBitsSkeletonComponent::UMoBitsSkeletonComponent()
{
    PrimaryComponentTick.bCanEverTick = true; PrimaryComponentTick.bTickEvenWhenPaused = true;
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork; bAutoActivate = true;
    const int32 Ids[] = {6,5,4,3,2,1,16,20,14,18,13,17,15,19,7,10,8,11,9,12};
    const TCHAR* Names[] = {TEXT("pelvis"),TEXT("spine_01"),TEXT("spine_02"),TEXT("spine_03"),TEXT("neck_01"),TEXT("head"),
        TEXT("clavicle_l"),TEXT("clavicle_r"),TEXT("upperarm_l"),TEXT("upperarm_r"),TEXT("lowerarm_l"),TEXT("lowerarm_r"),
        TEXT("hand_l"),TEXT("hand_r"),TEXT("thigh_l"),TEXT("thigh_r"),TEXT("calf_l"),TEXT("calf_r"),TEXT("foot_l"),TEXT("foot_r")};
    for (int32 I = 0; I < 20; ++I) { FMoBitsBoneBinding B; B.MoBitsBoneId = Ids[I]; B.RigBone = FName(Names[I]); Bindings.Add(B); }
}
void UMoBitsSkeletonComponent::Recalibrate() { bHasReference = false; }
void UMoBitsSkeletonComponent::Deactivate() { bIsTracked = false; bHasReference = false; Super::Deactivate(); }
void UMoBitsSkeletonComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
    Super::TickComponent(DeltaTime, TickType, TickFunction);
    if (LastConnection.Get() != Connection || LastMesh.Get() != TargetMesh)
    {
        if (LastConnection.IsValid()) RemoveTickPrerequisiteComponent(LastConnection.Get());
        if (Connection) AddTickPrerequisiteComponent(Connection);
        LastConnection = Connection; LastMesh = TargetMesh; Recalibrate();
    }
    FMoBitsMotionFrame Frame;
    bIsTracked = IsActive() && Connection && TargetMesh && Connection->GetLatestFrame(Frame) && Frame.bTrackingActive && !Frame.Bones.IsEmpty();
    if (!bIsTracked) { bHasReference = false; return; }
    const FTransform Origin = MotionOrigin ? MotionOrigin->GetComponentTransform() : FTransform::Identity;
    TMap<int32, FQuat> GlobalRotations;
    TMap<int32, FVector> GlobalPositions;
    for (const auto& B : Frame.Bones)
    {
        const FQuat ParentRotation = B.ParentBoneId ? GlobalRotations.FindChecked(B.ParentBoneId) : FQuat::Identity;
        const FVector ParentPosition = B.ParentBoneId ? GlobalPositions.FindChecked(B.ParentBoneId) : FVector::ZeroVector;
        GlobalRotations.Add(B.BoneId, (ParentRotation * B.LocalRotation).GetNormalized());
        GlobalPositions.Add(B.BoneId, ParentPosition + ParentRotation.RotateVector(B.LocalPositionCm));
    }
    if (!bHasReference || OriginRevision != Frame.OriginRevision)
    {
        TrackingToMesh = TargetMesh->GetComponentQuat().Inverse() * Origin.GetRotation();
        SourceReference = GlobalRotations; RigReference.Reset();
        for (const auto& B : Bindings)
            if (TargetMesh->GetBoneIndex(B.RigBone) != INDEX_NONE && GlobalRotations.Contains(B.MoBitsBoneId))
                RigReference.Add(B.MoBitsBoneId, TargetMesh->GetBoneRotationByName(B.RigBone, EBoneSpaces::ComponentSpace).Quaternion());
        const auto* Hips = GlobalPositions.Find(6);
        SourceRootReference = Hips ? Origin.TransformPosition(*Hips) : FVector::ZeroVector;
        MeshRootReference = TargetMesh->GetComponentLocation();
        bHasReference = true; OriginRevision = Frame.OriginRevision;
    }
    // Apply in source hierarchy order, even if the user reordered Bindings.
    for (const auto& Source : Frame.Bones)
    {
        const FQuat* Base = RigReference.Find(Source.BoneId);
        const FQuat* Ref = SourceReference.Find(Source.BoneId);
        if (!Base || !Ref) continue;
        const FQuat Delta = TrackingToMesh * GlobalRotations.FindChecked(Source.BoneId) * Ref->Inverse() * TrackingToMesh.Inverse();
        for (const auto& B : Bindings)
            if (B.MoBitsBoneId == Source.BoneId)
                TargetMesh->SetBoneRotationByName(B.RigBone, (Delta * *Base).Rotator(), EBoneSpaces::ComponentSpace);
    }
    if (bApplyEstimatedRootPosition)
    {
        const auto* Hips = GlobalPositions.Find(6);
        if (Hips) TargetMesh->SetWorldLocation(MeshRootReference + Origin.TransformPosition(*Hips) - SourceRootReference);
    }
}
