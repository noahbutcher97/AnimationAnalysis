#include "AnimationCapture/AnimationCaptureSession.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

namespace
{
struct FHostWorld
{
	UWorld *World = UWorld::CreateWorld(EWorldType::Game, false);
	FHostWorld()
	{
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	}
	~FHostWorld()
	{
		Destroy();
	}
	void Destroy()
	{
		if (World)
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			World = nullptr;
		}
	}
	AActor *Spawn()
	{
		AActor *Actor = World->SpawnActor<AActor>();
		auto *Root = NewObject<USceneComponent>(Actor);
		Actor->SetRootComponent(Root);
		Root->RegisterComponent();
		return Actor;
	}
};

FAnimationCaptureSettings Settings()
{
	FAnimationCaptureSettings Result;
	Result.Scenario = TEXT("MovingFixture");
	Result.OutputRoot = FPaths::ProjectSavedDir() / TEXT("Observations");
	Result.FrameHz = 0;
	return Result;
}

TSharedPtr<FJsonObject> Read(const FString &File)
{
	FString Text;
	TSharedPtr<FJsonObject> Result;
	if (FFileHelper::LoadFileToString(Text, *File))
	{
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Result);
	}
	return Result;
}

class FFixtureExtension : public IAnimationCaptureExtension
{
  public:
	int32 Starts = 0, Ends = 0, Collections = 0;
	bool RejectStart = false, ReplaceIdentity = false, EscapeOutput = false;
	bool Begin(TConstArrayView<FAnimationCaptureSubject>, FString &Error) override
	{
		++Starts;
		if (RejectStart)
		{
			Error = TEXT("Fixture producer unavailable");
			return false;
		}
		return true;
	}
	void Collect() override
	{
		++Collections;
	}
	TSharedPtr<FJsonObject> ObserveSubject(int32) const override
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("sensor_reading"), 17);
		if (ReplaceIdentity)
		{
			Result->SetStringField(TEXT("role"), TEXT("Impostor"));
		}
		return Result;
	}
	void End(TArray<FAnimationCaptureTextArtifact> &Artifacts) override
	{
		++Ends;
		Artifacts.Add({EscapeOutput ? TEXT("../escaped.txt") : TEXT("sensor.txt"), TEXT("sensor complete")});
	}
};
} // namespace

