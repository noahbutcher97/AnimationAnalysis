#include "AnimationCaptureHostFixture.h"
#include "AnimationCapture/AnimationCaptureReadback.h"
#include "AnimationCapture/AnimationCaptureSession.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealClient.h"

namespace
{
class FSessionFixtureExtension : public IAnimationCaptureExtension
{
public:
	int32 Begins = 0, Ends = 0, Collections = 0;
	bool Begin(TConstArrayView<FAnimationCaptureSubject>, FString&) override { ++Begins; return true; }
	void Collect() override { ++Collections; }
	TSharedPtr<FJsonObject> DescribeSession() const override
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("fixture_extension_begins"), Begins);
		Result->SetNumberField(TEXT("fixture_extension_ends"), Ends);
		return Result;
	}
	void End(TArray<FAnimationCaptureTextArtifact>& Artifacts) override
	{
		++Ends;
		Artifacts.Add({TEXT("fixture-producer.txt"), TEXT("Neutral producer released")});
	}
};

TSharedPtr<FJsonObject> ReadObject(FAutomationTestBase* Test, const FString& Path)
{
	FString Text;
	TSharedPtr<FJsonObject> Object;
	if (!Test->TestTrue(TEXT("Session evidence exists: ") + Path, FFileHelper::LoadFileToString(Text, *Path))) { return nullptr; }
	Test->TestTrue(TEXT("Session evidence parses: ") + Path,
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) && Object.IsValid());
	return Object;
}

TArray<TSharedPtr<FJsonObject>> ReadLines(FAutomationTestBase* Test, const FString& Path)
{
	TArray<TSharedPtr<FJsonObject>> Result;
	FString Text;
	if (!Test->TestTrue(TEXT("Session stream exists: ") + Path, FFileHelper::LoadFileToString(Text, *Path))) { return Result; }
	TArray<FString> Lines; Text.ParseIntoArrayLines(Lines, true);
	for (const FString& Line : Lines)
	{
		TSharedPtr<FJsonObject> Object;
		if (Test->TestTrue(TEXT("Session stream row parses: ") + Path,
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Line), Object) && Object.IsValid())) { Result.Add(Object); }
	}
	return Result;
}

bool Number(FAutomationTestBase* Test, const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double& Value)
{
	return Test->TestTrue(FString(TEXT("Finite evidence field: ")) + Field,
		Object.IsValid() && Object->TryGetNumberField(Field, Value) && FMath::IsFinite(Value));
}

bool TextField(FAutomationTestBase* Test, const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FString& Value)
{
	return Test->TestTrue(FString(TEXT("Text evidence field: ")) + Field,
		Object.IsValid() && Object->TryGetStringField(Field, Value));
}

struct FSessionDrawWitness
{
	int32 FirstSample = 0, SecondSample = 0;
};

class FHostSessionControls : public IAutomationLatentCommand
{
public:
	FHostSessionControls(FAutomationTestBase* InTest, bool bInCleanup) : Test(InTest), bCleanupTest(bInCleanup) {}
	~FHostSessionControls() { Cleanup(); }

