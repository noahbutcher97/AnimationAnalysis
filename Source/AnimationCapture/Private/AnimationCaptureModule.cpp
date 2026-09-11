#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

class FAnimationCaptureModule : public IModuleInterface
{
public:
	void StartupModule() override
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AnimationAnalysis"));
		check(Plugin);
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/AnimationAnalysis"), FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
	}
	bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FAnimationCaptureModule, AnimationCapture)
