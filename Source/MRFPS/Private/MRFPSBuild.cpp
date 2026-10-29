#include "MRFPSBuild.h"

bool MRFPSBuild::Valid(EMRFPSBuildKind Kind) { return Kind==EMRFPSBuildKind::Wall || Kind==EMRFPSBuildKind::Crate; }
FVector MRFPSBuild::Extent(EMRFPSBuildKind Kind)
{
    if (Kind==EMRFPSBuildKind::Wall) return FVector(WallHalfThicknessCm,WallWidthCm*.5,100);
    if (Kind==EMRFPSBuildKind::Crate) return FVector(50);
    return FVector::ZeroVector;
}
FString MRFPSBuild::Label(EMRFPSBuildKind Kind)
{
    return Kind==EMRFPSBuildKind::Wall?TEXT("壁"):Kind==EMRFPSBuildKind::Crate?TEXT("木箱"):TEXT("");
}
bool MRFPSBuild::Snap(const FMRFPSArenaLayout& Layout,EMRFPSBuildKind Kind,const FVector& Surface,float Yaw,FTransform& Result)
{
    if(!Valid(Kind)||!Layout.IsSane()||Surface.ContainsNaN()||!FMath::IsFinite(Yaw)||FMath::Abs(Yaw)>360) return false;
    FVector P=Layout.ToLocal(Surface);
    P.X=FMath::GridSnap(P.X,GridCm);P.Y=FMath::GridSnap(P.Y,GridCm);
    P.Z=FMath::GridSnap(P.Z,WallRowHeightCm)+Extent(Kind).Z;
    Result=FTransform(FRotator(0,Layout.YawDegrees+FMath::GridSnap(double(Yaw),90.0),0),Layout.ToWorld(P));
    return Fits(Layout,Kind,Result);
}
bool MRFPSBuild::Fits(const FMRFPSArenaLayout& Layout,EMRFPSBuildKind Kind,const FTransform& Transform)
{
    if (Kind==EMRFPSBuildKind::Wall) return FitsWall(Layout,Transform,FMRFPSWallShape());
    if (Kind!=EMRFPSBuildKind::Crate || !Layout.IsSane() || Transform.ContainsNaN() ||
        !Transform.GetScale3D().Equals(FVector::OneVector)) return false;
    const FVector E=Extent(Kind),P=Transform.GetLocation();
    if (P.Z-E.Z<Layout.Center.Z-.1 || P.Z+E.Z>Layout.Center.Z+MaxHeightCm) return false;
    for (double X:{-E.X,E.X}) for (double Y:{-E.Y,E.Y})
        if (!Layout.ContainsPlanar(P+Transform.GetRotation().RotateVector(FVector(X,Y,0)),2)) return false;
    return true;
}
bool MRFPSBuild::FitsWall(const FMRFPSArenaLayout& Layout,const FTransform& Transform,const FMRFPSWallShape& Shape)
{
    if(!Shape.IsSane()||!Layout.IsSane()||Transform.ContainsNaN()||!Transform.GetScale3D().Equals(FVector::OneVector)) return false;
    const FVector E=Extent(EMRFPSBuildKind::Wall),P=Transform.GetLocation();
    const double Base=P.Z-E.Z,Top=Base+Shape.HeightRows*WallRowHeightCm;
    if(Base<Layout.Center.Z-.1||Top>Layout.Center.Z+MaxHeightCm) return false;
    for(double X:{-E.X,E.X}) for(double Y:{-E.Y,E.Y})
        if(!Layout.ContainsPlanar(P+Transform.GetRotation().RotateVector(FVector(X,Y,0)),2)) return false;
    return true;
}
FVector MRFPSBuild::CellCenter(int32 Column,int32 Row)
{
    return FVector(0,-WallWidthCm*.5+(Column+.5)*WallWidthCm/WallColumns,-100+(Row+.5)*WallRowHeightCm);
}
FVector MRFPSBuild::CellExtent()
{
    return FVector(WallHalfThicknessCm,WallWidthCm/(2*WallColumns),WallRowHeightCm*.5);
}