	bool Update() override
	{
		if (!StartWall) { StartWall = FPlatformTime::Seconds(); }
		if (FPlatformTime::Seconds() - StartWall > 60)
		{
			Test->AddError(TEXT("Async session fixture deadline exceeded; requested behavior remains unverified"));
			Cleanup(); return true;
		}
		if (Stage == 0)
		{
			UWorld* World = FAnimationCaptureHostFixture::FindWorld();
			if (!World) { return false; }
			FString Error;
			if (!Fixture.Start(World, Error)) { Test->AddError(Error); return true; }
			Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Observations") /
				(TEXT("SessionAsync-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			// UE 5.6 broadcasts native multicast delegates in reverse registration
			// order. Enroll before the sessions so the witness follows their draws.
			DrawHandle = UGameViewportClient::OnViewportRendered().AddRaw(this, &FHostSessionControls::Draw);
			Actor = Fixture.Meshes[0]->GetOwner(); OriginalActorPath = Actor->GetPathName();
			Actor->SetActorLabel(TEXT("SessionMovingPart"));
			FirstExtension = MakeShared<FSessionFixtureExtension>(); SecondExtension = MakeShared<FSessionFixtureExtension>();
			if (!StartSession(First, FirstExtension, Actor.Get(), TEXT("ConcurrentFirst")) ||
				!StartSession(Second, SecondExtension, Actor.Get(), TEXT("ConcurrentSecond"))) { Cleanup(); return true; }
			Test->TestNotEqual(TEXT("Concurrent sessions own distinct directories"), First.GetOutputDirectory(), Second.GetOutputDirectory());
			Stage = 1; return false;
		}
		if (bCleanupRequested)
		{
			if (First.IsRecording() || Second.IsRecording()) { return false; }
			const int32 Cancelled = VerifySession(First, FirstExtension, 0, true, false) +
				VerifySession(Second, SecondExtension, 1, true, false);
			Test->TestTrue(TEXT("PIE cleanup exercised admitted in-flight requests, evidenced by cancellation"), Cancelled > 0);
			Test->TestEqual(TEXT("World cleanup stopped first session"), First.GetStopReason(), FString(TEXT("world_cleanup")));
			Test->TestEqual(TEXT("World cleanup stopped independent session"), Second.GetStopReason(), FString(TEXT("world_cleanup")));
			Cleanup(); return true;
		}
		if (!First.IsRecording() || (Stage == 1 && !Second.IsRecording()))
		{
			Test->AddError(TEXT("A session stopped before the requested frame/sample evidence was acquired: ") + First.GetStopReason());
			Cleanup(); return true;
		}
		if (Actor.IsValid()) { Actor->SetActorLocation(FVector(400, -60 + (++MovementStep % 5) * 4, 10000)); }
		if (bCleanupTest) { return false; }
		if (Stage == 1 && First.GetFrameCount() >= 3 && Second.GetFrameCount() >= 3)
		{
			FString Error;
			Test->TestTrue(TEXT("One concurrent session drains admitted RGB frames on normal stop"), Second.Stop(TEXT("inspection_complete"), Error));
			if (!Error.IsEmpty()) { Test->AddError(Error); }
			Test->TestTrue(TEXT("Stopping one session preserves the other owner"), First.IsRecording());
			VerifySession(Second, SecondExtension, 1, false, false);
			SamplesBeforeDestroy = First.GetSampleCount();
			Actor->Destroy(); Actor.Reset();
			// A same-label, same-shape actor does not replace fixed enrollment.
			AActor* NewActor = Fixture.World->SpawnActor<AActor>(); Replacement = NewActor;
			NewActor->SetActorLabel(TEXT("SessionMovingPart"));
			auto* Mesh = NewObject<UStaticMeshComponent>(NewActor); NewActor->SetRootComponent(Mesh);
			Mesh->SetStaticMesh(Fixture.Meshes[1]->GetStaticMesh()); Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Mesh->RegisterComponent(); NewActor->SetActorLocation(FVector(400, -60, 10000));
			Stage = 2; return false;
		}
		if (Stage == 2 && First.GetSampleCount() >= SamplesBeforeDestroy + 3)
		{
			FString Error;
			Test->TestTrue(TEXT("Moving-subject session drains on normal stop"), First.Stop(TEXT("inspection_complete"), Error));
			if (!Error.IsEmpty()) { Test->AddError(Error); }
			VerifySession(First, FirstExtension, 0, false, true);
			const FString Previous = First.GetOutputDirectory(); DrawWitnesses.Reset();
			FirstExtension = MakeShared<FSessionFixtureExtension>();
			Actor = Fixture.Meshes[1]->GetOwner(); OriginalActorPath = Actor->GetPathName();
			if (!StartSession(First, FirstExtension, Actor.Get(), TEXT("RestartedExplicitEnrollment"))) { Cleanup(); return true; }
			Test->TestNotEqual(TEXT("Restart gets a new session identity"), First.GetOutputDirectory(), Previous);
			Stage = 3; return false;
		}
		if (Stage == 3 && First.GetFrameCount() >= 3)
		{
			FString Error;
			Test->TestTrue(TEXT("Restarted async session drains on stop"), First.Stop(TEXT("inspection_complete"), Error));
			if (!Error.IsEmpty()) { Test->AddError(Error); }
			VerifySession(First, FirstExtension, 0, false, false);
			Cleanup(); return true;
		}
		return false;
	}

private:
	bool StartSession(FAnimationCaptureSession& Session, const TSharedPtr<FSessionFixtureExtension>& Extension,
		AActor* SubjectActor, const FString& Scenario)
	{
		FAnimationCaptureSettings Settings;
		Settings.Scenario = Scenario; Settings.OutputRoot = Directory;
		Settings.SampleHz = 60; Settings.FrameHz = 60; Settings.MaxWallSeconds = 10;
		Settings.MaxSamples = 1200; Settings.MaxFrames = 32; Settings.MaxDataBytes = 64ll * 1024 * 1024;
		Settings.bUseAsyncReadback = true;
		Settings.bUseAsyncDiagnosticResolution = true;
		Settings.SharedBudget = SharedBudget;
		FAnimationCaptureSubject Subject; Subject.Id = TEXT("MovingPart"); Subject.Actor = SubjectActor;
		TArray<FAnimationCaptureSubject> Subjects = {Subject}; FString Error;
		if (!Test->TestTrue(TEXT("Explicit async session starts"), Session.Start(Fixture.World.Get(), Settings, Subjects, Error, Extension)))
		{ Test->AddError(Error); return false; }
		return true;
	}

	void Draw(FViewport* Viewport)
	{
		if (!Fixture.World.IsValid() || Viewport != Fixture.Viewport || bCleanupRequested) { return; }
		DrawWitnesses.Add(GFrameCounter, {First.GetSampleCount(), Second.GetSampleCount()});
		if (bCleanupTest && First.GetFrameCount() >= 1 && Second.GetFrameCount() >= 1)
		{
			// Queue the ordinary editor teardown from after-draw, never destroy a
			// world from inside its tick/capture callback. Editor Tick handles this
			// after rendering; final cancelled events establish actual pending work.
			bCleanupRequested = true;
			GEditor->RequestEndPlayMap();
		}
	}

	void VerifyPNG(const FString& Path, int32 Width, int32 Height)
	{
		TArray<uint8> Bytes;
		if (!Test->TestTrue(TEXT("Completed PNG exists: ") + Path, FFileHelper::LoadFileToArray(Bytes, *Path))) { return; }
		if (!Test->TestTrue(TEXT("PNG contains a complete signature and IHDR"), Bytes.Num() >= 33)) { return; }
		const uint8 Signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
		Test->TestTrue(TEXT("Completed output has a PNG signature"), FMemory::Memcmp(Bytes.GetData(), Signature, 8) == 0);
		const auto BigEndian = [&Bytes](int32 Offset)
		{ return (uint32(Bytes[Offset]) << 24) | (uint32(Bytes[Offset + 1]) << 16) | (uint32(Bytes[Offset + 2]) << 8) | uint32(Bytes[Offset + 3]); };
		Test->TestEqual(TEXT("PNG begins with IHDR"), BigEndian(12), uint32(0x49484452));
		Test->TestEqual(TEXT("PNG width agrees with acquisition"), BigEndian(16), uint32(Width));
		Test->TestEqual(TEXT("PNG height agrees with acquisition"), BigEndian(20), uint32(Height));
		// Full image decoding remains available from the retained replay archive;
		// this check deliberately claims only actual PNG output/header integrity.
	}

	int32 VerifySession(FAnimationCaptureSession& Session, const TSharedPtr<FSessionFixtureExtension>& Extension,
		int32 Slot, bool bWorldCleanup, bool bDestroyedSubject)
	{
		const FString Output = Session.GetOutputDirectory(); Test->AddInfo(TEXT("SESSION_ASYNC_OUTPUT=") + Output);
		Test->TestFalse(TEXT("Session is stopped before reading final evidence"), Session.IsRecording());
		Test->TestEqual(TEXT("Extension begins once"), Extension->Begins, 1);
		Test->TestEqual(TEXT("Extension ends exactly once"), Extension->Ends, 1);
		Test->TestTrue(TEXT("Extension received live collections"), Extension->Collections > 0);
		const auto Manifest = ReadObject(Test, Output / TEXT("session.json"));
		const auto Frames = ReadLines(Test, Output / TEXT("frames.jsonl"));
		const auto Samples = ReadLines(Test, Output / TEXT("samples.jsonl"));
		const auto Events = ReadLines(Test, Output / TEXT("readbacks.jsonl"));
		if (!Manifest.IsValid()) { return 0; }
		FString Mode, Status;
		if (TextField(Test, Manifest, TEXT("readback_mode"), Mode)) { Test->TestEqual(TEXT("Manifest identifies async acquisition"), Mode, FString(TEXT("asynchronous"))); }
		TextField(Test, Manifest, TEXT("status"), Status);
		for (const TCHAR* Field : {TEXT("readback_pending_requests"), TEXT("image_pending_frames"), TEXT("readback_failed"),
			TEXT("readback_timed_out"), TEXT("readback_rejected"), TEXT("image_rejected_frames")})
		{
			double Value = -1;
			if (Number(Test, Manifest, Field, Value)) { Test->TestEqual(FString(TEXT("No residual/failure counter: ")) + Field, Value, 0.0); }
		}
		double ManifestFrames = -1, Submitted = -1, ManifestCancelled = -1;
		double PipelinePeakFrames = -1, PipelinePeakBytes = -1;
		if (Number(Test, Manifest, TEXT("capture_pipeline_peak_frames"), PipelinePeakFrames))
		{ Test->TestTrue(TEXT("Combined readback/encoding pipeline stays within four frames"), PipelinePeakFrames >= 1 && PipelinePeakFrames <= 4); }
		if (Number(Test, Manifest, TEXT("capture_pipeline_peak_reserved_bytes"), PipelinePeakBytes))
		{ Test->TestTrue(TEXT("Combined readback/encoding reservation stays within 64 MiB"), PipelinePeakBytes > 0 && PipelinePeakBytes <= 64.0 * 1024 * 1024); }
		if (Number(Test, Manifest, TEXT("frame_count"), ManifestFrames)) { Test->TestEqual(TEXT("Manifest counts completed rows"), ManifestFrames, double(Frames.Num())); }
		if (Number(Test, Manifest, TEXT("image_submitted_frames"), Submitted)) { Test->TestEqual(TEXT("Every admitted request has one terminal event"), Submitted, double(Events.Num())); }
		Number(Test, Manifest, TEXT("readback_cancelled"), ManifestCancelled);
		Test->TestTrue(TEXT("Completed RGB frames were acquired before stop"), Frames.Num() >= (bWorldCleanup ? 1 : 3));
		TMap<int32, TSharedPtr<FJsonObject>> Terminal;
		int32 Cancelled = 0, Completed = 0;
		for (const auto& Event : Events)
		{
			double Id = -1, Acquired = -1, Complete = -1; FString EventStatus, EventSession, EventError;
			if (!Number(Test, Event, TEXT("request_id"), Id) || !TextField(Test, Event, TEXT("status"), EventStatus)) { continue; }
			Test->TestTrue(TEXT("Terminal request IDs are positive integers"), Id >= 1 && Id <= 32 && FMath::FloorToDouble(Id) == Id);
			Test->TestFalse(TEXT("Each request completes only once"), Terminal.Contains(int32(Id))); Terminal.Add(int32(Id), Event);
			if (TextField(Test, Event, TEXT("session_id"), EventSession)) { Test->TestEqual(TEXT("Terminal event belongs to this session"), EventSession, FPaths::GetCleanFilename(Output)); }
			if (Number(Test, Event, TEXT("acquisition_wall_elapsed_s"), Acquired) && Number(Test, Event, TEXT("completed_wall_elapsed_s"), Complete))
			{ Test->TestTrue(TEXT("Terminal completion follows acquisition"), Complete >= Acquired && Acquired >= 0); }
			if (EventStatus == TEXT("completed")) { ++Completed; }
			else if (EventStatus == TEXT("cancelled"))
			{
				++Cancelled; Test->TestTrue(TEXT("Cancellation is limited to intentional world teardown"), bWorldCleanup);
				if (TextField(Test, Event, TEXT("error"), EventError)) { Test->TestFalse(TEXT("Cancelled request explains its missing evidence"), EventError.IsEmpty()); }
			}
			else { Test->AddError(TEXT("Unexpected terminal readback status: ") + EventStatus); }
		}
		Test->TestEqual(TEXT("All completed requests became actual image rows"), Completed, Frames.Num());
		Test->TestEqual(TEXT("Manifest cancellation counter agrees with explicit gaps"), ManifestCancelled, double(Cancelled));
		Test->TestEqual(TEXT("Manifest reports complete or explicit cancelled evidence"), Status, FString(Cancelled ? TEXT("error") : TEXT("complete")));
		Test->TestTrue(TEXT("Fresh session request sequence starts at one"), Terminal.Contains(1));
		TMap<int32, TSharedPtr<FJsonObject>> SamplesByIndex;
		int32 MissingAfterDestroy = 0; double MinimumY = TNumericLimits<double>::Max(), MaximumY = TNumericLimits<double>::Lowest();
		for (const auto& Sample : Samples)
		{
			double Index = -1; if (!Number(Test, Sample, TEXT("index"), Index)) { continue; }
			SamplesByIndex.Add(int32(Index), Sample);
			const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
			if (!Test->TestTrue(TEXT("Each sample keeps explicit enrollment"), Sample->TryGetArrayField(TEXT("actors"), Actors) && Actors->Num() == 1)) { continue; }
			const auto Subject = (*Actors)[0]->AsObject(); bool bValid = false; FString Role;
			if (!Test->TestTrue(TEXT("Sample carries subject validity"), Subject.IsValid() && Subject->TryGetBoolField(TEXT("valid"), bValid))) { continue; }
			if (TextField(Test, Subject, TEXT("role"), Role)) { Test->TestEqual(TEXT("Explicit subject ID retained"), Role, FString(TEXT("MovingPart"))); }
			if (bDestroyedSubject && Index > SamplesBeforeDestroy)
			{
				Test->TestFalse(TEXT("Destroyed enrollment does not switch to same-label replacement"), bValid);
				MissingAfterDestroy += !bValid; Test->TestFalse(TEXT("Missing actor has no replacement transform"), Subject->HasField(TEXT("position_cm")));
			}
			if (bValid)
			{
				const TArray<TSharedPtr<FJsonValue>>* Position = nullptr;
				if (Test->TestTrue(TEXT("Observed actor has a position"), Subject->TryGetArrayField(TEXT("position_cm"), Position) && Position->Num() == 3))
				{ MinimumY = FMath::Min(MinimumY, (*Position)[1]->AsNumber()); MaximumY = FMath::Max(MaximumY, (*Position)[1]->AsNumber()); }
			}
		}
		if (bDestroyedSubject) { Test->TestTrue(TEXT("Destroyed actor remains missing over subsequent samples"), MissingAfterDestroy >= 3); }
		if (!bWorldCleanup) { Test->TestTrue(TEXT("Live movement was actually sampled"), MaximumY - MinimumY > 1); }
		const TArray<TSharedPtr<FJsonValue>>* Participants = nullptr;
		if (Test->TestTrue(TEXT("Manifest retains one enrolled actor"), Manifest->TryGetArrayField(TEXT("participants"), Participants) && Participants->Num() == 1))
		{
			FString Path; if (TextField(Test, (*Participants)[0]->AsObject(), TEXT("actor"), Path)) { Test->TestEqual(TEXT("Enrollment actor path remains the original"), Path, OriginalActorPath); }
		}
		bool bDelayedFrame = false; TSet<int32> FrameRequests;
		for (const auto& Frame : Frames)
		{
			double Id = -1, Index = -1, EngineFrame = -1, CollectedFrame = -1, SampleIndex = -1;
			if (!Number(Test, Frame, TEXT("readback_request_id"), Id) || !Number(Test, Frame, TEXT("engine_frame"), EngineFrame) ||
				!Number(Test, Frame, TEXT("sample_index"), SampleIndex)) { continue; }
			Test->TestFalse(TEXT("A request never creates duplicate image rows"), FrameRequests.Contains(int32(Id))); FrameRequests.Add(int32(Id));
			if (Number(Test, Frame, TEXT("index"), Index)) { Test->TestEqual(TEXT("Frame index is its admission request ID"), Index, Id); }
			if (Number(Test, Frame, TEXT("readback_collected_engine_frame"), CollectedFrame))
			{
				Test->TestTrue(TEXT("Collection does not predate the acquisition frame"), CollectedFrame >= EngineFrame);
				bDelayedFrame |= CollectedFrame > EngineFrame;
			}
			const FSessionDrawWitness* Witness = DrawWitnesses.Find(uint64(EngineFrame));
			if (Test->TestTrue(TEXT("Frame retains an actual originating draw"), Witness != nullptr))
			{ Test->TestEqual(TEXT("Sample link belongs to acquisition rather than collection"), SampleIndex, double(Slot ? Witness->SecondSample : Witness->FirstSample)); }
			const auto* Sample = SamplesByIndex.Find(int32(SampleIndex));
			if (Test->TestTrue(TEXT("Image sample link resolves"), Sample != nullptr))
			{
				double SampleEngineFrame = -1;
				if (Number(Test, *Sample, TEXT("engine_frame"), SampleEngineFrame)) { Test->TestTrue(TEXT("Image links to an acquired sample, never a future sample"), SampleEngineFrame <= EngineFrame); }
			}
			const auto* Event = Terminal.Find(int32(Id));
			if (Test->TestTrue(TEXT("Image request has terminal evidence"), Event != nullptr))
			{
				double EventEngine = -1; FString EventStatus;
				if (Number(Test, *Event, TEXT("engine_frame"), EventEngine)) { Test->TestEqual(TEXT("Image retains the producer acquisition frame"), EngineFrame, EventEngine); }
				if (TextField(Test, *Event, TEXT("status"), EventStatus)) { Test->TestEqual(TEXT("Only completed requests create images"), EventStatus, FString(TEXT("completed"))); }
			}
			FString FrameSession, FrameMode, Decoder, File;
			if (TextField(Test, Frame, TEXT("readback_session_id"), FrameSession)) { Test->TestEqual(TEXT("Image retains its session identity"), FrameSession, FPaths::GetCleanFilename(Output)); }
			if (TextField(Test, Frame, TEXT("readback_mode"), FrameMode)) { Test->TestEqual(TEXT("Image uses async acquisition"), FrameMode, FString(TEXT("asynchronous"))); }
			if (TextField(Test, Frame, TEXT("readback_decoder"), Decoder)) { Test->TestEqual(TEXT("Image identifies the actual producer decoder"), Decoder, FString(FAnimationCaptureReadbackProducer::DecoderIdentity())); }
			double Acquisition = -1, Complete = -1, Collected = -1, Encoded = -1;
			if (Number(Test, Frame, TEXT("wall_elapsed_s"), Acquisition) && Number(Test, Frame, TEXT("readback_completed_wall_elapsed_s"), Complete) &&
				Number(Test, Frame, TEXT("readback_collected_wall_elapsed_s"), Collected) && Number(Test, Frame, TEXT("image_collected_wall_elapsed_s"), Encoded))
			{ Test->TestTrue(TEXT("Acquisition, GPU completion, collection and encoding clocks remain ordered"), Acquisition >= 0 && Complete >= Acquisition && Collected >= Complete && Encoded >= Collected); }
			const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
			if (Test->TestTrue(TEXT("Image retains its explicit pose-link inventory"), Frame->TryGetArrayField(TEXT("pose_links"), Links) && Links->Num() == 1))
			{
				const auto Link = (*Links)[0]->AsObject(); FString Role; double Revision = -1, PoseFrame = -1;
				if (TextField(Test, Link, TEXT("role"), Role)) { Test->TestEqual(TEXT("Pose link retains explicit subject identity"), Role, FString(TEXT("MovingPart"))); }
				if (Number(Test, Link, TEXT("pose_evaluation_serial"), Revision)) { Test->TestEqual(TEXT("Static fixture does not fabricate a skeletal revision"), Revision, 0.0); }
				if (Number(Test, Link, TEXT("pose_engine_frame"), PoseFrame)) { Test->TestEqual(TEXT("Absent skeletal pose is not refreshed to collection frame"), PoseFrame, 0.0); }
			}
			double Width = -1, Height = -1;
			if (TextField(Test, Frame, TEXT("file"), File) && Number(Test, Frame, TEXT("width"), Width) && Number(Test, Frame, TEXT("height"), Height))
			{
				if (Test->TestTrue(TEXT("PNG path remains inside its session"), File.StartsWith(TEXT("frames/")) && !File.Contains(TEXT("..")) && !File.Contains(TEXT(":"))))
				{ VerifyPNG(Output / File, int32(Width), int32(Height)); }
			}
		}
		Test->TestTrue(TEXT("Real delayed completion was exercised without relabeling acquisition"), bDelayedFrame);
		FString RepeatError; const bool bRepeatSuccess = Session.Stop(TEXT("repeat"), RepeatError);
		Test->TestEqual(TEXT("Repeat stop preserves final evidence status"), bRepeatSuccess, Cancelled == 0);
		Test->TestEqual(TEXT("Repeat stop does not release the extension twice"), Extension->Ends, 1);
		return Cancelled;
	}

	void Cleanup()
	{
		UGameViewportClient::OnViewportRendered().Remove(DrawHandle); DrawHandle.Reset();
		FString Error; First.Stop(TEXT("fixture_cleanup"), Error); Second.Stop(TEXT("fixture_cleanup"), Error);
		Test->TestEqual(TEXT("Shared session/image/PNG admission retires on cleanup"), SharedBudget->GetStats().LiveBytes, int64(0));
		if (Replacement.IsValid()) { Replacement->Destroy(); } Replacement.Reset(); Actor.Reset();
		Fixture.Stop();
	}

	FAutomationTestBase* Test;
	FAnimationCaptureHostFixture Fixture;
	FAnimationCaptureSession First, Second;
	TSharedRef<FAnimationCaptureBudget, ESPMode::ThreadSafe> SharedBudget = MakeShared<FAnimationCaptureBudget, ESPMode::ThreadSafe>(FAnimationCaptureBudgetLimits{32, 128ll * 1024 * 1024});
	TSharedPtr<FSessionFixtureExtension> FirstExtension, SecondExtension;
	TWeakObjectPtr<AActor> Actor, Replacement;
	TMap<uint64, FSessionDrawWitness> DrawWitnesses;
	FDelegateHandle DrawHandle;
	FString Directory, OriginalActorPath;
	double StartWall = 0;
	int32 Stage = 0, MovementStep = 0, SamplesBeforeDestroy = 0;
	bool bCleanupTest = false, bCleanupRequested = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHostAsyncSessionRGBTest, "AnimationAnalysis.Capture.Async.SessionRGB",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHostAsyncSessionRGBTest::RunTest(const FString&)
{
	if (!FApp::CanEverRender()) { AddWarning(TEXT("ANIMATION_ANALYSIS_RENDERED_SKIP: session RGB requires D3D11 PIE")); return true; }
	ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Engine/Maps/Entry")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FHostSessionControls(this, false));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand()); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHostAsyncSessionCleanupTest, "AnimationAnalysis.Capture.Async.SessionCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHostAsyncSessionCleanupTest::RunTest(const FString&)
{
	if (!FApp::CanEverRender()) { AddWarning(TEXT("ANIMATION_ANALYSIS_RENDERED_SKIP: session cleanup requires D3D11 PIE")); return true; }
	ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Engine/Maps/Entry")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FHostSessionControls(this, true));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand()); return true;
}
