#pragma once

#include "CoreMinimal.h"

// Canvas rendering and hit testing use the same viewport-dependent layout.
// Keep the centered round panel clear of the existing spectator controls.
struct FMRFPSInspectorHudRect
{
    float X=0, Y=0, Width=0, Height=0;
    bool Contains(float PX,float PY) const { return PX>=X && PX<X+Width && PY>=Y && PY<Y+Height; }
};

struct FMRFPSInspectorHudLayout
{
    explicit FMRFPSInspectorHudLayout(float InWidth)
        : Width(FMath::Max(0.f,InWidth)), bCompact(Width<860.f)
    {
        HeaderWidth=FMath::Min(300.f,FMath::Max(0.f,Width-36.f));
        InfoTop=bCompact?96.f:18.f;
        ButtonTop=bCompact?208.f:96.f;
        ButtonStart=bCompact?18.f:300.f;
        ButtonStep=bCompact?FMath::Min(100.f,FMath::Max(0.f,(Width-36.f)/3.f)):100.f;
    }
    FMRFPSInspectorHudRect RoundPanel() const { return {(Width-HeaderWidth)*.5f,18.f,HeaderWidth,66.f}; }
    FMRFPSInspectorHudRect InfoPanel() const { return {18.f,InfoTop,260.f,104.f}; }
    FMRFPSInspectorHudRect Button(int32 Index) const
    {
        if(Index<0 || Index>=3) return {};
        const float ButtonWidth=bCompact?FMath::Max(0.f,ButtonStep-8.f):(Index==2?112.f:92.f);
        return {ButtonStart+Index*ButtonStep,ButtonTop,ButtonWidth,30.f};
    }
    int32 HitButton(float X,float Y) const
    {
        for(int32 Index=0;Index<3;++Index) if(Button(Index).Contains(X,Y)) return Index;
        return INDEX_NONE;
    }
    float Width=0, HeaderWidth=0, InfoTop=18, ButtonTop=96, ButtonStart=300, ButtonStep=100;
    bool bCompact=false;
};
