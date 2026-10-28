#include "MRFPSAuthCrypto.h"

#include <algorithm>
#include <limits>
#include <map>
#include <utility>

#if defined(THIRD_PARTY_INCLUDES_START)
THIRD_PARTY_INCLUDES_START
#endif
// Matches UE5.8 DTLSHandlerComponent/BuildPatchServices: OpenSSL's UI typedef
// otherwise collides with the UE::UI namespace alias from the shared PCH.
#define UI UI_ST
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#undef UI
#if defined(THIRD_PARTY_INCLUDES_END)
THIRD_PARTY_INCLUDES_END
#endif

namespace MRFPSAuthCore
{
namespace
{
bool Alnum(char C)
{
    return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9');
}

bool Identifier(const std::string& Value, std::size_t Max, bool bAudience)
{
    if (Value.empty() || Value.size() > Max || !Alnum(Value[0])) return false;
    for (char C : Value)
        if (!Alnum(C) && C != '.' && C != '_' && C != '-' && !(bAudience && (C == ':' || C == '/'))) return false;
    return true;
}

bool Uuid(const std::string& Value)
{
    if (Value.size() != 36 || Value[14] != '4' ||
        (Value[19] != '8' && Value[19] != '9' && Value[19] != 'a' && Value[19] != 'b')) return false;
    for (std::size_t Index = 0; Index < Value.size(); ++Index)
    {
        if (Index == 8 || Index == 13 || Index == 18 || Index == 23)
        { if (Value[Index] != '-') return false; }
        else if (!((Value[Index] >= '0' && Value[Index] <= '9') || (Value[Index] >= 'a' && Value[Index] <= 'f'))) return false;
    }
    return true;
}

int Base64Digit(char C, bool bUrl)
{
    if (C >= 'A' && C <= 'Z') return C - 'A';
    if (C >= 'a' && C <= 'z') return C - 'a' + 26;
    if (C >= '0' && C <= '9') return C - '0' + 52;
    if (C == (bUrl ? '-' : '+')) return 62;
    if (C == (bUrl ? '_' : '/')) return 63;
    return -1;
}

bool Decode(const std::string& Value, std::size_t MaxBytes, bool bUrl, std::vector<std::uint8_t>& Bytes)
{
    Bytes.clear();
    if (Value.empty() || Value.size() % 4 == 1 || Value.size() > ((MaxBytes + 2) / 3) * 4) return false;
    std::uint32_t Accumulator = 0;
    int Bits = 0;
    for (char C : Value)
    {
        const int Digit = Base64Digit(C, bUrl);
        if (Digit < 0) return false;
        Accumulator = (Accumulator << 6) | static_cast<std::uint32_t>(Digit);
        Bits += 6;
        if (Bits >= 8)
        {
            Bits -= 8;
            if (Bytes.size() >= MaxBytes) return false;
            Bytes.push_back(static_cast<std::uint8_t>((Accumulator >> Bits) & 255));
        }
    }
    return Bits == 0 || (Accumulator & ((1u << Bits) - 1u)) == 0;
}

struct Value
{
    bool bString = false;
    std::string Text;
    std::int64_t Number = 0;
};

// Deliberately narrow JWT profile: flat objects, safe ASCII strings without
// escapes, canonical nonnegative integer NumericDates. No permissive JSON DOM
// can silently replace a duplicate key or coerce a numeric/string value.
class ObjectParser
{
public:
    explicit ObjectParser(const std::vector<std::uint8_t>& InBytes) : Bytes(InBytes) {}
    bool Parse(std::map<std::string, Value>& Fields)
    {
        Fields.clear();
        Space();
        if (!Take('{')) return false;
        Space();
        if (Take('}')) return false;
        for (int Count = 0; Count < 16; ++Count)
        {
            std::string Name;
            if (!String(Name) || Name.empty() || Name.size() > 16) return false;
            Space();
            if (!Take(':')) return false;
            Space();
            Value Item;
            if (Position < Bytes.size() && Bytes[Position] == '"')
            { Item.bString = true; if (!String(Item.Text)) return false; }
            else if (!Integer(Item.Number)) return false;
            if (!Fields.emplace(std::move(Name), std::move(Item)).second) return false;
            Space();
            if (Take('}')) { Space(); return Position == Bytes.size(); }
            if (!Take(',')) return false;
            Space();
        }
        return false;
    }
private:
    bool Take(char C)
    {
        if (Position >= Bytes.size() || Bytes[Position] != static_cast<std::uint8_t>(C)) return false;
        ++Position;
        return true;
    }
    void Space()
    {
        while (Position < Bytes.size() && (Bytes[Position] == ' ' || Bytes[Position] == '\t' ||
               Bytes[Position] == '\r' || Bytes[Position] == '\n')) ++Position;
    }
    bool String(std::string& Out)
    {
        if (!Take('"')) return false;
        Out.clear();
        while (Position < Bytes.size())
        {
            const auto C = Bytes[Position++];
            if (C == '"') return true;
            if (C < 32 || C > 126 || C == '\\' || Out.size() >= 128) return false;
            Out.push_back(static_cast<char>(C));
        }
        return false;
    }
    bool Integer(std::int64_t& Out)
    {
        if (Position >= Bytes.size() || Bytes[Position] < '0' || Bytes[Position] > '9') return false;
        const bool bZero = Bytes[Position] == '0';
        Out = 0;
        std::size_t Digits = 0;
        while (Position < Bytes.size() && Bytes[Position] >= '0' && Bytes[Position] <= '9')
        {
            const int Digit = Bytes[Position++] - '0';
            if ((bZero && Digits > 0) || ++Digits > 19 ||
                Out > (std::numeric_limits<std::int64_t>::max() - Digit) / 10) return false;
            Out = Out * 10 + Digit;
        }
        return true;
    }
    const std::vector<std::uint8_t>& Bytes;
    std::size_t Position = 0;
};

bool FieldString(const std::map<std::string, Value>& Fields, const char* Name, std::string& Out)
{
    const auto Found = Fields.find(Name);
    if (Found == Fields.end() || !Found->second.bString) return false;
    Out = Found->second.Text;
    return true;
}

bool FieldNumber(const std::map<std::string, Value>& Fields, const char* Name, std::int64_t& Out)
{
    const auto Found = Fields.find(Name);
    if (Found == Fields.end() || Found->second.bString) return false;
    Out = Found->second.Number;
    return true;
}

struct Parsed
{
    Claims Data;
    std::string SigningInput;
    std::array<std::uint8_t, 64> Signature{};
};

bool ParseToken(const std::string& Token, Parsed& Out)
{
    if (Token.empty() || Token.size() > MaxTokenBytes) return false;
    const auto First = Token.find('.');
    const auto Second = First == std::string::npos ? std::string::npos : Token.find('.', First + 1);
    if (First == std::string::npos || Second == std::string::npos || Token.find('.', Second + 1) != std::string::npos) return false;
    std::vector<std::uint8_t> Header, Payload, Signature;
    if (!Decode(Token.substr(0, First), 256, true, Header) ||
        !Decode(Token.substr(First + 1, Second - First - 1), 1024, true, Payload) ||
        !Decode(Token.substr(Second + 1), 64, true, Signature) || Signature.size() != 64) return false;
    std::map<std::string, Value> HeaderFields, PayloadFields;
    if (!ObjectParser(Header).Parse(HeaderFields) || !ObjectParser(Payload).Parse(PayloadFields) ||
        HeaderFields.size() != 2 || PayloadFields.size() != 9) return false;
    std::string Algorithm, Type;
    if (!FieldString(HeaderFields, "alg", Algorithm) || Algorithm != "EdDSA" ||
        !FieldString(HeaderFields, "typ", Type) || Type != "JWT") return false;
    Claims Data;
    if (!FieldString(PayloadFields, "iss", Data.Issuer) || Data.Issuer != "mrfps-issuer" ||
        !FieldString(PayloadFields, "aud", Data.Audience) || !ValidAudience(Data.Audience) ||
        !FieldString(PayloadFields, "sub", Data.Subject) || !Identifier(Data.Subject, 64, false) ||
        !FieldString(PayloadFields, "role", Data.Role) || (Data.Role != "player" && Data.Role != "inspector" && Data.Role != "admin") ||
        !FieldString(PayloadFields, "jti", Data.TokenId) || !Uuid(Data.TokenId) ||
        !FieldString(PayloadFields, "dtls_fp", Data.ServerFingerprint) || !ValidFingerprint(Data.ServerFingerprint) ||
        !FieldNumber(PayloadFields, "iat", Data.IssuedAt) || !FieldNumber(PayloadFields, "nbf", Data.NotBefore) ||
        !FieldNumber(PayloadFields, "exp", Data.ExpiresAt)) return false;
    if (Data.ExpiresAt <= Data.IssuedAt || Data.ExpiresAt - Data.IssuedAt > MaxLifetimeSeconds ||
        Data.NotBefore < Data.IssuedAt || Data.NotBefore >= Data.ExpiresAt) return false;
    Out.Data = std::move(Data);
    Out.SigningInput = Token.substr(0, Second);
    std::copy(Signature.begin(), Signature.end(), Out.Signature.begin());
    return true;
}
}

bool ValidAudience(const std::string& Audience) { return Identifier(Audience, 128, true); }
bool ValidFingerprint(const std::string& Fingerprint)
{
    if (Fingerprint.size() != 64) return false;
    for (char C : Fingerprint) if (!((C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'))) return false;
    return true;
}

std::string EncodeBase64Url(const std::uint8_t* Bytes, std::size_t Size)
{
    static const char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string Out;
    std::uint32_t Buffer = 0;
    int Bits = 0;
    for (std::size_t Index = 0; Index < Size; ++Index)
    {
        Buffer = (Buffer << 8) | Bytes[Index];
        Bits += 8;
        while (Bits >= 6) { Bits -= 6; Out.push_back(Alphabet[(Buffer >> Bits) & 63]); }
    }
    if (Bits) Out.push_back(Alphabet[(Buffer << (6 - Bits)) & 63]);
    return Out;
}

bool DecodePublicKeyBase64(const std::string& Encoded, std::string& Pem)
{
    Pem.clear();
    if (Encoded.empty() || Encoded.size() > 2048 || Encoded.size() % 4 != 0) return false;
    std::size_t Padding = 0;
    if (Encoded.back() == '=') ++Padding;
    if (Encoded.size() >= 2 && Encoded[Encoded.size() - 2] == '=') ++Padding;
    const std::string Body = Encoded.substr(0, Encoded.size() - Padding);
    if ((Padding == 1 && Body.size() % 4 != 3) || (Padding == 2 && Body.size() % 4 != 2)) return false;
    std::vector<std::uint8_t> Bytes;
    if (!Decode(Body, 1024, false, Bytes)) return false;
    Pem.assign(Bytes.begin(), Bytes.end());
    return true;
}

PublicKey::~PublicKey() { Reset(); }
void PublicKey::Reset()
{
    if (Key) EVP_PKEY_free(static_cast<EVP_PKEY*>(Key));
    Key = nullptr;
}

bool PublicKey::LoadPem(const std::string& Pem)
{
    Reset();
    if (Pem.empty() || Pem.size() > 1024 || OPENSSL_init_crypto(0, nullptr) != 1) return false;
    for (unsigned char C : Pem) if (C == 0 || C > 127) return false;
    const auto Start = Pem.find_first_not_of(" \t\r\n");
    if (Start == std::string::npos || Pem.compare(Start, 26, "-----BEGIN PUBLIC KEY-----") != 0) return false;
    BIO* Bio = BIO_new_mem_buf(Pem.data(), static_cast<int>(Pem.size()));
    if (!Bio) return false;
    EVP_PKEY* ParsedKey = PEM_read_bio_PUBKEY(Bio, nullptr, nullptr, nullptr);
    bool bTrailingClean = true;
    char Byte;
    while (BIO_read(Bio, &Byte, 1) == 1)
        if (Byte != ' ' && Byte != '\t' && Byte != '\r' && Byte != '\n') bTrailingClean = false;
    BIO_free(Bio);
    std::size_t Length = 0;
    if (!ParsedKey || !bTrailingClean || EVP_PKEY_id(ParsedKey) != EVP_PKEY_ED25519 ||
        EVP_PKEY_get_raw_public_key(ParsedKey, nullptr, &Length) != 1 || Length != 32)
    {
        if (ParsedKey) EVP_PKEY_free(ParsedKey);
        return false;
    }
    Key = ParsedKey;
    return true;
}

bool PublicKey::Verify(const std::string& Message, const std::array<std::uint8_t, 64>& Signature) const
{
    if (!Key) return false;
    EVP_MD_CTX* Context = EVP_MD_CTX_new();
    if (!Context) return false;
    const bool Result = EVP_DigestVerifyInit(Context, nullptr, nullptr, nullptr, static_cast<EVP_PKEY*>(Key)) == 1 &&
        EVP_DigestVerify(Context, Signature.data(), Signature.size(),
            reinterpret_cast<const unsigned char*>(Message.data()), Message.size()) == 1;
    EVP_MD_CTX_free(Context);
    return Result;
}

bool WellFormed(const std::string& Token) { Parsed Data; return ParseToken(Token, Data); }

bool Verify(const std::string& Token, const PublicKey& Key, const std::string& Audience,
            const std::string& ExpectedRole, std::int64_t Now, Claims& Out, std::string& SafeError,
            const std::string& ExpectedFingerprint)
{
    Out = Claims();
    SafeError = "AUTH_INVALID";
    if (Token.empty()) { SafeError = "AUTH_MISSING"; return false; }
    if (!Key.IsReady() || !ValidAudience(Audience)) { SafeError = "AUTH_CONFIG"; return false; }
    Parsed Data;
    if (!ParseToken(Token, Data) || !Key.Verify(Data.SigningInput, Data.Signature)) return false;
    if (Data.Data.Audience != Audience) { SafeError = "AUTH_AUDIENCE"; return false; }
    if (ExpectedRole != Data.Data.Role) { SafeError = "AUTH_ROLE"; return false; }
    if (!ExpectedFingerprint.empty() && Data.Data.ServerFingerprint != ExpectedFingerprint) { SafeError = "AUTH_TRANSPORT"; return false; }
    if (Now < ClockSkewSeconds || Now > std::numeric_limits<std::int64_t>::max() - ClockSkewSeconds ||
        Data.Data.IssuedAt > Now + ClockSkewSeconds || Data.Data.NotBefore > Now + ClockSkewSeconds) return false;
    if (Data.Data.ExpiresAt <= Now - ClockSkewSeconds) { SafeError = "AUTH_EXPIRED"; return false; }
    Out = std::move(Data.Data);
    SafeError.clear();
    return true;
}
}
