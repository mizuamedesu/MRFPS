#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace MRFPSLocalPoseStatus
{
inline TArray<TSharedPtr<FJsonValue>> Triple(double X,double Y,double Z)
{
    return {MakeShared<FJsonValueNumber>(X),MakeShared<FJsonValueNumber>(Y),MakeShared<FJsonValueNumber>(Z)};
}

// This is the latest local tracking pass, not an acknowledgement from the
// authoritative server. Do not expose stale or non-finite coordinates.
inline void Write(FJsonObject& Info,const FVector& Head,const FRotator& View,bool SampleFresh,bool HeadTracked)
{
    Info.SetStringField(TEXT("local_head_pose_source"),TEXT("local_sample_not_server_confirmed"));
    const bool Available=SampleFresh && HeadTracked && !Head.ContainsNaN() && !View.ContainsNaN();
    Info.SetBoolField(TEXT("local_head_pose_fresh"),Available);
    Info.RemoveField(TEXT("local_head_position_cm"));
    Info.RemoveField(TEXT("local_view_rotation_degrees"));
    if (Available) {
        Info.SetArrayField(TEXT("local_head_position_cm"),Triple(Head.X,Head.Y,Head.Z));
        Info.SetArrayField(TEXT("local_view_rotation_degrees"),Triple(View.Pitch,View.Yaw,View.Roll));
    }
}
}
