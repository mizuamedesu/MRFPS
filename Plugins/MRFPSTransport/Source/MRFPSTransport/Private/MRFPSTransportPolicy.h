#pragma once

#include "CoreMinimal.h"
#include "Serialization/BitReader.h"

namespace MRFPSTransportPolicy
{
inline bool ParseFingerprint(const FString& Hex, TArray<uint8>& Bytes)
{
    Bytes.Reset();
    if (Hex.Len() != 64) return false;
    const auto Nibble = [](TCHAR C) -> int32
    {
        if (C >= TEXT('0') && C <= TEXT('9')) return C - TEXT('0');
        if (C >= TEXT('a') && C <= TEXT('f')) return C - TEXT('a') + 10;
        if (C >= TEXT('A') && C <= TEXT('F')) return C - TEXT('A') + 10;
        return -1;
    };
    for (int32 I = 0; I < 64; I += 2)
    {
        const int32 High = Nibble(Hex[I]);
        const int32 Low = Nibble(Hex[I + 1]);
        if (High < 0 || Low < 0) { Bytes.Reset(); return false; }
        Bytes.Add(uint8((High << 4) | Low));
    }
    return true;
}

inline bool HasNativeEncryptionFlag(const FBitReader& Packet)
{
    if (Packet.IsError() || Packet.GetBitsLeft() < 1) return false;
    const int64 Position = Packet.GetPosBits();
    return (Packet.GetData()[Position >> 3] & uint8(1U << (Position & 7))) != 0;
}

inline bool MayAcceptIncoming(bool EncryptionRequested, const FBitReader& Packet)
{
    // The native framing is an encryption flag, a handshake flag, then payload.
    // Reject a truncated header before the native reader can overflow/ensure.
    return !EncryptionRequested || (Packet.GetBitsLeft() >= 2 + 8 && HasNativeEncryptionFlag(Packet));
}

inline bool IsNativeHandshakeRecord(const FBitReader& Packet)
{
    if (Packet.GetBitsLeft() < 2 || !HasNativeEncryptionFlag(Packet)) return false;
    const int64 Position = Packet.GetPosBits() + 1;
    return (Packet.GetData()[Position >> 3] & uint8(1U << (Position & 7))) != 0;
}
}
