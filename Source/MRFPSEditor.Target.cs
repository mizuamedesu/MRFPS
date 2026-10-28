using UnrealBuildTool;
public class MRFPSEditorTarget : TargetRules
{
    public MRFPSEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
        ExtraModuleNames.AddRange(new[] { "MRFPS", "MRFPSEditor" });
    }
}
