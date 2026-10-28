using UnrealBuildTool;
public class MRFPSTarget : TargetRules
{
    public MRFPSTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
        ExtraModuleNames.Add("MRFPS");
    }
}
