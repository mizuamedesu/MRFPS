using System.IO;
using UnrealBuildTool;

public class MRFPSXR : ModuleRules
{
    public MRFPSXR(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        // JNI callbacks for geometry and video keep independent mailboxes.
        bUseUnity = false;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.AddRange(new[] { "HeadMountedDisplay", "XRBase", "AugmentedReality", "RenderCore", "RHI" });
        if (Target.Platform != UnrealTargetPlatform.Mac) PrivateDependencyModuleNames.AddRange(new[] { "OpenXRHMD", "OpenXR" });
        if ((Target.Platform == UnrealTargetPlatform.Win64 || Target.Platform == UnrealTargetPlatform.Mac) && Target.Type != TargetType.Server)
        {
            PrivateDependencyModuleNames.Add("ImageCore");
        }
        if (Target.Platform == UnrealTargetPlatform.Android)
        {
            PrivateDependencyModuleNames.AddRange(new[] { "VulkanRHI", "AndroidPermission" });
            AdditionalPropertiesForReceipt.Add("AndroidPlugin", Path.Combine(ModuleDirectory, "MRFPSXR_UPL.xml"));
            AdditionalPropertiesForReceipt.Add("AndroidPlugin", Path.Combine(ModuleDirectory, "MRFPSViewCapture_UPL.xml"));
            AdditionalPropertiesForReceipt.Add("AndroidPlugin", Path.Combine(ModuleDirectory, "MRFPSGeometry_UPL.xml"));
        }
    }
}
