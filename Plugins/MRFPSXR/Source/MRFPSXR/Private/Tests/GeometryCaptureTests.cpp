#include "GeometrySurface.h"
#include "GeometryMathCases.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSGeometryMathTest, "MRFPS.XR.Geometry.DepthAndTopology",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSGeometryMathTest::RunTest(const FString&)
{
    RunMRFPSGeometryMathCases([this](bool Passed, const char* Message) { TestTrue(UTF8_TO_TCHAR(Message),Passed); });
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSGeometryFrameTest, "MRFPS.XR.Geometry.FreshFrameAndEpoch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMRFPSGeometryFrameTest::RunTest(const FString&)
{
    using namespace MRFPSGeometryMath;
    TArray<FVector4f> Points; Points.SetNumZeroed(PointCount);
    FView View{-1,1,1,-1,.1f,10};
    for (uint32 I=0; I<PointCount; ++I)
    {
        FPoint P;
        UnprojectForwardDepth(View,1,GridPixel(I%GridWidth,GridWidth,51),GridPixel(I/GridWidth,GridHeight,37),51,37,P);
        Points[I]=FVector4f(P.X,P.Y,P.Z,P.ForwardMeters);
    }
    TestTrue(TEXT("Actual sampled positions agree with camera/frustum metadata"), MRFPSGeometrySurface::ProjectionMatches(Points,View,51,37));
    Points[50].Y+=2;
    TestFalse(TEXT("A GPU coordinate/row mismatch rejects the frame instead of replacing a point"), MRFPSGeometrySurface::ProjectionMatches(Points,View,51,37));
    Points[50].Y-=2;
    const auto NewFrame = [&]()
    {
        FMRFPSEnvironmentGeometryFrame Frame;
        Frame.Sequence=1; Frame.TrackingSpaceEpoch=3; Frame.CaptureMonotonicTime=10; Frame.bTrackingValid=true;
        TestTrue(TEXT("Surface builds within public caps"), MRFPSGeometrySurface::Build(Points,Frame));
        TestTrue(TEXT("Vertex/index caps are respected"), Frame.Vertices.Num()<=MRFPSGeometry::MaxVertices && Frame.Indices.Num()<=MRFPSGeometry::MaxIndices);
        return Frame;
    };
    FMRFPSEnvironmentGeometryFrame Out;
    TOptional<FMRFPSEnvironmentGeometryFrame> Latest(NewFrame());
    TestTrue(TEXT("A tracked fresh matching-epoch frame is consumed once"), MRFPSGeometrySurface::Take(Latest,3,10.2,Out));
    TestFalse(TEXT("Consumed frame cannot be repeated"), MRFPSGeometrySurface::Take(Latest,3,10.3,Out));
    TestTrue(TEXT("False dequeue clears previously valid output"), Out.Vertices.IsEmpty() && Out.Indices.IsEmpty() && !Out.bTrackingValid);
    Latest.Emplace(NewFrame());
    TestFalse(TEXT("Recenter epoch mismatch rejects a still-young frame"), MRFPSGeometrySurface::Take(Latest,4,10.2,Out));
    TestTrue(TEXT("Epoch rejection clears pending and output"), !Latest.IsSet() && Out.Vertices.IsEmpty() && Out.Sequence==0);
    Latest.Emplace(NewFrame());
    TestFalse(TEXT("No live session epoch is unsupported"), MRFPSGeometrySurface::Take(Latest,0,10.2,Out));
    Latest.Emplace(NewFrame());
    TestFalse(TEXT("A delayed GPU readback expires by acquisition time"), MRFPSGeometrySurface::Take(Latest,3,10.75,Out));
    Latest.Emplace(NewFrame()); Latest->bTrackingValid=false;
    TestFalse(TEXT("Invalid positional tracking cannot become geometry"), MRFPSGeometrySurface::Take(Latest,3,10.2,Out));
    Latest.Emplace(NewFrame()); Latest->CaptureMonotonicTime=11;
    TestFalse(TEXT("Future acquisition timestamp is rejected"), MRFPSGeometrySurface::Take(Latest,3,10.2,Out));
    Out=NewFrame();
    TestFalse(TEXT("Invalid grid build fails"), MRFPSGeometrySurface::Build(TConstArrayView<FVector4f>(),Out));
    TestTrue(TEXT("Failed build clears metadata and mesh"), Out.Vertices.IsEmpty() && Out.Indices.IsEmpty() && Out.Sequence==0 && !Out.bTrackingValid);
    return true;
}
#endif