// The namespace follows the repository's automation routing convention; this host
// compiles from Engine + the copied plugin only and contains no gameplay module.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIndependentCaptureLifecycleTest,
								 "AnimationAnalysis.Capture.Portability.IndependentSessions",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIndependentCaptureLifecycleTest::RunTest(const FString &)
{
	FHostWorld Fixture;
	AActor *Actor = Fixture.Spawn();
	FAnimationCaptureSubject Subject;
	Subject.Id = TEXT("MovingPart");
	Subject.Actor = Actor;
	TArray<FAnimationCaptureSubject> Subjects = {Subject};
	FAnimationCaptureSession First, Second;
	auto Extension = MakeShared<FFixtureExtension>();
	auto Configuration = Settings();
	Configuration.MaxSamples = 2;
	FString Error;
	if (!TestTrue(TEXT("Explicit Game-world capture starts"),
				  First.Start(Fixture.World, Configuration, Subjects, Error, Extension)))
	{
		AddError(Error);
		return false;
	}
	Configuration.MaxSamples = 100;
	if (!TestTrue(TEXT("Independent sessions may coexist"),
				  Second.Start(Fixture.World, Configuration, Subjects, Error)))
	{
		AddError(Error);
		return false;
	}
	TestNotEqual(TEXT("Sessions use separate output directories"), First.GetOutputDirectory(),
				 Second.GetOutputDirectory());
	Actor->SetActorLocation(FVector(10, 20, 30));
	Fixture.World->Tick(LEVELTICK_All, .02f);
	Actor->SetActorLocation(FVector(20, 20, 30));
	Fixture.World->Tick(LEVELTICK_All, .02f);
	Fixture.World->Tick(LEVELTICK_All, .02f);
	TestFalse(TEXT("Sample limit stops only the bounded session"), First.IsRecording());
	TestTrue(TEXT("Independent session remains live"), Second.IsRecording());
	TestEqual(TEXT("Extension released once by automatic stop"), Extension->Ends, 1);
	TestTrue(TEXT("Repeat stop remains successful"), First.Stop(TEXT("repeat"), Error));
	TestEqual(TEXT("Repeat stop does not release twice"), Extension->Ends, 1);
	TestEqual(TEXT("Native count bound preserved"), First.GetSampleCount(), 2);
	auto Manifest = Read(First.GetOutputDirectory() / TEXT("session.json"));
	if (!TestTrue(TEXT("Native manifest is parseable"), Manifest.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Native status complete"), Manifest->GetStringField(TEXT("status")), FString(TEXT("complete")));
	TestFalse(TEXT("No gameplay telemetry defaults"), Manifest->HasField(TEXT("max_telemetry_records_per_actor")));
	const auto Enrollment = Manifest->GetArrayField(TEXT("participants"))[0]->AsObject();
	TestEqual(TEXT("No humanoid point defaults"), Enrollment->GetArrayField(TEXT("points")).Num(), 0);
	FString Samples;
	FFileHelper::LoadFileToString(Samples, *(First.GetOutputDirectory() / TEXT("samples.jsonl")));
	TArray<FString> Lines;
	Samples.ParseIntoArrayLines(Lines);
	TestEqual(TEXT("Both observations exported"), Lines.Num(), 2);
	if (Lines.Num() == 2)
	{
		TSharedPtr<FJsonObject> Sample;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Lines[1]), Sample);
		if (TestTrue(TEXT("Observation parses"), Sample.IsValid()))
		{
			auto Observed = Sample->GetArrayField(TEXT("actors"))[0]->AsObject();
			TestEqual(TEXT("Known moving prop observed"), Observed->GetArrayField(TEXT("position_cm"))[0]->AsNumber(),
					  20.0);
			TestEqual(TEXT("Extension reading retained"), Observed->GetNumberField(TEXT("sensor_reading")), 17.0);
		}
	}
	Fixture.Destroy();
	TestFalse(TEXT("World teardown stops remaining session"), Second.IsRecording());
	TestTrue(TEXT("Teardown finalizes evidence"), Second.Stop(TEXT("repeat"), Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIndependentCaptureExtensionTest,
								 "AnimationAnalysis.Capture.Portability.ExtensionIntegrity",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIndependentCaptureExtensionTest::RunTest(const FString &)
{
	FHostWorld Fixture;
	FAnimationCaptureSubject Subject;
	Subject.Id = TEXT("SensorPart");
	Subject.Actor = Fixture.Spawn();
	TArray<FAnimationCaptureSubject> Subjects = {Subject};
	FAnimationCaptureSession Session;
	auto Extension = MakeShared<FFixtureExtension>();
	Extension->RejectStart = true;
	FString Error;
	TestFalse(TEXT("Unavailable producer rejects start"),
			  Session.Start(Fixture.World, Settings(), Subjects, Error, Extension));
	TestFalse(TEXT("Rejected start does not record"), Session.IsRecording());
	TestEqual(TEXT("Failed acquisition has no release obligation"), Extension->Ends, 0);
	Extension = MakeShared<FFixtureExtension>();
	Extension->ReplaceIdentity = true;
	TestTrue(TEXT("Recorder can restart after rejected producer"),
			 Session.Start(Fixture.World, Settings(), Subjects, Error, Extension));
	Fixture.World->Tick(LEVELTICK_All, .02f);
	TestFalse(TEXT("Conflicting producer fields invalidate evidence"), Session.Stop(TEXT("complete"), Error));
	TestTrue(TEXT("Identity collision explained"), Error.Contains(TEXT("replace recorder field")));
	TestEqual(TEXT("Failed evidence still releases producer"), Extension->Ends, 1);
	auto Manifest = Read(Session.GetOutputDirectory() / TEXT("session.json"));
	if (TestTrue(TEXT("Rejected evidence retains manifest"), Manifest.IsValid()))
	{
		TestEqual(TEXT("Conflicting result is an error"), Manifest->GetStringField(TEXT("status")),
				  FString(TEXT("error")));
	}
	FString Samples;
	FFileHelper::LoadFileToString(Samples, *(Session.GetOutputDirectory() / TEXT("samples.jsonl")));
	TestFalse(TEXT("Producer cannot replace subject identity"), Samples.Contains(TEXT("Impostor")));
	Extension = MakeShared<FFixtureExtension>();
	Extension->EscapeOutput = true;
	TestTrue(TEXT("Artifact fixture starts"), Session.Start(Fixture.World, Settings(), Subjects, Error, Extension));
	TestFalse(TEXT("Escaping artifact cannot complete"), Session.Stop(TEXT("complete"), Error));
	TestTrue(TEXT("Artifact path failure explained"), Error.Contains(TEXT("relative filename")));
	TestEqual(TEXT("Invalid artifact still releases producer"), Extension->Ends, 1);
	TestFalse(TEXT("No escaping file created"),
			  IFileManager::Get().FileExists(*(Settings().OutputRoot / TEXT("escaped.txt"))));
	return true;
}
