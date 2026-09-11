#include "Modules/ModuleManager.h"
#include "AnimationCaptureHostFixture.h"
void RegisterAnimationCaptureMeshHostCommands();
void UnregisterAnimationCaptureMeshHostCommands();

class FAnimationCaptureHostModule : public IModuleInterface
{
public:
	void StartupModule() override { RegisterAnimationCaptureHostCommands(); RegisterAnimationCaptureMeshHostCommands(); }
	void ShutdownModule() override { UnregisterAnimationCaptureMeshHostCommands(); UnregisterAnimationCaptureHostCommands(); }
};
IMPLEMENT_MODULE(FAnimationCaptureHostModule, AnimationCaptureHost)
