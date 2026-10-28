#pragma once

#include "../GeometryDepthMath.h"
#include <array>
#include <limits>

// Shared by the small standalone check and UE Automation. These are explicit
// mathematical fixtures, never selectable as a capture source or runtime frame.
template<class FCheck> void RunMRFPSGeometryMathCases(FCheck&& Check)
{
    using namespace MRFPSGeometryMath;
    const auto Near = [](float A, float B) { return std::fabs(A-B) < 0.002f; };
    float Z = -1;
    const float EncodedTwoMeters = 10.0f*(2.0f-0.1f)/(2.0f*(10.0f-0.1f));
    Check(LinearDepth(EncodedTwoMeters, .1f, 10.0f, Z) && Near(Z, 2), "Finite D16 projection linearizes to forward depth");
    Check(LinearDepth(.95f, .1f, 0.0f, Z) && Near(Z, 2), "far less than near selects infinite projection");
    Check(LinearDepth(.95f, .1f, std::numeric_limits<float>::infinity(), Z) && Near(Z, 2), "Positive infinite far plane is supported");
    for (float D : {0.0f, 1.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
        Check(!LinearDepth(D, .1f, 10.0f, Z) && Z == 0, "Invalid/endpoint depth never produces a sample");
    Check(!LinearDepth(.9f, 0.0f, 10.0f, Z) && Z == 0, "Nonpositive near is rejected");
    Check(!LinearDepth(.9f, 1.0f, 1.0f, Z) && Z == 0, "Equal planes are rejected");
    Check(!LinearDepth(.9f, .1f, -std::numeric_limits<float>::infinity(), Z), "Negative infinite far is rejected");
    Check(!LinearDepth(.99f, .1f, 0.0f, Z) && Z == 0, "Distant points exceed the observation bound");

    FView V{-1,1,1,-1,.1f,10.0f};
    FPoint P;
    Check(UnprojectForwardDepth(V, 2, 0, 0, 1, 1, P) && Near(P.X, 200) && Near(P.Y, 0) && Near(P.Z, 0), "XR minus Z becomes UE forward X in centimeters");
    Check(UnprojectForwardDepth(V, 2, 0, 0, 4, 4, P) && Near(P.X, 200) && Near(P.Y, -150) && Near(P.Z, 150), "Top-left pixel uses angleUp and left ray, without ray normalization");
    Check(UnprojectForwardDepth(V, 2, 3, 3, 4, 4, P) && Near(P.X, 200) && Near(P.Y, 150) && Near(P.Z, -150), "Bottom-right pixel uses angleDown and right ray");
    V.Left=-.7f; V.Right=1.1f; V.Up=.8f; V.Down=-.5f;
    Check(UnprojectForwardDepth(V, 2, 0, 0, 1, 1, P) && Near(P.Y, 40) && Near(P.Z, 30), "Asymmetric returned field of view is preserved");
    V=FView{-1,1,1,-1,.1f,10.0f};
    V.Qy=V.Qw=std::sqrt(.5f); V.Px=1; V.Py=2; V.Pz=3;
    Check(UnprojectForwardDepth(V, 2, 0, 0, 1, 1, P) && Near(P.X,-300) && Near(P.Y,-100) && Near(P.Z,200), "Actual depth-view rotation and translation precede UE basis conversion");
    V.Qw=0; V.Qy=0;
    Check(!UnprojectForwardDepth(V, 2, 0, 0, 1, 1, P) && P.ForwardMeters==0, "Invalid quaternion cannot produce a point");
    V=FView{-1,1,1,-1,.1f,10.0f}; V.Right=V.Left;
    Check(!ValidView(V), "Flipped or empty FOV is rejected");
    Check(GridPixel(0,48,51)==0 && GridPixel(47,48,51)==50 && GridPixel(35,36,37)==36, "Odd-size nearest pixel-center grid matches integer shader addressing");

    std::array<FPoint, PointCount> Samples{};
    for (uint32_t Y=0; Y<GridHeight; ++Y)
        for (uint32_t X=0; X<GridWidth; ++X)
            Samples[Y*GridWidth+X] = {100.0f, float(X)*3, float(Y)*3, 1};
    std::array<uint16_t, MaximumIndices+1> Indices{};
    Indices[MaximumIndices]=0xfeed;
    size_t Written=99;
    Check(BuildTriangles(Samples.data(),Samples.size(),Indices.data(),MaximumIndices,Written) && Written==MaximumIndices &&
        Indices[MaximumIndices]==0xfeed, "Full plane emits bounded triangles without overrunning the index buffer");
    bool InBounds=true;
    for (size_t I=0; I<Written; ++I) InBounds=InBounds && Indices[I]<PointCount;
    Check(InBounds, "All topology indices refer to actual input samples");
    const uint32_t Hole = 18*GridWidth+24;
    Samples[Hole] = FPoint{};
    Check(BuildTriangles(Samples.data(),Samples.size(),Indices.data(),MaximumIndices,Written) && Written==MaximumIndices-18, "One invalid interior pixel leaves six triangle holes");
    bool HoleAbsent=true;
    for (size_t I=0; I<Written; ++I) HoleAbsent=HoleAbsent && Indices[I]!=Hole;
    Check(HoleAbsent, "Missing depth is never filled from adjacent samples");
    for (uint32_t Y=0; Y<GridHeight; ++Y)
        for (uint32_t X=0; X<GridWidth; ++X)
            Samples[Y*GridWidth+X] = {X<24 ? 100.0f : 200.0f, float(X)*3, float(Y)*3, X<24 ? 1.0f : 2.0f};
    Check(BuildTriangles(Samples.data(),Samples.size(),Indices.data(),MaximumIndices,Written) && Written==MaximumIndices-210, "A one-meter depth discontinuity removes the connecting strip");
    bool NoBridge=true;
    for (size_t I=0; I<Written; I+=3)
        NoBridge=NoBridge && Samples[Indices[I]].ForwardMeters==Samples[Indices[I+1]].ForwardMeters &&
            Samples[Indices[I]].ForwardMeters==Samples[Indices[I+2]].ForwardMeters;
    Check(NoBridge, "Every retained triangle stays on one side of the depth edge");
    Check(!BuildTriangles(Samples.data(),Samples.size(),Indices.data(),MaximumIndices-1,Written) && Written==0, "Insufficient output capacity fails before writing");
    Check(!BuildTriangles(Samples.data(),Samples.size()-1,Indices.data(),MaximumIndices,Written) && Written==0, "Incorrect input grid size fails empty");
    Check(KeepTriangle({0,0,0,1},{75,0,0,1},{37,1,0,1}), "75-centimeter neighbor edge is allowed");
    Check(!KeepTriangle({0,0,0,1},{75.1f,0,0,1},{37,1,0,1}), "Edges over 75 centimeters are rejected, not 750 centimeters");
    Check(!KeepTriangle({0,0,0,1},{1,0,0,1},{2,0,0,1}), "Collinear samples do not create a collision triangle");
    Check(!KeepTriangle({0,0,0,1},{1,0,0,1},{0,1,0,2}), "Depth discontinuity rejects an otherwise short triangle");
    Check(!ValidPoint({std::numeric_limits<float>::quiet_NaN(),0,0,1}), "NaN cannot enter a surface");
    Check(!ValidPoint({20001,0,0,1}), "Tracking coordinates have a finite resource bound");
    Check(IsIdentityOffset(0,0,0,1,0,0,0) && IsIdentityOffset(0,0,0,-1,0,0,0), "Tracking clone alignment accepts both quaternion identity signs");
    Check(!IsIdentityOffset(0,0,0,1,.01f,0,0), "An unpreserved UE base/recenter translation is rejected");
    Check(!IsIdentityOffset(0,.01f,0,.99995f,0,0,0), "An unpreserved UE base/recenter rotation is rejected");
    Check(!IsIdentityOffset(0,0,0,0,0,0,0), "An invalid clone transform cannot be called aligned");
    Check(IsFresh(10,10) && IsFresh(10,10.749) && !IsFresh(10,10.75), "Acquisition-age TTL expires at 0.75 seconds");
    Check(!IsFresh(10,9.99) && !IsFresh(0,0) && !IsFresh(std::numeric_limits<double>::quiet_NaN(),10), "Future/zero/nonfinite acquisition times are rejected");
}
