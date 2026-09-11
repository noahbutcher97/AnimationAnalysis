#include "AnimationCaptureHostFixture.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Slate/SceneViewport.h"
#include "UnrealClient.h"

FAnimationCaptureHostFixture::~FAnimationCaptureHostFixture() { Stop(); }
UWorld* FAnimationCaptureHostFixture::FindWorld()
{
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* Candidate = Context.World();
		if (Candidate && (Candidate->WorldType == EWorldType::PIE || Candidate->WorldType == EWorldType::Game)
			&& Candidate->GetFirstPlayerController() && Candidate->GetGameViewport()) { return Candidate; }
	}
	return nullptr;
}
const TCHAR* FAnimationCaptureHostFixture::ControlName(int32 Index)
{
	static const TCHAR* Names[] = {TEXT("Separated"), TEXT("Touching"), TEXT("Intersecting"), TEXT("DepthOccluded"), TEXT("SceneOccluded")};
	return Index >= 0 && Index < 5 ? Names[Index] : TEXT("Invalid");
}
bool FAnimationCaptureHostFixture::Start(UWorld* InWorld, FString& Error)
{
	Stop();
	if (!InWorld || !InWorld->GetGameViewport() || !InWorld->GetFirstPlayerController()
		|| !InWorld->GetGameViewport()->Viewport) { Error = TEXT("Start PIE before inspecting the fixture"); return false; }
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Cube) { Error = TEXT("Engine cube unavailable"); return false; }
	World = InWorld; Client = InWorld->GetGameViewport(); Viewport = Client->Viewport;
	PreviousFlags = Client->EngineShowFlags; PreviousViewMode = Client->ViewModeIndex;
	PreviousSize = Viewport->GetSizeXY(); PreviousFixedSize = Client->GetGameViewport()->HasFixedSize();
	Client->SetViewMode(VMI_Unlit); Client->EngineShowFlags.SetScreenPercentage(false);
	Client->EngineShowFlags.SetAntiAliasing(false); Client->EngineShowFlags.SetLighting(false);
	Client->GetGameViewport()->SetFixedViewportSize(640, 480);
	CustomDepth = IConsoleManager::Get().FindConsoleVariable(TEXT("r.CustomDepth"));
	PreviousCustomDepth = CustomDepth->GetInt(); CustomDepth->SetWithCurrentPriority(3);
	Player = InWorld->GetFirstPlayerController(); PreviousTarget = Player->GetViewTarget();
	Camera = InWorld->SpawnActor<ACameraActor>(FVector(0, 0, 10000), FRotator::ZeroRotator);
	Camera->GetCameraComponent()->FieldOfView = 60;
	Camera->GetCameraComponent()->bConstrainAspectRatio = false; Player->SetViewTarget(Camera.Get());
	for (int32 Index = 0; Index < 3; ++Index)
	{
		AActor* Actor = InWorld->SpawnActor<AActor>();
		auto* Mesh = NewObject<UStaticMeshComponent>(Actor); Actor->SetRootComponent(Mesh);
		Mesh->SetStaticMesh(Cube); Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->RegisterComponent(); Meshes.Add(Mesh);
	}
	TArray<FSurfaceCaptureLabel> Subjects = {{7, TEXT("FirstSolid"), Meshes[0]}, {23, TEXT("SecondSolid"), Meshes[1]}};
	if (!Labels.Apply(InWorld, Subjects, Error)) { Stop(); return false; }
	SetControl(0); return true;
}
void FAnimationCaptureHostFixture::SetControl(int32 Index)
{
	if (Meshes.Num() != 3 || !Meshes[0].IsValid() || !Meshes[1].IsValid() || !Meshes[2].IsValid()) { return; }
	Meshes[0]->SetWorldLocation(FVector(400, -60, 10000));
	Meshes[1]->SetWorldLocation(FVector(400, 80, 10000));
	Meshes[2]->SetWorldLocation(FVector(200, 500, 10000)); Meshes[2]->SetWorldScale3D(FVector::OneVector);
	if (Index == 1) { Meshes[1]->SetWorldLocation(FVector(400, 40, 10000)); }
	if (Index == 2) { Meshes[1]->SetWorldLocation(FVector(425, 20, 10000)); }
	if (Index == 3) { Meshes[1]->SetWorldLocation(FVector(700, -105, 10000)); }
	if (Index == 4) { Meshes[2]->SetWorldLocation(FVector(250, 80, 10000)); Meshes[2]->SetWorldScale3D(FVector(1, 2, 2)); }
}
void FAnimationCaptureHostFixture::Stop()
{
	Labels.Restore();
	if (Player.IsValid() && PreviousTarget.IsValid()) { Player->SetViewTarget(PreviousTarget.Get()); }
	if (Client.IsValid())
	{
		Client->SetViewMode(EViewModeIndex(PreviousViewMode)); Client->EngineShowFlags = PreviousFlags;
		if (Client->GetGameViewport()) { Client->GetGameViewport()->SetFixedViewportSize(PreviousFixedSize ? PreviousSize.X : 0, PreviousFixedSize ? PreviousSize.Y : 0); }
	}
	if (CustomDepth) { CustomDepth->SetWithCurrentPriority(PreviousCustomDepth); CustomDepth = nullptr; }
	for (auto Mesh : Meshes) { if (Mesh.IsValid() && Mesh->GetOwner()) { Mesh->GetOwner()->Destroy(); } }
	if (Camera.IsValid()) { Camera->Destroy(); }
	Meshes.Reset(); Camera.Reset(); Player.Reset(); PreviousTarget.Reset(); Client.Reset(); World.Reset(); Viewport = nullptr;
}
namespace
{
TUniquePtr<FAnimationCaptureHostFixture> Inspection;
IConsoleObject* InspectCommand = nullptr;
IConsoleObject* StopCommand = nullptr;
FDelegateHandle CleanupHandle;
}
void RegisterAnimationCaptureHostCommands()
{
	InspectCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("AnimationAnalysis.Host.Inspect"),
		TEXT("In PIE: inspect control 0=separated 1=touching 2=intersecting 3=label-occluded 4=scene-occluded"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const int32 Index = Args.IsEmpty() ? 0 : FCString::Atoi(*Args[0]);
			if (Index < 0 || Index > 4) { UE_LOG(LogTemp, Error, TEXT("Control must be 0..4")); return; }
			if (!Inspection)
			{
				Inspection = MakeUnique<FAnimationCaptureHostFixture>(); FString Error;
				if (!Inspection->Start(FAnimationCaptureHostFixture::FindWorld(), Error)) { UE_LOG(LogTemp, Error, TEXT("%s"), *Error); Inspection.Reset(); return; }
			}
			Inspection->SetControl(Index); UE_LOG(LogTemp, Display, TEXT("Inspecting %s"), FAnimationCaptureHostFixture::ControlName(Index));
		}), ECVF_Default);
	StopCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("AnimationAnalysis.Host.Stop"), TEXT("Restore and remove the neutral inspection fixture"),
		FConsoleCommandDelegate::CreateLambda([] { Inspection.Reset(); }), ECVF_Default);
	CleanupHandle = FWorldDelegates::OnWorldCleanup.AddLambda([](UWorld* World, bool, bool)
	{
		if (Inspection && Inspection->World.Get() == World) { Inspection.Reset(); }
	});
}
void UnregisterAnimationCaptureHostCommands()
{
	Inspection.Reset(); FWorldDelegates::OnWorldCleanup.Remove(CleanupHandle);
	IConsoleManager::Get().UnregisterConsoleObject(InspectCommand); IConsoleManager::Get().UnregisterConsoleObject(StopCommand);
}
