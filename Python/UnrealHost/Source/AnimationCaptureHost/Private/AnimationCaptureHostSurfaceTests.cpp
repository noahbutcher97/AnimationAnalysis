#include "AnimationCaptureHostFixture.h"
#include "AnimationCapture/ViewportSurfaceCapture.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealClient.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHostLabelOwnershipTest, "AnimationAnalysis.Capture.Surfaces.LabelOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHostLabelOwnershipTest::RunTest(const FString&)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	AActor* Actor = World->SpawnActor<AActor>();
	auto* A = NewObject<UStaticMeshComponent>(Actor); A->RegisterComponent();
	auto* B = NewObject<UStaticMeshComponent>(Actor); B->RegisterComponent();
	A->SetCustomDepthStencilValue(27); A->SetCustomDepthStencilWriteMask(ERendererStencilMask::ERSM_1);
	FScopedSurfaceCaptureLabels Scope; FString Error;
	TArray<FSurfaceCaptureLabel> Labels = {{5, TEXT("First"), A}, {5, TEXT("Second"), B}};
	TestFalse(TEXT("Duplicate IDs reject without mutation"), Scope.Apply(World, Labels, Error));
	TestEqual(TEXT("Original value retained"), A->CustomDepthStencilValue, 27);
	Labels[1].Id = 9;
	TestTrue(TEXT("Explicit labels accepted"), Scope.Apply(World, Labels, Error));
	FScopedSurfaceCaptureLabels Other;
	TestFalse(TEXT("Scope cannot be reacquired"), Scope.Apply(World, Labels, Error));
	TestFalse(TEXT("Another owner cannot acquire the same primitive"), Other.Apply(World, Labels, Error));
	B->DestroyComponent(); Scope.Restore(); Scope.Restore();
	TestFalse(TEXT("Original flag restored"), bool(A->bRenderCustomDepth));
	TestEqual(TEXT("Original stencil restored"), A->CustomDepthStencilValue, 27);
	TestEqual(TEXT("Original mask restored"), A->CustomDepthStencilWriteMask, ERendererStencilMask::ERSM_1);
	World->DestroyWorld(false); return true;
}

namespace
{
bool SaveJson(const TSharedRef<FJsonObject>& Object, const FString& Path)
{
	FString Text; FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text));
	return FFileHelper::SaveStringToFile(Text, *Path);
}

class FHostSurfaceControls : public IAutomationLatentCommand
{
public:
	explicit FHostSurfaceControls(FAutomationTestBase* InTest, bool InPerformance = false) : Test(InTest), Performance(InPerformance) {}
	~FHostSurfaceControls() { Cleanup(); }
	bool Update() override
	{
		if (!StartWall) { StartWall = FPlatformTime::Seconds(); }
		if (Done) { Cleanup(); return true; }
		if (FPlatformTime::Seconds() - StartWall > (Performance ? 240 : 80)) { Test->AddError(TEXT("Rendered host deadline exceeded")); Cleanup(); return true; }
		if (!Reader)
		{
			UWorld* World = FAnimationCaptureHostFixture::FindWorld(); if (!World) { return false; }
			FString Error;
			if (!Fixture.Start(World, Error)) { Test->AddError(Error); return true; }
			Reader = MakeUnique<FViewportSurfaceCapture>(World, Fixture.Viewport, true);
			Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Observations") /
				(Performance ? TEXT("Performance-") : TEXT("Surfaces-")) + FGuid::NewGuid().ToString(EGuidFormats::Digits));
			IFileManager::Get().MakeDirectory(*Directory, true);
			DrawHandle = UGameViewportClient::OnViewportRendered().AddRaw(this, &FHostSurfaceControls::Draw);
			return false;
		}
		if (!Waiting && ++Warmup >= (Performance ? 60 : 8))
		{
			if (Performance && Mode == 0) { Waiting = true; return false; }
			FString Error;
			if (!Reader->Request(Error)) { Test->AddError(Error); Done = true; }
			else { Waiting = true; if (!Performance) { Test->TestFalse(TEXT("Duplicate pending request rejected"), Reader->Request(Error)); } }
		}
		return false;
	}
