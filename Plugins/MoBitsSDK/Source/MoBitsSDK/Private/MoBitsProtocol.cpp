#include "MoBitsProtocol.h"
#include "MoBitsCore.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace MoBitsProtocol
{
bool Parse(const FString& Json, TSharedPtr<FJsonObject>& Object)
{
    if (Json.Len() > 256 * 1024) return false;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    return FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid();
}
bool String(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, FString& Value, int32 MaxLength)
{
    const auto V = O.IsValid() ? O->TryGetField(Key) : nullptr;
    return V.IsValid() && V->Type == EJson::String && V->TryGetString(Value) && Value.Len() <= MaxLength;
}
bool Number(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, double& Value)
{
    const auto V = O.IsValid() ? O->TryGetField(Key) : nullptr;
    return V.IsValid() && V->Type == EJson::Number && V->TryGetNumber(Value) && FMath::IsFinite(Value);
}
bool Integer(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, int64& Value, int64 Minimum, int64 Maximum)
{
    double N = 0;
    if (!Number(O, Key, N) || N < Minimum || N > Maximum || std::floor(N) != N) return false;
    Value = static_cast<int64>(N); return true;
}
bool Boolean(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, bool& Value)
{
    const auto V = O.IsValid() ? O->TryGetField(Key) : nullptr;
    return V.IsValid() && V->Type == EJson::Boolean && V->TryGetBool(Value);
}
bool Array(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, const TArray<TSharedPtr<FJsonValue>>*& Values, int32 Limit)
{ return O.IsValid() && O->TryGetArrayField(Key, Values) && Values && Values->Num() <= Limit; }
bool Session(const FString& Json, FMoBitsSessionOptions& Options)
{
    if (Json.Len() > 4096) return false;
    TSharedPtr<FJsonObject> O;
    FString Control, Motion, Token;
    int ControlPort = 0, MotionPort = 0;
    if (!Parse(Json, O) || O->Values.Num() != 3 || !String(O, TEXT("control_url"), Control) || !String(O, TEXT("motion_url"), Motion) ||
        !String(O, TEXT("token"), Token) || !MoBitsCore::ValidToken(TCHAR_TO_UTF8(*Token)) ||
        !MoBitsCore::ParseEndpoint(TCHAR_TO_UTF8(*Control), "control", ControlPort) ||
        !MoBitsCore::ParseEndpoint(TCHAR_TO_UTF8(*Motion), "motion", MotionPort) || ControlPort != MotionPort) return false;
    Options.Port = ControlPort; Options.Token = Token; Options.ExpectedSessionId.Empty(); return true;
}
bool Discovery(const FString& Json, FMoBitsSessionOptions& Options)
{
    if (Json.Len() > 4096) return false;
    TSharedPtr<FJsonObject> O;
    FString Control, Motion, Token, AppVersion, SessionId;
    int ControlPort = 0, MotionPort = 0; int64 DiscoveryVersion = 0, ProtocolVersion = 0;
    if (!Parse(Json, O) || O->Values.Num() != 7 ||
        !Integer(O, TEXT("discovery_version"), DiscoveryVersion, 1, 1) ||
        !Integer(O, TEXT("protocol_version"), ProtocolVersion, 2, 2) ||
        !String(O, TEXT("app_version"), AppVersion, 64) || AppVersion.IsEmpty() ||
        !String(O, TEXT("session_id"), SessionId, 36) || !MoBitsCore::ValidSessionId(TCHAR_TO_UTF8(*SessionId)) ||
        !String(O, TEXT("control_url"), Control) || !String(O, TEXT("motion_url"), Motion) ||
        !String(O, TEXT("token"), Token) || !MoBitsCore::ValidToken(TCHAR_TO_UTF8(*Token)) ||
        !MoBitsCore::ParseEndpoint(TCHAR_TO_UTF8(*Control), "control", ControlPort) ||
        !MoBitsCore::ParseEndpoint(TCHAR_TO_UTF8(*Motion), "motion", MotionPort) || ControlPort != MotionPort) return false;
    Options.Port = ControlPort; Options.Token = Token; Options.ExpectedSessionId = SessionId;
    return true;
}
bool Hello(const TSharedPtr<FJsonObject>& O, bool bMotion, FString& SessionId, double& ServerMs)
{
    int64 Version = 0, Heartbeat = 0, Timeout = 0;
    FString Type, Channel;
    return String(O, TEXT("type"), Type) && Type == TEXT("HELLO") && Integer(O, TEXT("version"), Version, 2, 2) &&
        String(O, TEXT("channel"), Channel) && Channel == (bMotion ? TEXT("motion") : TEXT("control")) &&
        String(O, TEXT("session_id"), SessionId) && MoBitsCore::ValidSessionId(TCHAR_TO_UTF8(*SessionId)) &&
        Number(O, TEXT("server_time_ms"), ServerMs) && ServerMs >= 0 &&
        Integer(O, TEXT("heartbeat_ms"), Heartbeat, 100, 100) && Integer(O, TEXT("timeout_ms"), Timeout, 500, 500);
}
bool Receipt(const TSharedPtr<FJsonObject>& O, FMoBitsReceipt& Value)
{
    double Level = 0;
    if (!Integer(O, TEXT("id"), Value.Id, 1, 4294967295LL) || !String(O, TEXT("status"), Value.Status) ||
        !String(O, TEXT("reason"), Value.Reason, 2048) || !String(O, TEXT("device_id"), Value.DeviceId) ||
        !Number(O, TEXT("applied_level"), Level) || Level < 0 || Level > 1) return false;
    const FString& S = Value.Status;
    if (S != TEXT("armed") && S != TEXT("device_acknowledged") && S != TEXT("effect_unconfigured") && S != TEXT("stopped_confirmed") &&
        S != TEXT("stop_unconfirmed") && S != TEXT("rejected") && S != TEXT("expired")) return false;
    Value.AppliedLevel = static_cast<float>(Level);
    Value.bSucceeded = S == TEXT("armed") || S == TEXT("device_acknowledged") || S == TEXT("stopped_confirmed");
    return true;
}
bool Devices(const TSharedPtr<FJsonObject>& O, TArray<FMoBitsDeviceInfo>& Values)
{
    const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
    if (!Array(O, TEXT("devices"), List, 128)) return false;
    Values.Reset();
    TSet<FString> Seen;
    for (const auto& V : *List)
    {
        if (!V.IsValid() || V->Type != EJson::Object) return false;
        const auto D = V->AsObject(); FMoBitsDeviceInfo Device;
        int64 Bone = 0, Model = 0, Firmware = 0, Battery = 0;
        if (!String(D, TEXT("device_id"), Device.DeviceId, 128) || Device.DeviceId.IsEmpty() || Seen.Contains(Device.DeviceId) ||
            !Integer(D, TEXT("bone_id"), Bone, 1, 20) || !Integer(D, TEXT("model_number"), Model, 0, 65535) ||
            !Integer(D, TEXT("firmware_version"), Firmware, 0, 65535) || !Integer(D, TEXT("battery_percent"), Battery, 0, 100) ||
            !Boolean(D, TEXT("connected"), Device.bConnected) || !Boolean(D, TEXT("tracking"), Device.bTracking) ||
            !Boolean(D, TEXT("ems_supported"), Device.bEmsSupported) || !Boolean(D, TEXT("lra_supported"), Device.bLraSupported) ||
            !Boolean(D, TEXT("haptics_permitted"), Device.bHapticsPermitted)) return false;
        Device.BoneId = static_cast<int32>(Bone); Device.ModelNumber = static_cast<int32>(Model);
        Device.FirmwareVersion = static_cast<int32>(Firmware); Device.BatteryPercent = static_cast<int32>(Battery);
        Seen.Add(Device.DeviceId); Values.Add(MoveTemp(Device));
    }
    return true;
}
template<std::size_t Count> bool Numbers(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, std::array<double, Count>& Values)
{
    const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
    if (!Array(O, Key, List, static_cast<int32>(Count)) || List->Num() != Count) return false;
    for (std::size_t I = 0; I < Count; ++I)
        if (!(*List)[I].IsValid() || (*List)[I]->Type != EJson::Number || !(*List)[I]->TryGetNumber(Values[I]) || !FMath::IsFinite(Values[I])) return false;
    return true;
}
bool Pose(const TSharedPtr<FJsonObject>& O, const TCHAR* RotationKey, const TCHAR* PositionKey, FQuat& Rotation, FVector& Position)
{
    std::array<double, 4> Q; std::array<double, 3> P;
    if (!Numbers(O, RotationKey, Q) || !MoBitsCore::ValidQuaternion(Q) || !Numbers(O, PositionKey, P)) return false;
    const auto R = MoBitsCore::Rotation(Q); const auto V = MoBitsCore::Position(P);
    Rotation = FQuat(R[0], R[1], R[2], R[3]); Position = FVector(V[0], V[1], V[2]);
    return !Position.ContainsNaN();
}
bool Motion(const TSharedPtr<FJsonObject>& O, const FString& SessionId, FMoBitsMotionFrame& Frame)
{
    int64 Version = 0;
    FString Type, Session, Space;
    const TArray<TSharedPtr<FJsonValue>>* DevicesList = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* BonesList = nullptr;
    if (!String(O, TEXT("type"), Type) || Type != TEXT("MOTION") || !Integer(O, TEXT("version"), Version, 2, 2) ||
        !String(O, TEXT("session_id"), Session) || Session != SessionId || !Integer(O, TEXT("sequence"), Frame.Sequence, 1) ||
        !Integer(O, TEXT("origin_revision"), Frame.OriginRevision, 1) || !Number(O, TEXT("server_time_ms"), Frame.ServerTimeMs) || Frame.ServerTimeMs < 0 ||
        !String(O, TEXT("coordinate_space"), Space) || Space != TEXT("unity_left_handed_y_up_meters") ||
        !Boolean(O, TEXT("tracking_active"), Frame.bTrackingActive) || !Array(O, TEXT("devices"), DevicesList, 128) || !Array(O, TEXT("bones"), BonesList, 20) ||
        (!Frame.bTrackingActive && !BonesList->IsEmpty())) return false;
    Frame.Devices.Reset(); Frame.Bones.Reset();
    TSet<FString> SeenDevices;
    for (const auto& V : *DevicesList)
    {
        if (!V.IsValid() || V->Type != EJson::Object) return false;
        const auto D = V->AsObject(); FMoBitsDevicePose P; int64 Bone = 0, Age = 0;
        if (!String(D, TEXT("device_id"), P.DeviceId, 128) || P.DeviceId.IsEmpty() || SeenDevices.Contains(P.DeviceId) ||
            !Integer(D, TEXT("bone_id"), Bone, 1, 20) || !Integer(D, TEXT("sample_age_ms"), Age, 0) ||
            !Boolean(D, TEXT("has_position"), P.bHasPosition) || !String(D, TEXT("position_source"), P.PositionSource) ||
            (P.PositionSource != TEXT("skeleton_estimate") && P.PositionSource != TEXT("unavailable")) ||
            (P.bHasPosition && P.PositionSource != TEXT("skeleton_estimate")) ||
            !Pose(D, TEXT("rotation_xyzw"), TEXT("position_m"), P.Rotation, P.PositionCm)) return false;
        P.BoneId = static_cast<int32>(Bone); P.SampleAgeMs = static_cast<double>(Age);
        SeenDevices.Add(P.DeviceId); Frame.Devices.Add(MoveTemp(P));
    }
    TSet<int32> SeenBones;
    for (const auto& V : *BonesList)
    {
        if (!V.IsValid() || V->Type != EJson::Object) return false;
        const auto B = V->AsObject(); FMoBitsBonePose P; int64 Bone = 0, Parent = 0; FString Name;
        if (!Integer(B, TEXT("bone_id"), Bone, 1, 20) || !Integer(B, TEXT("parent_bone_id"), Parent, 0, 20) || SeenBones.Contains(static_cast<int32>(Bone)) ||
            (Parent != 0 && !SeenBones.Contains(static_cast<int32>(Parent))) || !String(B, TEXT("name"), Name, 64) || Name.IsEmpty() ||
            !Pose(B, TEXT("local_rotation_xyzw"), TEXT("local_position_m"), P.LocalRotation, P.LocalPositionCm)) return false;
        P.BoneId = static_cast<int32>(Bone); P.ParentBoneId = static_cast<int32>(Parent); P.Name = FName(*Name);
        SeenBones.Add(P.BoneId); Frame.Bones.Add(MoveTemp(P));
    }
    return true;
}
FString Encode(const TSharedRef<FJsonObject>& Object)
{
    FString Result;
    const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
    FJsonSerializer::Serialize(Object, Writer);
    return Result;
}
}
