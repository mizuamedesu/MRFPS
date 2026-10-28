using UnrealBuildTool;
using System.IO;

public class MoBitsSDK : ModuleRules
{
    public MoBitsSDK(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        // Generated code can include ModuleName/Public/Header.h when the
        // source plugin is outside the project's normal Plugins directory.
        PublicIncludePaths.Add(Path.GetFullPath(Path.Combine(ModuleDirectory, "..")));
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.AddRange(new[] { "Json", "Sockets", "ApplicationCore" });
    }
}
