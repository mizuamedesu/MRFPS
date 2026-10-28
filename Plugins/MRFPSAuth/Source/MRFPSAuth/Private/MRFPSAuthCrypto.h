#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace MRFPSAuthCore
{
constexpr std::size_t MaxTokenBytes = 2048;
constexpr std::int64_t MaxLifetimeSeconds = 3600;
constexpr std::int64_t ClockSkewSeconds = 5;

struct Claims
{
    std::string Subject, Role, Issuer, Audience, TokenId, ServerFingerprint;
    std::int64_t IssuedAt = 0, NotBefore = 0, ExpiresAt = 0;
};

class PublicKey
{
public:
    PublicKey() = default;
    ~PublicKey();
    PublicKey(const PublicKey&) = delete;
    PublicKey& operator=(const PublicKey&) = delete;
    bool LoadPem(const std::string& Pem);
    bool IsReady() const { return Key != nullptr; }
    void Reset();
    bool Verify(const std::string& Message, const std::array<std::uint8_t, 64>& Signature) const;
private:
    void* Key = nullptr;
};

bool ValidAudience(const std::string& Audience);
bool ValidFingerprint(const std::string& Fingerprint);
bool WellFormed(const std::string& Token);
bool Verify(const std::string& Token, const PublicKey& Key, const std::string& Audience,
            const std::string& ExpectedRole, std::int64_t Now, Claims& Out, std::string& SafeError,
            const std::string& ExpectedFingerprint = std::string());
std::string EncodeBase64Url(const std::uint8_t* Bytes, std::size_t Size);
bool DecodePublicKeyBase64(const std::string& Encoded, std::string& Pem);
}
