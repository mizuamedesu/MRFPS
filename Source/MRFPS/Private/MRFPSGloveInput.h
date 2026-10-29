#pragma once

#include "MRFPSGame.h"
#include "MRFPSGloveSubsystem.h"

namespace MRFPSGloveInput
{
// Only fresh DivingStation/ContactGlove flexion supplies finger animation.
inline void CopyFingers(const FMRFPSGloveHandFrame& Glove,FMRFPSFingerPose& Fingers)
{
    Fingers.bValid=false;
    if (!Glove.bFlexValid || Glove.Flexion.Num()!=15 || Glove.Splay.Num()!=5) return;
    Fingers.bValid=true;
    for (int32 J=0;J<15;++J) Fingers.Joint[J]=uint8(FMath::RoundToInt(FMath::Clamp(Glove.Flexion[J],0.f,1.f)*255));
    for (int32 J=0;J<5;++J) Fingers.Splay[J]=uint8(FMath::RoundToInt((FMath::Clamp(Glove.Splay[J],-1.f,1.f)+1)*127.5f));
}

inline float LeftOpenness(const FMRFPSGloveHandFrame& Glove)
{
    return Glove.IsOpenPalm()?1.f:0.f;
}
}
