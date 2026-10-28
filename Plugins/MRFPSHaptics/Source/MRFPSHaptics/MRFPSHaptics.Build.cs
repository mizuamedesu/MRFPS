using UnrealBuildTool;

public class MRFPSHaptics : ModuleRules
{
    public MRFPSHaptics(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "MoBitsSDK" });
    }
}
