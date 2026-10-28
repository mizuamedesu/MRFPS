using UnrealBuildTool;
public class MRFPSServerTarget : TargetRules
{
    public MRFPSServerTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Server;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
        ExtraModuleNames.Add("MRFPS");
    }
}
