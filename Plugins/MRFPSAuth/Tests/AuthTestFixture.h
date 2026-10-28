#pragma once

#include "../Source/MRFPSAuth/Private/MRFPSAuthCrypto.h"
#include <string>
#include <vector>
#if defined(THIRD_PARTY_INCLUDES_START)
THIRD_PARTY_INCLUDES_START
#endif
#define UI UI_ST
#include <openssl/evp.h>
#include <openssl/pem.h>
#undef UI
#if defined(THIRD_PARTY_INCLUDES_END)
THIRD_PARTY_INCLUDES_END
#endif

namespace MRFPSAuthTests
{
inline const std::string& Header()
{
    static const std::string Value = "{\"alg\":\"EdDSA\",\"typ\":\"JWT\"}";
    return Value;
}

inline std::string Encode(const std::string& Value)
{
    return MRFPSAuthCore::EncodeBase64Url(reinterpret_cast<const std::uint8_t*>(Value.data()), Value.size());
}

inline std::string Replace(std::string Value, const std::string& From, const std::string& To)
{
    const auto Position = Value.find(From);
    if (Position != std::string::npos) Value.replace(Position, From.size(), To);
    return Value;
}

struct TokenSpec
{
    explicit TokenSpec(std::int64_t Now) : IssuedAt(Now), NotBefore(Now), ExpiresAt(Now + 600) {}
    std::string Subject = "test-player";
    std::string Audience = "mrfps";
    std::string Role = "player";
    std::string Issuer = "mrfps-issuer";
    std::string TokenId = "12345678-abcd-4123-8abc-123456789abc";
    std::string Fingerprint = std::string(64, 'a');
    std::int64_t IssuedAt, NotBefore, ExpiresAt;
    std::string Json() const
    {
        return "{\"iss\":\"" + Issuer + "\",\"aud\":\"" + Audience + "\",\"sub\":\"" + Subject +
            "\",\"role\":\"" + Role + "\",\"iat\":" + std::to_string(IssuedAt) +
            ",\"nbf\":" + std::to_string(NotBefore) + ",\"exp\":" + std::to_string(ExpiresAt) +
            ",\"jti\":\"" + TokenId + "\",\"dtls_fp\":\"" + Fingerprint + "\"}";
    }
};

// Ephemeral keys exist only in test memory. Production code never generates a
// key, signs a token, writes a private key, or falls back to these fixtures.
class Signer
{
public:
    explicit Signer(int Type = EVP_PKEY_ED25519)
    {
        EVP_PKEY_CTX* Context = EVP_PKEY_CTX_new_id(Type, nullptr);
        if (!Context) return;
        if (EVP_PKEY_keygen_init(Context) == 1) EVP_PKEY_keygen(Context, &Key);
        EVP_PKEY_CTX_free(Context);
    }
    ~Signer() { if (Key) EVP_PKEY_free(Key); }
    Signer(const Signer&) = delete;
    Signer& operator=(const Signer&) = delete;
    bool Ready() const { return Key != nullptr; }
    std::string PublicPem() const { return Pem(false); }
    std::string PrivatePem() const { return Pem(true); }
    std::string Sign(const TokenSpec& Spec) const { return SignJson(Header(), Spec.Json()); }
    std::string SignJson(const std::string& JsonHeader, const std::string& JsonPayload) const
    {
        if (!Key) return std::string();
        const std::string Message = Encode(JsonHeader) + "." + Encode(JsonPayload);
        EVP_MD_CTX* Context = EVP_MD_CTX_new();
        if (!Context) return std::string();
        std::array<std::uint8_t, 64> Signature{};
        std::size_t Length = Signature.size();
        const bool Success = EVP_DigestSignInit(Context, nullptr, nullptr, nullptr, Key) == 1 &&
            EVP_DigestSign(Context, Signature.data(), &Length,
                reinterpret_cast<const unsigned char*>(Message.data()), Message.size()) == 1 && Length == Signature.size();
        EVP_MD_CTX_free(Context);
        return Success ? Message + "." + MRFPSAuthCore::EncodeBase64Url(Signature.data(), Signature.size()) : std::string();
    }
private:
    std::string Pem(bool bPrivate) const
    {
        if (!Key) return std::string();
        BIO* Output = BIO_new(BIO_s_mem());
        if (!Output) return std::string();
        const int Result = bPrivate ? PEM_write_bio_PrivateKey(Output, Key, nullptr, nullptr, 0, nullptr, nullptr) : PEM_write_bio_PUBKEY(Output, Key);
        char* Bytes = nullptr;
        const long Size = BIO_get_mem_data(Output, &Bytes);
        const std::string Value = Result == 1 && Size > 0 ? std::string(Bytes, static_cast<std::size_t>(Size)) : std::string();
        BIO_free(Output);
        return Value;
    }
    EVP_PKEY* Key = nullptr;
};
}
