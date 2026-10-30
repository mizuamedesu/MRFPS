using UnrealBuildTool;
using System.IO;
public class MRFPS : ModuleRules
{
    public MRFPS(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        // Per-file helpers in the video modules intentionally have internal
        // linkage. Keep translation units separate, with bounded compiler RAM.
        bUseUnity = false;
        PublicDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "InputCore", "NetCore",
            "HeadMountedDisplay", "XRBase", "UMG", "Slate", "SlateCore",
            "MRFPSHaptics", "MoBitsSDK", "MRFPSAuth", "MRFPSTransport", "PacketHandler", "OnlineSubsystemUtils"
        });
        PrivateDependencyModuleNames.AddRange(new[] { "HTTP", "Json", "AIModule", "NavigationSystem", "GameplayTasks", "Sockets", "SSL" });
        if (Target.Platform == UnrealTargetPlatform.Android) {
            PrivateIncludePaths.Add(Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", ".local", "mobits-connect-headers", "include")));
            PublicSystemLibraries.Add("dl");
        }
        AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
        if (Target.Type != TargetType.Server) {
            PrivateDependencyModuleNames.AddRange(new[] { "ImageCore", "ImageWrapper", "RenderCore", "RHI" });
            PrivateDependencyModuleNames.Add("MRFPSXR");
            PrivateDependencyModuleNames.Add("ProceduralMeshComponent");
            PrivateDependencyModuleNames.Add("PBIK");
            RuntimeDependencies.Add("$(EngineDir)/Content/Slate/Fonts/DroidSansFallback.ttf", StagedFileType.UFS);
        }
    }
}
