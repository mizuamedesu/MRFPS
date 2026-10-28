#include "../../Plugins/MoBitsSDK/Source/MoBitsSDK/Private/MoBitsCore.h"
#include "../../Plugins/MoBitsSDK/Source/MoBitsSDK/Private/MoBitsWire.h"
#include <cstdlib>
#include <iostream>

static int Checks = 0;
static void Check(bool Value, const char* Name)
{ ++Checks; if (!Value) { std::cerr << "FAIL: " << Name << '\n'; std::exit(1); } }
static std::vector<std::uint8_t> ServerFrame(std::uint8_t Opcode, const std::string& Payload, bool Fin = true)
{
    std::vector<std::uint8_t> Out{static_cast<std::uint8_t>((Fin ? 128 : 0) | Opcode)};
    if (Payload.size() < 126) Out.push_back(static_cast<std::uint8_t>(Payload.size()));
    else { Out.push_back(126); Out.push_back(static_cast<std::uint8_t>(Payload.size() >> 8)); Out.push_back(static_cast<std::uint8_t>(Payload.size())); }
    Out.insert(Out.end(), Payload.begin(), Payload.end()); return Out;
}
int main()
{
    using namespace MoBitsCore;
    for (int I = 1; I <= 30; ++I) Check(ValidPlayback(I, .15, 200), "effect registration 1..30");
    Check(!ValidPlayback(0,.15,200) && !ValidPlayback(31,.15,200), "effect bounds");
    Check(!ValidPlayback(1,0,200) && !ValidPlayback(1,1.1,200) && !ValidPlayback(1,-1,200), "level bounds");
    Check(!ValidPlayback(1,std::numeric_limits<double>::quiet_NaN(),200), "NaN level");
    Check(!ValidPlayback(1,std::numeric_limits<double>::infinity(),200), "infinite level");
    Check(ValidPlayback(30,1,500) && ValidPlayback(1,.01,1), "level and TTL edge");
    Check(std::string(EffectName(1)) == "Tap" && std::string(EffectName(2)) == "Nudge" && std::string(EffectName(3)) == "Impact", "effect names");
    Check(EffectName(0) == nullptr && EffectName(4) == nullptr && EffectName(31) == nullptr, "unnamed effect slots");
    for (int I = 4; I <= 13; ++I) Check(MrFpsEffectId(I) == I && ValidPlayback(MrFpsEffectId(I), .25, 500), "MR FPS body wire ID and startup TTL");
    for (int I : {-1, 0, 1, 3, 14, 30, 256}) Check(MrFpsEffectId(I) == 0, "MR FPS rejects other effect namespaces/IDs");
    for (int I = 0; I < 22; ++I) Check(MrFpsTarget(I) == (I == 13 || I == 17), "MR FPS only accepts forearms");
    Check(!ValidPlayback(1,.15,0) && !ValidPlayback(1,.15,501), "TTL bounds");
    int Targets = 0; for (int I = 0; I <= 21; ++I) if (TargetName(I)) ++Targets;
    Check(Targets == 11 && std::string(TargetName(14)) == "left_upper_arm", "body-target map");
    int Port = 0;
    Check(ParseEndpoint("ws://127.0.0.1:49152/mobits/v2/control","control",Port) && Port == 49152, "control URL");
    Check(ParseEndpoint("ws://127.0.0.1:65535/mobits/v2/motion","motion",Port), "motion URL");
    for (const auto& Url : {"ws://localhost:1/mobits/v2/control","ws://127.0.0.1:0/mobits/v2/control", "ws://127.0.0.1:65536/mobits/v2/control", "ws://127.0.0.1:01/mobits/v2/control", "ws://127.0.0.1:1/mobits/v2/control?token=x", "ws://127.0.0.1:1/mobits/v2/control#fragment", "ws://127.0.0.1:1/mobits/v2/motion", "wss://127.0.0.1:1/mobits/v2/control", "ws://127.0.0.1:1@evil.test/mobits/v2/control"})
        Check(!ParseEndpoint(Url,"control",Port), "invalid URL rejected");
    Check(ValidToken(std::string(64,'a')) && !ValidToken(std::string(64,'g')) && !ValidToken(std::string(63,'a')), "token validation");
    Check(ValidSessionId("12345678-abcd-1234-abcd-123456789abc") && !ValidSessionId("12345678\r\nX: secret"), "session/header validation");
    Check(Position({{1,2,-3}}) == std::array<double,3>{{-300,100,200}}, "UE positions in cm");
    Check(Position({{0,0,1}}) == std::array<double,3>{{100,0,0}}, "forward axis");
    Check(Position({{1,0,0}}) == std::array<double,3>{{0,100,0}}, "right axis");
    Check(Position({{0,1,0}}) == std::array<double,3>{{0,0,100}}, "up axis");
    Check(ValidQuaternion({{0,0,0,1}}) && !ValidQuaternion({{0,0,0,0}}), "unit quaternion");
    Check(!ValidQuaternion({{0,0,0,std::numeric_limits<double>::infinity()}}), "finite quaternion");
    const double H = std::sqrt(.5);
    auto R = Rotation({{0,H,0,H}}); Check(std::abs(R[2] - H) < 1e-10 && std::abs(R[3] - H) < 1e-10, "quaternion axes");
    Clock C; Check(!std::isfinite(C.AgeMs(0,10)), "unanchored clock is stale");
    C.Observe(100,10); Check(std::abs(C.AgeMs(100,10.6) - 600) < 1e-8, "server-age clock");
    C.Observe(110,10.6); Check(std::abs(C.AgeMs(100,10.6) - 600) < 1e-8, "delayed pong cannot rejuvenate motion");
    C.Observe(800,10.7); Check(std::abs(C.AgeMs(100,10.7) - 700) < 1e-8, "fresh anchor improves clock");
    Check(!C.Observe(-1,10) && !C.Observe(std::numeric_limits<double>::quiet_NaN(),10), "invalid clock anchor");
    OutputGate Gate; auto Epoch = Gate.Epoch(); Gate.ArmReply(Epoch,true); Check(Gate.Armed(), "explicit arm");
    Gate.PlayReply(Epoch,"effect_unconfigured"); Check(Gate.Armed(), "unconfigured is nonfatal");
    Gate.Stop(); Gate.ArmReply(Epoch,true); Check(!Gate.Armed(), "old ARM cannot re-arm after STOP");
    Gate.ArmReply(Gate.Epoch(),true); Gate.PlayReply(Epoch,"rejected"); Check(Gate.Armed(), "old rejected PLAY cannot affect new epoch");
    Gate.PlayReply(Gate.Epoch(),"expired"); Check(!Gate.Armed(), "current expired PLAY clears arm");

    using namespace MoBitsWire;
    const auto Request = UpgradeRequest(40000,true,std::string(64,'a'),"test-key","12345678-abcd-1234-abcd-123456789abc");
    Check(Request.find("Origin:") == std::string::npos && Request.find("X-MoBits-Session:") != std::string::npos, "native request has no Origin");
    Check(Request.find("Host: 127.0.0.1:40000\r\n") != std::string::npos, "exact host header");
    const std::string Response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Accept: expected\r\n\r\n";
    Check(ValidateUpgrade(Response,"expected"), "upgrade response validation");
    Check(!ValidateUpgrade(Response,"wrong"), "accept mismatch");
    Check(!ValidateUpgrade("HTTP/1.1 401 Unauthorized\r\n\r\n","expected"), "authentication rejected");
    Check(!ValidateUpgrade(Response.substr(0,Response.size()-2) + "Sec-WebSocket-Extensions: permessage-deflate\r\n\r\n","expected"), "compression rejected");
    Check(!ValidateUpgrade(Response.substr(0,Response.size()-2) + "Upgrade: websocket\r\n\r\n","expected"), "duplicate headers rejected");
    Check(Utf8("\xe6\x97\xa5\xe6\x9c\xac") && !Utf8("\xc0\x80") && !Utf8("\xed\xa0\x80") && !Utf8("\xf4\x90\x80\x80"), "strict UTF-8");
    for (std::size_t N : {0u,1u,125u,126u,2048u})
    {
        const std::string S(N,'x'); const auto Frame = Encode(1,S,0x12345678);
        Check((Frame[0] == 129) && (Frame[1] & 128), "outgoing client frame is masked text");
        std::size_t Header = N < 126 ? 2 : 4;
        std::string Decoded;
        for (std::size_t I = 0; I < N; ++I) Decoded += static_cast<char>(Frame[Header+4+I] ^ Frame[Header+I%4]);
        Check(Decoded == S, "mask round trip");
        Decoder D; std::vector<Message> M; const auto F = ServerFrame(1,S);
        for (const auto Byte : F) Check(D.Feed(&Byte,1,M), "TCP fragmented frame");
        Check(M.size() == 1 && M[0].Payload == S, "incoming text decoded once");
    }
    Decoder D; std::vector<Message> Messages;
    auto A = ServerFrame(1,"hel",false), Ping = ServerFrame(9,"p"), B = ServerFrame(0,"lo");
    Check(D.Feed(A.data(),A.size(),Messages) && Messages.empty(), "non-final text fragment");
    Check(D.Feed(Ping.data(),Ping.size(),Messages) && Messages[0].Opcode == 9, "interleaved ping");
    Check(D.Feed(B.data(),B.size(),Messages) && Messages.size() == 2 && Messages[1].Payload == "hello", "continuation reassembly");
    for (auto Bad : {std::vector<std::uint8_t>{0x81,0x80}, std::vector<std::uint8_t>{0xc1,0}, std::vector<std::uint8_t>{0x82,0}, std::vector<std::uint8_t>{0x80,0}, std::vector<std::uint8_t>{0x09,0}, std::vector<std::uint8_t>{0x81,126,0,1,0}, std::vector<std::uint8_t>{0x81,127,0,0,0,0,0,5,0,0}, std::vector<std::uint8_t>{0x88,1,0}})
    { Decoder BadDecoder; std::vector<Message> M; Check(!BadDecoder.Feed(Bad.data(),Bad.size(),M), "invalid frame rejected"); }
    // Deterministic malformed input coverage under the address/UB sanitizers.
    std::uint32_t Seed = 42;
    for (int Iteration = 0; Iteration < 2000; ++Iteration)
    {
        std::uint8_t Bytes[80]; for (auto& Byte : Bytes) { Seed = Seed*1664525u + 1013904223u; Byte = static_cast<std::uint8_t>(Seed >> 24); }
        Decoder Fuzz; std::vector<Message> M; Fuzz.Feed(Bytes,sizeof(Bytes),M);
    }
    std::cout << "MoBits core and WebSocket codec: " << Checks << " checks passed; 2000 malformed-input cases completed.\n";
}
