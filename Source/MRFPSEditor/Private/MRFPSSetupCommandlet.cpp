#include "MRFPSSetupCommandlet.h"
#include "FileHelpers.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "GameFramework/WorldSettings.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetCompilingManager.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Engine/StaticMesh.h"
#include "Engine/PrimaryAssetLabel.h"

static bool ImportBlenderAssets()
{
    const TCHAR* Assets[]={TEXT("SM_MRPistol"),TEXT("SM_MRWall"),TEXT("SM_MRMiniShield"),TEXT("SM_MRHead"),TEXT("SM_MRBody"),TEXT("SM_MRHand")};
    int32 Available=0;
    for (const TCHAR* Name:Assets) if (FPaths::FileExists(FPaths::ProjectDir()/TEXT("Art/Generated/Exports")/(FString(Name)+TEXT(".fbx")))) ++Available;
    if (!Available) { UE_LOG(LogTemp,Display,TEXT("MRFPS_ART_PENDING Blender exports have not been generated")); return true; }
    if (Available!=UE_ARRAY_COUNT(Assets)) { UE_LOG(LogTemp,Error,TEXT("MRFPS_ART_INCOMPLETE only %d of 6 exports exist"),Available); return false; }
    TArray<UAssetImportTask*> Tasks;
    for (const TCHAR* Name:Assets) {
        auto* Task=NewObject<UAssetImportTask>();
        Task->Filename=FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()/TEXT("Art/Generated/Exports")/(FString(Name)+TEXT(".fbx")));
        Task->DestinationPath=TEXT("/Game/Meshes"); Task->DestinationName=Name;
        Task->bAutomated=true; Task->bReplaceExisting=true; Task->bReplaceExistingSettings=true; Task->bSave=true; Task->bAsync=false;
        auto* Options=NewObject<UFbxImportUI>(Task);
        Options->bAutomatedImportShouldDetectType=false;
        Options->MeshTypeToImport=FBXIT_StaticMesh; Options->OriginalImportType=FBXIT_StaticMesh;
        Options->bImportAsSkeletal=false; Options->bImportMesh=true; Options->bImportMaterials=false; Options->bImportTextures=false;
        Options->StaticMeshImportData->bCombineMeshes=true;
        Options->StaticMeshImportData->bAutoGenerateCollision=false;
        Options->StaticMeshImportData->bConvertScene=true;
        Options->StaticMeshImportData->bConvertSceneUnit=true;
        Options->StaticMeshImportData->bForceFrontXAxis=false;
        Task->Options=Options; Tasks.Add(Task);
    }
    FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().ImportAssetTasks(Tasks);
    FAssetCompilingManager::Get().FinishAllCompilation();
    const FVector Expected[]={FVector(32,6.5,16),FVector(16,160,200),FVector(4,44,44),FVector(26),FVector(40,40,80),FVector(18,10,5.5)};
    for (int32 I=0; I<UE_ARRAY_COUNT(Assets); ++I) {
        const FString ObjectPath=FString::Printf(TEXT("/Game/Meshes/%s.%s"),Assets[I],Assets[I]);
        UStaticMesh* Mesh=LoadObject<UStaticMesh>(nullptr,*ObjectPath);
        if (!Mesh) { UE_LOG(LogTemp,Error,TEXT("MRFPS_ART_IMPORT_FAILED %s"),Assets[I]); return false; }
        for (FStaticMaterial& Slot:Mesh->GetStaticMaterials()) {
            FString Name=Slot.ImportedMaterialSlotName.ToString();
            if (!Name.StartsWith(TEXT("M_"))) Name=TEXT("M_")+Name;
            const FString MaterialPath=FString::Printf(TEXT("/Game/Materials/%s.%s"),*Name,*Name);
            if (auto* Material=LoadObject<UMaterialInterface>(nullptr,*MaterialPath)) { Slot.MaterialInterface=Material; Slot.MaterialSlotName=FName(*Name); }
            else { UE_LOG(LogTemp,Error,TEXT("MRFPS_ART_UNKNOWN_MATERIAL %s in %s"),*Name,Assets[I]); return false; }
        }
        const FVector Size=Mesh->GetBoundingBox().GetSize();
        if (Size.GetMin()<=0 || Size.ContainsNaN()) return false;
        for (int32 Axis=0;Axis<3;++Axis) if (Size[Axis]<Expected[I][Axis]*.5 || Size[Axis]>Expected[I][Axis]*1.5) {
            UE_LOG(LogTemp,Error,TEXT("MRFPS_ART_UNITS_OR_AXIS_ERROR %s actual_cm=%s expected_cm=%s"),Assets[I],*Size.ToString(),*Expected[I].ToString()); return false;
        }
        Mesh->MarkPackageDirty(); FSavePackageArgs Args; Args.TopLevelFlags=RF_Public|RF_Standalone;
        const FString Filename=FPackageName::LongPackageNameToFilename(Mesh->GetOutermost()->GetName(),FPackageName::GetAssetPackageExtension());
        if (!UPackage::SavePackage(Mesh->GetOutermost(),Mesh,*Filename,Args)) return false;
        UE_LOG(LogTemp,Display,TEXT("MRFPS_ART_IMPORTED name=%s size_cm=%s"),Assets[I],*Size.ToString());
    }
    return true;
}

UMRFPSSetupCommandlet::UMRFPSSetupCommandlet() { IsClient = false; IsEditor = true; LogToConsole = true; }

