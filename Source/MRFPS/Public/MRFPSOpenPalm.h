#pragma once

#include <cmath>
#include <cstddef>

// Local ContactGlove shield gesture. Values are normalized finger curls,
// thumb through little; fresh tracking is required for every update.
namespace MRFPSOpenPalm
{
struct FState
{
    bool Open=false;

    void Reset() { Open=false; }

    bool Update(const float* Curls,std::size_t Count,bool Fresh)
    {
        if (!Fresh || !Curls || Count!=5) { Reset(); return false; }
        for (std::size_t I=0;I<Count;++I) {
            if (!std::isfinite(Curls[I]) || Curls[I]<0.f || Curls[I]>1.f) { Reset(); return false; }
        }

        // Three extended fingers are sufficient, so a naturally bent little
        // finger does not require the player to flatten every joint. A lower
        // activation threshold than the hold threshold prevents flicker.
        const float FingerLimit=Open?.60f:.50f;
        const float MeanLimit=Open?.58f:.50f;
        const float ThumbLimit=Open?.95f:.85f;
        float Total=0.f; int Extended=0;
        for (std::size_t I=1;I<5;++I) {
            Total+=Curls[I];
            if (Curls[I]<=FingerLimit) ++Extended;
        }
        Open=Curls[0]<=ThumbLimit && Extended>=3 && Total*.25f<=MeanLimit;
        return Open;
    }
};
}
