using UnrealBuildTool;

public class AnimationCaptureHostEditorTarget : TargetRules
{
    public AnimationCaptureHostEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V5;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_6;
        ExtraModuleNames.Add("AnimationCaptureHost");
    }
}