int32 UMRFPSSetupCommandlet::Main(const FString& Params)
{
    const TCHAR* Names[] = { TEXT("Blue"), TEXT("Orange"), TEXT("Wall"), TEXT("Shield"), TEXT("Floor"), TEXT("Tracer"),TEXT("Graphite"),TEXT("Trim"),TEXT("White") };
    const FLinearColor Colors[] = {
        {0.04f,0.55f,1,1}, {1,0.24f,0.035f,1}, {0.08f,0.4f,0.65f,1},
        {0.1f,0.95f,0.6f,1}, {0.035f,0.05f,0.08f,1}, {1,0.8f,0.1f,1},
        {.018f,.024f,.038f,1}, {.12f,.2f,.26f,1}, {.65f,.78f,.86f,1}
    };
    for (int32 I=0; I<UE_ARRAY_COUNT(Names); ++I) {
        const FString Name = FString(TEXT("M_")) + Names[I];
        const FString PackageName = TEXT("/Game/Materials/") + Name;
        if (FPackageName::DoesPackageExist(PackageName)) continue;
        UPackage* Package = CreatePackage(*PackageName);
        UMaterial* Material = NewObject<UMaterial>(Package, *Name, RF_Public | RF_Standalone);
        Material->SetShadingModel(MSM_Unlit);
        UMaterialExpressionConstant3Vector* Color = NewObject<UMaterialExpressionConstant3Vector>(Material);
        Color->Constant = Colors[I];
        Material->GetExpressionCollection().AddExpression(Color);
        Material->GetEditorOnlyData()->EmissiveColor.Expression = Color;
        Material->TwoSided = true;
        Material->PostEditChange();
        FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
        const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
        if (!UPackage::SavePackage(Package, Material, *Filename, Args)) return 1;
    }
    if (!FPackageName::DoesPackageExist(TEXT("/Game/Materials/M_Preview"))) {
        const FString PackageName=TEXT("/Game/Materials/M_Preview");
        auto* Package=CreatePackage(*PackageName);
        auto* Material=NewObject<UMaterial>(Package,TEXT("M_Preview"),RF_Public|RF_Standalone);
        Material->SetShadingModel(MSM_Unlit);Material->BlendMode=BLEND_Translucent;Material->TwoSided=true;
        auto* Tint=NewObject<UMaterialExpressionVectorParameter>(Material);Tint->ParameterName=TEXT("Tint");Tint->DefaultValue=FLinearColor(.05,1,.55);
        auto* Opacity=NewObject<UMaterialExpressionConstant>(Material);Opacity->R=.3f;
        Material->GetExpressionCollection().AddExpression(Tint);Material->GetExpressionCollection().AddExpression(Opacity);
        Material->GetEditorOnlyData()->EmissiveColor.Expression=Tint;Material->GetEditorOnlyData()->Opacity.Expression=Opacity;
        Material->PostEditChange();FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;
        const FString Filename=FPackageName::LongPackageNameToFilename(PackageName,FPackageName::GetAssetPackageExtension());
        if(!UPackage::SavePackage(Package,Material,*Filename,Args)) return 4;
    }
    if (!FPackageName::DoesPackageExist(TEXT("/Game/Maps/MRFPSArena"))) {
        IFileManager::Get().MakeDirectory(*(FPaths::ProjectContentDir()/TEXT("Maps")),true);
        UWorld* World = UEditorLoadingAndSavingUtils::NewBlankMap(false);
        if (World) World->GetWorldSettings()->WorldToMeters=100;
        if (!World || !UEditorLoadingAndSavingUtils::SaveMap(World, TEXT("/Game/Maps/MRFPSArena"))) return 2;
    }
    if (!ImportBlenderAssets()) return 3;
    // These paths are loaded by the native avatar at runtime. Explicit labels
    // cook their dependencies without cooking unused template Blueprints/rigs.
    const FString AvatarPackage=TEXT("/Game/RuntimeAssets/Manny");
    if(!FPackageName::DoesPackageExist(AvatarPackage)) {
        auto* Package=CreatePackage(*AvatarPackage);
        auto* Label=NewObject<UPrimaryAssetLabel>(Package,TEXT("Manny"),RF_Public|RF_Standalone);
        Label->bIsRuntimeLabel=true;Label->bLabelAssetsInMyDirectory=false;
        Label->Rules.CookRule=EPrimaryAssetCookRule::AlwaysCook;Label->Rules.bApplyRecursively=true;
        for(const TCHAR* Path:{TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"),
            TEXT("/Game/Characters/Mannequins/Anims/Unarmed/BS_Idle_Walk_Run.BS_Idle_Walk_Run"),
            TEXT("/Game/Characters/Mannequins/Anims/Unarmed/MM_Idle.MM_Idle")}) Label->ExplicitAssets.Add(TSoftObjectPtr<UObject>(FSoftObjectPath(Path)));
        Label->UpdateAssetBundleData();Label->PostEditChange();
        const FString Filename=FPackageName::LongPackageNameToFilename(AvatarPackage,FPackageName::GetAssetPackageExtension());
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename),true);
        FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;
        if(!UPackage::SavePackage(Package,Label,*Filename,Args)) return 5;
    }
    UE_LOG(LogTemp, Display, TEXT("MRFPS_SETUP_PASS materials and arena map ready"));
    return 0;
}
