#pragma once

#include "CoreMinimal.h"

namespace MRFPSDeviceAuthUrl
{
// Development-only override: exact loopback authority, numeric port and a
// plain path. Userinfo, percent encoding, queries, fragments and backslashes
// are not accepted, so no alternate interpretation can change its host.
inline bool IsSafeTestUrl(const FString& Url)
{
    const FString IpPrefix=TEXT("http://127.0.0.1:");
    const FString NamePrefix=TEXT("http://localhost:");
    int32 Start=0;
    if (Url.StartsWith(IpPrefix,ESearchCase::CaseSensitive)) Start=IpPrefix.Len();
    else if (Url.StartsWith(NamePrefix,ESearchCase::CaseSensitive)) Start=NamePrefix.Len();
    else return false;
    const FString Rest=Url.Mid(Start);
    int32 Slash=INDEX_NONE; Rest.FindChar('/',Slash);
    const FString Port=Slash==INDEX_NONE?Rest:Rest.Left(Slash);
    if (Port.IsEmpty() || Port.Len()>5) return false;
    for (TCHAR C:Port) if (C<'0' || C>'9') return false;
    const int32 Number=FCString::Atoi(*Port); if (Number<1 || Number>65535) return false;
    if (Slash!=INDEX_NONE) for (TCHAR C:Rest.Mid(Slash)) {
        const bool Ascii=(C>='a' && C<='z') || (C>='A' && C<='Z') || (C>='0' && C<='9');
        if (!Ascii && C!='/' && C!='-' && C!='_' && C!='.') return false;
    }
    return true;
}
}
