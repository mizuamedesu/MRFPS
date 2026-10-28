#pragma once

// Minimal RFC 6455 text client codec. No Origin, extensions, or compression.
// The Unreal transport and the host tests use exactly this implementation.
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace MoBitsWire
{
constexpr std::size_t MaxMessageBytes = 256 * 1024;
constexpr std::size_t MaxHeaderBytes = 8192;

inline bool Utf8(const std::string& S)
{
    std::size_t I = 0;
    while (I < S.size())
    {
        const auto C = static_cast<unsigned char>(S[I++]);
        if (C < 0x80) continue;
        std::uint32_t Code;
        int Count;
        std::uint32_t Minimum;
        if (C >= 0xc2 && C <= 0xdf) { Code = C & 31; Count = 1; Minimum = 0x80; }
        else if (C >= 0xe0 && C <= 0xef) { Code = C & 15; Count = 2; Minimum = 0x800; }
        else if (C >= 0xf0 && C <= 0xf4) { Code = C & 7; Count = 3; Minimum = 0x10000; }
        else return false;
        if (I + Count > S.size()) return false;
        while (Count--)
        {
            const auto B = static_cast<unsigned char>(S[I++]);
            if ((B & 0xc0) != 0x80) return false;
            Code = (Code << 6) | (B & 63);
        }
        if (Code < Minimum || Code > 0x10ffff || (Code >= 0xd800 && Code <= 0xdfff)) return false;
    }
    return true;
}
inline std::string Lower(std::string S)
{ for (char& C : S) if (C >= 'A' && C <= 'Z') C = static_cast<char>(C + ('a' - 'A')); return S; }
inline std::string Trim(const std::string& S)
{
    const auto First = S.find_first_not_of(" \t");
    if (First == std::string::npos) return {};
    return S.substr(First, S.find_last_not_of(" \t") - First + 1);
}
inline bool ConnectionHasUpgrade(const std::string& S)
{
    std::size_t Start = 0;
    while (Start <= S.size())
    {
        const auto End = S.find(',', Start);
        if (Lower(Trim(S.substr(Start, End == std::string::npos ? End : End - Start))) == "upgrade") return true;
        if (End == std::string::npos) break;
        Start = End + 1;
    }
    return false;
}
inline bool ValidateUpgrade(const std::string& Headers, const std::string& ExpectedAccept)
{
    if (Headers.size() > MaxHeaderBytes || Headers.compare(0, 13, "HTTP/1.1 101 ") != 0) return false;
    std::vector<std::string> Seen;
    std::string Upgrade, Connection, Accept;
    auto Start = Headers.find("\r\n");
    if (Start == std::string::npos) return false;
    Start += 2;
    while (Start < Headers.size())
    {
        const auto End = Headers.find("\r\n", Start);
        if (End == std::string::npos) return false;
        if (End == Start) return End + 2 == Headers.size() && Lower(Upgrade) == "websocket" && ConnectionHasUpgrade(Connection) && Accept == ExpectedAccept;
        const auto Line = Headers.substr(Start, End - Start);
        const auto Colon = Line.find(':');
        if (Colon == std::string::npos || Colon == 0 || Line.front() == ' ' || Line.front() == '\t') return false;
        const auto Name = Lower(Line.substr(0, Colon));
        if (std::find(Seen.begin(), Seen.end(), Name) != Seen.end()) return false;
        Seen.push_back(Name);
        const auto Value = Trim(Line.substr(Colon + 1));
        if (Name == "upgrade") Upgrade = Value;
        else if (Name == "connection") Connection = Value;
        else if (Name == "sec-websocket-accept") Accept = Value;
        else if (Name == "sec-websocket-extensions" || Name == "sec-websocket-protocol") return false;
        Start = End + 2;
    }
    return false;
}
inline std::string UpgradeRequest(int Port, bool bMotion, const std::string& Token, const std::string& Key, const std::string& Session = {})
{
    std::string S = std::string("GET /mobits/v2/") + (bMotion ? "motion" : "control") + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(Port) +
        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + Key + "\r\nAuthorization: Bearer " + Token + "\r\n";
    if (bMotion) S += "X-MoBits-Session: " + Session + "\r\n";
    return S + "\r\n";
}
inline std::vector<std::uint8_t> Encode(std::uint8_t Opcode, const std::string& Payload, std::uint32_t Mask)
{
    std::vector<std::uint8_t> Out;
    Out.push_back(0x80 | Opcode);
    const std::size_t N = Payload.size();
    if (N < 126) Out.push_back(0x80 | static_cast<std::uint8_t>(N));
    else if (N <= 65535)
    { Out.push_back(0xfe); Out.push_back(static_cast<std::uint8_t>(N >> 8)); Out.push_back(static_cast<std::uint8_t>(N)); }
    else
    { Out.push_back(0xff); for (int Shift = 56; Shift >= 0; Shift -= 8) Out.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(N) >> Shift)); }
    std::uint8_t M[4];
    for (int I = 0; I < 4; ++I) { M[I] = static_cast<std::uint8_t>(Mask >> (24 - I*8)); Out.push_back(M[I]); }
    for (std::size_t I = 0; I < N; ++I) Out.push_back(static_cast<std::uint8_t>(Payload[I]) ^ M[I % 4]);
    return Out;
}
struct Message { std::uint8_t Opcode; std::string Payload; };
class Decoder
{
public:
    bool Feed(const std::uint8_t* Bytes, std::size_t Count, std::vector<Message>& Out)
    {
        if (Buffer.size() + Count > MaxMessageBytes + 14) return false;
        Buffer.insert(Buffer.end(), Bytes, Bytes + Count);
        std::size_t Used = 0;
        while (Buffer.size() - Used >= 2)
        {
            const auto A = Buffer[Used], B = Buffer[Used + 1];
            const bool Fin = (A & 0x80) != 0;
            const auto Op = static_cast<std::uint8_t>(A & 15);
            if ((A & 0x70) || (B & 0x80)) return false; // server frames must not be masked
            if (!(Op == 0 || Op == 1 || Op == 8 || Op == 9 || Op == 10)) return false;
            std::uint64_t N = B & 127;
            std::size_t Header = 2;
            if (N == 126)
            {
                if (Buffer.size() - Used < 4) break;
                N = (static_cast<std::uint64_t>(Buffer[Used + 2]) << 8) | Buffer[Used + 3]; Header = 4;
                if (N < 126) return false;
            }
            else if (N == 127)
            {
                if (Buffer.size() - Used < 10) break;
                N = 0; Header = 10;
                for (std::size_t I = 2; I < 10; ++I) N = (N << 8) | Buffer[Used + I];
                if (N < 65536) return false;
            }
            if (N > MaxMessageBytes || (Op >= 8 && (!Fin || N > 125))) return false;
            if (Buffer.size() - Used - Header < N) break;
            const auto* Data = Buffer.data() + Used + Header;
            const std::string Payload(reinterpret_cast<const char*>(Data), static_cast<std::size_t>(N));
            Used += Header + static_cast<std::size_t>(N);
            if (Out.size() >= 64) return false;
            if (Op >= 8)
            {
                if (Op == 8 && (N == 1 || (N >= 2 && !Utf8(Payload.substr(2))))) return false;
                Out.push_back({Op, Payload}); continue;
            }
            if ((Op == 1 && bFragment) || (Op == 0 && !bFragment)) return false;
            if (Op == 1 && Fin)
            { if (!Utf8(Payload)) return false; Out.push_back({1, Payload}); }
            else
            {
                if (Fragments.size() + Payload.size() > MaxMessageBytes) return false;
                Fragments += Payload;
                bFragment = true;
                if (Fin)
                { if (!Utf8(Fragments)) return false; Out.push_back({1, std::move(Fragments)}); Fragments.clear(); bFragment = false; }
            }
        }
        Buffer.erase(Buffer.begin(), Buffer.begin() + static_cast<std::ptrdiff_t>(Used));
        return true;
    }
private:
    std::vector<std::uint8_t> Buffer;
    std::string Fragments;
    bool bFragment = false;
};
}
