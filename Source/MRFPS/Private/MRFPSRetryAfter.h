#pragma once

#include <cstddef>

namespace MRFPSRetryAfter
{
// HTTP delta-seconds only. Zero means invalid / use the generic message.
// Accept bounded ASCII digits and HTTP optional whitespace, not dates,
// signs, decimal values, control characters, Unicode digits or overflow.
template <typename Character>
inline int Parse(const Character* Text, std::size_t Length)
{
    if (!Text || Length==0 || Length>128) return 0;
    std::size_t I=0;
    const auto IsOws=[](Character C) { return C==' ' || C=='\t'; };
    while (I<Length && IsOws(Text[I])) ++I;
    bool HasDigit=false;
    unsigned int Seconds=0;
    while (I<Length && Text[I]>='0' && Text[I]<='9') {
        HasDigit=true;
        const unsigned int Digit=static_cast<unsigned int>(Text[I]-'0');
        if (Seconds>6 || (Seconds==6 && Digit>0)) return 0;
        Seconds=Seconds*10+Digit;
        ++I;
    }
    while (I<Length && IsOws(Text[I])) ++I;
    return HasDigit && I==Length && Seconds>=1 && Seconds<=60 ? static_cast<int>(Seconds) : 0;
}
}
