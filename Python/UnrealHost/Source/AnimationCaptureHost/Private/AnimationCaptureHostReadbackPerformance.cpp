#include "AnimationCaptureHostFixture.h"
#include "Misc/AutomationTest.h"
#include "Misc/App.h"

#if __has_include("AnimationCapture/ViewportAsyncCapture.h")
#include "AnimationCapture/ViewportAsyncCapture.h"
#include "AnimationCapture/ViewportSurfaceCapture.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "LegacyScreenPercentageDriver.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "RenderingThread.h"
#include "SceneViewExtension.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealClient.h"
#define ANIMATION_CAPTURE_HOST_PERFORMANCE_HAS_ASYNC 1

namespace
{
const TCHAR* ReadbackModeName(int32 Mode)
{
	static const TCHAR* Names[] = {TEXT("disabled"), TEXT("synchronous"), TEXT("asynchronous")};
	return Names[Mode];
}
const TCHAR* ReadbackOutcome(EAnimationCaptureReadbackStatus Status)
{
	switch (Status)
	{
	case EAnimationCaptureReadbackStatus::Completed: return TEXT("completed");
	case EAnimationCaptureReadbackStatus::Cancelled: return TEXT("cancelled");
	case EAnimationCaptureReadbackStatus::TimedOut: return TEXT("timed_out");
	default: return TEXT("failed");
	}
}
void PerformanceStats(const TSharedRef<FJsonObject>& Row, const FAnimationCaptureReadbackStats& Stats, bool bMemoryMeasured = true)
{
	Row->SetNumberField(TEXT("pending_requests"), Stats.PendingRequests);
	if (bMemoryMeasured)
	{
		Row->SetNumberField(TEXT("reserved_bytes"), double(Stats.ReservedBytes));
		Row->SetNumberField(TEXT("peak_bytes"), double(Stats.PeakReservedBytes));
	}
	Row->SetNumberField(TEXT("rejected"), double(Stats.Rejected));
	Row->SetNumberField(TEXT("cancelled"), double(Stats.Cancelled));
	Row->SetNumberField(TEXT("failed"), double(Stats.Failed));
	Row->SetNumberField(TEXT("timeouts"), double(Stats.TimedOut));
}

struct FPerformanceViewPolicyState
{
	FCriticalSection Mutex;
	uint64 Views = 0, UnsupportedViews = 0;
};

/** The same view policy applies even when capture is disabled or no request is pending. */
class FPerformanceViewPolicy : public FSceneViewExtensionBase
{
public:
	FPerformanceViewPolicy(const FAutoRegister& AutoRegister, FViewport* InViewport,
		TSharedRef<FPerformanceViewPolicyState, ESPMode::ThreadSafe> InState)
		: FSceneViewExtensionBase(AutoRegister), Viewport(InViewport), State(InState) {}
	void SetupViewFamily(FSceneViewFamily&) override {}
	void SetupView(FSceneViewFamily&, FSceneView&) override {}
	bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override
	{ return Context.Viewport == Viewport; }
	void BeginRenderViewFamily(FSceneViewFamily& Family) override
	{
		if (Family.RenderTarget != Viewport) { return; }
		const auto* Previous = Family.GetScreenPercentageInterface();
		Family.SetScreenPercentageInterface_Unchecked(new FLegacyScreenPercentageDriver(Family, 1.f));
		delete Previous;
		Family.EngineShowFlags.SetScreenPercentage(false);
		Family.SecondaryViewFraction = 1.f;
	}
	void PostRenderView_RenderThread(FRDGBuilder&, FSceneView& View) override
	{
		if (View.Family->RenderTarget != Viewport) { return; }
		const bool bSupported = View.Family->Views.Num() == 1 && View.UnscaledViewRect == FIntRect(0, 0, 640, 480)
			&& View.IsPerspectiveProjection() && View.AntiAliasingMethod == AAM_None
			&& !View.Family->EngineShowFlags.ScreenPercentage
			&& FMath::IsNearlyEqual(View.Family->SecondaryViewFraction, 1.f);
		FScopeLock Lock(&State->Mutex);
		++State->Views; State->UnsupportedViews += !bSupported;
	}
private:
	FViewport* Viewport;
	TSharedRef<FPerformanceViewPolicyState, ESPMode::ThreadSafe> State;
};

struct FPerformancePendingRequest
{
	FAnimationCaptureReadbackTicket Ticket;
	TSharedPtr<FJsonObject> Row; // Null during warmup.
};

/** Runs all modes against one fixture, with one admission attempt per viewport draw. */
class FHostReadbackComparison : public IAutomationLatentCommand
{
public:
	explicit FHostReadbackComparison(FAutomationTestBase* InTest)
		: Test(InTest) {}
	~FHostReadbackComparison() { Cleanup(); }
	bool Update() override
	{
		if (!Started) { Started = FPlatformTime::Seconds(); }
		if (bDone)
		{
			if (!bSaved) { AbortPending(TEXT("comparison_ended_early")); Save(); }
			Cleanup(); return true;
		}
		if (FPlatformTime::Seconds() - Started > 300)
		{
			Test->AddError(TEXT("Readback comparison exceeded its 300 second deadline"));
			AbortPending(TEXT("comparison_deadline"));
			Save(); Cleanup(); return true;
		}
		if (!Fixture.World.IsValid())
		{
			UWorld* World = FAnimationCaptureHostFixture::FindWorld();
			if (!World) { return false; }
			FString Error;
			if (!Fixture.Start(World, Error)) { Test->AddError(Error); return true; }
			Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Observations")
				/ (TEXT("Performance-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			IFileManager::Get().MakeDirectory(*Directory, true);
			for (const TCHAR* Name : {TEXT("r.VSync"), TEXT("t.MaxFPS"), TEXT("r.OneFrameThreadLag"),
				TEXT("r.ScreenPercentage"), TEXT("r.SecondaryScreenPercentage.GameViewport"), TEXT("r.DynamicRes.OperationMode")})
			{
				if (const auto* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
				{ Scheduling->SetStringField(Name, Variable->GetString()); }
			}
			ViewPolicy = FSceneViewExtensions::NewExtension<FPerformanceViewPolicy>(Fixture.Viewport, ViewPolicyState);
			StartMode();
			return false;
		}
		PollAsync();
		if (bDraining)
		{
			if (Pending.IsEmpty() && (!Async || Async->GetStats().ReservedBytes == 0))
			{
				FinishMode();
				if (++ModePosition == 3) { ModePosition = 0; ++Repetition; }
				if (Repetition == 3) { Save(); bDone = true; return false; }
				Mode = (Repetition + ModePosition) % 3;
				StartMode();
				return false;
			}
			if (FPlatformTime::Seconds() > DrainDeadline)
			{
				Test->AddError(TEXT("Readback comparison drain exceeded ten seconds"));
				AbortPending(TEXT("run_drain_timeout"));
			}
			return false;
		}
		if (bAwaitingDraw) { return false; }
		// Fixed-size render targets must have materialized before adapter enrollment.
		if (InitialDraws < 8) { return false; }
		if (!bReadersReady)
		{
			UGameViewportClient::OnViewportRendered().Remove(DrawHandle);
			DrawHandle = UGameViewportClient::OnViewportRendered().AddRaw(this, &FHostReadbackComparison::Draw);
			// The common host extension already supplies the identical full-resolution policy.
			if (Mode == 1) { Sync = MakeUnique<FViewportSurfaceCapture>(Fixture.World.Get(), Fixture.Viewport, false); }
			if (Mode == 2) { Async = MakeUnique<FViewportAsyncCapture>(Fixture.World.Get(), Fixture.Viewport, false); }
			// UE 5.6 currently broadcasts in reverse registration order. Delegate order
			// is not an API guarantee, so Draw verifies this same-frame marker every time.
			BeginDrawHandle = UGameViewportClient::OnViewportRendered().AddRaw(this, &FHostReadbackComparison::BeginDraw);
			bReadersReady = true;
		}
		PrepareNextRequest();
		return false;
	}
private:
	void PrepareNextRequest()
	{
		CurrentRow.Reset();
		if (WarmupDraws >= 60)
		{
			CurrentRow = MakeShared<FJsonObject>();
			CurrentRow->SetStringField(TEXT("mode"), ReadbackModeName(Mode));
			CurrentRow->SetNumberField(TEXT("repetition"), Repetition);
			CurrentRow->SetNumberField(TEXT("sample"), Sample);
			CurrentRow->SetNumberField(TEXT("request_id"), double(++NextRequestId));
			CurrentRow->SetStringField(TEXT("terminal_status"), TEXT("pending"));
			CurrentRow->SetField(TEXT("completion_latency_s"), MakeShared<FJsonValueNull>());
			if (Mode == 2) { CurrentRow->SetField(TEXT("decode_wall_s"), MakeShared<FJsonValueNull>()); }
			Rows.Add(MakeShared<FJsonValueObject>(CurrentRow));
		}
		else { ++NextRequestId; }
		CurrentRequestId = NextRequestId;
		bCurrentAdmitted = Mode == 0;
		FString Error;
		AdmissionBeginWall = FPlatformTime::Seconds();
		AdmissionWall = 0;
		if (Mode == 1)
		{
			bCurrentAdmitted = Sync->Request(Error);
			AdmissionWall = FPlatformTime::Seconds() - AdmissionBeginWall;
			if (!bCurrentAdmitted) { ++SyncRejected; RecordAdmissionFailure(Error); }
		}
		else if (Mode == 2)
		{
			FAnimationCaptureReadbackRequest Request;
			Request.SessionId = Directory + FString::Printf(TEXT("/%d/%d"), Repetition, Mode);
			Request.RequestId = CurrentRequestId;
			Request.ViewportId = TEXT("NeutralFixtureViewport");
			Request.ViewportGeneration = 1;
			Request.ClockId = TEXT("NeutralWorldTime");
			Request.AcquisitionTimeSeconds = Fixture.World->GetTimeSeconds();
			Request.Size = FIntPoint(640, 480);
			FAnimationCaptureReadbackTicket Ticket;
			AdmissionBeginWall = FPlatformTime::Seconds();
			bCurrentAdmitted = Async->Request(Request, Ticket, Error);
			AdmissionWall = FPlatformTime::Seconds() - AdmissionBeginWall;
			if (bCurrentAdmitted) { Pending.Add(CurrentRequestId, {Ticket, CurrentRow}); }
			else { RecordAdmissionFailure(Error); }
		}
		bAwaitingDraw = true;
	}
	void StartMode()
	{
		WarmupDraws = 0; Sample = 0; InitialDraws = 0; LastAttemptDraw = 0; SampleIdleDraws = 0;
		PreviousDraw = 0; DrawBegin = 0; bDraining = false; bAwaitingDraw = false;
		bReadersReady = false; SyncRejected = 0; SyncFailed = 0; bRunLost = false;
		RunWall = FPlatformTime::Seconds();
		{
			FScopeLock Lock(&ViewPolicyState->Mutex);
			RunInitialViews = ViewPolicyState->Views; RunInitialUnsupportedViews = ViewPolicyState->UnsupportedViews;
		}
		DrawHandle = UGameViewportClient::OnViewportRendered().AddRaw(this, &FHostReadbackComparison::Draw);
	}
	void BeginDraw(FViewport* Viewport)
	{
		if (Viewport != Fixture.Viewport || bDone) { return; }
		DrawBegin = FPlatformTime::Seconds();
		BeginDrawFrame = GFrameCounter;
		FrameWall = PreviousDraw > 0 ? DrawBegin - PreviousDraw : 0;
		PreviousDraw = DrawBegin;
	}
	void Draw(FViewport* Viewport)
	{
		if (Viewport != Fixture.Viewport || bDone) { return; }
		++InitialDraws;
		if (!bAwaitingDraw && Sample > 0 && Sample < 120) { ++SampleIdleDraws; }
		if (!bAwaitingDraw || bDraining) { return; }
		if (BeginDrawFrame != GFrameCounter)
		{
			Test->AddError(TEXT("Post-draw timing callback order changed; measurements cannot be compared"));
			bAnyLoss = true; bRunLost = true; bDone = true;
			return;
		}
		const double CallbackBegin = FPlatformTime::Seconds();
		double Acquisition = AdmissionWall + FMath::Max(0.0, CallbackBegin - DrawBegin);
		FViewportSurfaceFrame Frame; TArray<FColor> RGB; FString Error;
		if (Mode == 1 && bCurrentAdmitted)
		{
			const double CollectBegin = FPlatformTime::Seconds();
			const bool bCollected = Sync->Collect(Viewport, GFrameCounter, Frame, RGB, Error);
			Acquisition += FPlatformTime::Seconds() - CollectBegin;
			if (!bCollected && Error.IsEmpty()) { return; }
			if (!bCollected)
			{
				++SyncFailed; bRunLost = true; bAnyLoss = true;
				if (CurrentRow) { CurrentRow->SetStringField(TEXT("terminal_status"), TEXT("failed")); CurrentRow->SetStringField(TEXT("error"), Error); }
			}
			else if (CurrentRow)
			{
				CurrentRow->SetStringField(TEXT("terminal_status"), TEXT("completed"));
				CurrentRow->SetNumberField(TEXT("depth_readback_wall_s"), Frame.ReadbackSeconds);
				CurrentRow->SetNumberField(TEXT("completion_latency_s"), FPlatformTime::Seconds() - AdmissionBeginWall);
				CurrentRow->SetNumberField(TEXT("acquisition_engine_frame"), double(Frame.EngineFrame));
			}
		}
		if (CurrentRow)
		{
			CurrentRow->SetNumberField(TEXT("engine_frame"), double(GFrameCounter));
			CurrentRow->SetNumberField(TEXT("viewport_draw_sequence"), InitialDraws);
			CurrentRow->SetNumberField(TEXT("draws_since_previous_attempt"), LastAttemptDraw ? InitialDraws - LastAttemptDraw : 1);
			CurrentRow->SetNumberField(TEXT("frame_wall_s"), FrameWall);
			CurrentRow->SetNumberField(TEXT("acquisition_wall_s"), Acquisition);
			CurrentRow->SetNumberField(TEXT("request_call_wall_s"), AdmissionWall);
			CurrentRow->SetNumberField(TEXT("postdraw_submission_bracket_wall_s"), FMath::Max(0.0, CallbackBegin - DrawBegin));
			if (Mode == 0)
			{
				CurrentRow->SetStringField(TEXT("terminal_status"), TEXT("completed"));
				CurrentRow->SetNumberField(TEXT("completion_latency_s"), 0);
				CurrentRow->SetNumberField(TEXT("decode_wall_s"), 0);
			}
			FAnimationCaptureReadbackStats Stats;
			if (Async) { Stats = Async->GetStats(); }
			else { Stats.Rejected = SyncRejected; Stats.Failed = SyncFailed; }
			PerformanceStats(CurrentRow.ToSharedRef(), Stats, Mode != 1);
			++Sample;
		}
		else { ++WarmupDraws; }
		LastAttemptDraw = InitialDraws;
		bAwaitingDraw = false; CurrentRow.Reset();
		if (Sample == 120)
		{
			bDraining = true; DrainDeadline = FPlatformTime::Seconds() + 10;
		}
		else { PrepareNextRequest(); }
	}
	void PollAsync()
	{
		if (!Async) { return; }
		FAnimationCaptureReadbackResult Result;
		while (Async->Poll(Result))
		{
			FPerformancePendingRequest* Entry = Pending.Find(Result.Request.RequestId);
			if (!Entry)
			{
				Test->AddError(TEXT("Async completion had an unknown or duplicate request ID"));
				bAnyLoss = true; bRunLost = true; continue;
			}
			if (Entry->Row)
			{
				auto Row = Entry->Row;
				Row->SetStringField(TEXT("terminal_status"), ReadbackOutcome(Result.Status));
				Row->SetStringField(TEXT("error"), Result.Error);
				Row->SetNumberField(TEXT("acquisition_engine_frame"), double(Result.View.EngineFrame));
				Row->SetNumberField(TEXT("request_time_s"), Result.Request.AcquisitionTimeSeconds);
				Row->SetNumberField(TEXT("renderer_world_time_s"), Result.View.WorldTimeSeconds);
				Row->SetNumberField(TEXT("renderer_real_time_s"), Result.View.RealTimeSeconds);
				Row->SetNumberField(TEXT("admitted_wall_s"), Result.AdmittedWallSeconds);
				Row->SetNumberField(TEXT("completion_wall_s"), Result.CompletedWallSeconds);
				Row->SetNumberField(TEXT("elapsed_terminal_s"), Result.CompletionLatencySeconds);
				if (Result.Status == EAnimationCaptureReadbackStatus::Completed)
				{
					Row->SetNumberField(TEXT("completion_latency_s"), Result.CompletionLatencySeconds);
					Row->SetNumberField(TEXT("decode_wall_s"), Result.DecodeSeconds);
				}
			}
			if (Result.Status != EAnimationCaptureReadbackStatus::Completed) { bRunLost = true; bAnyLoss = true; }
			Pending.Remove(Result.Request.RequestId);
			// Arrays die here, before the next admission: the consumer keeps no uncharged image backlog.
		}
		const auto Stats = Async->GetStats();
		Test->TestTrue(TEXT("Async requests remain within declared capacity"), Stats.PendingRequests <= 4);
		Test->TestTrue(TEXT("Async bytes remain within declared capacity"), Stats.ReservedBytes <= 64ll * 1024 * 1024);
	}
	void RecordAdmissionFailure(const FString& Error)
	{
		bRunLost = true; bAnyLoss = true;
		if (CurrentRow)
		{
			CurrentRow->SetStringField(TEXT("terminal_status"), TEXT("rejected"));
			CurrentRow->SetStringField(TEXT("error"), Error);
		}
	}
	void AbortPending(const TCHAR* Reason)
	{
		bRunLost = true; bAnyLoss = true;
		if (Async)
		{
			for (auto& Pair : Pending) { Async->Cancel(Pair.Value.Ticket, Reason); }
			Async->Shutdown(); PollAsync();
		}
		for (auto& Pair : Pending)
		{
			if (Pair.Value.Row)
			{
				Pair.Value.Row->SetStringField(TEXT("terminal_status"), TEXT("failed"));
				Pair.Value.Row->SetStringField(TEXT("error"), TEXT("No terminal result after explicit shutdown"));
			}
		}
		Pending.Empty();
	}
	void FinishMode()
	{
		auto Run = MakeShared<FJsonObject>();
		Run->SetStringField(TEXT("mode"), ReadbackModeName(Mode)); Run->SetNumberField(TEXT("repetition"), Repetition);
		Run->SetNumberField(TEXT("sample_count"), Sample); Run->SetNumberField(TEXT("warmup_draws"), WarmupDraws);
		Run->SetNumberField(TEXT("sample_idle_draws"), SampleIdleDraws);
		Run->SetNumberField(TEXT("run_wall_s"), FPlatformTime::Seconds() - RunWall);
		FAnimationCaptureReadbackStats Stats;
		const double ShutdownBegin = FPlatformTime::Seconds();
		if (Async) { Async->Shutdown(); Stats = Async->GetStats(); }
		else { Stats.Rejected = SyncRejected; Stats.Failed = SyncFailed; }
		UGameViewportClient::OnViewportRendered().Remove(DrawHandle);
		UGameViewportClient::OnViewportRendered().Remove(BeginDrawHandle);
		Async.Reset(); Sync.Reset();
		if (Mode == 0) { FlushRenderingCommands(); } // Match the explicit reader teardown drain between modes.
		Run->SetNumberField(TEXT("shutdown_wall_s"), FPlatformTime::Seconds() - ShutdownBegin);
		{
			FScopeLock Lock(&ViewPolicyState->Mutex);
			Run->SetNumberField(TEXT("renderer_views"), double(ViewPolicyState->Views - RunInitialViews));
			Run->SetNumberField(TEXT("unsupported_renderer_views"), double(ViewPolicyState->UnsupportedViews - RunInitialUnsupportedViews));
			if (ViewPolicyState->UnsupportedViews != RunInitialUnsupportedViews)
			{ Test->AddError(TEXT("Comparison renderer view policy diverged")); bRunLost = true; bAnyLoss = true; }
		}
		Test->TestEqual(TEXT("Every sampled draw uses the same admission cadence"), SampleIdleDraws, 0);
		if (SampleIdleDraws) { bRunLost = true; bAnyLoss = true; }
		Run->SetBoolField(TEXT("comparison_eligible"), !bRunLost);
		PerformanceStats(Run, Stats, Mode != 1);
		RunRows.Add(MakeShared<FJsonValueObject>(Run));
	}
	void Save()
	{
		if (Directory.IsEmpty()) { return; }
		auto Manifest = MakeShared<FJsonObject>();
		Manifest->SetNumberField(TEXT("schema_version"), 1);
		Manifest->SetArrayField(TEXT("samples"), Rows); Manifest->SetArrayField(TEXT("runs"), RunRows);
		Manifest->SetStringField(TEXT("output_policy"), TEXT("readback_only_no_encoding_or_file_export_in_timed_windows"));
		Manifest->SetStringField(TEXT("cadence"), TEXT("one_admission_attempt_per_viewport_draw; next_request_submitted_after_previous_draw; async_poll_on_automation_update"));
		Manifest->SetStringField(TEXT("fixture"), TEXT("Separated_engine_cubes_front350cm_unlit_no_aa_no_screen_percentage"));
		Manifest->SetStringField(TEXT("backend"), TEXT("D3D11_D32F_S8"));
		Manifest->SetStringField(TEXT("decoder"), FAnimationCaptureReadbackProducer::DecoderIdentity());
		Manifest->SetStringField(TEXT("view_policy"), TEXT("common_host_extension_all_modes_all_draws_including_idle; readers_do_not_override_resolution"));
		Manifest->SetStringField(TEXT("mode_order_policy"), TEXT("rotating_order: disabled_sync_async; sync_async_disabled; async_disabled_sync"));
		Manifest->SetObjectField(TEXT("engine_scheduling"), Scheduling);
		Manifest->SetNumberField(TEXT("width"), 640); Manifest->SetNumberField(TEXT("height"), 480);
		Manifest->SetNumberField(TEXT("warmup_draws"), 60); Manifest->SetNumberField(TEXT("samples_per_run"), 120);
		Manifest->SetNumberField(TEXT("repetitions"), 3); Manifest->SetBoolField(TEXT("comparison_eligible"), !bAnyLoss && Rows.Num() == 1080);
		Manifest->SetStringField(TEXT("acquisition_timing_scope"),
			TEXT("game_thread_request_call_plus_postdraw_submission_callback_bracket_and_synchronous_collect; render_thread_execution_is_in_frame_time"));
		Manifest->SetStringField(TEXT("decode_timing_scope"),
			TEXT("async_decode_separate; synchronous_decode_in_acquisition_and_depth_readback_wall_s_and_not_separately_measured"));
		Manifest->SetStringField(TEXT("completion_timing_scope"),
			TEXT("admission_to_completion_including_wait_for_next_draw; async_from_producer_admission; sync_from_request_call_start"));
		Manifest->SetStringField(TEXT("memory_accounting_scope"),
			TEXT("async_reserved_and_peak_bytes_include_producer_staging_packed_and_decoded_capacity; synchronous_peak_memory_not_instrumented_and_omitted"));
		Manifest->SetStringField(TEXT("frame_timing_scope"), TEXT("wall_time_between_viewport_draw_callbacks_includes_engine_and_automation_scheduling"));
		Manifest->SetStringField(TEXT("loss_policy"), TEXT("all_attempts_retained; null_latency_and_decode_for_noncompleted_attempts; losses_make_comparison_ineligible"));
		Manifest->SetStringField(TEXT("limits"), TEXT("Workstation and fixed fixture only; no speedup claimed by the test"));
		FString Text;
		const bool bSavedSuccessfully = FJsonSerializer::Serialize(Manifest, TJsonWriterFactory<>::Create(&Text))
			&& FFileHelper::SaveStringToFile(Text, *(Directory / TEXT("performance.json")));
		Test->TestTrue(TEXT("Raw comparison samples retained"), bSavedSuccessfully);
		bSaved = bSavedSuccessfully;
		Test->AddInfo(TEXT("PERFORMANCE_OUTPUT=") + Directory);
		if (bAnyLoss) { Test->AddWarning(TEXT("Performance comparison has losses; inspect outcomes before comparing timings")); }
	}
	void Cleanup()
	{
		UGameViewportClient::OnViewportRendered().Remove(DrawHandle);
		UGameViewportClient::OnViewportRendered().Remove(BeginDrawHandle);
		if (Async) { Async->Shutdown(); Async.Reset(); }
		Sync.Reset();
		if (ViewPolicy)
		{
			ViewPolicy.Reset();
			// One explicit fixture teardown drain, outside the timed measurement windows.
			FlushRenderingCommands();
		}
		Fixture.Stop();
	}
	FAutomationTestBase* Test;
	FAnimationCaptureHostFixture Fixture;
	TUniquePtr<FViewportSurfaceCapture> Sync;
	TUniquePtr<FViewportAsyncCapture> Async;
	TSharedRef<FPerformanceViewPolicyState, ESPMode::ThreadSafe> ViewPolicyState = MakeShared<FPerformanceViewPolicyState, ESPMode::ThreadSafe>();
	TSharedPtr<FPerformanceViewPolicy, ESPMode::ThreadSafe> ViewPolicy;
	TMap<uint64, FPerformancePendingRequest> Pending;
	FDelegateHandle BeginDrawHandle, DrawHandle;
	TArray<TSharedPtr<FJsonValue>> Rows, RunRows;
	TSharedPtr<FJsonObject> CurrentRow;
	TSharedRef<FJsonObject> Scheduling = MakeShared<FJsonObject>();
	FString Directory;
	int32 Mode = 0, ModePosition = 0, Repetition = 0, Sample = 0, WarmupDraws = 0, InitialDraws = 0, LastAttemptDraw = 0, SampleIdleDraws = 0;
	uint64 NextRequestId = 0, CurrentRequestId = 0, SyncRejected = 0, SyncFailed = 0, BeginDrawFrame = 0;
	uint64 RunInitialViews = 0, RunInitialUnsupportedViews = 0;
	bool bReadersReady = false, bAwaitingDraw = false, bCurrentAdmitted = false, bDraining = false;
	bool bRunLost = false, bAnyLoss = false, bDone = false, bSaved = false;
	double Started = 0, RunWall = 0, PreviousDraw = 0, DrawBegin = 0, FrameWall = 0, AdmissionWall = 0, AdmissionBeginWall = 0, DrainDeadline = 0;
};
}
#else
#define ANIMATION_CAPTURE_HOST_PERFORMANCE_HAS_ASYNC 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHostReadbackComparisonTest, "AnimationAnalysis.Capture.Performance.ReadbackComparison",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHostReadbackComparisonTest::RunTest(const FString&)
{
#if ANIMATION_CAPTURE_HOST_PERFORMANCE_HAS_ASYNC
	if (!FApp::CanEverRender()) { AddWarning(TEXT("ANIMATION_ANALYSIS_RENDERED_SKIP: D3D11 required")); return true; }
	ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Engine/Maps/Entry")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FHostReadbackComparison(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
#else
	AddError(TEXT("Comparable disabled/synchronous/asynchronous readback measurements are not implemented"));
#endif
	return !HasAnyErrors();
}
