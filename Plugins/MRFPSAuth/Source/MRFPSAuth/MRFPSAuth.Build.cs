using UnrealBuildTool;

public class MRFPSAuth : ModuleRules
{
    public MRFPSAuth(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.Add("SSL");
        AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
    }
}
