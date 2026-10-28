#pragma once
#include "CoreMinimal.h"

namespace MoBitsDiscovery
{
FString ConnectionFilePath();
bool Read(FString& Json, FString& Error);
bool ReadFile(const FString& Path, FString& Json, FString& Error);
}
