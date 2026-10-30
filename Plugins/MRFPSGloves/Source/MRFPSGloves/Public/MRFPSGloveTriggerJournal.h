#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// A bounded, non-consuming journal of right-glove input transitions. The
// receiver still updates the ordinary gameplay frame exactly as before.
namespace MRFPSGloveTrigger
{
enum class EStream : std::uint8_t { Controller, Flex };
struct FEvent
{
    std::uint64_t Sequence=0;
    double At=0;
    EStream Stream=EStream::Controller;
    float Value=0;
    bool Valid=false, Button=false;
};

class FJournal
{
public:
    static constexpr std::size_t Capacity=64;

    std::uint64_t Latest() const { return Next-1; }

    // Recording is on the game thread, using the receiver's receipt time.
    // A gap means the previous state was stale, not a release.
    void Sample(EStream Stream,double At,bool Valid,float Value,bool Button,double StaleAfter)
    {
        if (!std::isfinite(At) || At<0 || !std::isfinite(StaleAfter) || StaleAfter<=0) return;
        const auto Index=static_cast<std::size_t>(Stream);
        if (LastAt[Index]>=0 && At<LastAt[Index]) { Invalidate(Stream,At); return; }
        if (LastAt[Index]>=0 && At-LastAt[Index]>StaleAfter)
            Invalidate(Stream,LastAt[Index]+StaleAfter);
        LastAt[Index]=At;
        if (!std::isfinite(Value)) Valid=false;
        const int Cat=!Valid?-1:(Button || Value>.55f)?2:Value<.3f?0:1;
        if (Cat!=LastCategory[Index]) { Append(Stream,At,Valid,Value,Button); LastCategory[Index]=Cat; }
    }
    void Invalidate(EStream Stream,double At)
    {
        const auto Index=static_cast<std::size_t>(Stream);
        if (LastCategory[Index]!=-1) { Append(Stream,At,false,0,false); LastCategory[Index]=-1; }
        LastAt[Index]=-1;
    }

    // A cursor gives each reader its own view. Overflow fails closed; callers
    // must cancel active interaction before taking a fresh baseline.
    bool Read(std::uint64_t& Cursor,std::array<FEvent,Capacity>& Out,std::size_t& Count) const
    {
        Count=0;
        const auto Oldest=Next>Capacity?Next-Capacity:1;
        if (Cursor>Latest() || Cursor+1<Oldest) { Cursor=Latest(); return false; }
        for (auto Sequence=Cursor+1;Sequence<Next;++Sequence) Out[Count++]=Events[(Sequence-1)%Capacity];
        Cursor=Latest(); return true;
    }
private:
    void Append(EStream Stream,double At,bool Valid,float Value,bool Button)
    {
        Events[(Next-1)%Capacity]={Next,At,Stream,Value,Valid,Button}; ++Next;
    }
    std::array<FEvent,Capacity> Events{};
    std::uint64_t Next=1;
    std::array<double,2> LastAt{{-1,-1}};
    std::array<int,2> LastCategory{{-1,-1}};
};
}
