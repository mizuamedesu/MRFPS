#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"

// Quest-only runtime pin. IP filtering is not cryptographic peer authentication.
namespace MRFPSGloveConnection
{
struct FPin
{
    FString Sender;
    int32 Port = 25788;
    FString Device;
};

enum class ELoad { Absent, Valid, Invalid };
static constexpr const TCHAR* Section = TEXT("MRFPS.Glove");
static constexpr const TCHAR* Key = TEXT("QuestRelayPin");

inline bool ParsePort(const FString& Text, int32& Port)
{
    if (Text.IsEmpty() || Text.Len() > 5) return false;
    for (const TCHAR C : Text) if (C < '0' || C > '9') return false;
    Port = FCString::Atoi(*Text);
    return Port >= 1024 && Port <= 65535;
}

inline bool Normalize(FPin& Pin)
{
    FIPv4Address IP;
    if (!FIPv4Address::Parse(Pin.Sender, IP) || Pin.Port < 1024 || Pin.Port > 65535 ||
        (IP.A == 0 && IP.B == 0 && IP.C == 0 && IP.D == 0) ||
        (IP.A == 255 && IP.B == 255 && IP.C == 255 && IP.D == 255) ||
        (IP.A >= 224 && IP.A <= 239) || Pin.Device.Len() > 128) return false;
    for (const TCHAR C : Pin.Device)
        if (!((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') ||
              C == '_' || C == '.' || C == ':' || C == '-')) return false;
    Pin.Sender = IP.ToString();
    return true;
}

inline ELoad Load(FConfigCacheIni* Config, const FString& Filename, FPin& Pin)
{
    FString Encoded;
    if (!Config || Filename.IsEmpty() || !Config->GetString(Section, Key, Encoded, Filename)) return ELoad::Absent;
    TArray<FString> Fields;
    Encoded.ParseIntoArray(Fields, TEXT("|"), false);
    if (Fields.Num() != 4 || Fields[0] != TEXT("v1") || !ParsePort(Fields[2], Pin.Port)) return ELoad::Invalid;
    Pin.Sender = Fields[1];
    Pin.Device = Fields[3] == TEXT("*") ? FString() : Fields[3];
    return Normalize(Pin) ? ELoad::Valid : ELoad::Invalid;
}

inline void Store(FConfigCacheIni* Config, const FString& Filename, const FPin& Pin)
{
    if (!Config || Filename.IsEmpty()) return;
    const FString Encoded = FString::Printf(TEXT("v1|%s|%d|%s"), *Pin.Sender, Pin.Port,
        Pin.Device.IsEmpty() ? TEXT("*") : *Pin.Device);
    Config->SetString(Section, Key, *Encoded, Filename);
}

// Explicit sender replaces the whole tuple; a missing device does not retain
// an earlier device ID. Port/device alone cannot silently edit a saved tuple.
inline bool Select(FConfigCacheIni* Config, const FString& Filename, const TCHAR* CommandLine,
                   const FPin& Defaults, FPin& Out, bool& bSaveExplicit, bool& bInvalidSaved)
{
    FString Sender, Port, Device;
    const bool bSender = FParse::Value(CommandLine, TEXT("MRFPSGloveSender="), Sender);
    const bool bPort = FParse::Value(CommandLine, TEXT("MRFPSGlovePort="), Port);
    const bool bDevice = FParse::Value(CommandLine, TEXT("MRFPSGloveDevice="), Device);
    bSaveExplicit = bSender;
    bInvalidSaved = false;
    Out = Defaults;
    if (bSender) {
        Out.Sender = Sender;
        Out.Device = bDevice ? Device : FString();
        if (bPort && !ParsePort(Port, Out.Port)) return false;
    } else {
        if (bPort || bDevice) return false;
        FPin Saved;
        const ELoad State = Load(Config, Filename, Saved);
        if (State == ELoad::Valid) Out = Saved;
        else if (State == ELoad::Invalid) {
            bInvalidSaved = true;
            Out = { TEXT("127.0.0.1"), 25788, FString() };
        }
    }
    return Normalize(Out);
}
}