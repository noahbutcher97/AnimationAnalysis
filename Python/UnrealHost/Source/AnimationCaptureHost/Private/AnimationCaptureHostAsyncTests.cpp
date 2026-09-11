#include "AnimationCaptureHostFixture.h"
#include "Misc/AutomationTest.h"
#include "Misc/App.h"

#if __has_include("AnimationCapture/ViewportAsyncCapture.h")
#include "AnimationCapture/ViewportAsyncCapture.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Slate/SceneViewport.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealClient.h"
#define ANIMATION_CAPTURE_HOST_HAS_ASYNC 1

namespace
{
bool SaveAsyncJson(const TSharedRef<FJsonObject>& Object, const FString& Path)
{
	FString Text;
	return FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text))
		&& FFileHelper::SaveStringToFile(Text, *Path);
}
FString AsyncDirectory(const TCHAR* Name)
{
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Observations")
		/ (FString(TEXT("Async-")) + Name + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*Directory, true);
	return Directory;
}
FAnimationCaptureReadbackRequest MakeAsyncRequest(const FAnimationCaptureHostFixture& Fixture,
	const FString& Session, uint64 RequestId)
{
	FAnimationCaptureReadbackRequest Request;
	Request.SessionId = Session;
	Request.RequestId = RequestId;
	Request.ViewportId = TEXT("NeutralFixtureViewport");
	Request.ViewportGeneration = 11;
	Request.ClockId = TEXT("NeutralWorldTime");
	Request.AcquisitionTimeSeconds = Fixture.World->GetTimeSeconds();
	Request.Size = FIntPoint(640, 480);
	Request.PoseRevisions = {{TEXT("FirstSolid"), RequestId * 10, GFrameCounter, false},
		{TEXT("SecondSolid"), RequestId * 10 + 1, GFrameCounter, false}};
	return Request;
}
TSharedRef<FJsonObject> AsyncResultIdentity(const FAnimationCaptureReadbackResult& Result)
{
	auto Row = MakeShared<FJsonObject>();
	Row->SetStringField(TEXT("session_id"), Result.Request.SessionId);
	Row->SetNumberField(TEXT("request_id"), double(Result.Request.RequestId));
	Row->SetStringField(TEXT("viewport_id"), Result.Request.ViewportId);
	Row->SetNumberField(TEXT("viewport_generation"), double(Result.Request.ViewportGeneration));
	Row->SetStringField(TEXT("request_clock_id"), Result.Request.ClockId);
	Row->SetNumberField(TEXT("request_time_s"), Result.Request.AcquisitionTimeSeconds);
	Row->SetNumberField(TEXT("simulation_time_s"), Result.View.WorldTimeSeconds);
	Row->SetNumberField(TEXT("renderer_world_time_s"), Result.View.WorldTimeSeconds);
	Row->SetNumberField(TEXT("renderer_real_time_s"), Result.View.RealTimeSeconds);
	Row->SetBoolField(TEXT("renderer_view_available"), Result.bHasView);
	Row->SetNumberField(TEXT("engine_frame"), double(Result.View.EngineFrame));
	Row->SetNumberField(TEXT("renderer_frame_number"), Result.View.RendererFrameNumber);
	Row->SetNumberField(TEXT("view_key"), Result.View.ViewKey);
	Row->SetNumberField(TEXT("completion_wall_s"), Result.CompletedWallSeconds);
	Row->SetNumberField(TEXT("collection_wall_s"), Result.CollectedWallSeconds);
	Row->SetNumberField(TEXT("completion_latency_s"), Result.CompletionLatencySeconds);
	Row->SetNumberField(TEXT("decode_wall_s"), Result.DecodeSeconds);
	Row->SetStringField(TEXT("error"), Result.Error);
	Row->SetNumberField(TEXT("terminal_status"), int32(Result.Status));
	TArray<TSharedPtr<FJsonValue>> Poses;
	for (const auto& Pose : Result.Request.PoseRevisions)
	{
		auto Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("subject_id"), Pose.SubjectId);
		Item->SetNumberField(TEXT("revision"), double(Pose.Revision));
		Item->SetNumberField(TEXT("engine_frame"), double(Pose.EngineFrame));
		Item->SetBoolField(TEXT("missing"), Pose.bMissing);
		Poses.Add(MakeShared<FJsonValueObject>(Item));
	}
	Row->SetArrayField(TEXT("pose_revisions"), Poses);
	return Row;
}
void AppendStats(const TSharedRef<FJsonObject>& Object, const FAnimationCaptureReadbackStats& Stats)
{
	Object->SetNumberField(TEXT("pending_requests"), Stats.PendingRequests);
	Object->SetNumberField(TEXT("reserved_bytes"), double(Stats.ReservedBytes));
	Object->SetNumberField(TEXT("peak_bytes"), double(Stats.PeakReservedBytes));
	Object->SetNumberField(TEXT("admitted"), double(Stats.Admitted));
	Object->SetNumberField(TEXT("completed"), double(Stats.Completed));
	Object->SetNumberField(TEXT("rejected"), double(Stats.Rejected));
	Object->SetNumberField(TEXT("cancelled"), double(Stats.Cancelled));
	Object->SetNumberField(TEXT("failed"), double(Stats.Failed));
	Object->SetNumberField(TEXT("timeouts"), double(Stats.TimedOut));
	Object->SetNumberField(TEXT("shutdown_wall_s"), Stats.ShutdownWallSeconds);
}

