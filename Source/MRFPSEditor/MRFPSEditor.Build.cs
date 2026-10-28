using UnrealBuildTool;
public class MRFPSEditor : ModuleRules
{
    public MRFPSEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "UnrealEd", "AssetRegistry", "AssetTools", "MRFPS"
        });
    }
}
