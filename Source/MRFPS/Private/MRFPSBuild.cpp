#include "MRFPSBuild.h"

bool MRFPSBuild::Valid(EMRFPSBuildKind Kind) { return uint8(Kind)<=uint8(EMRFPSBuildKind::Crate); }
FVector MRFPSBuild::Extent(EMRFPSBuildKind Kind)
{
    switch(Kind) {
    case EMRFPSBuildKind::Wall:return FVector(8,80,100);
    case EMRFPSBuildKind::Floor:return FVector(50,50,4);
    case EMRFPSBuildKind::Stairs:return FVector(50,75,50);
    case EMRFPSBuildKind::Crate:return FVector(50,50,50);
    default:return FVector::ZeroVector;
    }
}
FString MRFPSBuild::Label(EMRFPSBuildKind Kind)
{
    switch(Kind) {
    case EMRFPSBuildKind::Wall:return TEXT("壁");
    case EMRFPSBuildKind::Floor:return TEXT("床");
    case EMRFPSBuildKind::Stairs:return TEXT("階段");
    case EMRFPSBuildKind::Crate:return TEXT("箱");
    default:return TEXT("");
    }
}
bool MRFPSBuild::Snap(const FMRFPSArenaLayout& Layout,EMRFPSBuildKind Kind,const FVector& Surface,float Yaw,FTransform& Result)
{
    if(!Valid(Kind)||!Layout.IsSane()||Surface.ContainsNaN()||!FMath::IsFinite(Yaw)||FMath::Abs(Yaw)>360) return false;
    FVector P=Layout.ToLocal(Surface);
    P.X=FMath::GridSnap(P.X,GridCm);P.Y=FMath::GridSnap(P.Y,GridCm);
    P.Z=FMath::GridSnap(P.Z,100.0)+Extent(Kind).Z;
    Result=FTransform(FRotator(0,Layout.YawDegrees+FMath::GridSnap(double(Yaw),90.0),0),Layout.ToWorld(P));
    return Fits(Layout,Kind,Result);
}
bool MRFPSBuild::Fits(const FMRFPSArenaLayout& Layout,EMRFPSBuildKind Kind,const FTransform& Transform)
{
    if(!Valid(Kind)||!Layout.IsSane()||Transform.ContainsNaN()||!Transform.GetScale3D().Equals(FVector::OneVector)) return false;
    const FVector E=Extent(Kind),P=Transform.GetLocation();
    if(P.Z-E.Z<Layout.Center.Z-.1||P.Z+E.Z>Layout.Center.Z+MaxHeightCm) return false;
    for(double X:{-E.X,E.X}) for(double Y:{-E.Y,E.Y})
        if(!Layout.ContainsPlanar(P+Transform.GetRotation().RotateVector(FVector(X,Y,0)),2)) return false;
    return true;
}