class FHostAsyncGeometry : public IAutomationLatentCommand
{
public:
	FHostAsyncGeometry(FAutomationTestBase* InTest, bool bInDelayed)
		: Test(InTest), bDelayed(bInDelayed) {}
	~FHostAsyncGeometry() { Cleanup(); }
	bool Update() override
	{
		if (!Started) { Started = FPlatformTime::Seconds(); }
		if (bDone) { Cleanup(); return true; }
		if (FPlatformTime::Seconds() - Started > 90)
		{
			Test->AddError(TEXT("Async rendered geometry exceeded its 90 second deadline"));
			Cleanup(); return true;
		}
		if (!Fixture.World.IsValid())
		{
			UWorld* World = FAnimationCaptureHostFixture::FindWorld();
			if (!World) { return false; }
			FString Error;
			if (!Fixture.Start(World, Error)) { Test->AddError(Error); return true; }
			Directory = AsyncDirectory(bDelayed ? TEXT("DelayedIdentity") : TEXT("Geometry"));
			DrawHandle = UGameViewportClient::OnViewportRendered().AddRaw(this, &FHostAsyncGeometry::Draw);
			return false;
		}
		if (!Reader)
		{
			if (WarmupDraws < 8) { return false; }
			Reader = MakeUnique<FViewportAsyncCapture>(Fixture.World.Get(), Fixture.Viewport, true);
		}
		if (bWaiting && bDelayed && DrawsSinceRequest > 0 && !bChangedSubject)
		{
			// The request and rendered command already own their values. Mutate the caller's
			// packet and the live subject before any completion polling.
			Submitted.ClockId = TEXT("ChangedAfterAcquisition");
			Submitted.AcquisitionTimeSeconds += 100;
			Submitted.PoseRevisions[0].Revision += 500;
			Fixture.Meshes[0]->SetWorldLocation(FVector(900, 300, 10000));
			Fixture.Meshes[0]->GetOwner()->Destroy();
			bChangedSubject = true;
		}
		if (bWaiting && (!bDelayed || DrawsSinceRequest >= 8))
		{
			FAnimationCaptureReadbackResult Result;
			if (Reader->Poll(Result))
			{
				bWaiting = false;
				Validate(Result);
				Test->TestFalse(TEXT("Successful request is collected exactly once"), Reader->Poll(Result));
				if (bDone) { FinishManifest(); return false; }
				Fixture.SetControl(Control); WarmupDraws = 0;
			}
		}
		if (!bWaiting && !bDone && WarmupDraws >= 8)
		{
			Submitted = MakeAsyncRequest(Fixture, Directory, uint64(Control + 1));
			Original = Submitted;
			FString Error;
			if (!Reader->Request(Submitted, Ticket, Error)) { Test->AddError(Error); bDone = true; FinishManifest(); }
			else { bWaiting = true; DrawsSinceRequest = 0; ExpectedFrame = 0; }
		}
		return false;
	}
private:
	void Draw(FViewport* Viewport)
	{
		if (Viewport != Fixture.Viewport || bDone) { return; }
		++WarmupDraws;
		if (bWaiting)
		{
			if (DrawsSinceRequest++ == 0) { ExpectedFrame = GFrameCounter; }
		}
	}
	void Validate(const FAnimationCaptureReadbackResult& Result)
	{
		if (!Test->TestTrue(*FString::Printf(TEXT("Async planes completed: %s"), *Result.Error),
			Result.Status == EAnimationCaptureReadbackStatus::Completed)) { bDone = true; return; }
		Test->TestTrue(TEXT("The result has an actual renderer view"), Result.bHasView);
		Test->TestEqual(TEXT("Originating rendered frame is retained"), Result.View.EngineFrame, ExpectedFrame);
		Test->TestEqual(TEXT("Request ID is retained"), Result.Request.RequestId, Original.RequestId);
		Test->TestEqual(TEXT("Session is retained"), Result.Request.SessionId, Original.SessionId);
		Test->TestEqual(TEXT("Viewport generation is retained"), Result.Request.ViewportGeneration, Original.ViewportGeneration);
		Test->TestEqual(TEXT("Supplied acquisition clock is retained"), Result.Request.ClockId, Original.ClockId);
		Test->TestEqual(TEXT("Supplied acquisition time is retained"), Result.Request.AcquisitionTimeSeconds, Original.AcquisitionTimeSeconds);
		Test->TestEqual(TEXT("640x480 acquisition dimensions"), Result.Request.Size, FIntPoint(640, 480));
		Test->TestEqual(TEXT("Full resolution view rectangle"), Result.View.Rect, FIntRect(0, 0, 640, 480));
		Test->TestEqual(TEXT("Both explicit pose witnesses retained"), Result.Request.PoseRevisions.Num(), 2);
		if (Result.Request.PoseRevisions.Num() == 2)
		{
			Test->TestEqual(TEXT("Pose revision predates completion mutation"), Result.Request.PoseRevisions[0].Revision,
				Original.PoseRevisions[0].Revision);
			Test->TestEqual(TEXT("Pose frame predates completion"), Result.Request.PoseRevisions[0].EngineFrame,
				Original.PoseRevisions[0].EngineFrame);
			Test->TestFalse(TEXT("Later destruction does not rewrite acquisition witness"), Result.Request.PoseRevisions[0].bMissing);
		}
		if (bDelayed)
		{
			Test->TestTrue(TEXT("Poll was withheld across eight real viewport draws"), DrawsSinceRequest >= 8);
			Test->TestTrue(TEXT("Completion is collected after the originating frame"), GFrameCounter > Result.View.EngineFrame);
			Test->TestTrue(TEXT("Live subject was moved and destroyed before polling"), bChangedSubject);
		}
		constexpr int32 Pixels = 640 * 480;
		if (Result.RGB.Num() != Pixels || Result.Labels.Num() != Pixels
			|| Result.SceneDepthCm.Num() != Pixels || Result.LabelDepthCm.Num() != Pixels)
		{
			Test->AddError(TEXT("Async surface result has incomplete planes")); bDone = true; return;
		}
		int32 First = 0, Second = 0, Front = 0, VisibleSecond = 0, Intersection = 0, Union = 0;
		TArray<FIntPoint> FirstBoundary, SecondBoundary;
		for (int32 Y = 0; Y < 480; ++Y)
		{
			int32 RightFirst = -1, LeftSecond = 640;
			for (int32 X = 0; X < 640; ++X)
			{
				const int32 I = Y * 640 + X;
				const bool A = Result.Labels[I] == 7, B = Result.Labels[I] == 23;
				First += A; Second += B;
				Front += A && FMath::IsNearlyEqual(Result.LabelDepthCm[I], 350.f, 1.f);
				VisibleSecond += B && Result.LabelDepthCm[I] <= Result.SceneDepthCm[I] + 1.f;
				const FColor Color = Result.RGB[I];
				const bool Foreground = FMath::Max3(Color.R, Color.G, Color.B) > 8;
				Intersection += Foreground && (A || B); Union += Foreground || A || B;
				if (A) { RightFirst = X; } if (B) { LeftSecond = FMath::Min(LeftSecond, X); }
			}
			if (RightFirst >= 0) { FirstBoundary.Add(FIntPoint(RightFirst, Y)); }
			if (LeftSecond < 640) { SecondBoundary.Add(FIntPoint(LeftSecond, Y)); }
		}
		Test->TestTrue(TEXT("Original first subject rasterized"), First > 100);
		Test->TestTrue(TEXT("Original 350cm front plane retained"), Front > 100);
		if (Control == 3) { Test->TestEqual(TEXT("Fully custom-depth-occluded label stays absent"), Second, 0); }
		else { Test->TestTrue(TEXT("Second custom-depth label exists"), Second > 100); }
		if (Control == 4) { Test->TestEqual(TEXT("Scene-occluded second subject is not visible"), VisibleSecond, 0); }
		const double IoU = Union ? double(Intersection) / Union : 0;
		if (Control < 4) { Test->TestTrue(TEXT("Independent RGB/label silhouette IoU >= 0.99"), IoU >= .99); }
		double Gap = TNumericLimits<double>::Max();
		for (FIntPoint A : FirstBoundary) { for (FIntPoint B : SecondBoundary)
			{ Gap = FMath::Min(Gap, FMath::Sqrt(double((A - B).SizeSquared()))); } }
		if (Control < 3)
		{
			const double Expected[] = {66, 1, 1};
			Test->TestEqual(TEXT("Known fixture pixel-centre gap"), Gap, Expected[Control]);
		}
		// Reproject an independently known fixture point using the retained acquisition
		// matrix. The live first subject may no longer exist in the delayed control.
		const FVector4 Clip = Result.View.WorldToClip.TransformFVector4(FVector4(400, -60, 10000, 1));
		if (Test->TestTrue(TEXT("Acquisition projection has a positive finite W"), FMath::IsFinite(Clip.W) && Clip.W > 0))
		{
			const int32 X = FMath::FloorToInt((.5 + .5 * Clip.X / Clip.W) * 640);
			const int32 Y = FMath::FloorToInt((.5 - .5 * Clip.Y / Clip.W) * 480);
			if (Test->TestTrue(TEXT("Original subject projects inside the captured image"), X >= 0 && X < 640 && Y >= 0 && Y < 480))
			{ Test->TestEqual(TEXT("Retained matrix projects the original subject onto label 7"), Result.Labels[Y * 640 + X], uint8(7)); }
		}
		TArray<uint8> Bytes;
		const ANSICHAR Magic[] = "SURFACE1";
		Bytes.Append(reinterpret_cast<const uint8*>(Magic), 8);
		Bytes.Append(reinterpret_cast<const uint8*>(&Result.View.EngineFrame), 8);
		const uint32 Width = 640, Height = 480;
		Bytes.Append(reinterpret_cast<const uint8*>(&Width), 4); Bytes.Append(reinterpret_cast<const uint8*>(&Height), 4);
		Bytes.Append(reinterpret_cast<const uint8*>(Result.RGB.GetData()), Pixels * 4);
		Bytes.Append(Result.Labels);
		Bytes.Append(reinterpret_cast<const uint8*>(Result.SceneDepthCm.GetData()), Pixels * 4);
		Bytes.Append(reinterpret_cast<const uint8*>(Result.LabelDepthCm.GetData()), Pixels * 4);
		const FString File = FString::Printf(TEXT("observation_%02d.surface"), Control);
		Test->TestTrue(TEXT("Async replay bundle retained"), FFileHelper::SaveArrayToFile(Bytes, *(Directory / File)));
		auto Row = AsyncResultIdentity(Result);
		Row->SetStringField(TEXT("control"), Fixture.ControlName(Control));
		Row->SetStringField(TEXT("file"), File);
		Row->SetNumberField(TEXT("width"), 640); Row->SetNumberField(TEXT("height"), 480);
		Row->SetNumberField(TEXT("rgb_label_iou"), IoU);
		Row->SetStringField(TEXT("gap_evidence"), Control < 3 ? TEXT("both_labels_visible") : TEXT("insufficient_visible_surface_evidence"));
		if (Control < 3) { Row->SetNumberField(TEXT("pixel_centre_gap"), Gap); }
		TArray<TSharedPtr<FJsonValue>> Matrix;
		for (int32 R = 0; R < 4; ++R) { for (int32 C = 0; C < 4; ++C)
			{ Matrix.Add(MakeShared<FJsonValueNumber>(Result.View.WorldToClip.M[R][C])); } }
		Row->SetArrayField(TEXT("world_to_clip_row_major"), Matrix);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
		bDone = bDelayed || ++Control == 5;
	}
	void FinishManifest()
	{
		auto Manifest = MakeShared<FJsonObject>();
		Manifest->SetNumberField(TEXT("schema_version"), 1);
		Manifest->SetStringField(TEXT("backend"), TEXT("D3D11_D32F_S8"));
		Manifest->SetStringField(TEXT("decoder"), FAnimationCaptureReadbackProducer::DecoderIdentity());
		Manifest->SetStringField(TEXT("acquisition"), TEXT("bounded_async"));
		Manifest->SetStringField(TEXT("depth_convention"), TEXT("camera_axis_cm_clear_infinity"));
		Manifest->SetStringField(TEXT("label_semantics"), TEXT("frontmost_custom_depth"));
		Manifest->SetStringField(TEXT("render_policy"), TEXT("perspective_no_aa_no_screen_percentage_unlit_control"));
		Manifest->SetStringField(TEXT("completion_metadata"), TEXT("wall_clock_separate_from_acquisition_clock"));
		Manifest->SetBoolField(TEXT("delayed_polling"), bDelayed);
		auto Subjects = MakeShared<FJsonObject>();
		Subjects->SetNumberField(TEXT("FirstSolid"), 7); Subjects->SetNumberField(TEXT("SecondSolid"), 23);
		Manifest->SetObjectField(TEXT("subjects"), Subjects); Manifest->SetArrayField(TEXT("frames"), Rows);
		Test->TestTrue(TEXT("Async surface manifest retained"), SaveAsyncJson(Manifest, Directory / TEXT("surfaces.json")));
		Test->AddInfo(TEXT("SURFACE_OBSERVATION_OUTPUT=") + Directory);
	}
	void Cleanup()
	{
		UGameViewportClient::OnViewportRendered().Remove(DrawHandle);
		if (Reader) { Reader->Shutdown(); Reader.Reset(); }
		Fixture.Stop();
	}
	FAutomationTestBase* Test;
	FAnimationCaptureHostFixture Fixture;
	TUniquePtr<FViewportAsyncCapture> Reader;
	FAnimationCaptureReadbackRequest Submitted, Original;
	FAnimationCaptureReadbackTicket Ticket;
	FDelegateHandle DrawHandle;
	FString Directory;
	TArray<TSharedPtr<FJsonValue>> Rows;
	bool bDelayed = false, bWaiting = false, bDone = false, bChangedSubject = false;
	int32 Control = 0, WarmupDraws = 0, DrawsSinceRequest = 0;
	uint64 ExpectedFrame = 0;
	double Started = 0;
};

