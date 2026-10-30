#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "MRFPSMenu.generated.h"

class AMRFPSPlayerController;
class UButton;
class UEditableTextBox;
class UScrollBox;
class UTextBlock;
class UWidgetSwitcher;

/** Desktop session controls. All actions go through the owning controller. */
UCLASS()
class MRFPS_API UMRFPSMenu : public UUserWidget
{
    GENERATED_BODY()
public:
    /** Creates, displays, and focuses the menu for a local controller. */
    static UMRFPSMenu* CreateFor(AMRFPSPlayerController* Controller);

    UFUNCTION(BlueprintCallable, Category="MRFPS") void Close();
    bool SetRosterVisible(bool Visible);
    bool IsRosterVisible() const { return bRosterVisible; }

protected:
    virtual void NativeOnInitialized() override;
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
    virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
    virtual FReply NativeOnPreviewKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:
    AMRFPSPlayerController* Controller() const;
    void RefreshStatus();
    void RefreshRoster();
    void ReleaseInput();
    void Join(bool bInspector);
    void JoinRole(const FString& Role);
    bool ReadAddress(FString& Address, FString& Error) const;
    void SaveAddress();

    UFUNCTION() void JoinPlayer();
    UFUNCTION() void JoinInspector();
    UFUNCTION() void JoinAdmin();
    UFUNCTION() void OpenDeviceLogin();
    UFUNCTION() void Align();
    UFUNCTION() void ConnectHaptics();
    UFUNCTION() void ArmHaptics();
    UFUNCTION() void StopHaptics();
    UFUNCTION() void ViewPlayerOne();
    UFUNCTION() void ViewPlayerTwo();
    UFUNCTION() void ViewFree();
    UFUNCTION() void AddressChanged(const FText& Text);
    UFUNCTION() void ShowSettings();
    UFUNCTION() void ShowRoster();

    UPROPERTY(Transient) TObjectPtr<UEditableTextBox> ServerBox;
    UPROPERTY(Transient) TObjectPtr<UEditableTextBox> AccessTokenBox;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> AuthenticationText;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> RoleText;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> MatchText;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> HapticsText;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> ErrorText;
    UPROPERTY(Transient) TObjectPtr<UButton> AlignButton;
    UPROPERTY(Transient) TObjectPtr<UButton> ConnectButton;
    UPROPERTY(Transient) TObjectPtr<UButton> ArmButton;
    UPROPERTY(Transient) TObjectPtr<UButton> StopButton;
    UPROPERTY(Transient) TObjectPtr<UButton> PlayerOneButton;
    UPROPERTY(Transient) TObjectPtr<UButton> PlayerTwoButton;
    UPROPERTY(Transient) TObjectPtr<UButton> FreeButton;
    UPROPERTY(Transient) TObjectPtr<UButton> RosterTabButton;
    UPROPERTY(Transient) TObjectPtr<UWidgetSwitcher> Pages;
    UPROPERTY(Transient) TObjectPtr<UScrollBox> MenuScroll;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> RosterText;
    FString ActionError;
    FString LastRosterText;
    float RefreshElapsed = 0;
    float RosterElapsed = 0;
    bool bInputCaptured = false;
    bool bRosterVisible = false;
};
