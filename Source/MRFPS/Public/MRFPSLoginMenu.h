#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Interfaces/IHttpRequest.h"
#include "MRFPSLoginPointer.h"
#include "MRFPSWeaponPose.h"
#include "MRFPSLoginMenu.generated.h"

class AMRFPSPlayerController;
class AMRFPSPawn;
class UEditableTextBox;
class UTextBlock;
class UVerticalBox;
class UWidgetComponent;
class UMRFPSLoginInteraction;
class UStaticMeshComponent;
class UMRFPSLoginMenu;
class SWidget;

/** An in-world keyboard key, so Quest input doesn't depend on an Android IME. */
UCLASS()
class MRFPS_API UMRFPSLoginKey : public UButton
{
    GENERATED_BODY()
public:
    FString KeyValue;
    TWeakObjectPtr<UMRFPSLoginMenu> Login;
    UFUNCTION() void PressedKey();
};

/** Server setup, followed by role-scoped, one-use device-code exchange. */
UCLASS()
class MRFPS_API UMRFPSLoginMenu : public UUserWidget
{
    GENERATED_BODY()
public:
    static UMRFPSLoginMenu* CreateFor(AMRFPSPlayerController* Owner);
    /** Named local control channel; Android uses internal app-private files. */
    static FString ControlPath();
    void SetPointerPressed(bool bPressed);
    void SetPointerTriggerAxis(float Value);
    void SetEndpoint(const FString& Address);
    void LoginCodeFromFile(const FString& File, const FString& Role);
    void InsertKey(const FString& Key);
    void CloseLogin();
    /** Poll on the owning player's game tick, even when Slate is not painting. */
    void TickControlFile();
    void TickXRPointerAfterPaint(UWidgetComponent* PaintedComponent,float DeltaTime);
    bool IsOpen() const { return bOpen; }
    UFUNCTION() void Next();
    UFUNCTION() void Back();
    UFUNCTION() void SelectPlayer();
    UFUNCTION() void SelectInspector();
    UFUNCTION() void SelectAdmin();
    UFUNCTION() void Exchange();
    UFUNCTION() void ResetTrust();

protected:
    virtual void NativeOnInitialized() override;
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
#if WITH_DEV_AUTOMATION_TESTS
    virtual void NativeTick(const FGeometry& Geometry,float DeltaTime) override;
#endif

private:
#if WITH_DEV_AUTOMATION_TESTS
    friend class FMRFPSLoginControlTickTest;
    friend class FMRFPSLoginWidgetPaintScenario;
    FString TestControlPath;
    bool bUseTestWeapon=false;
    FMRFPSWeaponPose TestWeapon;
    TFunction<void()> TestDuringPaint;
#endif
    bool ReadAddress(FString& Out) const;
    void SelectRole(const FString& InRole);
    void SetStage(bool bLogin);
    void SetStatus(const FString& Message, bool bError=false);
    void HandleExchange(FHttpRequestPtr Request,FHttpResponsePtr Response,bool bSucceeded);
    void ConnectAfterLogin();
    void ProcessControlFile();
    void UpdateXRPointer(float DeltaTime);
    bool ResolvePointerWeapon(AMRFPSPawn* Pawn,FMRFPSWeaponPose& Weapon);
    void ApplyPointerAction(MRFPSLoginPointer::EAction Action);
    void HandleFocusLost();
    void HandleFocusGained();
    TWeakObjectPtr<AMRFPSPlayerController> Controller;
    UPROPERTY(Transient) TObjectPtr<UEditableTextBox> AddressBox;
    UPROPERTY(Transient) TObjectPtr<UEditableTextBox> CodeBox;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> Heading;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> Status;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> PointerHint;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> RoleLabel;
    UPROPERTY(Transient) TObjectPtr<UVerticalBox> AddressPage;
    UPROPERTY(Transient) TObjectPtr<UVerticalBox> LoginPage;
    UPROPERTY(Transient) TObjectPtr<UButton> TrustButton;
    UPROPERTY(Transient) TObjectPtr<UWidgetComponent> WorldWidget;
    UPROPERTY(Transient) TObjectPtr<UMRFPSLoginInteraction> Interaction;
    UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent> PointerRay;
    UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent> PointerDot;
    TSharedPtr<IHttpRequest,ESPMode::ThreadSafe> PendingRequest;
    FString Role=TEXT("player");
    FString Destination;
    bool bVR=false,bOpen=false,bLoginPage=false,bExchanging=false,bClosing=false;
    bool bPointerHit=false,bPointerPoseReported=false,bPointerHitReported=false;
    bool bPointerGridReady=false;
    bool bRawTraceReported=false,bRawTraceWidget=false,bRawTraceSlate=false;
    MRFPSLoginPointer::FPressState PointerInput;
    MRFPSLoginPointer::FEventQueue PendingPointerEvents;
    // Keep sampled target identities alive until their queued edges are consumed.
    TArray<TSharedPtr<SWidget>> PendingPointerTargets;
    TWeakPtr<SWidget> PreviousPointerHit;
    std::uint64_t GloveCursor=0;
    bool bGloveCursorInitialized=false;
    MRFPSLoginPointer::EInput AnnouncedInput=MRFPSLoginPointer::EInput::None;
    bool bInputAnnounced=false;
    FDelegateHandle FocusLostHandle,FocusGainedHandle,BackgroundHandle,ForegroundHandle;
    double NextControlCheck=0,LastPointerTick=-1,LastPointerPaint=-1;
};
