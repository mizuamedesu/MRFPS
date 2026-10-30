#include "MRFPSIpConnection.h"
#include "Engine/NetDriver.h"
#include "HAL/PlatformTime.h"

void UMRFPSIpConnection::RecordDiagnosticServerPin(const FString& Pin)
{
    const FString Normalized=Pin.ToLower();
    bool Valid=Normalized.Len()==64;
    for (TCHAR C:Normalized) Valid &= (C>='0' && C<='9') || (C>='a' && C<='f');
    if (!Valid || bDiagnosticPinAmbiguous || (!DiagnosticServerPin.IsEmpty() && DiagnosticServerPin!=Normalized)) {
        DiagnosticServerPin.Reset(); bDiagnosticPinAmbiguous=true; return;
    }
    DiagnosticServerPin=Normalized;
}

float UMRFPSIpConnection::GetTimeoutValue()
{
    const float Ordinary=Super::GetTimeoutValue();
    if (!Driver || GetConnectionState()!=USOCK_Open) return Ordinary;
    // UE's receive timeout uses its platform clock. Use that same clock here,
    // not the world's paused delta or rewritten packet timestamps.
    return static_cast<float>(CaptureConsentLease.Timeout(FPlatformTime::Seconds(),LastReceiveRealtime,Ordinary));
}
double UMRFPSIpConnection::BeginCaptureConsentPause(double Seconds)
{
    if (!Driver || GetConnectionState()!=USOCK_Open) return 0;
    return CaptureConsentLease.Begin(FPlatformTime::Seconds(),Seconds);
}
double UMRFPSIpConnection::BeginCaptureConsentPauseUntil(double Deadline)
{
    if (!Driver || GetConnectionState()!=USOCK_Open) return 0;
    return CaptureConsentLease.BeginUntil(FPlatformTime::Seconds(),Deadline);
}
void UMRFPSIpConnection::EndCaptureConsentPause() { CaptureConsentLease.End(); }
