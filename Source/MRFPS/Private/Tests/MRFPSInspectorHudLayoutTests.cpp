#if WITH_DEV_AUTOMATION_TESTS && !UE_SERVER
#include "MRFPSInspectorHudLayout.h"
#include "Misc/AutomationTest.h"

namespace
{
bool Overlaps(const FMRFPSInspectorHudRect& A,const FMRFPSInspectorHudRect& B)
{
    return A.X<B.X+B.Width && B.X<A.X+A.Width && A.Y<B.Y+B.Height && B.Y<A.Y+A.Height;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRFPSInspectorHudLayoutTest,"MRFPS.Inspector.RoundHudLayout",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FMRFPSInspectorHudLayoutTest::RunTest(const FString& Parameters)
{
    // Exercise narrow, transition, and standard desktop widths. The layout
    // serves both painting and mouse hit detection, so test them together.
    for(float Width:{320.f,640.f,859.f,860.f,960.f,1280.f,1920.f}) {
        const FMRFPSInspectorHudLayout Layout{Width};
        const auto Header=Layout.RoundPanel(), Info=Layout.InfoPanel();
        TestTrue(TEXT("Round panel is centered"),FMath::IsNearlyEqual(Header.X+Header.Width*.5f,Width*.5f));
        TestTrue(TEXT("Round panel stays inside viewport"),Header.X>=0 && Header.X+Header.Width<=Width);
        TestFalse(TEXT("Round panel does not overlap player information"),Overlaps(Header,Info));
        for(int32 Index=0;Index<3;++Index) {
            const auto Button=Layout.Button(Index);
            TestTrue(TEXT("Popout button fits viewport"),Button.X>=0 && Button.X+Button.Width<=Width && Button.Width>0);
            TestFalse(TEXT("Round panel does not overlap popout button"),Overlaps(Header,Button));
            TestFalse(TEXT("Player information does not overlap popout button"),Overlaps(Info,Button));
            TestEqual(TEXT("Visible button center opens the matching window"),Layout.HitButton(Button.X+Button.Width*.5f,Button.Y+Button.Height*.5f),Index);
            if(Index>0) TestFalse(TEXT("Popout buttons are separate"),Overlaps(Layout.Button(Index-1),Button));
        }
        TestEqual(TEXT("Unrelated point does not open a window"),Layout.HitButton(0,0),INDEX_NONE);
    }
    return true;
}
#endif