class FHostAsyncLifecycle : public IAutomationLatentCommand
{
public:
	explicit FHostAsyncLifecycle(FAutomationTestBase* InTest) : Test(InTest) {}
	~FHostAsyncLifecycle() { Cleanup(); }
	bool Update() override
	{
		if (!Started) { Started = FPlatformTime::Seconds(); }
		if (FPlatformTime::Seconds() - Started > 80)
		{ Test->AddError(TEXT("Async lifecycle exceeded its 80 second deadline")); Finish(); return true; }
		if (!Fixture.World.IsValid())
		{
			UWorld* World = FAnimationCaptureHostFixture::FindWorld();
			if (!World) { return false; }
			FString Error;
			if (!Fixture.Start(World, Error)) { Test->AddError(Error); return true; }
			Directory = AsyncDirectory(TEXT("Lifecycle"));
			Limits.MaxPendingRequests = 1;
			DrawHandle = UGameViewportClient::OnViewportRendered().AddRaw(this, &FHostAsyncLifecycle::Draw);
			return false;
		}
		if (!Reader)
		{
			if (DrawsSinceRequest < 8) { return false; }
			Reader = MakeUnique<FViewportAsyncCapture>(Fixture.World.Get(), Fixture.Viewport, true, Limits);
		}
		FString Error;
		FAnimationCaptureReadbackResult Result;
		if (Stage == 0)
		{
			auto Request = MakeAsyncRequest(Fixture, Directory, ++RequestId);
			if (!Admit(Request)) { Finish(); return true; }
			Test->TestTrue(TEXT("Pending pre-draw request cancels"), Reader->Cancel(Ticket, TEXT("pending_control")));
			Test->TestFalse(TEXT("Pending cancellation occurs once"), Reader->Cancel(Ticket, TEXT("duplicate")));
			Stage = 1;
		}
		if (Stage == 1)
		{
			if (!CollectTerminal(EAnimationCaptureReadbackStatus::Cancelled, TEXT("pending_cancel"))) { return false; }
			FAnimationCaptureReadbackLimits TinyLimits = Limits;
			TinyLimits.MaxReservedBytes = 53ll * 640 * 480 - 1;
			FViewportAsyncCapture TooSmall(Fixture.World.Get(), Fixture.Viewport, true, TinyLimits);
			FAnimationCaptureReadbackTicket Rejected;
			Test->TestFalse(TEXT("Rendered RGB/depth request exceeds one-byte-short capacity"),
				TooSmall.Request(MakeAsyncRequest(Fixture, Directory, ++RequestId), Rejected, Error));
			Test->TestEqual(TEXT("Rejected request allocates no payload"), TooSmall.GetStats().ReservedBytes, int64(0));
			Test->TestEqual(TEXT("Byte rejection is counted"), TooSmall.GetStats().Rejected, uint64(1));
			TooSmall.Shutdown();
			if (!Admit(MakeAsyncRequest(Fixture, Directory, ++RequestId))) { Finish(); return true; }
			Test->TestFalse(TEXT("Second pre-draw request exceeds request count"),
				Reader->Request(MakeAsyncRequest(Fixture, Directory, ++RequestId), Rejected, Error));
			Test->TestEqual(TEXT("Request count stays bounded"), Reader->GetStats().PendingRequests, 1);
			DrawsSinceRequest = 0; Stage = 2; return false;
		}
		if (Stage == 2)
		{
			if (DrawsSinceRequest < 2) { return false; }
			Test->TestTrue(TEXT("In-flight rendered request cancels"), Reader->Cancel(Ticket, TEXT("inflight_control")));
			Test->TestFalse(TEXT("In-flight cancellation occurs once"), Reader->Cancel(Ticket, TEXT("duplicate")));
			Stage = 3;
		}
		if (Stage == 3)
		{
			if (!CollectTerminal(EAnimationCaptureReadbackStatus::Cancelled, TEXT("inflight_cancel"))) { return false; }
			if (Reader->GetStats().ReservedBytes != 0) { Reader->Poll(Result); return false; }
			if (!Admit(MakeAsyncRequest(Fixture, Directory, ++RequestId))) { Finish(); return true; }
			auto* Client = Fixture.World->GetGameViewport();
			Client->GetGameViewport()->SetFixedViewportSize(320, 240);
			DrawsSinceRequest = 0;
			Stage = 4; return false;
		}
		if (Stage == 4)
		{
			if (DrawsSinceRequest < 2) { return false; }
			// Allow the actual resized target to draw before checking stale dimensions.
			const bool bCollected = Reader->Poll(Result);
			FAnimationCaptureReadbackTicket Rejected;
			Test->TestFalse(TEXT("Old dimensions reject after actual viewport resize"),
				Reader->Request(MakeAsyncRequest(Fixture, Directory, ++RequestId), Rejected, Error));
			auto* Client = Fixture.World->GetGameViewport();
			Client->GetGameViewport()->SetFixedViewportSize(640, 480);
			if (bCollected) { RecordNonSuccess(Result, TEXT("resize")); bResizeTerminal = true; }
			DrawsSinceRequest = 0;
			Stage = 5; return false;
		}
		if (Stage == 5)
		{
			if (DrawsSinceRequest < 2) { return false; }
			if (!bResizeTerminal)
			{
				if (!Reader->Poll(Result)) { return false; }
				RecordNonSuccess(Result, TEXT("resize")); bResizeTerminal = true;
			}
			Reader->Shutdown();
			Test->TestFalse(TEXT("Resize terminal delivered exactly once"), Reader->Poll(Result));
			Reader = MakeUnique<FViewportAsyncCapture>(Fixture.World.Get(), Fixture.Viewport, true, Limits);
			if (!Admit(MakeAsyncRequest(Fixture, Directory, ++RequestId))) { Finish(); return true; }
			auto* Client = Fixture.World->GetGameViewport();
			FViewport* OriginalViewport = Client->Viewport;
			// Synchronous replacement witness: restore before any editor/world tick.
			Client->Viewport = nullptr;
			const bool bCollected = Reader->Poll(Result);
			FAnimationCaptureReadbackTicket Rejected;
			const bool bAdmitted = Reader->Request(MakeAsyncRequest(Fixture, Directory, ++RequestId), Rejected, Error);
			Client->Viewport = OriginalViewport;
			Test->TestFalse(TEXT("Replaced viewport rejects new admission without stale dereference"), bAdmitted);
			if (bCollected) { RecordNonSuccess(Result, TEXT("replacement")); bReplacementTerminal = true; }
			Stage = 6; return false;
		}
		if (Stage == 6)
		{
			if (!bReplacementTerminal)
			{
				if (!Reader->Poll(Result)) { return false; }
				RecordNonSuccess(Result, TEXT("replacement")); bReplacementTerminal = true;
			}
			Reader->Shutdown();
			Test->TestFalse(TEXT("Replacement terminal delivered exactly once"), Reader->Poll(Result));
			Test->TestEqual(TEXT("Replacement teardown releases reservation"), Reader->GetStats().ReservedBytes, int64(0));
			for (int32 Cycle = 0; Cycle < 3; ++Cycle) { VerifyRepeatedOwner(Cycle); }
			Finish(); return true;
		}
		return false;
	}
private:
	void Draw(FViewport* Viewport) { if (Viewport == Fixture.Viewport) { ++DrawsSinceRequest; } }
	bool Admit(const FAnimationCaptureReadbackRequest& Request)
	{
		FString Error;
		const bool bAdmitted = Reader->Request(Request, Ticket, Error);
		return Test->TestTrue(*FString::Printf(TEXT("Lifecycle request admitted: %s"), *Error), bAdmitted);
	}
	bool CollectTerminal(EAnimationCaptureReadbackStatus Expected, const TCHAR* Case)
	{
		if (CollectedStages.Contains(Stage)) { return true; }
		FAnimationCaptureReadbackResult Result;
		if (!Reader->Poll(Result)) { return false; }
		Test->TestTrue(TEXT("Cancellation has the expected terminal status"), Result.Status == Expected);
		auto Row = AsyncResultIdentity(Result); Row->SetStringField(TEXT("case"), Case);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
		Test->TestFalse(TEXT("Terminal result is delivered exactly once"), Reader->Poll(Result));
		CollectedStages.Add(Stage);
		return true;
	}
	void RecordNonSuccess(const FAnimationCaptureReadbackResult& Result, const TCHAR* Case)
	{
		Test->TestTrue(TEXT("Changed viewport cannot complete an old capture"),
			Result.Status != EAnimationCaptureReadbackStatus::Completed);
		Test->TestTrue(TEXT("Changed viewport outcome explains the cause"), !Result.Error.IsEmpty());
		auto Row = AsyncResultIdentity(Result); Row->SetStringField(TEXT("case"), Case);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	void VerifyRepeatedOwner(int32 Cycle)
	{
		UWorld* World = Fixture.World.Get();
		AActor* Actor = World->SpawnActor<AActor>();
		auto* Mesh = NewObject<UStaticMeshComponent>(Actor);
		Mesh->RegisterComponent();
		Mesh->SetCustomDepthStencilValue(27);
		Mesh->SetCustomDepthStencilWriteMask(ERendererStencilMask::ERSM_1);
		FScopedSurfaceCaptureLabels Labels, Competitor;
		TArray<FSurfaceCaptureLabel> Subjects = {{5, TEXT("IndependentOwnershipControl"), Mesh}};
		FString Error;
		Test->TestTrue(TEXT("Independent explicit label owner acquires"), Labels.Apply(World, Subjects, Error));
		Test->TestFalse(TEXT("Competing label owner rejects"), Competitor.Apply(World, Subjects, Error));
		FViewportAsyncCapture Owner(World, Fixture.Viewport, true, Limits);
		FAnimationCaptureReadbackTicket OwnerTicket;
		Test->TestTrue(TEXT("Repeated capture owner admits"), Owner.Request(
			MakeAsyncRequest(Fixture, Directory, ++RequestId), OwnerTicket, Error));
		Owner.Shutdown();
		FAnimationCaptureReadbackResult Result;
		Test->TestTrue(TEXT("Repeated owner shutdown produces a terminal result"), Owner.Poll(Result));
		Test->TestTrue(TEXT("Repeated owner shutdown cancels"), Result.Status == EAnimationCaptureReadbackStatus::Cancelled);
		Test->TestFalse(TEXT("Repeated owner shutdown completes once"), Owner.Poll(Result));
		Labels.Restore(); Labels.Restore();
		Test->TestFalse(TEXT("Original custom depth flag restored after owner teardown"), bool(Mesh->bRenderCustomDepth));
		Test->TestEqual(TEXT("Original stencil value restored after owner teardown"), Mesh->CustomDepthStencilValue, 27);
		Test->TestEqual(TEXT("Original stencil write mask restored after owner teardown"),
			Mesh->CustomDepthStencilWriteMask, ERendererStencilMask::ERSM_1);
		Test->TestTrue(TEXT("Released labels can be acquired by an independent owner"), Competitor.Apply(World, Subjects, Error));
		Competitor.Restore();
		auto Row = AsyncResultIdentity(Result);
		Row->SetStringField(TEXT("case"), TEXT("repeated_owner")); Row->SetNumberField(TEXT("cycle"), Cycle);
		AppendStats(Row, Owner.GetStats()); Rows.Add(MakeShared<FJsonValueObject>(Row));
		Actor->Destroy();
	}
	void Finish()
	{
		if (Reader) { Reader->Shutdown(); }
		if (!Directory.IsEmpty())
		{
			auto Manifest = MakeShared<FJsonObject>(); Manifest->SetArrayField(TEXT("results"), Rows);
			if (Reader) { AppendStats(Manifest, Reader->GetStats()); }
			Test->TestTrue(TEXT("Lifecycle observations retained"), SaveAsyncJson(Manifest, Directory / TEXT("lifecycle.json")));
			Test->AddInfo(TEXT("ASYNC_LIFECYCLE_OUTPUT=") + Directory);
		}
		Cleanup();
	}
	void Cleanup()
	{
		UGameViewportClient::OnViewportRendered().Remove(DrawHandle);
		if (Reader) { Reader->Shutdown(); Reader.Reset(); }
		Fixture.Stop();
	}
	FAutomationTestBase* Test;
	FAnimationCaptureHostFixture Fixture;
	TUniquePtr<FViewportAsyncCapture> Reader;
	FAnimationCaptureReadbackLimits Limits;
	FAnimationCaptureReadbackTicket Ticket;
	FDelegateHandle DrawHandle;
	TSet<int32> CollectedStages;
	TArray<TSharedPtr<FJsonValue>> Rows;
	FString Directory;
	int32 Stage = 0, DrawsSinceRequest = 0;
	uint64 RequestId = 0;
	bool bResizeTerminal = false, bReplacementTerminal = false;
	double Started = 0;
};

struct FHostAsyncCleanupState
{
	FAnimationCaptureHostFixture Fixture;
	TUniquePtr<FViewportAsyncCapture> Reader;
	FString Directory;
	uint64 RequestId = 1;
	int32 Draws = 0, AdmissionDraw = -1;
	FDelegateHandle DrawHandle;
	~FHostAsyncCleanupState()
	{
		UGameViewportClient::OnViewportRendered().Remove(DrawHandle);
		if (Reader) { Reader->Shutdown(); Reader.Reset(); }
		Fixture.Stop();
	}
};
class FHostAsyncBeforeWorldCleanup : public IAutomationLatentCommand
{
public:
	FHostAsyncBeforeWorldCleanup(FAutomationTestBase* InTest, TSharedRef<FHostAsyncCleanupState> InState)
		: Test(InTest), State(InState) {}
	bool Update() override
	{
		if (!Started) { Started = FPlatformTime::Seconds(); }
		if (FPlatformTime::Seconds() - Started > 45)
		{ Test->AddError(TEXT("World-cleanup setup exceeded its deadline")); return true; }
		if (!State->Fixture.World.IsValid())
		{
			UWorld* World = FAnimationCaptureHostFixture::FindWorld();
			if (!World) { return false; }
			FString Error;
			if (!State->Fixture.Start(World, Error)) { Test->AddError(Error); return true; }
			State->Directory = AsyncDirectory(TEXT("WorldCleanup"));
			const TWeakPtr<FHostAsyncCleanupState> WeakState = State;
			State->DrawHandle = UGameViewportClient::OnViewportRendered().AddLambda([WeakState](FViewport* Viewport)
			{ if (auto Pinned = WeakState.Pin()) { if (Viewport == Pinned->Fixture.Viewport) { ++Pinned->Draws; } } });
			return false;
		}
		if (!State->Reader)
		{
			if (State->Draws < 8) { return false; }
			State->Reader = MakeUnique<FViewportAsyncCapture>(State->Fixture.World.Get(), State->Fixture.Viewport, true);
			FString Error;
			FAnimationCaptureReadbackTicket Ticket;
			if (!State->Reader->Request(MakeAsyncRequest(State->Fixture, State->Directory, State->RequestId), Ticket, Error))
			{ Test->AddError(Error); return true; }
			State->AdmissionDraw = State->Draws;
		}
		if (State->Draws <= State->AdmissionDraw) { return false; }
		UGameViewportClient::OnViewportRendered().Remove(State->DrawHandle);
		return true; // The next command actually ends PIE while the owner remains alive.
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FHostAsyncCleanupState> State;
	double Started = 0;
};
class FHostAsyncAfterWorldCleanup : public IAutomationLatentCommand
{
public:
	FHostAsyncAfterWorldCleanup(FAutomationTestBase* InTest, TSharedRef<FHostAsyncCleanupState> InState)
		: Test(InTest), State(InState) {}
	bool Update() override
	{
		if (!Started) { Started = FPlatformTime::Seconds(); }
		if (!State->Reader) { Test->AddError(TEXT("World-cleanup control never admitted a request")); return true; }
		FAnimationCaptureReadbackResult Result;
		if (State->Reader->Poll(Result))
		{
			Test->TestTrue(TEXT("Actual PIE world cleanup cancels the uncollected request"),
				Result.Status == EAnimationCaptureReadbackStatus::Cancelled);
			Test->TestEqual(TEXT("Cleanup preserves original request ID"), Result.Request.RequestId, State->RequestId);
			auto Manifest = AsyncResultIdentity(Result);
			State->Reader->Shutdown();
			Test->TestFalse(TEXT("World cleanup delivers the terminal result once"), State->Reader->Poll(Result));
			Test->TestEqual(TEXT("World cleanup retires every reserved payload"), State->Reader->GetStats().ReservedBytes, int64(0));
			AppendStats(Manifest, State->Reader->GetStats());
			Test->TestTrue(TEXT("World cleanup observations retained"), SaveAsyncJson(Manifest, State->Directory / TEXT("lifecycle.json")));
			Test->AddInfo(TEXT("ASYNC_LIFECYCLE_OUTPUT=") + State->Directory);
			State->Reader.Reset(); State->Fixture.Stop();
			return true;
		}
		if (FPlatformTime::Seconds() - Started > 15)
		{
			Test->AddError(TEXT("World cleanup produced no terminal result within 15 seconds"));
			State->Reader->Shutdown(); return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FHostAsyncCleanupState> State;
	double Started = 0;
};
}
#else
#define ANIMATION_CAPTURE_HOST_HAS_ASYNC 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHostAsyncGeometryTest, "AnimationAnalysis.Capture.Async.RenderedGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHostAsyncGeometryTest::RunTest(const FString&)
{
#if ANIMATION_CAPTURE_HOST_HAS_ASYNC
	if (!FApp::CanEverRender()) { AddWarning(TEXT("ANIMATION_ANALYSIS_RENDERED_SKIP: D3D11 required")); return true; }
	ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Engine/Maps/Entry")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FHostAsyncGeometry(this, false));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
#else
	AddError(TEXT("Bounded asynchronous viewport API is not implemented"));
#endif
	return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHostAsyncDelayedIdentityTest, "AnimationAnalysis.Capture.Async.DelayedIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHostAsyncDelayedIdentityTest::RunTest(const FString&)
{
#if ANIMATION_CAPTURE_HOST_HAS_ASYNC
	if (!FApp::CanEverRender()) { AddWarning(TEXT("ANIMATION_ANALYSIS_RENDERED_SKIP: D3D11 required")); return true; }
	ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Engine/Maps/Entry")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FHostAsyncGeometry(this, true));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
#else
	AddError(TEXT("Bounded asynchronous viewport API is not implemented"));
#endif
	return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHostAsyncLifecycleTest, "AnimationAnalysis.Capture.Async.RenderedLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHostAsyncLifecycleTest::RunTest(const FString&)
{
#if ANIMATION_CAPTURE_HOST_HAS_ASYNC
	if (!FApp::CanEverRender()) { AddWarning(TEXT("ANIMATION_ANALYSIS_RENDERED_SKIP: D3D11 required")); return true; }
	ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Engine/Maps/Entry")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FHostAsyncLifecycle(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
#else
	AddError(TEXT("Bounded asynchronous viewport API is not implemented"));
#endif
	return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHostAsyncWorldCleanupTest, "AnimationAnalysis.Capture.Async.WorldCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHostAsyncWorldCleanupTest::RunTest(const FString&)
{
#if ANIMATION_CAPTURE_HOST_HAS_ASYNC
	if (!FApp::CanEverRender()) { AddWarning(TEXT("ANIMATION_ANALYSIS_RENDERED_SKIP: D3D11 required")); return true; }
	TSharedRef<FHostAsyncCleanupState> State = MakeShared<FHostAsyncCleanupState>();
	ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Engine/Maps/Entry")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FHostAsyncBeforeWorldCleanup(this, State));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FHostAsyncAfterWorldCleanup(this, State));
#else
	AddError(TEXT("Bounded asynchronous viewport API is not implemented"));
#endif
	return !HasAnyErrors();
}
