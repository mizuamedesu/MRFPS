#pragma once
#include "CoreMinimal.h"
#include "MoBitsTypes.h"
#include "Dom/JsonObject.h"

struct FMoBitsSessionOptions { int32 Port = 0; FString Token; FString ExpectedSessionId; };

namespace MoBitsProtocol
{
bool Parse(const FString& Json, TSharedPtr<FJsonObject>& Object);
bool String(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, FString& Value, int32 MaxLength = 256);
bool Number(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, double& Value);
bool Integer(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, int64& Value, int64 Minimum, int64 Maximum = 9007199254740991LL);
bool Boolean(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, bool& Value);
bool Session(const FString& Json, FMoBitsSessionOptions& Options);
bool Discovery(const FString& Json, FMoBitsSessionOptions& Options);
bool Hello(const TSharedPtr<FJsonObject>& Object, bool bMotion, FString& SessionId, double& ServerMs);
bool Receipt(const TSharedPtr<FJsonObject>& Object, FMoBitsReceipt& Value);
bool Devices(const TSharedPtr<FJsonObject>& Object, TArray<FMoBitsDeviceInfo>& Values);
bool Motion(const TSharedPtr<FJsonObject>& Object, const FString& SessionId, FMoBitsMotionFrame& Frame);
FString Encode(const TSharedRef<FJsonObject>& Object);
}