private:
	void Draw(FViewport* Viewport)
	{
		if (Done || Viewport != Fixture.Viewport) { return; }
		const double Begin = FPlatformTime::Seconds();
		const double Delta = PreviousDraw ? Begin - PreviousDraw : 0; PreviousDraw = Begin;
		if (!Waiting) { return; }
		FViewportSurfaceFrame Frame; TArray<FColor> RGB; FString Error;
		if (!Performance && !StaleChecked)
		{
			Test->TestFalse(TEXT("Different viewport rejected"), Reader->Collect(nullptr, GFrameCounter, Frame, RGB, Error));
			const bool Result = Reader->Collect(Viewport, GFrameCounter + 1, Frame, RGB, Error);
			if (!Result && Error.IsEmpty()) { return; }
			Test->TestFalse(TEXT("Different expected frame rejected"), Result);
			Test->TestTrue(TEXT("Frame mismatch explained"), Error.Contains(TEXT("frame identity")));
			StaleChecked = true; Waiting = false; Warmup = 0; return;
		}
		if (!(Performance && Mode == 0) && !Reader->Collect(Viewport, GFrameCounter, Frame, RGB, Error))
		{
			if (!Error.IsEmpty()) { Test->AddError(Error); Done = true; } return;
		}
		const double Acquisition = FPlatformTime::Seconds() - Begin;
		Waiting = false;
		if (Performance)
		{
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("mode"), Mode == 0 ? TEXT("disabled") : TEXT("synchronous"));
			Row->SetNumberField(TEXT("repetition"), Repetition); Row->SetNumberField(TEXT("sample"), Sample);
			Row->SetNumberField(TEXT("engine_frame"), double(GFrameCounter)); Row->SetNumberField(TEXT("frame_wall_s"), Delta);
			Row->SetNumberField(TEXT("acquisition_wall_s"), Acquisition); Row->SetNumberField(TEXT("depth_readback_wall_s"), Frame.ReadbackSeconds);
			Row->SetNumberField(TEXT("completion_latency_s"), Acquisition); Row->SetNumberField(TEXT("pending_requests"), 0);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			if (++Sample == 120)
			{
				Sample = 0; Warmup = 0; PreviousDraw = 0;
				if (++Mode == 2) { Mode = 0; ++Repetition; }
				if (Repetition == 3)
				{
					auto Manifest = MakeShared<FJsonObject>(); Manifest->SetArrayField(TEXT("samples"), Rows);
					Manifest->SetStringField(TEXT("output_policy"), TEXT("readback_only_no_encoding_or_export_in_samples"));
					Manifest->SetNumberField(TEXT("width"), 640); Manifest->SetNumberField(TEXT("height"), 480);
					Manifest->SetNumberField(TEXT("warmup_draws"), 60); Manifest->SetStringField(TEXT("cadence"), TEXT("one_request_per_automation_update"));
					Test->TestTrue(TEXT("Raw performance samples retained"), SaveJson(Manifest, Directory / TEXT("performance.json")));
					Test->AddInfo(TEXT("PERFORMANCE_OUTPUT=") + Directory); Done = true;
				}
			}
			return;
		}
		Test->TestEqual(TEXT("Renderer frame agrees"), Frame.EngineFrame, GFrameCounter);
		Test->TestEqual(TEXT("640x480 dimensions"), Frame.Size, FIntPoint(640, 480));
		const int32 Pixels = 640 * 480;
		if (Frame.Labels.Num() != Pixels || Frame.SceneDepthCm.Num() != Pixels || Frame.LabelDepthCm.Num() != Pixels || RGB.Num() != Pixels)
		{ Test->AddError(TEXT("Incomplete surface planes")); Done = true; return; }
		int32 First = 0, Second = 0, Front = 0, VisibleSecond = 0, Intersection = 0, Union = 0;
		TArray<FIntPoint> FirstBoundary, SecondBoundary;
		for (int32 Y = 0; Y < 480; ++Y)
		{
			int32 RightFirst = -1, LeftSecond = 640;
			for (int32 X = 0; X < 640; ++X)
			{
				const int32 I = Y * 640 + X;
				const bool A = Frame.Labels[I] == 7, B = Frame.Labels[I] == 23;
				First += A; Second += B; Front += A && FMath::IsNearlyEqual(Frame.LabelDepthCm[I], 350.f, 1.f);
				VisibleSecond += B && Frame.LabelDepthCm[I] <= Frame.SceneDepthCm[I] + 1.f;
				const bool Foreground = FMath::Max3(RGB[I].R, RGB[I].G, RGB[I].B) > 8;
				Intersection += Foreground && (A || B); Union += Foreground || A || B;
				if (A) { RightFirst = X; } if (B) { LeftSecond = FMath::Min(LeftSecond, X); }
			}
			if (RightFirst >= 0) { FirstBoundary.Add(FIntPoint(RightFirst, Y)); }
			if (LeftSecond < 640) { SecondBoundary.Add(FIntPoint(LeftSecond, Y)); }
		}
		Test->TestTrue(TEXT("First subject rasterized"), First > 100);
		Test->TestTrue(TEXT("Known 350cm front plane"), Front > 100);
		if (Control == 3) { Test->TestEqual(TEXT("Label occlusion remains absent"), Second, 0); }
		else { Test->TestTrue(TEXT("Second custom-depth label exists"), Second > 100); }
		if (Control == 4) { Test->TestEqual(TEXT("Scene-occluded label is not visible"), VisibleSecond, 0); }
		const double IoU = Union ? double(Intersection) / Union : 0;
		if (Control < 4) { Test->TestTrue(TEXT("Independent RGB/label silhouette IoU >= 0.99"), IoU >= .99); }
		double Gap = TNumericLimits<double>::Max();
		for (FIntPoint A : FirstBoundary) { for (FIntPoint B : SecondBoundary) { Gap = FMath::Min(Gap, FMath::Sqrt(double((A - B).SizeSquared()))); } }
		if (Control < 3) { const double Expected[] = {66, 1, 1}; Test->TestEqual(TEXT("Fixture pixel-centre gap"), Gap, Expected[Control]); }
		TArray<uint8> Bytes;
		const ANSICHAR Magic[] = "SURFACE1"; Bytes.Append(reinterpret_cast<const uint8*>(Magic), 8);
		Bytes.Append(reinterpret_cast<const uint8*>(&Frame.EngineFrame), 8);
		uint32 Width = 640, Height = 480;
		Bytes.Append(reinterpret_cast<const uint8*>(&Width), 4); Bytes.Append(reinterpret_cast<const uint8*>(&Height), 4);
		Bytes.Append(reinterpret_cast<const uint8*>(RGB.GetData()), Pixels * 4); Bytes.Append(Frame.Labels);
		Bytes.Append(reinterpret_cast<const uint8*>(Frame.SceneDepthCm.GetData()), Pixels * 4);
		Bytes.Append(reinterpret_cast<const uint8*>(Frame.LabelDepthCm.GetData()), Pixels * 4);
		const FString File = FString::Printf(TEXT("observation_%02d.surface"), Control);
		Test->TestTrue(TEXT("Replay bundle saved"), FFileHelper::SaveArrayToFile(Bytes, *(Directory / File)));
		auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("control"), Fixture.ControlName(Control));
		Row->SetStringField(TEXT("file"), File); Row->SetNumberField(TEXT("engine_frame"), double(Frame.EngineFrame));
		Row->SetNumberField(TEXT("simulation_time_s"), Fixture.World->GetTimeSeconds());
		Row->SetNumberField(TEXT("width"), 640); Row->SetNumberField(TEXT("height"), 480);
		Row->SetNumberField(TEXT("readback_wall_s"), Frame.ReadbackSeconds); Row->SetNumberField(TEXT("rgb_label_iou"), IoU);
		TArray<TSharedPtr<FJsonValue>> Matrix;
		for (int32 R = 0; R < 4; ++R) { for (int32 C = 0; C < 4; ++C) { Matrix.Add(MakeShared<FJsonValueNumber>(Frame.WorldToClip.M[R][C])); } }
		Row->SetArrayField(TEXT("world_to_clip_row_major"), Matrix); Rows.Add(MakeShared<FJsonValueObject>(Row));
		if (++Control == 5)
		{
			auto Manifest = MakeShared<FJsonObject>(); Manifest->SetNumberField(TEXT("schema_version"), 1);
			Manifest->SetStringField(TEXT("backend"), TEXT("D3D11_D32F_S8"));
			Manifest->SetStringField(TEXT("depth_convention"), TEXT("camera_axis_cm_clear_infinity"));
			Manifest->SetStringField(TEXT("label_semantics"), TEXT("frontmost_custom_depth"));
			Manifest->SetStringField(TEXT("render_policy"), TEXT("perspective_no_aa_no_screen_percentage_unlit_control"));
			auto Subjects = MakeShared<FJsonObject>(); Subjects->SetNumberField(TEXT("FirstSolid"), 7); Subjects->SetNumberField(TEXT("SecondSolid"), 23);
			Manifest->SetObjectField(TEXT("subjects"), Subjects); Manifest->SetArrayField(TEXT("frames"), Rows);
			Test->TestTrue(TEXT("Surface manifest saved"), SaveJson(Manifest, Directory / TEXT("surfaces.json")));
			Test->AddInfo(TEXT("SURFACE_OBSERVATION_OUTPUT=") + Directory); Done = true;
		}
		else { Fixture.SetControl(Control); Warmup = 0; }
	}
	void Cleanup()
	{
		UGameViewportClient::OnViewportRendered().Remove(DrawHandle); Reader.Reset(); Fixture.Stop();
	}
	FAutomationTestBase* Test;
	FAnimationCaptureHostFixture Fixture;
	TUniquePtr<FViewportSurfaceCapture> Reader;
	FDelegateHandle DrawHandle;
	FString Directory;
	TArray<TSharedPtr<FJsonValue>> Rows;
	bool Performance = false, Waiting = false, Done = false, StaleChecked = false;
	int32 Warmup = 0, Control = 0, Mode = 0, Repetition = 0, Sample = 0;
	double StartWall = 0, PreviousDraw = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHostSurfaceGeometryTest, "AnimationAnalysis.Capture.Surfaces.RenderedGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHostSurfaceGeometryTest::RunTest(const FString&)
{
	if (!FApp::CanEverRender()) { AddWarning(TEXT("ANIMATION_ANALYSIS_RENDERED_SKIP: D3D11 required")); return true; }
	ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Engine/Maps/Entry")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FHostSurfaceControls(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand()); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHostSurfacePerformanceTest, "AnimationAnalysis.Capture.Surfaces.Performance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHostSurfacePerformanceTest::RunTest(const FString&)
{
	if (!FApp::CanEverRender()) { AddWarning(TEXT("ANIMATION_ANALYSIS_RENDERED_SKIP: D3D11 required")); return true; }
	ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Engine/Maps/Entry")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FHostSurfaceControls(this, true));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand()); return true;
}
