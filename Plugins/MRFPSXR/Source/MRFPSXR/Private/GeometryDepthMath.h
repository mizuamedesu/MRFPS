#pragma once

// Dependency-free reference math also used by runtime metadata/topology checks.
// No UObject, OpenXR, Vulkan, allocation, fabricated samples or tracking state.
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace MRFPSGeometryMath
{
inline constexpr uint32_t GridWidth = 48;
inline constexpr uint32_t GridHeight = 36;
inline constexpr uint32_t PointCount = GridWidth * GridHeight;
inline constexpr uint32_t MaximumIndices = (GridWidth-1) * (GridHeight-1) * 6;
inline constexpr float MinimumDepthMeters = 0.25f;
inline constexpr float MaximumDepthMeters = 7.0f;
inline constexpr float MaximumEdgeCentimeters = 75.0f;
inline constexpr double FreshSeconds = 0.75;

struct FPoint { float X = 0, Y = 0, Z = 0, ForwardMeters = 0; };
struct FView
{
    float Left = 0, Right = 0, Up = 0, Down = 0; // Tangents of returned fov.
    float Near = 0, Far = 0;
    float Qx = 0, Qy = 0, Qz = 0, Qw = 1;
    float Px = 0, Py = 0, Pz = 0; // Returned view pose, OpenXR meters.
};

inline bool IsFresh(double AcquiredAt, double Now)
{
    return std::isfinite(AcquiredAt) && std::isfinite(Now) && AcquiredAt > 0 &&
        Now >= AcquiredAt && Now-AcquiredAt < FreshSeconds;
}

inline bool IsIdentityOffset(float Qx, float Qy, float Qz, float Qw, float X, float Y, float Z)
{
    // Runtime comparison of clone->actual UE tracking space, in native meters.
    // q and -q describe the same rotation. The check never supplies a pose.
    return std::isfinite(Qx) && std::isfinite(Qy) && std::isfinite(Qz) && std::isfinite(Qw) &&
        std::isfinite(X) && std::isfinite(Y) && std::isfinite(Z) &&
        std::fabs(Qx)<0.0001f && std::fabs(Qy)<0.0001f && std::fabs(Qz)<0.0001f &&
        std::fabs(std::fabs(Qw)-1)<0.0001f &&
        std::fabs(X)<0.0005f && std::fabs(Y)<0.0005f && std::fabs(Z)<0.0005f;
}

inline bool ValidPlanes(float Near, float Far)
{
    // OpenXR uses far<near for an infinite projection. Positive IEEE infinity
    // is handled too; NaN, negative infinity and a zero denominator are rejected.
    return std::isfinite(Near) && Near > 0 && Far != Near &&
        (std::isfinite(Far) || (std::isinf(Far) && Far > 0));
}

inline bool ValidView(const FView& V)
{
    const float Length2 = V.Qx*V.Qx + V.Qy*V.Qy + V.Qz*V.Qz + V.Qw*V.Qw;
    return ValidPlanes(V.Near, V.Far) && std::isfinite(V.Left) && std::isfinite(V.Right) &&
        std::isfinite(V.Up) && std::isfinite(V.Down) && V.Left < V.Right && V.Down < V.Up &&
        std::isfinite(Length2) && std::fabs(Length2-1.0f) < 0.01f &&
        std::isfinite(V.Px) && std::isfinite(V.Py) && std::isfinite(V.Pz);
}

inline bool LinearDepth(float Encoded, float Near, float Far, float& ForwardMeters)
{
    ForwardMeters = 0;
    if (!ValidPlanes(Near, Far) || !(Encoded > 0 && Encoded < 1)) return false;
    const float Z = (Far < Near || std::isinf(Far)) ? Near/(1-Encoded) : Near*Far/(Far-Encoded*(Far-Near));
    if (!std::isfinite(Z) || Z < MinimumDepthMeters || Z > MaximumDepthMeters) return false;
    ForwardMeters = Z;
    return true;
}

inline uint32_t GridPixel(uint32_t Sample, uint32_t Samples, uint32_t SourcePixels)
{
    // The caller validates its fixed bounds. Integer arithmetic matches the
    // shader exactly, including odd source sizes and pixel-center selection.
    return ((2*Sample+1)*SourcePixels)/(2*Samples);
}

inline bool UnprojectForwardDepth(const FView& V, float Z, uint32_t PixelX, uint32_t PixelY,
    uint32_t Width, uint32_t Height, FPoint& Out)
{
    Out = FPoint{};
    if (!ValidView(V) || Width == 0 || Height == 0 || Width > 4096 || Height > 4096 ||
        PixelX >= Width || PixelY >= Height || !std::isfinite(Z) || Z < MinimumDepthMeters || Z > MaximumDepthMeters) return false;
    const float U = (float(PixelX)+0.5f)/float(Width);
    const float Row = (float(PixelY)+0.5f)/float(Height);
    const float X = (V.Left+(V.Right-V.Left)*U)*Z;
    const float Y = (V.Up+(V.Down-V.Up)*Row)*Z;
    // Vulkan rows increase downward. The view's top ray is angleUp. Z is
    // camera-forward distance, not Euclidean range: never normalize this ray.
    const float Zr = -Z;
    const float Cx = V.Qy*Zr-V.Qz*Y + V.Qw*X;
    const float Cy = V.Qz*X-V.Qx*Zr + V.Qw*Y;
    const float Cz = V.Qx*Y-V.Qy*X + V.Qw*Zr;
    const float Xr = X + 2*(V.Qy*Cz-V.Qz*Cy) + V.Px;
    const float Yr = Y + 2*(V.Qz*Cx-V.Qx*Cz) + V.Py;
    const float Zrot = Zr + 2*(V.Qx*Cy-V.Qy*Cx) + V.Pz;
    Out = FPoint{-Zrot*100, Xr*100, Yr*100, Z};
    return std::isfinite(Out.X) && std::isfinite(Out.Y) && std::isfinite(Out.Z);
}

inline bool Unproject(const FView& V, float Encoded, uint32_t PixelX, uint32_t PixelY,
    uint32_t Width, uint32_t Height, FPoint& Out)
{
    Out = FPoint{};
    float Z = 0;
    return LinearDepth(Encoded, V.Near, V.Far, Z) && UnprojectForwardDepth(V, Z, PixelX, PixelY, Width, Height, Out);
}

inline bool ValidPoint(const FPoint& P)
{
    return std::isfinite(P.X) && std::isfinite(P.Y) && std::isfinite(P.Z) && std::isfinite(P.ForwardMeters) &&
        std::fabs(P.X) <= 20000 && std::fabs(P.Y) <= 20000 && std::fabs(P.Z) <= 20000 &&
        P.ForwardMeters >= MinimumDepthMeters && P.ForwardMeters <= MaximumDepthMeters;
}

inline bool KeepTriangle(const FPoint& A, const FPoint& B, const FPoint& C)
{
    if (!ValidPoint(A) || !ValidPoint(B) || !ValidPoint(C)) return false;
    float MinimumZ = A.ForwardMeters < B.ForwardMeters ? A.ForwardMeters : B.ForwardMeters;
    MinimumZ = MinimumZ < C.ForwardMeters ? MinimumZ : C.ForwardMeters;
    const float Limit = 0.15f + 0.02f*MinimumZ;
    if (std::fabs(A.ForwardMeters-B.ForwardMeters) > Limit || std::fabs(A.ForwardMeters-C.ForwardMeters) > Limit ||
        std::fabs(B.ForwardMeters-C.ForwardMeters) > Limit) return false;
    const float Bx=B.X-A.X, By=B.Y-A.Y, Bz=B.Z-A.Z;
    const float Cx=C.X-A.X, Cy=C.Y-A.Y, Cz=C.Z-A.Z;
    const float Dx=B.X-C.X, Dy=B.Y-C.Y, Dz=B.Z-C.Z;
    constexpr float Edge2 = MaximumEdgeCentimeters*MaximumEdgeCentimeters;
    if (Bx*Bx+By*By+Bz*Bz > Edge2 || Cx*Cx+Cy*Cy+Cz*Cz > Edge2 || Dx*Dx+Dy*Dy+Dz*Dz > Edge2) return false;
    const float Nx=By*Cz-Bz*Cy, Ny=Bz*Cx-Bx*Cz, Nz=Bx*Cy-By*Cx;
    return Nx*Nx+Ny*Ny+Nz*Nz > 0.0001f; // Reject duplicate/collinear samples.
}

inline bool BuildTriangles(const FPoint* Points, size_t Count, uint16_t* Out, size_t Capacity, size_t& Written)
{
    Written = 0;
    if (!Points || !Out || Count != PointCount || Capacity < MaximumIndices) return false;
    const auto Emit = [&](uint32_t A, uint32_t B, uint32_t C)
    {
        if (!KeepTriangle(Points[A], Points[B], Points[C])) return;
        Out[Written++] = static_cast<uint16_t>(A);
        Out[Written++] = static_cast<uint16_t>(B);
        Out[Written++] = static_cast<uint16_t>(C);
    };
    for (uint32_t Y=0; Y<GridHeight-1; ++Y)
        for (uint32_t X=0; X<GridWidth-1; ++X)
        {
            const uint32_t A=Y*GridWidth+X;
            Emit(A, A+1, A+GridWidth);
            Emit(A+1, A+GridWidth+1, A+GridWidth);
        }
    return Written > 0;
}
}
