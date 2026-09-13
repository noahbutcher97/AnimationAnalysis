#pragma once

// Included by MeshTests.cpp to share its transient procedural fixture and independent oracle.
namespace
{
class FMeshGPUCombinedControl : public IAutomationLatentCommand
{
public:
	explicit FMeshGPUCombinedControl(FAutomationTestBase* T) : Test(T), Started(FPlatformTime::Seconds()) {}
	~FMeshGPUCombinedControl() { Close(); }
	bool Update() override
	{
		if (FPlatformTime::Seconds() - Started > 180) { Test->AddError(TEXT("GPU combined deadline")); return true; }
		FString Error;
		if (!Ready)
		{
			auto* World = FAnimationCaptureHostFixture::FindWorld(); if (!World) { return false; }
			Cache = IConsoleManager::Get().FindConsoleVariable(TEXT("r.SkinCache.Mode")); FPS = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS"));
			OldCache = Cache->GetInt(); OldFPS = FPS->GetFloat(); Cache->SetWithCurrentPriority(1); FPS->SetWithCurrentPriority(60.f);
			if (!Fixture.Start(World, Error)) { Test->AddError(Error); return true; }
			Fixture.Mesh->SetWorldLocation(FVector(400, -75, 10000)); Freeze(Fixture.Mesh);
			Second = NewObject<USkeletalMeshComponent>(Fixture.Actor.Get()); Second->SetSkeletalMesh(Fixture.Mesh->GetSkeletalMeshAsset());
			Second->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Second->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
			Second->RegisterComponent(); Second->PlayAnimation(Fixture.Mesh->GetSingleNodeInstance()->GetCurrentAsset(), true);
			Second->SetWorldTransform(FTransform(FRotator(0, 12, 0), FVector(400, 150, 10000), FVector(1.2, .8, 1.1))); Freeze(Second);
			for (auto* M : {Fixture.Mesh, Second}) { M->SkinCacheUsage.Init(ESkinCacheUsage::Enabled, 2); M->MarkRenderStateDirty(); }
			Diagnostic = MakeUnique<FViewportAsyncCapture>(World, Fixture.View.Viewport, true);
			for (auto M : Fixture.View.Meshes) { M->SetRenderCustomDepth(false); } // Release fixture-owned label values before assigning new subjects.
			TArray<FSurfaceCaptureLabel> Subjects{{7, TEXT("Primary"), Fixture.Mesh}, {23, TEXT("Second"), Second}, {29, TEXT("Prop"), Fixture.Prop}};
			if (!Labels.Apply(World, Subjects, Error)) { Test->AddError(Error); return true; }
			Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Observations") / (TEXT("GpuMeshCombined-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			IFileManager::Get().MakeDirectory(*Directory, true);
			Mode = 3; if (!CreateOwners(Error)) { Test->AddError(Error); return true; }
			Ready = true; return false;
		}
		if (!RasterSaved)
		{
			if (++Warm < 8) { return false; }
			if (!RasterRequested)
			{
				FAnimationMeshGPUTicket Ticket;
				if (!G0->Request(TEXT("raster-mesh"), Ticket, Error) || !RequestImage(1, Error)) { Test->AddError(Error); return true; }
				RasterRequested = true; return false;
			}
			G0->Pump();
			if (!HaveMesh) { HaveMesh = G0->Collect(Mesh); }
			if (!HaveSurface) { HaveSurface = Raster->Poll(Surface); }
			if (!HaveMesh || !HaveSurface) { return false; }
			if (!SaveRaster(Error)) { Test->AddError(Error); return true; }
			Mesh = {}; Surface = {}; StopOwners(); RasterSaved = true; Repetition = 0; ModeIndex = 0;
			if (!StartMode(Error)) { Test->AddError(Error); return true; } return false;
		}
		const double Now = FPlatformTime::Seconds();
		const double WorkStart = Now;
		Poll();
		if (Draining)
		{
			if ((G0 && G0->GetStats().PendingRequests) || (G1 && G1->GetStats().PendingRequests)
				|| (Raster && Raster->GetStats().PendingRequests) || Writer->GetPendingCount()) { return false; }
			const double DrainMs = (Now - DrainStarted) * 1000;
			FinishMode(DrainMs);
			if (++ModeIndex == 4) { ModeIndex = 0; ++Repetition; }
			if (Repetition == 3)
			{
				auto Report = MakeShared<FJsonObject>(); Report->SetArrayField(TEXT("runs"), Runs);
				Report->SetNumberField(TEXT("requested_hz"), 60); Report->SetNumberField(TEXT("warmup_frames"), 30);
				Report->SetNumberField(TEXT("measured_frames"), 90); Report->SetNumberField(TEXT("subjects"), 2);
				Report->SetNumberField(TEXT("vertices_each"), 10302); Report->SetNumberField(TEXT("width"), 640); Report->SetNumberField(TEXT("height"), 480);
				Report->SetNumberField(TEXT("png_request_id_multiple"), 10);
				Report->SetStringField(TEXT("cadence"), TEXT("One attempt per automation update; achieved cadence measured; two bone meshes plus attached rigid reference"));
				if (!SaveJson(Report, TEXT("benchmark.json"))) { Test->AddError(TEXT("Could not save benchmark")); }
				Test->AddInfo(TEXT("GPU_COMBINED_OUTPUT=") + Directory); return true;
			}
			if (!StartMode(Error)) { Test->AddError(Error); return true; } return false;
		}
		const bool Measured = Frame >= 30;
		if (Measured) { Intervals.Add(MakeShared<FJsonValueNumber>((Now - Last) * 1000)); }
		Last = Now;
		if (Mode == 1)
		{
			for (auto* CPU : {CPU0.Get(), CPU1.Get(), Rigid.Get()})
			{
				auto Sample = CPU->Capture(TEXT("benchmark"), Error);
				if (!Sample) { ++CPUFailures; if (FirstError.IsEmpty()) { FirstError = Error; } }
			}
		}
		if (Mode >= 2)
		{
			for (int32 Subject = 0; Subject < 2; ++Subject)
			{
				FAnimationMeshGPUTicket Ticket;
				const FString Id = FString::Printf(TEXT("%s-%d-%d"), Measured ? TEXT("m") : TEXT("w"), Frame, Subject);
				if (!(Subject == 0 ? G0 : G1)->Request(Id, Ticket, Error)) { ++MeshRejected; }
			}
			auto Prop = Rigid->Capture(TEXT("prop"), Error); if (!Prop) { ++CPUFailures; if (FirstError.IsEmpty()) { FirstError = Error; } }
		}
		if (Mode == 3 && !RequestImage(uint64(Frame + 1), Error)) { ++ImageRejected; }
		if (Measured)
		{
			Work.Add(MakeShared<FJsonValueNumber>((FPlatformTime::Seconds() - WorkStart) * 1000));
			const auto Stats = Budget->GetStats(); Occupancy.Add(MakeShared<FJsonValueNumber>(double(Stats.LiveBytes)));
		}
		if (++Frame == 120) { Draining = true; DrainStarted = FPlatformTime::Seconds(); }
		return false;
	}
private:
	void Freeze(USkeletalMeshComponent* M)
	{
		M->SetForcedLOD(1); M->SetPosition(.25, false); M->GetSingleNodeInstance()->SetPlaying(false);
		M->TickAnimation(0, false); M->RefreshBoneTransforms();
	}
	bool CreateOwners(FString& Error)
	{
		Budget = MakeShared<FAnimationCaptureBudget, ESPMode::ThreadSafe>(FAnimationCaptureBudgetLimits{64, 128ll * 1024 * 1024});
		auto MeshBudget = MakeShared<FAnimationMeshBudget, ESPMode::ThreadSafe>(ReferenceLimits(), Budget);
		CPU0 = FAnimationCaptureMeshReference::Create(Fixture.Mesh, Enrollment(Fixture.Mesh, TEXT("combined-primary")), MeshBudget, Error);
		CPU1 = FAnimationCaptureMeshReference::Create(Second, Enrollment(Second, TEXT("combined-second")), MeshBudget, Error);
		Rigid = FAnimationCaptureMeshReference::Create(Fixture.Prop, Enrollment(Fixture.Prop, TEXT("combined-prop")), MeshBudget, Error);
		if (!CPU0 || !CPU1 || !Rigid) { return false; }
		Writer = MakeUnique<FAnimationCaptureImageWriter>(Budget);
		if (Mode >= 2)
		{
			G0 = FAnimationCaptureMeshGPU::Create(Fixture.View.World.Get(), Fixture.View.Viewport, Fixture.Mesh,
				Enrollment(Fixture.Mesh, TEXT("combined-primary")), ReferenceLimits(), {}, Budget.ToSharedRef(), Error);
			G1 = FAnimationCaptureMeshGPU::Create(Fixture.View.World.Get(), Fixture.View.Viewport, Second,
				Enrollment(Second, TEXT("combined-second")), ReferenceLimits(), {}, Budget.ToSharedRef(), Error);
			if (!G0 || !G1) { return false; }
		}
		if (Mode == 3) { Raster = MakeUnique<FViewportAsyncCapture>(Fixture.View.World.Get(), Fixture.View.Viewport, true, FAnimationCaptureReadbackLimits{}, Budget); }
		return true;
	}
	bool RequestImage(uint64 Id, FString& Error)
	{
		FAnimationCaptureReadbackRequest Request; Request.SessionId = TEXT("combined"); Request.ViewportId = TEXT("main");
		Request.ClockId = TEXT("unreal-monotonic"); Request.RequestId = Id; Request.Size = FIntPoint(640, 480);
		Request.AcquisitionTimeSeconds = FPlatformTime::Seconds(); FAnimationCaptureReadbackTicket Ticket;
		return Raster->Request(Request, Ticket, Error);
	}
	bool SaveJson(const TSharedRef<FJsonObject>& Value, const FString& Name)
	{
		FString Json; return FJsonSerializer::Serialize(Value, TJsonWriterFactory<>::Create(&Json))
			&& FFileHelper::SaveStringToFile(Json, *(Directory / Name));
	}
	bool SaveRaster(FString& Error)
	{
		if (Mesh.Status != EAnimationMeshGPUStatus::Completed || !Mesh.Snapshot || Surface.Status != EAnimationCaptureReadbackStatus::Completed)
		{ Error = TEXT("Combined capture failed: ") + Mesh.Error + TEXT("; ") + Surface.Error; return false; }
		if (Mesh.View.EngineFrame != Surface.View.EngineFrame || Mesh.View.RendererFrameNumber != Surface.View.RendererFrameNumber
			|| !Mesh.View.WorldToClip.Equals(Surface.View.WorldToClip))
		{ Error = TEXT("Combined mesh and image acquisition views differ"); return false; }
		if (!AnimationCaptureMeshReplay::Write(Directory, TEXT("raster-mesh"), *Mesh.Snapshot, Error)) { return false; }
		const uint32 Width = 640, Height = 480; const int32 Pixels = Width * Height;
		if (Surface.RGB.Num() != Pixels || Surface.Labels.Num() != Pixels || Surface.SceneDepthCm.Num() != Pixels || Surface.LabelDepthCm.Num() != Pixels)
		{ Error = TEXT("Combined image channel sizes differ"); return false; }
		TArray<uint8> Bytes; const ANSICHAR Magic[] = "SURFACE1";
		auto Append = [&](const void* Data, int32 Count) { Bytes.Append(static_cast<const uint8*>(Data), Count); };
		Append(Magic, 8); Append(&Surface.View.EngineFrame, 8); Append(&Width, 4); Append(&Height, 4);
		Append(Surface.RGB.GetData(), Pixels * 4); Bytes.Append(Surface.Labels);
		Append(Surface.SceneDepthCm.GetData(), Pixels * 4); Append(Surface.LabelDepthCm.GetData(), Pixels * 4);
		if (!FFileHelper::SaveArrayToFile(Bytes, *(Directory / TEXT("raster.surface")))) { Error = TEXT("Could not save raster"); return false; }
		auto View = MakeShared<FJsonObject>(); View->SetStringField(TEXT("format"), TEXT("gpu_mesh_raster_view")); View->SetNumberField(TEXT("schema_version"), 1);
		View->SetNumberField(TEXT("engine_frame"), double(Mesh.View.EngineFrame)); View->SetNumberField(TEXT("renderer_frame"), Mesh.View.RendererFrameNumber);
		TArray<TSharedPtr<FJsonValue>> Matrix;
		for (int32 R = 0; R < 4; ++R) for (int32 C = 0; C < 4; ++C) { Matrix.Add(MakeShared<FJsonValueNumber>(Mesh.View.WorldToClip.M[R][C])); }
		View->SetArrayField(TEXT("world_to_clip_row_major"), Matrix); View->SetNumberField(TEXT("label"), 7);
		View->SetStringField(TEXT("topology_id"), Mesh.Snapshot->Data().TopologyId); View->SetStringField(TEXT("configuration_id"), Mesh.Snapshot->Data().ConfigurationId);
		View->SetStringField(TEXT("producer_id"), Mesh.Snapshot->Data().ProducerId); View->SetNumberField(TEXT("lod"), 0);
		View->SetStringField(TEXT("render_pass_id"), TEXT("viewport-main-depth-v1"));
		if (!SaveJson(View, TEXT("raster-view.json"))) { Error = TEXT("Could not save view"); return false; }
		auto Saturation = Budget->Reserve(128ll * 1024 * 1024 - Budget->GetStats().LiveBytes);
		if (!Test->TestTrue(TEXT("Combined budget saturation admitted"), Saturation.IsValid())) { return false; }
		TArray<FColor> Copy = Surface.RGB;
		Test->TestFalse(TEXT("Shared byte saturation rejects PNG before ownership transfer"), Writer->Enqueue(Directory / TEXT("rejected.png"), FIntPoint(Width, Height), MoveTemp(Copy), Error));
		Test->TestEqual(TEXT("Rejected writer preserves caller pixels"), Copy.Num(), Pixels);
		Saturation.Reset();
		if (!Writer->Enqueue(Directory / TEXT("raster.png"), FIntPoint(Width, Height), MoveTemp(Copy), Error)) { return false; }
		FAnimationCaptureImageWriter::FResult Written;
		if (!Writer->Collect(Written, true) || !Written.Error.IsEmpty()) { Error = Written.Error; return false; }
		return true;
	}
	bool StartMode(FString& Error)
	{
		Mode = (ModeIndex + Repetition) % 4; Frame = 0; Draining = false; Last = FPlatformTime::Seconds();
		CPUFailures = MeshRejected = ImageRejected = PngRejected = PngCompleted = 0; FirstError.Reset();
		Intervals.Reset(); Work.Reset(); Occupancy.Reset(); MeshRows.Reset(); ImageRows.Reset();
		return CreateOwners(Error);
	}
	void Poll()
	{
		for (auto* GPU : {G0.Get(), G1.Get()}) if (GPU)
		{
			GPU->Pump(); FAnimationMeshGPUResult Result;
			while (GPU->Collect(Result))
			{
				if (Result.RequestId.StartsWith(TEXT("m-")))
				{
					auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("request"), Result.RequestId);
					Row->SetNumberField(TEXT("status"), int32(Result.Status)); Row->SetStringField(TEXT("error"), Result.Error);
					Row->SetNumberField(TEXT("prepare_ms"), Result.PrepareSeconds * 1000); Row->SetNumberField(TEXT("setup_wait_ms"), Result.SetupWaitSeconds * 1000);
					Row->SetNumberField(TEXT("render_capture_ms"), Result.CaptureSeconds * 1000); Row->SetNumberField(TEXT("copy_enqueue_ms"), Result.EnqueueWorkSeconds * 1000);
					Row->SetNumberField(TEXT("decode_ms"), Result.DecodeSeconds * 1000); Row->SetNumberField(TEXT("completion_latency_ms"), (Result.CompletedSeconds - Result.RequestedSeconds) * 1000);
					MeshRows.Add(MakeShared<FJsonValueObject>(Row));
				}
				Result = {};
			}
		}
		if (Raster)
		{
			FAnimationCaptureReadbackResult Result;
			while (Raster->Poll(Result))
			{
				if (Result.Request.RequestId > 30)
				{
					auto Row = MakeShared<FJsonObject>(); Row->SetNumberField(TEXT("request"), double(Result.Request.RequestId));
					Row->SetNumberField(TEXT("status"), int32(Result.Status)); Row->SetStringField(TEXT("error"), Result.Error);
					Row->SetNumberField(TEXT("decode_ms"), Result.DecodeSeconds * 1000); Row->SetNumberField(TEXT("completion_latency_ms"), Result.CompletionLatencySeconds * 1000);
					ImageRows.Add(MakeShared<FJsonValueObject>(Row));
				}
				if (Result.Status == EAnimationCaptureReadbackStatus::Completed && Result.Request.RequestId % 10 == 0)
				{
					FString Error; const FString Name = FString::Printf(TEXT("benchmark-%d-%d-%llu.png"), Repetition, Mode, Result.Request.RequestId);
					if (!Writer->Enqueue(Directory / Name, Result.Request.Size, MoveTemp(Result.RGB), Error)) { ++PngRejected; }
				}
				Result = {};
			}
		}
		FAnimationCaptureImageWriter::FResult Written;
		while (Writer->Collect(Written, false)) { ++PngCompleted; if (!Written.Error.IsEmpty()) { Test->AddError(Written.Error); } }
	}
	void FinishMode(double DrainMs)
	{
		auto Row = MakeShared<FJsonObject>(); static const TCHAR* Names[] = {TEXT("disabled"), TEXT("cpu"), TEXT("gpu"), TEXT("combined")};
		Row->SetStringField(TEXT("mode"), Names[Mode]); Row->SetNumberField(TEXT("repetition"), Repetition);
		Row->SetArrayField(TEXT("frame_interval_ms"), Intervals); Row->SetArrayField(TEXT("game_thread_work_ms"), Work);
		Row->SetArrayField(TEXT("live_bytes"), Occupancy); Row->SetArrayField(TEXT("mesh_results"), MeshRows); Row->SetArrayField(TEXT("image_results"), ImageRows);
		Row->SetNumberField(TEXT("cpu_failures"), CPUFailures); Row->SetStringField(TEXT("first_cpu_error"), FirstError);
		Row->SetNumberField(TEXT("mesh_rejected"), MeshRejected); Row->SetNumberField(TEXT("image_rejected"), ImageRejected);
		Row->SetNumberField(TEXT("png_rejected"), PngRejected); Row->SetNumberField(TEXT("png_completed"), PngCompleted); Row->SetNumberField(TEXT("drain_ms"), DrainMs);
		TArray<TSharedPtr<FJsonValue>> Producers;
		for (auto* GPU : {G0.Get(), G1.Get()}) if (GPU)
		{
			GPU->Shutdown(); const auto Stats = GPU->GetStats(); auto P = MakeShared<FJsonObject>();
			P->SetNumberField(TEXT("admitted"), double(Stats.Admitted)); P->SetNumberField(TEXT("completed"), double(Stats.Completed));
			P->SetNumberField(TEXT("unavailable"), double(Stats.Unavailable)); P->SetNumberField(TEXT("failed"), double(Stats.Failed));
			P->SetNumberField(TEXT("cancelled"), double(Stats.Cancelled)); P->SetNumberField(TEXT("timed_out"), double(Stats.TimedOut));
			P->SetNumberField(TEXT("peak_source_bytes"), double(Stats.PeakSourceBytes)); P->SetNumberField(TEXT("peak_staging_bytes"), double(Stats.PeakStagingBytes));
			P->SetNumberField(TEXT("peak_pending"), Stats.PeakPendingRequests); P->SetNumberField(TEXT("shutdown_ms"), Stats.ShutdownSeconds * 1000);
			Producers.Add(MakeShared<FJsonValueObject>(P));
			Test->TestEqual(TEXT("Every admitted benchmark mesh request completes exactly once"), Stats.Completed + Stats.Unavailable + Stats.Failed + Stats.Cancelled + Stats.TimedOut, Stats.Admitted);
			Test->TestEqual(TEXT("Qualified workload supplies every admitted mesh result"), Stats.Completed, Stats.Admitted);
		}
		Row->SetArrayField(TEXT("mesh_producers"), Producers);
		if (Raster)
		{
			Raster->Shutdown(); const auto Stats = Raster->GetStats();
			Row->SetNumberField(TEXT("image_admitted"), double(Stats.Admitted)); Row->SetNumberField(TEXT("image_completed"), double(Stats.Completed));
			Row->SetNumberField(TEXT("image_failed"), double(Stats.Failed)); Row->SetNumberField(TEXT("image_cancelled"), double(Stats.Cancelled));
			Row->SetNumberField(TEXT("image_timed_out"), double(Stats.TimedOut)); Row->SetNumberField(TEXT("image_shutdown_ms"), Stats.ShutdownWallSeconds * 1000);
			Test->TestEqual(TEXT("Qualified workload supplies every admitted image"), Stats.Completed, Stats.Admitted);
		}
		StopOwners(); const auto Stats = Budget->GetStats();
		Row->SetNumberField(TEXT("peak_bytes"), double(Stats.PeakBytes)); Row->SetNumberField(TEXT("peak_reservations"), Stats.PeakReservations);
		Row->SetNumberField(TEXT("shared_admitted"), double(Stats.Admitted)); Row->SetNumberField(TEXT("shared_rejected"), double(Stats.Rejected));
		Test->TestEqual(TEXT("Combined mode teardown releases every shared byte"), Stats.LiveBytes, int64(0));
		Test->TestEqual(TEXT("Qualified CPU benchmark has no missing pose"), CPUFailures, 0);
		Runs.Add(MakeShared<FJsonValueObject>(Row));
	}
	void StopOwners()
	{
		if (Raster) { Raster->Shutdown(); } if (G0) { G0->Shutdown(); } if (G1) { G1->Shutdown(); }
		Raster.Reset(); G0.Reset(); G1.Reset(); Writer.Reset(); CPU0.Reset(); CPU1.Reset(); Rigid.Reset();
	}
	void Close()
	{
		StopOwners(); if (Diagnostic) { Diagnostic->Shutdown(); Diagnostic.Reset(); } Labels.Restore();
		if (Second) { Second->DestroyComponent(); Second = nullptr; } Fixture.Stop();
		if (Cache) { Cache->SetWithCurrentPriority(OldCache); Cache = nullptr; }
		if (FPS) { FPS->SetWithCurrentPriority(OldFPS); FPS = nullptr; }
	}
	FAutomationTestBase* Test;
	double Started, Last = 0, DrainStarted = 0;
	FMeshFixture Fixture; USkeletalMeshComponent* Second = nullptr;
	TSharedPtr<FAnimationCaptureBudget, ESPMode::ThreadSafe> Budget;
	TUniquePtr<FAnimationCaptureMeshGPU> G0, G1;
	TUniquePtr<FViewportAsyncCapture> Raster, Diagnostic;
	TUniquePtr<FAnimationCaptureImageWriter> Writer;
	TUniquePtr<FAnimationCaptureMeshReference> CPU0, CPU1, Rigid;
	FScopedSurfaceCaptureLabels Labels;
	IConsoleVariable* Cache = nullptr; IConsoleVariable* FPS = nullptr; int32 OldCache = 0; float OldFPS = 0;
	bool Ready = false, RasterRequested = false, HaveMesh = false, HaveSurface = false, RasterSaved = false, Draining = false;
	int32 Warm = 0, Mode = 0, ModeIndex = 0, Frame = 0, Repetition = 0;
	int32 CPUFailures = 0, MeshRejected = 0, ImageRejected = 0, PngRejected = 0, PngCompleted = 0;
	FString Directory, FirstError;
	FAnimationMeshGPUResult Mesh; FAnimationCaptureReadbackResult Surface;
	TArray<TSharedPtr<FJsonValue>> Intervals, Work, Occupancy, MeshRows, ImageRows, Runs;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshGPUCombinedTest, "AnimationAnalysis.Capture.Mesh.GPUCombined",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMeshGPUCombinedTest::RunTest(const FString&)
{
	if (!FApp::CanEverRender()) { AddError(TEXT("GPU combined requires rendering")); return false; }
	ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Engine/Maps/Entry")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FMeshGPUCombinedControl(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand()); return true;
}
