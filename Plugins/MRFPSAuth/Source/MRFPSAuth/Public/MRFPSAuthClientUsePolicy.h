#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace MRFPSAuthClientPolicy
{
enum class EUse { Fresh, AlreadyAttempted, Capacity, Invalid };
class FConnectionUses
{
public:
    explicit FConnectionUses(std::size_t Limit=128) : Capacity(Limit) {}
    bool WasAttempted(const std::string& Id) const { return Uses.find(Id)!=Uses.end(); }
    EUse Check(const std::string& Id,std::int64_t Expires,std::int64_t Now)
    {
        Prune(Now);
        if (Now<0 || Id.size()!=36 || Expires<=Now) return EUse::Invalid;
        if (Uses.find(Id)!=Uses.end()) return EUse::AlreadyAttempted;
        return Uses.size()>=Capacity ? EUse::Capacity : EUse::Fresh;
    }
    EUse Begin(const std::string& Id,std::int64_t Expires,std::int64_t Now)
    {
        const EUse Result=Check(Id,Expires,Now);
        if (Result==EUse::Fresh) Uses.emplace(Id,Expires);
        return Result;
    }
private:
    void Prune(std::int64_t Now)
    {
        if (Now<0) return;
        for (auto It=Uses.begin();It!=Uses.end();)
            if (It->second<Now && Now-It->second>5) It=Uses.erase(It); else ++It;
    }
    std::size_t Capacity;
    std::map<std::string,std::int64_t> Uses;
};
}
