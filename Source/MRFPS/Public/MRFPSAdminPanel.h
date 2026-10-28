#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "MRFPSAdminPanel.generated.h"

class AMRFPSPlayerController;
class UEditableTextBox;
class UTextBlock;

/** Local operator UI. Every mutation is reauthorized by the server. */
UCLASS()
class MRFPS_API UMRFPSAdminPanel : public UUserWidget
{
    GENERATED_BODY()
public:
    static UMRFPSAdminPanel* CreateFor(AMRFPSPlayerController* Controller);
    UFUNCTION() void Close();
protected:
    virtual void NativeOnInitialized() override;
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
    virtual void NativeTick(const FGeometry& Geometry,float DeltaSeconds) override;
    virtual FReply NativeOnPreviewKeyDown(const FGeometry& Geometry,const FKeyEvent& Key) override;
private:
    AMRFPSPlayerController* Controller() const;
    void ReleaseInput();
    void RefreshStatus();
    UFUNCTION() void Start();
    UFUNCTION() void Pause();
    UFUNCTION() void Resume();
    UFUNCTION() void Reset();
    UFUNCTION() void ConfirmSharedFloor();
    UFUNCTION() void CalibrateFloor();
    UFUNCTION() void SetDimensions();
    UFUNCTION() void PlaceWalls();
    UFUNCTION() void ClearWalls();
    UPROPERTY(Transient) TObjectPtr<UTextBlock> Status;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> Result;
    UPROPERTY(Transient) TObjectPtr<UEditableTextBox> WidthBox;
    UPROPERTY(Transient) TObjectPtr<UEditableTextBox> LengthBox;
    UPROPERTY(Transient) TObjectPtr<UEditableTextBox> WallYawBox;
    bool bInputCaptured=false;
    float Elapsed=0;
};
