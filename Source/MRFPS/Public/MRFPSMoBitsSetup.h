#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MRFPSMoBitsSetup.generated.h"

class UMRFPSLoginMenu;
class UMoBitsQuestStandaloneComponent;
class UTextBlock;
class UVerticalBox;
class UWidgetTree;

/** In-game, wearer-operated setup for the Quest direct-Bluetooth runtime. */
UCLASS()
class MRFPS_API UMRFPSMoBitsSetup : public UObject
{
    GENERATED_BODY()
public:
    void Build(UMRFPSLoginMenu* InMenu, UWidgetTree* Tree, UVerticalBox* Parent);
    void Refresh();

private:
    UMoBitsQuestStandaloneComponent* Runtime() const;
    void Note(const FString& Message);
    FString CandidateId() const;
    void SelectSide(bool Right);
    void MoveCandidate(int32 Direction);
    void MoveWave(int32 Direction);

    UFUNCTION() void Start();
    UFUNCTION() void Scan();
    UFUNCTION() void PreviousDevice();
    UFUNCTION() void NextDevice();
    UFUNCTION() void Pair();
    UFUNCTION() void AssignLeft();
    UFUNCTION() void AssignRight();
    UFUNCTION() void BeginSession();
    UFUNCTION() void StartCalibration();
    UFUNCTION() void LeftArm();
    UFUNCTION() void RightArm();
    UFUNCTION() void LowerCurrent();
    UFUNCTION() void RaiseCurrent();
    UFUNCTION() void CalibrationPulse();
    UFUNCTION() void MarkThreshold();
    UFUNCTION() void ConfirmComfort();
    UFUNCTION() void Prepare();
    UFUNCTION() void PreviousWave();
    UFUNCTION() void NextWave();
    UFUNCTION() void TryWave();
    UFUNCTION() void ConfirmWave();
    UFUNCTION() void Arm();
    UFUNCTION() void Stop();
    UFUNCTION() void CloseSession();
    UFUNCTION() void Back();
    UFUNCTION() void Finish();

    TWeakObjectPtr<UMRFPSLoginMenu> Menu;
    UPROPERTY() TObjectPtr<UVerticalBox> DevicesPage;
    UPROPERTY() TObjectPtr<UVerticalBox> SessionPage;
    UPROPERTY() TObjectPtr<UVerticalBox> CalibrationPage;
    UPROPERTY() TObjectPtr<UVerticalBox> PresetsPage;
    UPROPERTY() TObjectPtr<UVerticalBox> ArmedPage;
    UPROPERTY() TObjectPtr<UVerticalBox> StoppedPage;
    UPROPERTY() TObjectPtr<UTextBlock> StateText;
    UPROPERTY() TObjectPtr<UTextBlock> DeviceText;
    UPROPERTY() TObjectPtr<UTextBlock> CalibrationText;
    UPROPERTY() TObjectPtr<UTextBlock> PresetText;
    UPROPERTY() TObjectPtr<UTextBlock> NoteText;
    FString LeftId, RightId, NoteValue;
    int32 Candidate = 0, Side = 0, Wave = 1;
    int32 TargetUa[2] = {500,500};
    int32 MaxDeliveredUa[2] = {0,0};
    int32 FeltUa[2] = {0,0};
    bool bPulsePending = false;
    int32 PulseSide = 0, PulsePreviousUa = 0, PulsePreviousCode = 0;
    FString PulsePreviousMessage;
};
