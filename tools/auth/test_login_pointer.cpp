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
    for (int I=0;I<10;++I) { S.Step(.05); assert(S.Next()==EAction::None); }
    S.SetButton(false); S.SetAxis(0); Neutral(S);
    assert(S.ButtonArmed && S.AxisArmed && S.Next()==EAction::None);
    S.SetButton(true); assert(S.Next()==EAction::Press);
    S.SetAxis(1); assert(S.Next()==EAction::None);
    S.SetButton(false); assert(S.Next()==EAction::Release);
    // The other representation is still held, so it cannot click again.
    Neutral(S); assert(S.Next()==EAction::None);
    S.SetAxis(0); Neutral(S); S.SetAxis(.61f); assert(S.Next()==EAction::Press);
    S.SetAxis(.45f); assert(S.Next()==EAction::None);
    S.SetAxis(.30f); assert(S.Next()==EAction::Release);

    S.SetAxis(0); S.SetButton(false); Neutral(S);
    S.SetButton(true); assert(S.Next()==EAction::Press);
    // On a pose switch, the caller cancels capture (never mouse-up).
    S.CancelActive(); S.SetSource(ESource::Grip);
    for (int I=0;I<10;++I) { S.Step(.05); assert(S.Next()==EAction::None); }
    S.SetButton(false); Neutral(S); S.SetButton(true); assert(S.Next()==EAction::Press);
    S.CancelActive(); S.SetSource(ESource::None); S.Step(.05); assert(S.Next()==EAction::None);
    S.SetSource(ESource::Hand); Neutral(S); assert(S.Next()==EAction::None);
    S.SetButton(false); Neutral(S); S.SetButton(true); assert(S.Next()==EAction::Press);

    // Focus loss cancels the captured pointer; focus recovery remains disarmed.
    S.CancelActive(); S.SetFocused(false); S.Step(.05); assert(S.Next()==EAction::None);
    S.SetFocused(true); Neutral(S); assert(S.Next()==EAction::None);
    S.SetButton(false); Neutral(S); S.SetButton(true); assert(S.Next()==EAction::Press);
    S.SetButton(false); assert(S.Next()==EAction::Release);

    // Digital release is lost: a paired axis release completes the press and
    // the stale button cannot cause a second press or block a new axis click.
    FPressState LostDigital; LostDigital.SetSource(ESource::Aim); LostDigital.SetAxis(0); Neutral(LostDigital);
    LostDigital.SetButton(true); assert(LostDigital.Next()==EAction::Press);
    LostDigital.SetAxis(1); assert(LostDigital.Next()==EAction::None);
    LostDigital.SetAxis(0); assert(LostDigital.Next()==EAction::Release);
    Neutral(LostDigital); assert(!LostDigital.ButtonArmed && LostDigital.AxisArmed && LostDigital.Next()==EAction::None);
    LostDigital.SetAxis(1); assert(LostDigital.Next()==EAction::Press);
    LostDigital.SetAxis(0); assert(LostDigital.Next()==EAction::Release);

    // Axis release is lost: a paired button release completes the press and
    // the stale axis cannot cause a second press or block a new button click.
    FPressState LostAxis; LostAxis.SetSource(ESource::Aim); LostAxis.SetAxis(0); Neutral(LostAxis);
    LostAxis.SetAxis(1); assert(LostAxis.Next()==EAction::Press);
    LostAxis.SetButton(true); assert(LostAxis.Next()==EAction::None);
    LostAxis.SetButton(false); assert(LostAxis.Next()==EAction::Release);
    Neutral(LostAxis); assert(LostAxis.ButtonArmed && !LostAxis.AxisArmed && LostAxis.Next()==EAction::None);
    LostAxis.SetButton(true); assert(LostAxis.Next()==EAction::Press);
    LostAxis.SetButton(false); assert(LostAxis.Next()==EAction::Release);

    FPressState Invalid; Invalid.SetSource(ESource::Aim); Invalid.SetAxis(0); Neutral(Invalid);
    Invalid.SetAxis(1); assert(Invalid.Next()==EAction::Press);
    Invalid.SetAxis(std::numeric_limits<float>::quiet_NaN()); assert(Invalid.Next()==EAction::Cancel);

    // A fast press/release between frames remains an ordered click, and the
    // duplicated axis edge does not generate a second click.
    FPressState Queued; Queued.SetSource(ESource::Aim); Queued.SetAxis(0); Neutral(Queued);
    FEventQueue Events;
    Events.Push(EInput::Button,1); Events.Push(EInput::Axis,1);
    Events.Push(EInput::Button,0); Events.Push(EInput::Axis,0);
    int Presses=0,Releases=0;
    for (std::size_t I=0;I<Events.Count;++I) {
        const auto& E=Events.Events[I];
        if (E.Kind==EInput::Button) Queued.SetButton(E.Value!=0); else Queued.SetAxis(E.Value);
        const auto Action=Queued.Next(); Presses+=Action==EAction::Press; Releases+=Action==EAction::Release;
    }
    assert(Presses==1 && Releases==1);
    Events.Clear(); Events.Push(EInput::Button,1); Events.Clear();
    assert(Events.Count==0); // Pose/focus change discards stale edges.
    for (int I=0;I<33;++I) Events.Push(EInput::Button,1);
    assert(Events.Overflow && Events.Count==0);
    Queued.FailClosed(); Events.Clear(); assert(Queued.Next()==EAction::None);

    FPressState Held; Held.SetSource(ESource::Aim); Held.SetAxis(0); Neutral(Held);
    Held.SetButton(true); assert(Held.Next()==EAction::Press);
    Held.Step(3.0); assert(Held.Next()==EAction::Cancel); // Lost release cannot stay stuck.
    Neutral(Held); assert(Held.Next()==EAction::None);
    Held.SetButton(false); Neutral(Held); Held.SetButton(true); assert(Held.Next()==EAction::Press);
    Held.CancelActive(); Held.Disarm(); // A long widget-tick gap cancels, never releases.
    Neutral(Held); assert(Held.Next()==EAction::None);
    Held.SetButton(false); Neutral(Held); Held.SetButton(true); assert(Held.Next()==EAction::Press);
    std::cout << "PASS: pointer arming, focus/pose loss, independent channels and invalid data\n";
}
