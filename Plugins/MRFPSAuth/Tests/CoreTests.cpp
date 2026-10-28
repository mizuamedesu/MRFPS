#include "AuthTestFixture.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{
int Checks = 0;
void Check(bool Value, const char* Name)
{
    ++Checks;
    if (!Value) { std::cerr << "FAIL: " << Name << '\n'; std::exit(1); }
}
}

int main()
{
    using namespace MRFPSAuthCore;
    using namespace MRFPSAuthTests;
    constexpr std::int64_t Now = 1800000000;
    Signer Issuer;
    Check(Issuer.Ready(), "test-only Ed25519 issuer initialized");
    PublicKey Key;
    Check(Key.LoadPem(Issuer.PublicPem()), "Ed25519 SPKI public key accepted");
    TokenSpec Spec(Now);
    Claims Out;
    std::string Error;
    const auto VerifyToken = [&](const std::string& Token, const std::string& Role = "player", const std::string& Audience = "mrfps")
    { return Verify(Token, Key, Audience, Role, Now, Out, Error, Spec.Fingerprint); };
    const std::string Valid = Issuer.Sign(Spec);
    Check(WellFormed(Valid) && VerifyToken(Valid), "actual signed JWT verifies");
    Check(Out.Subject == Spec.Subject && Out.TokenId == Spec.TokenId && Out.ServerFingerprint == Spec.Fingerprint, "verified admission claims preserved");
    Check(!VerifyToken(std::string()) && Error == "AUTH_MISSING", "missing credential rejected");
    Check(!VerifyToken(Valid, "inspector") && Error == "AUTH_ROLE", "wrong role rejected");
    Check(!VerifyToken(Valid, "player", "another-arena") && Error == "AUTH_AUDIENCE", "wrong audience rejected");
    Check(!Verify(Valid, Key, "mrfps", "player", Now, Out, Error, std::string(64, 'b')) && Error == "AUTH_TRANSPORT", "wrong DTLS server binding rejected");

    Signer OtherIssuer;
    Check(!VerifyToken(OtherIssuer.Sign(Spec)) && Error == "AUTH_INVALID", "another private key cannot forge admission");
    const auto LastDot = Valid.rfind('.');
    TokenSpec Tampered = Spec;
    Tampered.Subject = "attacker";
    Check(!VerifyToken(Encode(Header()) + "." + Encode(Tampered.Json()) + Valid.substr(LastDot)), "payload edits invalidate the signature");

    for (const auto& HeaderJson : {std::string("{\"alg\":\"none\",\"typ\":\"JWT\"}"),
                                  std::string("{\"alg\":\"HS256\",\"typ\":\"JWT\"}"),
                                  std::string("{\"alg\":\"RS256\",\"typ\":\"JWT\"}"),
                                  std::string("{\"alg\":\"EdDSA\",\"typ\":\"JWS\"}"),
                                  std::string("{\"alg\":\"EdDSA\",\"typ\":\"JWT\",\"kid\":\"other\"}"),
                                  std::string("{\"alg\":\"EdDSA\",\"alg\":\"EdDSA\",\"typ\":\"JWT\"}"),
                                  std::string("{\"\\u0061lg\":\"EdDSA\",\"typ\":\"JWT\"}")})
        Check(!VerifyToken(Issuer.SignJson(HeaderJson, Spec.Json())), "algorithm/type/extensions/duplicate/escaped headers refused");

    const std::string Json = Spec.Json();
    const std::string Iat = "\"iat\":" + std::to_string(Spec.IssuedAt);
    for (const auto& InvalidIat : {std::string("\"iat\":1800000000.0"), std::string("\"iat\":18e8"),
                                  std::string("\"iat\":01800000000"), std::string("\"iat\":\"1800000000\""),
                                  std::string("\"iat\":-1"), std::string("\"iat\":9223372036854775808"),
                                  std::string("\"iat\":true"), std::string("\"iat\":null"), std::string("\"iat\":[]")})
        Check(!VerifyToken(Issuer.SignJson(Header(), Replace(Json, Iat, InvalidIat))), "NumericDate must be a bounded canonical integer");
    Check(!VerifyToken(Issuer.SignJson(Header(), Replace(Json, "\"sub\":\"test-player\"", "\"sub\":\"first\",\"sub\":\"second\""))), "duplicate payload key refused before DOM replacement");
    Check(!VerifyToken(Issuer.SignJson(Header(), Replace(Json, "\"sub\":\"test-player\"", "\"sub\":\"test\\u002dplayer\""))), "escaped profile strings refused");
    Check(!VerifyToken(Issuer.SignJson(Header(), Json + "{}")), "trailing JSON refused");
    Check(!VerifyToken(Issuer.SignJson(Header(), Replace(Json, "\"dtls_fp\":", "\"extra\":"))), "missing required DTLS claim refused");

    TokenSpec Case = Spec;
    Case.ExpiresAt = Now - 6; Case.IssuedAt = Case.NotBefore = Now - 60;
    Check(!VerifyToken(Issuer.Sign(Case)) && Error == "AUTH_EXPIRED", "expired token refused");
    Case.ExpiresAt = Now - 4;
    Check(VerifyToken(Issuer.Sign(Case)), "core verification uses at most five seconds leeway");
    Case = Spec; Case.IssuedAt = Case.NotBefore = Now + 6;
    Check(!VerifyToken(Issuer.Sign(Case)), "future issue/not-before refused");
    Case = Spec; Case.NotBefore = Now + 5;
    Check(VerifyToken(Issuer.Sign(Case)), "five-second not-before leeway accepted");
    Case = Spec; Case.ExpiresAt = Case.IssuedAt + 3601;
    Check(!VerifyToken(Issuer.Sign(Case)), "excessive lifetime refused");
    Case = Spec; Case.ExpiresAt = Case.IssuedAt;
    Check(!VerifyToken(Issuer.Sign(Case)), "empty lifetime refused");
    Case = Spec; Case.NotBefore = Case.IssuedAt - 1;
    Check(!VerifyToken(Issuer.Sign(Case)), "not-before preceding issue refused");
    Case = Spec; Case.Subject = "../unsafe/id";
    Check(!VerifyToken(Issuer.Sign(Case)), "unsafe subject refused");
    Case = Spec; Case.TokenId = "12345678-abcd-1123-8abc-123456789abc";
    Check(!VerifyToken(Issuer.Sign(Case)), "non-v4 token ID refused");
    Case = Spec; Case.TokenId = "12345678-ABCD-4123-8abc-123456789abc";
    Check(!VerifyToken(Issuer.Sign(Case)), "noncanonical token ID refused");
    Case = Spec; Case.Fingerprint[0] = 'A';
    Check(!VerifyToken(Issuer.Sign(Case)), "noncanonical DTLS fingerprint refused");

    for (const auto& Bad : {Valid + ".tail", Valid + "=", " " + Valid, Valid + "\n", std::string(".."),
                            std::string(2049, 'a'), Valid.substr(0, Valid.size() - 1), Valid + "a"})
        Check(!VerifyToken(Bad), "malformed/bounded compact serialization refused");
    std::string Noncanonical = Valid;
    const std::string Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    Noncanonical.back() = Alphabet[Alphabet.find(Noncanonical.back()) + 1];
    Check(!VerifyToken(Noncanonical), "nonzero base64url padding bits refused");
    std::string Nul = Valid; Nul.push_back('\0');
    Check(!VerifyToken(Nul), "embedded NUL tail cannot disappear during conversion");
    Check(!VerifyToken(Issuer.SignJson(std::string(257, ' '), Json)), "oversized decoded header refused");
    Check(!VerifyToken(Issuer.SignJson(Header(), Json + std::string(1025, ' '))), "oversized decoded payload refused");
    Check(!Verify(Valid, Key, "mrfps", "player", std::numeric_limits<std::int64_t>::max(), Out, Error), "clock arithmetic cannot overflow");

    PublicKey InvalidKey;
    Check(!InvalidKey.LoadPem(Issuer.PrivatePem()), "a private key is never accepted as server verification configuration");
    Check(!InvalidKey.LoadPem(Issuer.PublicPem() + Issuer.PublicPem()), "multiple public keys refused");
    Check(!InvalidKey.LoadPem(Issuer.PublicPem() + "junk"), "trailing key data refused");
    Signer Ed448(EVP_PKEY_ED448);
    Check(Ed448.Ready() && !InvalidKey.LoadPem(Ed448.PublicPem()), "other public-key algorithm refused");
    Check(!Verify(Valid, InvalidKey, "mrfps", "player", Now, Out, Error) && Error == "AUTH_CONFIG", "unconfigured verification fails closed");

    const std::string Pem = Issuer.PublicPem();
    std::vector<unsigned char> Standard(4 * ((Pem.size() + 2) / 3) + 1);
    const int Length = EVP_EncodeBlock(Standard.data(), reinterpret_cast<const unsigned char*>(Pem.data()), static_cast<int>(Pem.size()));
    std::string Decoded;
    Check(Length > 0 && DecodePublicKeyBase64(std::string(reinterpret_cast<const char*>(Standard.data()), static_cast<std::size_t>(Length)), Decoded) && Decoded == Pem, "environment base64 public PEM decodes exactly");
    Check(!DecodePublicKeyBase64("not a public key", Decoded) && !DecodePublicKeyBase64("A===", Decoded), "malformed environment base64 refused");

    std::uint32_t Seed = 42;
    for (int Index = 0; Index < 3000; ++Index)
    {
        std::string Bytes;
        for (int Byte = 0; Byte < Index % 511; ++Byte)
        { Seed = Seed * 1664525u + 1013904223u; Bytes.push_back(static_cast<char>(Seed >> 24)); }
        Check(!WellFormed(Bytes), "deterministic malformed token input refused");
    }
    std::cout << "MRFPS auth: " << Checks << " checks passed; actual Ed25519/OpenSSL verification and 3000 malformed inputs.\n";
}
