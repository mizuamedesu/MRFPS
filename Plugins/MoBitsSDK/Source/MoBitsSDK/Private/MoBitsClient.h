#pragma once
#include "CoreMinimal.h"
#include "MoBitsCore.h"
#include "MoBitsProtocol.h"
#include "MoBitsSocket.h"

class FMoBitsClient
{
public:
    enum class EEventKind { State, Receipt, Devices, Motion, Error };
    struct FEvent
    {
        EEventKind Kind = EEventKind::Error;
        uint64 Generation = 0;
        EMoBitsConnectionState State = EMoBitsConnectionState::Disconnected;
        FMoBitsReceipt Receipt;
        TArray<FMoBitsDeviceInfo> Devices;
        FMoBitsMotionFrame Frame;
        FString Error;
    };
    bool Connect(const FString& Json, FString& Error, bool bDiscovery = false);
    bool ConnectLocal(FString& Error, const FString& DiscoveryPath = FString());
    bool bAutoReconnect = true;
    bool IsReconnecting() const { return bConnectionWanted && bAutoReconnect && State != EMoBitsConnectionState::Connected && RetryCount > 0; }
    int32 GetReconnectAttempts() const { return RetryCount; }
    void Tick();
    void Suspend();
    void Resume();
    void Disconnect();
    void ForceClose();
    int64 Arm();
    int64 Play(int32 TargetBone, int32 Effect, float Level, int32 TtlMs);
    int64 PlayGlobal(int32 Effect, float Level, int32 TtlMs);
    int64 Stop();
    int64 Devices();
    bool Frame(FMoBitsMotionFrame& Out) const;
    bool DevicePose(const FString& Id, FMoBitsDevicePose& Out) const;
    bool Armed() const { return State == EMoBitsConnectionState::Connected && Gate.Armed() && Fresh(); }
    EMoBitsConnectionState GetState() const { return State; }
    uint64 GetGeneration() const { return Generation; }
    void Drain(TArray<FEvent>& Out) { Out = MoveTemp(Events); Events.Reset(); }
    void Report(const FString& Error);
private:
    enum class ECommand { Arm, Play, Global, Stop, Devices };
    struct FPending { ECommand Kind; uint64 Epoch; double Deadline; };
    int64 Request(ECommand Kind, const TSharedRef<FJsonObject>& Object, int32 TtlMs = 0);
    bool Receive(bool bMotion, const FString& Text);
    bool Start(const FMoBitsSessionOptions& NewOptions, FString& Error);
    bool ResolveAndStart(FString& Error);
    void ScheduleRetry();
    void SetState(EMoBitsConnectionState NewState);
    void Fault(const FString& Reason);
    void CompletePending(const FString& Reason);
    void CloseTransports();
    void DiscardTrackingEvents();
    bool Fresh() const;
    void Emit(FEvent&& Event);
    EMoBitsConnectionState State = EMoBitsConnectionState::Disconnected;
    TUniquePtr<FMoBitsSocket> Control, MotionSocket;
    FMoBitsSessionOptions Options;
    FString SessionId, DiscoveryFile;
    FMoBitsSessionOptions ManualOptions;
    bool bConnectionWanted = false, bDiscover = false, bSuspended = false;
    int32 RetryCount = 0;
    double RetryAt = 0, ConnectedAt = 0;
    MoBitsCore::Clock Clock;
    MoBitsCore::OutputGate Gate;
    TMap<int64, FPending> Pending;
    TArray<FEvent> Events;
    FMoBitsMotionFrame Latest;
    bool bStopUnconfirmed = false;
    bool bHasFrame = false, bControlHello = false, bMotionHello = false;
    uint64 Generation = 0;
    int64 NextId = 0, ConsumedSequence = 0, DisconnectId = 0;
    double Began = 0, LastTick = 0, LastPing = 0, LastPong = 0, LastMotion = 0;
};
