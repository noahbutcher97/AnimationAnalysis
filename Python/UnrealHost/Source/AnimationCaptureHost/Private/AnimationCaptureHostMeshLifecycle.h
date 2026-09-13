#pragma once
#include "Engine/GameViewportClient.h"
#include "Slate/SceneViewport.h"

namespace
{
struct FMeshGPURetirementFixture
{
	FMeshFixture Fixture;
	TSharedRef<FAnimationCaptureBudget, ESPMode::ThreadSafe> Budget = MakeShared<FAnimationCaptureBudget, ESPMode::ThreadSafe>(FAnimationCaptureBudgetLimits{16, 32ll * 1024 * 1024});
	TUniquePtr<FAnimationCaptureMeshGPU> GPU;
	TUniquePtr<FViewportAsyncCapture> Diagnostic;
	FAnimationMeshGPUTicket Ticket;
	IConsoleVariable* Cache = nullptr; int32 OldCache = 0;
	FString Directory;
	FDelegateHandle CleanupHandle;
	bool bWorldCleaned = false;
	TArray<TSharedPtr<FJsonValue>> Rows;
	~FMeshGPURetirementFixture()
	{
		FWorldDelegates::OnWorldCleanup.Remove(CleanupHandle);
		GPU.Reset(); Diagnostic.Reset(); Fixture.Stop();
		if (Cache) { Cache->SetWithCurrentPriority(OldCache); }
	}
	bool Start(FString& Error)
	{
		auto* World = FAnimationCaptureHostFixture::FindWorld(); if (!World) { return false; }
		Cache = IConsoleManager::Get().FindConsoleVariable(TEXT("r.SkinCache.Mode")); OldCache = Cache->GetInt(); Cache->SetWithCurrentPriority(1);
		if (!Fixture.Start(World, Error)) { return false; }
		CleanupHandle = FWorldDelegates::OnWorldCleanup.AddLambda([this, World](UWorld* Cleaned, bool, bool) { if (Cleaned == World) { bWorldCleaned = true; } });
		Fixture.Mesh->SkinCacheUsage.Init(ESkinCacheUsage::Enabled, 2); Fixture.Mesh->MarkRenderStateDirty();
		Diagnostic = MakeUnique<FViewportAsyncCapture>(World, Fixture.View.Viewport, true);
		Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Observations") / (TEXT("GpuMeshLifecycle-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		IFileManager::Get().MakeDirectory(*Directory, true); return true;
	}
	bool Enroll(FString& Error)
	{
		if (!Diagnostic) { Diagnostic = MakeUnique<FViewportAsyncCapture>(Fixture.View.World.Get(), Fixture.View.Viewport, true); }
		GPU = FAnimationCaptureMeshGPU::Create(Fixture.View.World.Get(), Fixture.View.Viewport, Fixture.Mesh,
			Enrollment(Fixture.Mesh, TEXT("retirement")), ReferenceLimits(), {}, Budget, Error);
		return GPU.IsValid() && GPU->Request(TEXT("retained-copy"), Ticket, Error);
	}
	void Record(const FString& Action, int64 ChargedBefore, const FAnimationMeshGPUResult& Result)
	{
		const auto Stats = GPU->GetStats(); auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("action"), Action); Row->SetNumberField(TEXT("charged_before"), double(ChargedBefore));
		Row->SetNumberField(TEXT("status"), int32(Result.Status)); Row->SetStringField(TEXT("error"), Result.Error);
		Row->SetNumberField(TEXT("shutdown_ms"), Stats.ShutdownSeconds * 1000); Row->SetNumberField(TEXT("peak_source_bytes"), double(Stats.PeakSourceBytes));
		Row->SetNumberField(TEXT("peak_staging_bytes"), double(Stats.PeakStagingBytes)); Rows.Add(MakeShared<FJsonValueObject>(Row));
		auto Report = MakeShared<FJsonObject>(); Report->SetArrayField(TEXT("controls"), Rows);
		FString Json; FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json)); FFileHelper::SaveStringToFile(Json, *(Directory / TEXT("lifecycle.json")));
	}
};
class FMeshGPURetirementControl : public IAutomationLatentCommand
{
public:
	FMeshGPURetirementControl(FAutomationTestBase* T, TSharedRef<FMeshGPURetirementFixture> F, bool World)
		: Test(T), State(F), bWorldCleanup(World), Started(FPlatformTime::Seconds()) {}
	bool Update() override
	{
		if (FPlatformTime::Seconds() - Started > 40) { Test->AddError(TEXT("Mesh retirement setup deadline")); return true; }
		FString Error;
		if (!Ready)
		{
			if (!FAnimationCaptureHostFixture::FindWorld()) { return false; }
			if (!State->Start(Error)) { Test->AddError(Error); return true; } Ready = true; return false;
		}
		if (++Warm < 8) { return false; }
		if (!State->GPU)
		{
			if (!State->Enroll(Error)) { Test->AddError(Error); return true; } return false;
		}
		if (!Resizing && State->GPU->GetStats().SourceBytes == 0)
		{
			FAnimationMeshGPUResult Failed;
			if (State->GPU->Collect(Failed)) { Test->AddError(TEXT("Expected a queued GPU copy: ") + Failed.Error); return true; }
			return false; // Deliberately never pump: ownership must survive unpolled, already queued GPU work.
		}
		if (bWorldCleanup) { return true; } // The next latent command performs real EndPIE/world cleanup.
		const int64 Charged = State->Budget->GetStats().LiveBytes;
		if (Action == 0)
		{
			Test->TestTrue(TEXT("Scheduled copy cancellation succeeds"), State->GPU->Cancel(State->Ticket, TEXT("cancel-after-scheduling")));
		}
		else if (Action == 1)
		{
			if (!Resizing)
			{
				State->Fixture.View.World->GetGameViewport()->GetGameViewport()->SetFixedViewportSize(641, 480);
				Resizing = true; return false;
			}
			State->GPU->Pump();
		}
		else { State->Fixture.Actor->Destroy(); State->GPU->Pump(); }
		FAnimationMeshGPUResult Result, Duplicate;
		if (!State->GPU->Collect(Result)) { return false; }
		Test->TestTrue(TEXT("Scheduled invalidation is cancelled exactly once"), Result.Status == EAnimationMeshGPUStatus::Cancelled && !Result.Snapshot);
		Test->TestFalse(TEXT("Scheduled cancellation does not collect twice"), State->GPU->Collect(Duplicate));
		if (Action == 0)
		{
			Result.Reservation.Reset();
			Test->TestTrue(TEXT("Collection and consumer release retain submitted resources"), State->GPU->GetStats().SourceBytes > 0 && State->Budget->GetStats().LiveBytes > 0);
		}
		State->GPU->Shutdown();
		State->Record(Action == 0 ? TEXT("scheduled-cancel") : Action == 1 ? TEXT("resize") : TEXT("component-destruction"), Charged, Result);
		Result = {}; State->GPU.Reset();
		Test->TestEqual(TEXT("Stale GPU ticket retains no retired payload"), State->Budget->GetStats().LiveBytes, int64(0));
		if (Action == 1)
		{
			State->Fixture.View.World->GetGameViewport()->GetGameViewport()->SetFixedViewportSize(640, 480);
			State->Diagnostic.Reset(); Resizing = false; // Its own resize contract closed the old diagnostic adapter.
		}
		if (++Action == 3) { return true; } Warm = 0; return false;
	}
private:
	FAutomationTestBase* Test; TSharedRef<FMeshGPURetirementFixture> State;
	bool bWorldCleanup, Ready = false, Resizing = false; double Started; int32 Warm = 0, Action = 0;
};
class FMeshGPUAfterWorldCleanup : public IAutomationLatentCommand
{
public:
	FMeshGPUAfterWorldCleanup(FAutomationTestBase* T, TSharedRef<FMeshGPURetirementFixture> F) : Test(T), State(F) {}
	bool Update() override
	{
		if (!State->bWorldCleaned)
		{
			if (++WaitFrames < 300) { return false; } Test->AddError(TEXT("Real world cleanup notification did not arrive")); return true;
		}
		if (!State->GPU) { Test->AddError(TEXT("World cleanup producer was not prepared")); return true; }
		FAnimationMeshGPUResult Result, Duplicate;
		Test->TestTrue(TEXT("World cleanup emits queued request result"), State->GPU->Collect(Result));
		Test->TestTrue(TEXT("Real world cleanup cancels rather than rebinding"), Result.Status == EAnimationMeshGPUStatus::Cancelled && !Result.Snapshot);
		Test->TestFalse(TEXT("World cleanup is exactly once"), State->GPU->Collect(Duplicate));
		State->Record(TEXT("world-cleanup"), State->Budget->GetStats().LiveBytes, Result);
		Result = {}; State->GPU.Reset();
		Test->TestEqual(TEXT("World teardown retires every allocation despite stale ticket"), State->Budget->GetStats().LiveBytes, int64(0)); return true;
	}
private:
	FAutomationTestBase* Test; TSharedRef<FMeshGPURetirementFixture> State;
	int32 WaitFrames = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshGPULifecycleTest, "AnimationAnalysis.Capture.Mesh.GPULifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMeshGPULifecycleTest::RunTest(const FString&)
{
	auto State = MakeShared<FMeshGPURetirementFixture>();
	ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Engine/Maps/Entry"))); ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false)); ADD_LATENT_AUTOMATION_COMMAND(FMeshGPURetirementControl(this, State, false));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand()); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshGPUWorldCleanupTest, "AnimationAnalysis.Capture.Mesh.GPUWorldCleanup", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMeshGPUWorldCleanupTest::RunTest(const FString&)
{
	auto State = MakeShared<FMeshGPURetirementFixture>();
	ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Engine/Maps/Entry"))); ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false)); ADD_LATENT_AUTOMATION_COMMAND(FMeshGPURetirementControl(this, State, true));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand()); ADD_LATENT_AUTOMATION_COMMAND(FMeshGPUAfterWorldCleanup(this, State)); return true;
}
