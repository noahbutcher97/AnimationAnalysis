#include "Modules/ModuleManager.h"
#include "AnimationCaptureHostFixture.h"

class FAnimationCaptureHostModule : public IModuleInterface
{
public:
	void StartupModule() override { RegisterAnimationCaptureHostCommands(); }
	void ShutdownModule() override { UnregisterAnimationCaptureHostCommands(); }
};
IMPLEMENT_MODULE(FAnimationCaptureHostModule, AnimationCaptureHost)
