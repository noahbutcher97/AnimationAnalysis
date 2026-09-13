using UnrealBuildTool;

public class AnimationCaptureHost : ModuleRules
{
    public AnimationCaptureHost(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false;
        PrivateDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "Json", "AnimationCapture", "UnrealEd", "Slate", "SlateCore", "RHI", "RenderCore" });
        PrivateDependencyModuleNames.AddRange(new[] { "AnimationCore", "AnimationDataController", "AnimGraph", "AnimationBlueprintEditor", "BlueprintGraph", "MeshDescription", "StaticMeshDescription", "SkeletalMeshDescription", "SkeletalMeshUtilitiesCommon" });
    }
}
