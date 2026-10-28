#pragma once

#include "CoreMinimal.h"

// A bounded OSC 1.0 subset. Packet parsing is independent of the vendor SDK.
namespace MRFPSOSC
{
struct FArgument
{
    TCHAR Type = 0;
    int32 Integer = 0;
    float Number = 0.0f;
    FString Text;
};
struct FMessage { FString Address; TArray<FArgument> Arguments; };

inline uint32 ReadU32(const uint8* P)
{
    return (uint32(P[0]) << 24) | (uint32(P[1]) << 16) | (uint32(P[2]) << 8) | uint32(P[3]);
}
inline bool ReadString(const uint8* Data, int32 Size, int32& Cursor, FString& Text)
{
    const int32 Start = Cursor;
    while (Cursor < Size && Data[Cursor] != 0) ++Cursor;
    if (Cursor == Size || Cursor - Start > 512) return false;
    // OSC addresses, device IDs and tags used here are ASCII. Reject embedded unsupported bytes.
    for (int32 I = Start; I < Cursor; ++I) if (Data[I] > 127 || Data[I] < 32) return false;
    const int32 End = Cursor;
    Cursor = (Cursor + 4) & ~3;
    if (Cursor > Size) return false;
    for (int32 I = End; I < Cursor; ++I) if (Data[I] != 0) return false;
    Text = FString(End - Start, reinterpret_cast<const ANSICHAR*>(Data + Start));
    return true;
}
inline bool Parse(const uint8* Data, int32 Size, FMessage& Out)
{
    if (!Data || Size < 8 || Size > 8192 || Size % 4 != 0) return false;
    int32 Cursor = 0;
    FMessage Message;
    FString Tags;
    if (!ReadString(Data, Size, Cursor, Message.Address) || !Message.Address.StartsWith(TEXT("/"))) return false;
    if (!ReadString(Data, Size, Cursor, Tags) || !Tags.StartsWith(TEXT(",")) || Tags.Len() > 80) return false;
    for (int32 I = 1; I < Tags.Len(); ++I)
    {
        FArgument Arg;
        Arg.Type = Tags[I];
        if (Arg.Type == TEXT('s'))
        {
            if (!ReadString(Data, Size, Cursor, Arg.Text)) return false;
        }
        else if (Arg.Type == TEXT('i') || Arg.Type == TEXT('f'))
        {
            if (Cursor > Size - 4) return false;
            const uint32 Bits = ReadU32(Data + Cursor);
            Cursor += 4;
            if (Arg.Type == TEXT('i')) FMemory::Memcpy(&Arg.Integer, &Bits, 4);
            else
            {
                FMemory::Memcpy(&Arg.Number, &Bits, 4);
                if (!FMath::IsFinite(Arg.Number)) return false;
            }
        }
        else return false;
        Message.Arguments.Add(MoveTemp(Arg));
    }
    if (Cursor != Size) return false;
    Out = MoveTemp(Message);
    return true;
}

inline bool Match(const FMessage& Message, const TCHAR* Types)
{
    const int32 Length = FCString::Strlen(Types);
    if (Length != Message.Arguments.Num()) return false;
    for (int32 I = 0; I < Length; ++I) if (Message.Arguments[I].Type != Types[I]) return false;
    return true;
}
inline bool IsFresh(double ReceivedAt, double Now, double MaximumAge)
{
    return ReceivedAt >= 0.0 && Now >= ReceivedAt && Now - ReceivedAt <= MaximumAge;
}
inline bool IsConnectedSample(bool Connected, double StatusReceivedAt, double SampleReceivedAt, double Now, double MaximumAge)
{
    return Connected && IsFresh(StatusReceivedAt, Now, MaximumAge) && IsFresh(SampleReceivedAt, Now, MaximumAge);
}
}
