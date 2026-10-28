using UnrealBuildTool;

public class MRFPSGloves : ModuleRules
{
    public MRFPSGloves(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "DeveloperSettings" });
        PrivateDependencyModuleNames.AddRange(new[] { "Sockets", "Networking" });
    }
}
