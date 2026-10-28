#pragma once

// Engine-independent rules, also used by the host-side tests.
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace MoBitsCore
{
constexpr int EffectMin = 1;
constexpr int EffectMax = 30;
// MR FPS uses the unchanged body PLAY contract; global IDs remain independent.
inline int MrFpsEffectId(int Preset) { return Preset >= 4 && Preset <= 13 ? Preset : 0; }
inline bool MrFpsTarget(int Target) { return Target == 13 || Target == 17; }
constexpr double TimeoutSeconds = 0.5;
constexpr double HeartbeatSeconds = 0.1;
constexpr int MaxPending = 16;

inline bool ValidEffect(int Effect) { return Effect >= EffectMin && Effect <= EffectMax; }
// Body effects 1..3 carry tactile names; 4..30 stay numbered until specified.
// Global effects have their own namespace and are never named through this table.
constexpr int NamedEffectMax = 3;
inline bool NamedEffect(int Effect) { return Effect >= EffectMin && Effect <= NamedEffectMax; }
inline const char* EffectName(int Effect)
{
    static constexpr const char* Names[] = {"Tap", "Nudge", "Impact"};
    return NamedEffect(Effect) ? Names[Effect - 1] : nullptr;
}
inline bool ValidPlayback(int Effect, double Level, int TtlMs)
{
    return ValidEffect(Effect) && std::isfinite(Level) && Level > 0 && Level <= 1 && TtlMs >= 1 && TtlMs <= 500;
}
inline const char* TargetName(int BoneId)
{
    switch (BoneId)
    {
    case 4: return "chest";
    case 5: return "waist";
    case 6: return "hip";
    case 14: return "left_upper_arm";
    case 18: return "right_upper_arm";
    case 13: return "left_forearm";
    case 17: return "right_forearm";
    case 7: return "left_thigh";
    case 10: return "right_thigh";
    case 8: return "left_shin";
    case 11: return "right_shin";
    default: return nullptr;
    }
}
inline bool ValidToken(const std::string& Token)
{
    if (Token.size() != 64) return false;
    for (char C : Token)
        if (!((C >= '0' && C <= '9') || (C >= 'a' && C <= 'f') || (C >= 'A' && C <= 'F'))) return false;
    return true;
}
inline bool ParseEndpoint(const std::string& Url, const char* Channel, int& Port)
{
    const std::string Prefix = "ws://127.0.0.1:";
    const std::string Suffix = std::string("/mobits/v2/") + Channel;
    if (Url.compare(0, Prefix.size(), Prefix) != 0 || Url.size() <= Prefix.size() + Suffix.size()) return false;
    const auto End = Url.size() - Suffix.size();
    if (Url.compare(End, Suffix.size(), Suffix) != 0 || End - Prefix.size() > 5) return false;
    int Value = 0;
    for (auto I = Prefix.size(); I < End; ++I)
    {
        if (Url[I] < '0' || Url[I] > '9') return false;
        Value = Value * 10 + (Url[I] - '0');
    }
    if (Value < 1 || Value > 65535 || Url[Prefix.size()] == '0') return false;
    Port = Value;
    return true;
}
inline bool ValidSessionId(const std::string& Session)
{
    if (Session.size() != 36) return false;
    for (std::size_t I = 0; I < Session.size(); ++I)
    {
        if (I == 8 || I == 13 || I == 18 || I == 23) { if (Session[I] != '-') return false; }
        else if (!((Session[I] >= '0' && Session[I] <= '9') || (Session[I] >= 'a' && Session[I] <= 'f') || (Session[I] >= 'A' && Session[I] <= 'F'))) return false;
    }
    return true;
}
inline std::array<double, 3> Position(const std::array<double, 3>& P) { return {{P[2] * 100, P[0] * 100, P[1] * 100}}; }
inline std::array<double, 4> Rotation(const std::array<double, 4>& Q)
{
    const double N = std::sqrt(Q[0]*Q[0] + Q[1]*Q[1] + Q[2]*Q[2] + Q[3]*Q[3]);
    return {{Q[2] / N, Q[0] / N, Q[1] / N, Q[3] / N}};
}
inline bool ValidQuaternion(const std::array<double, 4>& Q)
{
    double Norm = 0;
    for (double V : Q) { if (!std::isfinite(V)) return false; Norm += V*V; }
    return std::abs(std::sqrt(Norm) - 1) <= 0.05;
}

class Clock
{
public:
    void Reset() { bHasAnchor = false; ServerAtAnchor = LocalAtAnchor = 0; }
    bool Observe(double ServerMs, double LocalSeconds)
    {
        if (!std::isfinite(ServerMs) || ServerMs < 0 || !std::isfinite(LocalSeconds)) return false;
        // Keep the smallest observed transit offset. A delayed PONG must not
        // move the clock backwards and make a buffered frame appear fresh.
        if (!bHasAnchor || ServerMs - LocalSeconds*1000 > ServerAtAnchor - LocalAtAnchor*1000)
        { ServerAtAnchor = ServerMs; LocalAtAnchor = LocalSeconds; bHasAnchor = true; }
        return true;
    }
    double AgeMs(double FrameServerMs, double NowSeconds) const
    {
        if (!bHasAnchor) return std::numeric_limits<double>::infinity();
        return std::fmax(0.0, ServerAtAnchor + (NowSeconds - LocalAtAnchor)*1000 - FrameServerMs);
    }
private:
    bool bHasAnchor = false;
    double ServerAtAnchor = 0;
    double LocalAtAnchor = 0;
};

class OutputGate
{
public:
    std::uint64_t Epoch() const { return CurrentEpoch; }
    bool Armed() const { return bArmed; }
    void Stop() { ++CurrentEpoch; bArmed = false; }
    void ArmReply(std::uint64_t RequestEpoch, bool bSucceeded)
    { if (RequestEpoch == CurrentEpoch) bArmed = bSucceeded; }
    void PlayReply(std::uint64_t RequestEpoch, const std::string& Status)
    { if (RequestEpoch == CurrentEpoch && Status != "device_acknowledged" && Status != "effect_unconfigured") bArmed = false; }
private:
    std::uint64_t CurrentEpoch = 1;
    bool bArmed = false;
};
}
