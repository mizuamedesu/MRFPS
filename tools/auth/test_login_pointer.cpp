#include "../../Source/MRFPS/Public/MRFPSLoginPressState.h"
#include <cassert>
#include <limits>
#include <iostream>

using namespace MRFPSLoginPointer;

static void Neutral(FPressState& S) { S.Step(.05); S.Step(.05); }

int main()
{
    FPressState S;
    S.SetButton(true); S.SetAxis(1); S.SetSource(ESource::Aim);
    for (int I=0;I<10;++I) S.Step(.05);
    assert(!S.Desired()); // Opening while held must not click.
    S.SetButton(false); S.SetAxis(0); Neutral(S); assert(S.ButtonArmed && S.AxisArmed);
    S.SetButton(true); assert(S.Desired());
    S.SetAxis(1); S.SetButton(false); assert(S.Desired()); // Both represent one trigger.
    S.SetAxis(0); assert(!S.Desired());
    S.SetAxis(.61f); assert(S.Desired());
    S.SetAxis(.45f); assert(S.Desired()); // Axis hysteresis.
    S.SetAxis(.30f); assert(!S.Desired());

    S.SetButton(true); assert(S.Desired());
    S.SetSource(ESource::Grip); assert(!S.Desired());
    for (int I=0;I<10;++I) S.Step(.05);
    assert(!S.Desired()); // Switching Aim to Grip while held requires release.
    S.SetButton(false); Neutral(S); S.SetButton(true); assert(S.Desired());
    S.SetSource(ESource::None); S.Step(.05); assert(!S.Desired());
    S.SetSource(ESource::Aim); Neutral(S); assert(!S.Desired());
    S.SetButton(false); Neutral(S); S.SetButton(true); assert(S.Desired());

    S.SetSource(ESource::Hand); assert(!S.Desired());
    S.SetButton(false); S.SetAxis(0); Neutral(S);
    S.SetButton(true); assert(S.Desired()); // Hand pose still needs a physical trigger.
    S.SetButton(false); assert(!S.Desired());

    S.SetSource(ESource::Aim); S.SetButton(false); S.SetAxis(0); Neutral(S);
    S.SetAxis(1); assert(S.Desired());
    S.SetAxis(std::numeric_limits<float>::quiet_NaN()); assert(!S.Desired());
    S.SetSource(ESource::Hand); S.SetButton(false); S.SetAxis(0); Neutral(S);
    S.Step(std::numeric_limits<double>::quiet_NaN()); assert(!S.Desired());
    S.Step(-1); assert(!S.Desired());

    FPressState LostDigital;
    LostDigital.SetSource(ESource::Aim); LostDigital.SetButton(true); LostDigital.SetAxis(1);
    LostDigital.SetSource(ESource::Grip); LostDigital.SetAxis(0); Neutral(LostDigital);
    assert(!LostDigital.ButtonArmed && LostDigital.AxisArmed && !LostDigital.Desired());
    LostDigital.SetAxis(1); assert(LostDigital.Desired());
    LostDigital.SetAxis(0); assert(!LostDigital.Desired()); // Lost digital release cannot force a click.

    FPressState LostAxis;
    LostAxis.SetSource(ESource::Aim); LostAxis.SetButton(true); LostAxis.SetAxis(1);
    LostAxis.SetSource(ESource::Grip); LostAxis.SetButton(false); LostAxis.SetAxis(1); Neutral(LostAxis);
    assert(LostAxis.ButtonArmed && !LostAxis.AxisArmed && !LostAxis.Desired());
    LostAxis.SetButton(true); assert(LostAxis.Desired());
    LostAxis.SetButton(false); assert(!LostAxis.Desired()); // Lost axis release cannot force a click.
    std::cout << "PASS: controller/hand pointer arming, loss, switching, hysteresis and invalid data\n";
}
