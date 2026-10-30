#pragma once

#include <cmath>
#include <cstddef>

// The ContactGlove reports normalized curls from thumb through little finger.
// A shield follows a deliberate left fist only while its tracking is fresh.
namespace MRFPSShieldGesture
{
struct FState
{
    bool Closed=false;

    void Reset() { Closed=false; }

    bool Update(const float* Curls,std::size_t Count,bool Fresh)
    {
        if (!Fresh || !Curls || Count!=5) { Reset(); return false; }
        for (std::size_t I=0;I<Count;++I) {
            if (!std::isfinite(Curls[I]) || Curls[I]<0.f || Curls[I]>1.f) { Reset(); return false; }
        }

        // A controller grip naturally curls the other fingers, so require a
        // bent index finger too. The thumb can rest outside a fist. Separate
        // activation and release thresholds prevent flickering near the edge.
        const float FingerLimit=Closed?.45f:.55f;
        const float MeanLimit=Closed?.45f:.55f;
        float Total=0.f; int Bent=0;
        for (std::size_t I=1;I<5;++I) {
            Total+=Curls[I];
            if (Curls[I]>=FingerLimit) ++Bent;
        }
        Closed=Curls[1]>=FingerLimit && Bent>=3 && Total*.25f>=MeanLimit;
        return Closed;
    }
};
}
