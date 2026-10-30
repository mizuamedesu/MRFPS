#include "../../Source/MRFPS/Public/MRFPSOpenPalm.h"
#include <array>
#include <cassert>
#include <limits>
#include <iostream>

using MRFPSOpenPalm::FState;

static bool Update(FState& State,const std::array<float,5>& Curls,bool Fresh=true)
{
    return State.Update(Curls.data(),Curls.size(),Fresh);
}

int main()
{
    FState State;
    const std::array<float,5> Relaxed{{.30f,.32f,.48f,.39f,.56f}};
    const std::array<float,5> NearBoundary{{.30f,.54f,.49f,.53f,.61f}};
    const std::array<float,5> Closed{{.70f,.83f,.88f,.84f,.75f}};
    const std::array<float,5> FormerLowSample{{.183f,.065f,.035f,.071f,.332f}};

    assert(Update(State,Relaxed)); // A bent little finger is tolerated.
    assert(Update(State,NearBoundary)); // Held open does not flicker near the boundary.
    assert(!Update(State,Closed)); // A clearly closed hand drops the shield.
    assert(!Update(State,NearBoundary)); // The hold range cannot activate from closed.
    assert(Update(State,FormerLowSample));
    assert(!Update(State,FormerLowSample,false)); // A missing glove deactivates immediately.
    assert(!Update(State,NearBoundary)); // Missing input also reset the latch.

    assert(Update(State,Relaxed));
    auto Invalid=Relaxed; Invalid[2]=std::numeric_limits<float>::quiet_NaN();
    assert(!Update(State,Invalid));
    assert(!Update(State,NearBoundary));
    Invalid=Relaxed; Invalid[1]=std::numeric_limits<float>::infinity();
    assert(!Update(State,Invalid));
    Invalid=Relaxed; Invalid[3]=1.01f;
    assert(!Update(State,Invalid));
    assert(!State.Update(Relaxed.data(),4,true));
    assert(!State.Update(nullptr,5,true));
    std::cout << "PASS: open palm hysteresis, closed hand and missing sensor data\n";
}
