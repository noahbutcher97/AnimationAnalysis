#pragma once
#include "CoreMinimal.h"
#include "AnimationCapture/ViewportSurfaceCapture.h"
#include "ShowFlags.h"

class UWorld;
class UGameViewportClient;
class UStaticMeshComponent;
class APlayerController;
class ACameraActor;
class IConsoleVariable;

/** Neutral fixture shared by automation and interactive inspection. Never saves assets. */
class FAnimationCaptureHostFixture
{
public:
	~FAnimationCaptureHostFixture();
	bool Start(UWorld* World, FString& Error);
	void SetControl(int32 Index);
	void Stop();
	static UWorld* FindWorld();
	static const TCHAR* ControlName(int32 Index);
	FViewport* Viewport = nullptr;
	TWeakObjectPtr<UWorld> World;
	TArray<TWeakObjectPtr<UStaticMeshComponent>> Meshes;
private:
	FScopedSurfaceCaptureLabels Labels;
	TWeakObjectPtr<UGameViewportClient> Client;
	TWeakObjectPtr<APlayerController> Player;
	TWeakObjectPtr<AActor> PreviousTarget;
	TWeakObjectPtr<ACameraActor> Camera;
	FEngineShowFlags PreviousFlags{ESFIM_Game};
	int32 PreviousViewMode = 0;
	FIntPoint PreviousSize = FIntPoint::ZeroValue;
	bool PreviousFixedSize = false;
	IConsoleVariable* CustomDepth = nullptr;
	int32 PreviousCustomDepth = 0;
};

void RegisterAnimationCaptureHostCommands();
void UnregisterAnimationCaptureHostCommands();
