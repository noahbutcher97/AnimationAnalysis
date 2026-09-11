#include "AnimationCaptureHostFixture.h"
#include "Camera/CameraActor.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDeviceNull.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Slate/SceneViewport.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealClient.h"

namespace
{
TArray<TSharedPtr<FJsonValue>> PositionValues(const FVector& Position)
{
	return {MakeShared<FJsonValueNumber>(Position.X), MakeShared<FJsonValueNumber>(Position.Y),
		MakeShared<FJsonValueNumber>(Position.Z)};
}

class FHostInteractiveCommandControls : public IAutomationLatentCommand
{
public:
	explicit FHostInteractiveCommandControls(FAutomationTestBase* InTest) : Test(InTest) {}
	~FHostInteractiveCommandControls() { Cleanup(); }
	bool Update() override
	{
		if (!StartWall) { StartWall = FPlatformTime::Seconds(); }
		if (FPlatformTime::Seconds() - StartWall > 40)
		{
			Test->AddError(TEXT("Interactive-command fixture deadline exceeded")); SaveEvidence(); Cleanup(); return true;
		}
		if (Stage == 0)
		{
			UWorld* FoundWorld = FAnimationCaptureHostFixture::FindWorld(); if (!FoundWorld) { return false; }
			World = FoundWorld; Client = FoundWorld->GetGameViewport(); Player = FoundWorld->GetFirstPlayerController();
			if (!Client.IsValid() || !Client->GetGameViewport() || !Player.IsValid())
			{ Test->AddError(TEXT("Interactive-command test needs a real PIE viewport and controller")); return true; }
			OriginalTarget = Player->GetViewTarget(); PreviousFlags = Client->EngineShowFlags;
			PreviousViewMode = Client->ViewModeIndex; PreviousSize = Client->Viewport->GetSizeXY();
			bPreviousFixedSize = Client->GetGameViewport()->HasFixedSize();
			CustomDepth = IConsoleManager::Get().FindConsoleVariable(TEXT("r.CustomDepth"));
			if (!Test->TestNotNull(TEXT("Custom depth setting is available"), CustomDepth)) { return true; }
			PreviousCustomDepth = CustomDepth->GetInt();
			for (TActorIterator<AActor> It(FoundWorld); It; ++It) { BaselineActors.Add(*It); }
			Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Observations") /
				(TEXT("InteractiveCommands-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			DrawHandle = UGameViewportClient::OnViewportRendered().AddRaw(this, &FHostInteractiveCommandControls::Draw);
			bFixtureActive = true;
			if (!Execute(TEXT("AnimationAnalysis.Host.Inspect")) || !FindCommandActors()) { SaveEvidence(); Cleanup(); return true; }
			Stage = 1; return false;
		}
		if (Stage == 4)
		{
			if (FAnimationCaptureHostFixture::FindWorld()) { return false; }
			Test->TestEqual(TEXT("World cleanup restores the command's custom-depth setting"), CustomDepth->GetInt(), PreviousCustomDepth);
			for (const auto& Actor : CommandActors) { Test->TestFalse(TEXT("World cleanup destroys command-owned actors"), Actor.IsValid()); }
			Record(TEXT("world_cleanup")); SaveEvidence(); Cleanup(); return true;
		}
		if (!World.IsValid() || !Client.IsValid())
		{ Test->AddError(TEXT("PIE ended before command checks completed")); SaveEvidence(); Cleanup(); return true; }
		if (Draws < 2) { return false; }
		if (Stage == 1)
		{
			CheckControl(Control);
			if (++Control < 5)
			{
				Execute(FString::Printf(TEXT("AnimationAnalysis.Host.Inspect %d"), Control)); Draws = 0; return false;
			}
			// Out-of-range numeric input must be rejected without changing the scene.
			// Non-numeric input currently follows the command's existing Atoi behavior;
			// this test does not claim strict text parsing that is not implemented.
			Test->AddExpectedErrorPlain(TEXT("Control must be 0..4"), EAutomationExpectedErrorFlags::Contains, 2);
			Execute(TEXT("AnimationAnalysis.Host.Inspect -1")); Execute(TEXT("AnimationAnalysis.Host.Inspect 5"));
			CheckControl(4); Record(TEXT("out_of_range_rejected"));
			Execute(TEXT("AnimationAnalysis.Host.Stop")); bFixtureActive = false; Draws = 0; Stage = 2; return false;
		}
		if (Stage == 2)
		{
			CheckStopped(); Execute(TEXT("AnimationAnalysis.Host.Stop")); CheckStopped(); Record(TEXT("stop_and_repeat"));
			PreviousCommandActors = CommandActors;
			Execute(TEXT("AnimationAnalysis.Host.Inspect 2")); bFixtureActive = true;
			if (!FindCommandActors()) { SaveEvidence(); Cleanup(); return true; }
			for (const auto& Actor : PreviousCommandActors) { Test->TestFalse(TEXT("Restart does not revive a stopped fixture actor"), Actor.IsValid()); }
			Draws = 0; Stage = 3; return false;
		}
		if (Stage == 3)
		{
			CheckControl(2); Record(TEXT("restarted_after_stop"));
			// Use the ordinary editor route. The registered world-cleanup handler
			// owns restoration; do not call Stop before checking its effects.
			GEditor->RequestEndPlayMap(); Stage = 4; return false;
		}
		return false;
	}

private:
	bool Execute(const FString& Command)
	{
		FOutputDeviceNull Output;
		return Test->TestTrue(TEXT("Registered console route handles: ") + Command,
			IConsoleManager::Get().ProcessUserConsoleInput(*Command, Output, World.Get()));
	}
	bool FindCommandActors()
	{
		CommandActors.Reset(); First.Reset(); Second.Reset(); Occluder.Reset(); Camera.Reset();
		for (TActorIterator<AActor> It(World.Get()); It; ++It)
		{
			if (BaselineActors.Contains(*It)) { continue; }
			CommandActors.Add(*It);
			if (auto* ViewCamera = Cast<ACameraActor>(*It)) { Camera = ViewCamera; }
			if (auto* Mesh = It->FindComponentByClass<UStaticMeshComponent>())
			{
				if (Mesh->CustomDepthStencilValue == 7) { First = Mesh; }
				else if (Mesh->CustomDepthStencilValue == 23) { Second = Mesh; }
				else { Occluder = Mesh; }
			}
		}
		return Test->TestTrue(TEXT("Inspect command creates its camera and three neutral primitives"),
			CommandActors.Num() == 4 && Camera.IsValid() && First.IsValid() && Second.IsValid() && Occluder.IsValid());
	}
	void CheckControl(int32 Index)
	{
		if (!Test->TestTrue(TEXT("Changing a control preserves its owned actors"),
			Camera.IsValid() && First.IsValid() && Second.IsValid() && Occluder.IsValid())) { return; }
		const FVector SecondPositions[] = {{400, 80, 10000}, {400, 40, 10000}, {425, 20, 10000}, {700, -105, 10000}, {400, 80, 10000}};
		Test->TestTrue(TEXT("First primitive has its declared neutral position"), First->GetComponentLocation().Equals(FVector(400, -60, 10000), .01));
		Test->TestTrue(TEXT("Command selects the requested control geometry"), Second->GetComponentLocation().Equals(SecondPositions[Index], .01));
		Test->TestTrue(TEXT("Scene occluder follows the requested control"),
			Occluder->GetComponentLocation().Equals(Index == 4 ? FVector(250, 80, 10000) : FVector(200, 500, 10000), .01));
		Test->TestTrue(TEXT("Scene occluder scale follows the requested control"),
			Occluder->GetComponentScale().Equals(Index == 4 ? FVector(1, 2, 2) : FVector::OneVector, .01));
		Test->TestEqual(TEXT("Registered command controls the actual PIE view target"), Player->GetViewTarget(), static_cast<AActor*>(Camera.Get()));
		Test->TestTrue(TEXT("Explicit subject labels are active"), bool(First->bRenderCustomDepth) && bool(Second->bRenderCustomDepth));
		Test->TestEqual(TEXT("Rendered command view is 640x480"), Client->Viewport->GetSizeXY(), FIntPoint(640, 480));
		Test->TestEqual(TEXT("Command enables diagnostic custom depth"), CustomDepth->GetInt(), 3);
		Test->TestFalse(TEXT("Diagnostic command disables anti-aliasing"), bool(Client->EngineShowFlags.AntiAliasing));
		int32 NewActors = 0;
		for (TActorIterator<AActor> It(World.Get()); It; ++It) { NewActors += !BaselineActors.Contains(*It); }
		Test->TestEqual(TEXT("Switching controls does not leak fixture actors"), NewActors, 4);
		auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("action"), TEXT("inspect"));
		Row->SetNumberField(TEXT("control"), Index); Row->SetStringField(TEXT("name"), FAnimationCaptureHostFixture::ControlName(Index));
		Row->SetNumberField(TEXT("engine_frame"), double(LastDrawFrame)); Row->SetNumberField(TEXT("observed_draws"), Draws);
		Row->SetArrayField(TEXT("first_position_cm"), PositionValues(First->GetComponentLocation()));
		Row->SetArrayField(TEXT("second_position_cm"), PositionValues(Second->GetComponentLocation()));
		Row->SetArrayField(TEXT("occluder_position_cm"), PositionValues(Occluder->GetComponentLocation()));
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	void CheckStopped()
	{
		Test->TestEqual(TEXT("Stop restores the previous view target"), Player->GetViewTarget(), OriginalTarget.Get());
		Test->TestEqual(TEXT("Stop restores every previous show flag"), Client->EngineShowFlags.ToString(), PreviousFlags.ToString());
		Test->TestEqual(TEXT("Stop restores the previous view mode"), Client->ViewModeIndex, PreviousViewMode);
		Test->TestEqual(TEXT("Stop restores fixed-size ownership"), Client->GetGameViewport()->HasFixedSize(), bPreviousFixedSize);
		Test->TestEqual(TEXT("Stop restores the previous viewport size"), Client->Viewport->GetSizeXY(), PreviousSize);
		Test->TestEqual(TEXT("Stop restores the previous custom-depth setting"), CustomDepth->GetInt(), PreviousCustomDepth);
		for (const auto& Actor : CommandActors) { Test->TestFalse(TEXT("Stop destroys command-owned actors"), Actor.IsValid()); }
	}
	void Draw(FViewport* Viewport)
	{
		if (Client.IsValid() && Viewport == Client->Viewport) { ++Draws; LastDrawFrame = GFrameCounter; }
	}
	void Record(const FString& Action)
	{
		auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("action"), Action);
		Row->SetNumberField(TEXT("engine_frame"), double(GFrameCounter)); Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	void SaveEvidence()
	{
		if (Directory.IsEmpty()) { return; }
		auto Root = MakeShared<FJsonObject>(); Root->SetArrayField(TEXT("actions"), Rows);
		Root->SetStringField(TEXT("status"), Test->HasAnyErrors() ? TEXT("failed") : TEXT("complete"));
		Root->SetStringField(TEXT("scope"), TEXT("Registered console routes, PIE scene state, observed draws and restoration; no pixel/contact assertion"));
		FString Text; FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Text));
		IFileManager::Get().MakeDirectory(*Directory, true);
		Test->TestTrue(TEXT("Interactive-command observations are retained"), FFileHelper::SaveStringToFile(Text, *(Directory / TEXT("commands.json"))));
		Test->AddInfo(TEXT("INTERACTIVE_COMMAND_OUTPUT=") + Directory);
	}
	void Cleanup()
	{
		UGameViewportClient::OnViewportRendered().Remove(DrawHandle); DrawHandle.Reset();
		if (bFixtureActive)
		{
			FOutputDeviceNull Output; IConsoleManager::Get().ProcessUserConsoleInput(TEXT("AnimationAnalysis.Host.Stop"), Output, World.Get());
			bFixtureActive = false;
		}
	}

