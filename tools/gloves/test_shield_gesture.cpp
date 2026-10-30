#include "../../Source/MRFPS/Public/MRFPSShieldGesture.h"
#include <array>
#include <cassert>
#include <limits>
#include <iostream>

using MRFPSShieldGesture::FState;

static bool Update(FState& State,const std::array<float,5>& Curls,bool Fresh=true)
{
    return State.Update(Curls.data(),Curls.size(),Fresh);
}

int main()
{
    FState State;
    const std::array<float,5> Open{{.18f,.12f,.07f,.10f,.37f}};
    const std::array<float,5> ControllerGrip{{.30f,.20f,.85f,.85f,.85f}};
    const std::array<float,5> Fist{{.10f,.83f,.88f,.84f,.75f}};
    const std::array<float,5> HoldRange{{.10f,.52f,.54f,.53f,.51f}};
    const std::array<float,5> WeakLittle{{.10f,.70f,.75f,.70f,.40f}};

    assert(!Update(State,Open));
    assert(!Update(State,ControllerGrip)); // Holding the controller alone is not a fist.
    assert(!Update(State,HoldRange)); // Holding range cannot activate a released shield.
    assert(Update(State,Fist));
    assert(Update(State,HoldRange)); // Small changes do not flicker.
    assert(!Update(State,ControllerGrip)); // Extending the index releases the shield.
    assert(Update(State,WeakLittle)); // One weaker finger is tolerated.
    assert(!Update(State,Open));
    assert(Update(State,Fist));
    assert(!Update(State,Fist,false)); // Stale glove input deactivates immediately.
    assert(!Update(State,HoldRange)); // The state was reset by tracking loss.

    assert(Update(State,Fist));
    auto Invalid=Fist; Invalid[2]=std::numeric_limits<float>::quiet_NaN();
    assert(!Update(State,Invalid));
    assert(!Update(State,HoldRange));
    Invalid=Fist; Invalid[1]=std::numeric_limits<float>::infinity();
    assert(!Update(State,Invalid));
    Invalid=Fist; Invalid[3]=1.01f;
    assert(!Update(State,Invalid));
    assert(!State.Update(Fist.data(),4,true));
    assert(!State.Update(nullptr,5,true));
    std::cout << "PASS: fist shield activation, release, hysteresis and tracking loss\n";
}
