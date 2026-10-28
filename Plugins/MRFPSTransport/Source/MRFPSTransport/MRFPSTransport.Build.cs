using UnrealBuildTool;

public class MRFPSTransport : ModuleRules
{
    public MRFPSTransport(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "PacketHandler" });
        PrivateDependencyModuleNames.AddRange(new[] { "NetCore", "DTLSHandlerComponent", "SSL" });
    }
}
