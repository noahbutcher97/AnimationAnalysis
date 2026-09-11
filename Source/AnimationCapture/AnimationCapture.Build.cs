using UnrealBuildTool;

public class AnimationCapture : ModuleRules
{
    public AnimationCapture(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "Json" });
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "UnrealEd", "ImageCore", "ImageWrapper", "Renderer", "RenderCore", "RHI"
        });
    }
}