	FAutomationTestBase* Test;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<UGameViewportClient> Client;
	TWeakObjectPtr<APlayerController> Player;
	TWeakObjectPtr<AActor> OriginalTarget;
	TWeakObjectPtr<ACameraActor> Camera;
	TWeakObjectPtr<UStaticMeshComponent> First, Second, Occluder;
	TSet<AActor*> BaselineActors;
	TArray<TWeakObjectPtr<AActor>> CommandActors, PreviousCommandActors;
	FEngineShowFlags PreviousFlags{ESFIM_Game};
	FIntPoint PreviousSize = FIntPoint::ZeroValue;
	IConsoleVariable* CustomDepth = nullptr;
	FDelegateHandle DrawHandle;
	TArray<TSharedPtr<FJsonValue>> Rows;
	FString Directory;
	int32 PreviousCustomDepth = 0, PreviousViewMode = 0, Stage = 0, Control = 0, Draws = 0;
	uint64 LastDrawFrame = 0;
	double StartWall = 0;
	bool bPreviousFixedSize = false, bFixtureActive = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHostInteractiveCommandTest, "AnimationAnalysis.Capture.Host.InteractiveCommands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHostInteractiveCommandTest::RunTest(const FString&)
{
	if (!FApp::CanEverRender()) { AddWarning(TEXT("ANIMATION_ANALYSIS_RENDERED_SKIP: interactive commands require rendered PIE")); return true; }
	IConsoleObject* Inspect = IConsoleManager::Get().FindConsoleObject(TEXT("AnimationAnalysis.Host.Inspect"));
	IConsoleObject* Stop = IConsoleManager::Get().FindConsoleObject(TEXT("AnimationAnalysis.Host.Stop"));
	if (!TestNotNull(TEXT("Inspection console command is registered"), Inspect ? Inspect->AsCommand() : nullptr) ||
		!TestNotNull(TEXT("Stop console command is registered"), Stop ? Stop->AsCommand() : nullptr)) { return false; }
	ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Engine/Maps/Entry")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FHostInteractiveCommandControls(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand()); return true;
}
