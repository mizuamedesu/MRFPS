#pragma once

#include <cmath>
#include <cstdint>

// The roster is visible only to an authenticated inspector/admin. The server
// remains authoritative; these checks also prevent stale client-side display.
namespace MRFPSRosterPolicy
{
enum class ERole : std::uint8_t { Hidden, Player, Bot, Inspector, Admin };

inline bool CanView(bool Local,bool Networked,bool Encrypted,bool Authenticated,bool Inspector,
                    bool Bot,int Slot,bool HasIdentity,std::int64_t Expires,std::int64_t Now)
{
    return Local && Networked && Encrypted && Authenticated && Inspector && !Bot &&
        Slot==-1 && HasIdentity && Expires>Now;
}

inline ERole Classify(bool Bot,bool Authenticated,bool Inspector,bool Admin,int Slot,
                      bool HasIdentity,std::int64_t Expires,std::int64_t Now)
{
    if (Bot) return !Inspector && !Admin && Slot>=0 && Slot<2 ? ERole::Bot : ERole::Hidden;
    if (!Authenticated || !HasIdentity || Expires<=Now) return ERole::Hidden;
    if (Inspector && Slot==-1) return Admin?ERole::Admin:ERole::Inspector;
    if (!Inspector && !Admin && Slot>=0 && Slot<2) return ERole::Player;
    return ERole::Hidden;
}

inline int PingMs(bool Bot,int Milliseconds,double SampleTime,double ServerTime)
{
    if (Bot || Milliseconds<0 || Milliseconds>60000 || !std::isfinite(SampleTime) ||
        !std::isfinite(ServerTime) || SampleTime<0 || ServerTime<SampleTime || ServerTime-SampleTime>5.) return -1;
    return Milliseconds;
}
}
