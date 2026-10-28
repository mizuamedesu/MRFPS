#include "MRFPSArenaLayout.h"

bool FMRFPSArenaLayout::IsSane() const
{
    return FMath::IsFinite(WidthCm) && FMath::IsFinite(LengthCm) && FMath::IsFinite(YawDegrees) && FMath::Abs(YawDegrees)<=180.01 &&
        WidthCm>=MRFPSArenaLimits::MinDimensionCm && WidthCm<=MRFPSArenaLimits::MaxDimensionCm &&
        LengthCm>=MRFPSArenaLimits::MinDimensionCm && LengthCm<=MRFPSArenaLimits::MaxDimensionCm &&
        !Center.ContainsNaN() && FMath::Abs(Center.X)<=10000 && FMath::Abs(Center.Y)<=10000 &&
        FMath::Abs(Center.Z)<=1000 && Revision>0;
}
FVector FMRFPSArenaLayout::ToLocal(const FVector& World) const
{ return FRotator(0,-YawDegrees,0).RotateVector(World-Center); }
FVector FMRFPSArenaLayout::ToWorld(const FVector& Local) const
{ return Center+FRotator(0,YawDegrees,0).RotateVector(Local); }
bool FMRFPSArenaLayout::ContainsPlanar(const FVector& World,double MarginCm) const
{
    if (!IsSane() || World.ContainsNaN() || !FMath::IsFinite(MarginCm) || MarginCm<0) return false;
    const FVector P=ToLocal(World);
    return WidthCm*.5>=MarginCm && LengthCm*.5>=MarginCm &&
        FMath::Abs(P.X)<=WidthCm*.5-MarginCm+1.e-4 && FMath::Abs(P.Y)<=LengthCm*.5-MarginCm+1.e-4;
}
bool FMRFPSArenaLayout::ContainsHead(const FVector& World) const
{ return ContainsPlanar(World) && World.Z-Center.Z>=30 && World.Z-Center.Z<=260; }
bool FMRFPSArenaLayout::ContainsGeometry(const FVector& World) const
{ return ContainsPlanar(World) && World.Z-Center.Z>=-20 && World.Z-Center.Z<=300; }
FVector FMRFPSArenaLayout::NominalStart(int32 Slot,double HeadHeightCm) const
{ return ToWorld(FVector(0,Slot==0?-LengthCm*.5:LengthCm*.5,HeadHeightCm)); }
FRotator FMRFPSArenaLayout::StartRotation(int32 Slot) const
{ return FRotator(0,YawDegrees+(Slot==0?90:-90),0); }
FVector FMRFPSArenaLayout::ClampPlanar(const FVector& World,double MarginCm) const
{
    if (!IsSane() || World.ContainsNaN() || !FMath::IsFinite(MarginCm) || MarginCm<0 || MarginCm>FMath::Min(WidthCm,LengthCm)*.5) return World;
    FVector P=ToLocal(World);
    P.X=FMath::Clamp(P.X,-WidthCm*.5+MarginCm,WidthCm*.5-MarginCm);
    P.Y=FMath::Clamp(P.Y,-LengthCm*.5+MarginCm,LengthCm*.5-MarginCm);
    return ToWorld(P);
}
bool FMRFPSArenaLayout::CalibrateFromAnchors(const FVector& A0,const FVector& A1,double RegisteredFloorZ,
    uint32 SharedRegistrationRevision,FString& SafeError)
{
    SafeError=TEXT("LAYOUT_SHARED_BASIS_UNAVAILABLE");
    if (!SharedRegistrationRevision) return false;
    SafeError=TEXT("LAYOUT_ANCHORS_INVALID");
    if (!IsSane() || A0.ContainsNaN() || A1.ContainsNaN() || !FMath::IsFinite(RegisteredFloorZ) ||
        FMath::Abs(RegisteredFloorZ)>1000 || Revision==MAX_uint32) return false;
    const FVector Direction(A1.X-A0.X,A1.Y-A0.Y,0);
    if (Direction.SizeSquared()<100.*100. || Direction.SizeSquared()>4000.*4000.) return false;
    FMRFPSArenaLayout Next=*this;
    Next.Center=FVector((A0.X+A1.X)*.5,(A0.Y+A1.Y)*.5,RegisteredFloorZ);
    Next.YawDegrees=FRotator::NormalizeAxis(FMath::RadiansToDegrees(FMath::Atan2(Direction.Y,Direction.X))-90);
    Next.RegistrationRevision=SharedRegistrationRevision; Next.bCalibrated=true; ++Next.Revision;
    if (!Next.IsSane()) return false;
    *this=Next; SafeError.Empty(); return true;
}
bool FMRFPSArenaLayout::SetDimensions(double Width,double Length,FString& SafeError)
{
    SafeError=TEXT("LAYOUT_DIMENSIONS_INVALID");
    if (Revision==MAX_uint32 || !FMath::IsFinite(Width) || !FMath::IsFinite(Length)) return false;
    FMRFPSArenaLayout Next=*this; Next.WidthCm=Width; Next.LengthCm=Length; ++Next.Revision;
    if (!Next.IsSane()) return false;
    *this=Next; SafeError.Empty(); return true;
}
