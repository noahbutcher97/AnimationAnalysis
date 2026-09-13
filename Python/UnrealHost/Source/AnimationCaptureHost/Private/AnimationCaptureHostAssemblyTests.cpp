#include "AnimationCapture/AnimationCaptureMeshReference.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
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

namespace
{
constexpr double AssemblyCubeScale = 25.0 / 64.0;

FAnimationMeshLimits AssemblyLimits(int32 MaxSnapshots = 16)
{
	return {MaxSnapshots, 1024, 4096, 8, 1, 32ll * 1024 * 1024};
}

FAnimationMeshEnrollment AssemblyEnrollment(UMeshComponent* Mesh, const TCHAR* Id)
{
	FAnimationMeshEnrollment Enrollment;
	Enrollment.ComponentId = Id;
	Enrollment.ComponentGeneration = 1;
	Enrollment.AssetId = FString(Id) + TEXT("-engine-cube");
	Enrollment.ConfigurationGeneration = 1;
	Enrollment.ConfigurationId = TEXT("engine-cube-100cm");
	Enrollment.SubjectId = Id;
	Enrollment.StreamId = TEXT("assembly");
	Enrollment.AnalysisLOD = 0;
	for (int32 Index = 0; Index < Mesh->GetNumMaterials(); ++Index)
	{
		Enrollment.MaterialIds.Add(FString::Printf(TEXT("engine-cube-material-%d"), Index));
	}
	return Enrollment;
}

TArray<TSharedPtr<FJsonValue>> IntegerArray(int32 Count)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Values.Add(MakeShared<FJsonValueNumber>(Index));
	}
	return Values;
}

struct FRigidAssemblyFixture
{
	TWeakObjectPtr<AActor> FixedActor;
	TWeakObjectPtr<AActor> MovingActor;
	TArray<TWeakObjectPtr<AActor>> ControlActors;
	UStaticMeshComponent* Fixed = nullptr;
	UStaticMeshComponent* Moving = nullptr;
	TSharedRef<FAnimationMeshBudget, ESPMode::ThreadSafe> Budget =
		MakeShared<FAnimationMeshBudget, ESPMode::ThreadSafe>(AssemblyLimits());
	TSharedRef<FAnimationMeshBudget, ESPMode::ThreadSafe> FailureBudget =
		MakeShared<FAnimationMeshBudget, ESPMode::ThreadSafe>(AssemblyLimits(1));
	TUniquePtr<FAnimationCaptureMeshReference> FixedReference;
	TUniquePtr<FAnimationCaptureMeshReference> MovingReference;
	TUniquePtr<FAnimationCaptureMeshReference> DuplicateComponentReference;
	TUniquePtr<FAnimationCaptureMeshReference> FailureFixedReference;
	TUniquePtr<FAnimationCaptureMeshReference> FailureMovingReference;
	TUniquePtr<FAnimationCaptureMeshReference> AttachedReference;
	TUniquePtr<FAnimationCaptureMeshReference> PhysicsReference;
	TUniquePtr<FAnimationCaptureMeshReference> SkeletalReference;
	TUniquePtr<FAnimationCaptureMeshReference> RetiredReference;

	~FRigidAssemblyFixture() { Stop(); }

	UStaticMeshComponent* AddStatic(UWorld* World, UStaticMesh* Cube, const FVector& Location,
		TWeakObjectPtr<AActor>& OutActor, FString& Error)
	{
		AActor* Actor = World->SpawnActor<AActor>();
		if (!Actor)
		{
			Error = TEXT("Could not spawn rigid assembly actor");
			return nullptr;
		}
		OutActor = Actor;
		auto* Component = NewObject<UStaticMeshComponent>(Actor);
		Actor->SetRootComponent(Component);
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetStaticMesh(Cube);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetComponentTickEnabled(false);
		Component->RegisterComponent();
		Component->SetWorldTransform(FTransform(FRotator::ZeroRotator, Location, FVector(AssemblyCubeScale)));
		return Component;
	}

	UStaticMeshComponent* AddControlStatic(UWorld* World, UStaticMesh* Cube, const FVector& Location,
		FString& Error)
	{
		TWeakObjectPtr<AActor> Actor;
		auto* Component = AddStatic(World, Cube, Location, Actor, Error);
		if (Actor.IsValid())
		{
			ControlActors.Add(Actor);
		}
		return Component;
	}

	bool Start(UWorld* World, FString& Error)
	{
		if (!World)
		{
			Error = TEXT("PIE world is unavailable");
			return false;
		}
		auto* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/EngineMeshes/Cube.Cube"));
		if (!Cube)
		{
			Error = TEXT("Engine cube is unavailable");
			return false;
		}
		Fixed = AddStatic(World, Cube, FVector(0, 0, 10000), FixedActor, Error);
		Moving = AddStatic(World, Cube, FVector(140, 0, 10000), MovingActor, Error);
		if (!Fixed || !Moving)
		{
			return false;
		}
		FixedReference = FAnimationCaptureMeshReference::Create(Fixed, AssemblyEnrollment(Fixed, TEXT("fixed-part")), Budget, Error);
		MovingReference = FAnimationCaptureMeshReference::Create(Moving, AssemblyEnrollment(Moving, TEXT("moving-part")), Budget, Error);
		DuplicateComponentReference = FAnimationCaptureMeshReference::Create(Fixed, AssemblyEnrollment(Fixed, TEXT("fixed-part-alternate")), Budget, Error);
		FailureFixedReference = FAnimationCaptureMeshReference::Create(Fixed, AssemblyEnrollment(Fixed, TEXT("failure-fixed")), FailureBudget, Error);
		FailureMovingReference = FAnimationCaptureMeshReference::Create(Moving, AssemblyEnrollment(Moving, TEXT("failure-moving")), FailureBudget, Error);
		if (!FixedReference || !MovingReference || !DuplicateComponentReference || !FailureFixedReference || !FailureMovingReference)
		{
			return false;
		}

		auto* Attached = AddControlStatic(World, Cube, FVector(400, 0, 10000), Error);
		AttachedReference = Attached ? FAnimationCaptureMeshReference::Create(
			Attached, AssemblyEnrollment(Attached, TEXT("attached-control")), Budget, Error) : nullptr;
		if (!AttachedReference)
		{
			return false;
		}
		Attached->AttachToComponent(Fixed, FAttachmentTransformRules::KeepWorldTransform);

		auto* Physics = AddControlStatic(World, Cube, FVector(600, 0, 10000), Error);
		PhysicsReference = Physics ? FAnimationCaptureMeshReference::Create(
			Physics, AssemblyEnrollment(Physics, TEXT("physics-control")), Budget, Error) : nullptr;
		if (!PhysicsReference)
		{
			return false;
		}
		Physics->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Physics->SetSimulatePhysics(true);

		auto* Retired = AddControlStatic(World, Cube, FVector(800, 0, 10000), Error);
		RetiredReference = Retired ? FAnimationCaptureMeshReference::Create(
			Retired, AssemblyEnrollment(Retired, TEXT("retired-control")), Budget, Error) : nullptr;
		if (!RetiredReference)
		{
			return false;
		}
		Retired->UnregisterComponent();

		AActor* SkeletalActor = World->SpawnActor<AActor>();
		if (!SkeletalActor)
		{
			Error = TEXT("Could not spawn skeletal boundary actor");
			return false;
		}
		ControlActors.Add(SkeletalActor);
		auto* Skeletal = NewObject<USkeletalMeshComponent>(SkeletalActor);
		SkeletalActor->SetRootComponent(Skeletal);
		auto* SkeletalAsset = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
		if (!SkeletalAsset)
		{
			Error = TEXT("Engine skeletal cube is unavailable");
			return false;
		}
		Skeletal->SetSkeletalMesh(SkeletalAsset);
		Skeletal->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		Skeletal->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Skeletal->RegisterComponent();
		SkeletalReference = FAnimationCaptureMeshReference::Create(
			Skeletal, AssemblyEnrollment(Skeletal, TEXT("skeletal-control")), Budget, Error);
		return SkeletalReference.IsValid();
	}

	void Stop()
	{
		FixedReference.Reset();
		MovingReference.Reset();
		DuplicateComponentReference.Reset();
		FailureFixedReference.Reset();
		FailureMovingReference.Reset();
		AttachedReference.Reset();
		PhysicsReference.Reset();
		SkeletalReference.Reset();
		RetiredReference.Reset();
		for (auto Actor : ControlActors)
		{
			if (Actor.IsValid())
			{
				Actor->Destroy();
			}
		}
		ControlActors.Reset();
		if (FixedActor.IsValid()) { FixedActor->Destroy(); }
		if (MovingActor.IsValid()) { MovingActor->Destroy(); }
		FixedActor.Reset();
		MovingActor.Reset();
		Fixed = nullptr;
		Moving = nullptr;
	}
};

class FRigidAssemblyControls : public IAutomationLatentCommand
{
public:
	explicit FRigidAssemblyControls(FAutomationTestBase* InTest)
		: Test(InTest), Started(FPlatformTime::Seconds()) {}
	~FRigidAssemblyControls() { Fixture.Stop(); }

	bool Update() override
	{
		if (FPlatformTime::Seconds() - Started > 45)
		{
			Test->AddError(TEXT("Rigid assembly control deadline exceeded"));
			bChecksPassed = false;
			return true;
		}
		FString Error;
		if (!bInitialized)
		{
			UWorld* World = nullptr;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::PIE) { World = Context.World(); break; }
			}
			if (!World) { return false; }
			if (!Fixture.Start(World, Error))
			{
				Test->AddError(Error);
				bChecksPassed = false;
				return true;
			}
			Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Observations") /
				(TEXT("RigidAssembly-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			if (!IFileManager::Get().MakeDirectory(*Directory, true))
			{
				Test->AddError(TEXT("Could not create rigid assembly evidence directory"));
				return true;
			}
			RunBoundaryControls();
			if (!bChecksPassed) { return true; }
			bInitialized = true;
			LastSampleSeconds = FPlatformTime::Seconds() - .1;
		}

		if (SampleIndex < 5 && FPlatformTime::Seconds() - LastSampleSeconds >= .1)
		{
			CaptureSample();
			LastSampleSeconds = FPlatformTime::Seconds();
			if (!bChecksPassed) { return true; }
		}
		if (SampleIndex < 5) { return false; }
		Finish();
		return true;
	}

private:
	void Record(bool Result) { bChecksPassed &= Result; }

	void RunBoundaryControls()
	{
		FString Error;
		auto IndividualFixed = Fixture.FixedReference->Capture(TEXT("individual-fixed"), Error);
		auto IndividualMoving = Fixture.MovingReference->Capture(TEXT("individual-moving"), Error);
		Record(Test->TestTrue(TEXT("Individual fixed capture succeeds"), IndividualFixed.IsValid()));
		Record(Test->TestTrue(TEXT("Individual moving capture succeeds"), IndividualMoving.IsValid()));
		if (IndividualFixed && IndividualMoving)
		{
			Record(Test->TestTrue(TEXT("Individual captures do not imply paired acquisition"),
				IndividualFixed->Data().AcquiredSeconds != IndividualMoving->Data().AcquiredSeconds));
		}
		IndividualFixed.Reset();
		IndividualMoving.Reset();
		Fixture.Fixed->SetWorldTransform(FTransform(FRotator::ZeroRotator,
			FVector(0.123456789, 0, 10000), FVector(AssemblyCubeScale)));
		auto RoundTrip = Fixture.FixedReference->Capture(TEXT("roundtrip-control"), Error);
		Record(Test->TestTrue(TEXT("Replay round-trip control capture succeeds"), RoundTrip.IsValid()));
		if (RoundTrip)
		{
			const FString RoundTripRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Observations") /
				(TEXT("MeshRoundTrip-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			Record(Test->TestTrue(TEXT("Replay round-trip control directory created"),
				IFileManager::Get().MakeDirectory(*RoundTripRoot, true)));
			Record(Test->TestTrue(TEXT("Replay round-trip control bundle written"),
				AnimationCaptureMeshReplay::Write(RoundTripRoot, TEXT("sample"), *RoundTrip, Error)));
			FString RecordJson;
			TSharedPtr<FJsonObject> RecordObject;
			const FString RecordPath = RoundTripRoot / TEXT("sample") / TEXT("record.json");
			const bool bParsed = FFileHelper::LoadFileToString(RecordJson, *RecordPath)
				&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(RecordJson), RecordObject)
				&& RecordObject.IsValid();
			Record(Test->TestTrue(TEXT("Replay round-trip record parses"), bParsed));
			if (bParsed)
			{
				const auto Observation = RecordObject->GetObjectField(TEXT("observation"));
				const double Acquired = Observation->GetObjectField(TEXT("acquired"))->GetNumberField(TEXT("seconds"));
				const double Completed = RecordObject->GetObjectField(TEXT("completed"))->GetNumberField(TEXT("seconds"));
				const auto& Matrix = Observation->GetArrayField(TEXT("component_to_world"));
				Record(Test->TestTrue(TEXT("Replay acquisition round-trips exactly"),
					Acquired == RoundTrip->Data().AcquiredSeconds));
				Record(Test->TestTrue(TEXT("Replay completion round-trips exactly"),
					Completed == RoundTrip->Data().CompletedSeconds));
				Record(Test->TestTrue(TEXT("Replay transform round-trips beyond six decimals"),
					Matrix.Num() == 16 && Matrix[12]->AsNumber() == RoundTrip->Data().ComponentToWorld.M[3][0]
					&& Matrix[12]->AsNumber() == 0.123456789));
			}
		}
		RoundTrip.Reset();
		Fixture.Fixed->SetWorldTransform(FTransform(FRotator::ZeroRotator,
			FVector(0, 0, 10000), FVector(AssemblyCubeScale)));

		TArray<TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe>> Pair;
		TArray<FAnimationCaptureMeshReference*> Empty;
		Record(Test->TestFalse(TEXT("Empty batch is unavailable"),
			FAnimationCaptureMeshReference::CaptureRigidBatch(Empty, TEXT("empty"), 2, Pair, Error)));
		Record(Test->TestEqual(TEXT("Empty batch leaves no snapshots"), Pair.Num(), 0));

		TArray<FAnimationCaptureMeshReference*> Valid{Fixture.FixedReference.Get(), Fixture.MovingReference.Get()};
		Record(Test->TestFalse(TEXT("Zero maximum is unavailable"),
			FAnimationCaptureMeshReference::CaptureRigidBatch(Valid, TEXT("zero-max"), 0, Pair, Error)));
		Record(Test->TestFalse(TEXT("Maximum above 64 is unavailable"),
			FAnimationCaptureMeshReference::CaptureRigidBatch(Valid, TEXT("large-max"), 65, Pair, Error)));
		Record(Test->TestFalse(TEXT("Count above the supplied maximum is unavailable"),
			FAnimationCaptureMeshReference::CaptureRigidBatch(Valid, TEXT("over-limit"), 1, Pair, Error)));

		TArray<FAnimationCaptureMeshReference*> DuplicateObserver{Fixture.FixedReference.Get(), Fixture.FixedReference.Get()};
		Record(Test->TestFalse(TEXT("Duplicate observer is unavailable"),
			FAnimationCaptureMeshReference::CaptureRigidBatch(DuplicateObserver, TEXT("duplicate-observer"), 2, Pair, Error)));
		TArray<FAnimationCaptureMeshReference*> DuplicateComponent{Fixture.FixedReference.Get(), Fixture.DuplicateComponentReference.Get()};
		Record(Test->TestFalse(TEXT("Duplicate component is unavailable"),
			FAnimationCaptureMeshReference::CaptureRigidBatch(DuplicateComponent, TEXT("duplicate-component"), 2, Pair, Error)));

		auto Rejects = [&](const TCHAR* Label, const TCHAR* Id, FAnimationCaptureMeshReference* Unsupported)
		{
			TArray<FAnimationCaptureMeshReference*> Inputs{Fixture.FixedReference.Get(), Unsupported};
			Record(Test->TestFalse(Label,
				FAnimationCaptureMeshReference::CaptureRigidBatch(Inputs, Id, 2, Pair, Error)));
			Record(Test->TestEqual(TEXT("Unsupported batch leaves no snapshots"), Pair.Num(), 0));
		};
		Rejects(TEXT("Attached static component is unavailable"), TEXT("attached"), Fixture.AttachedReference.Get());
		Rejects(TEXT("Physics component is unavailable"), TEXT("physics"), Fixture.PhysicsReference.Get());
		Rejects(TEXT("Skeletal component is unavailable"), TEXT("skeletal"), Fixture.SkeletalReference.Get());
		Rejects(TEXT("Retired component is unavailable"), TEXT("retired"), Fixture.RetiredReference.Get());

		TArray<FAnimationCaptureMeshReference*> AdmissionInputs{
			Fixture.FailureFixedReference.Get(), Fixture.FailureMovingReference.Get()};
		Record(Test->TestFalse(TEXT("Batch admission is transactional"),
			FAnimationCaptureMeshReference::CaptureRigidBatch(AdmissionInputs, TEXT("admission"), 2, Pair, Error)));
		Record(Test->TestTrue(TEXT("Failed batch reached partial admission"), Fixture.FailureBudget->PeakBytes() > 0));
		Record(Test->TestEqual(TEXT("Failed batch leaves no snapshots"), Pair.Num(), 0));
		Record(Test->TestEqual(TEXT("Failed batch releases admission"), Fixture.FailureBudget->LiveBytes(), int64(0)));
		Record(Test->TestEqual(TEXT("Failed batch releases snapshot count"), Fixture.FailureBudget->LiveSnapshots(), 0));
	}

	void CaptureSample()
	{
		static const int32 XPositions[] = {140, 110, 100, 80, 140};
		const FString SampleId = FString::Printf(TEXT("step-%02d"), SampleIndex);
		Fixture.Moving->SetWorldLocation(FVector(XPositions[SampleIndex], 0, 10000));
		TArray<FAnimationCaptureMeshReference*> Samplers{Fixture.FixedReference.Get(), Fixture.MovingReference.Get()};
		TArray<TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe>> Pair;
		FString Error;
		if (!FAnimationCaptureMeshReference::CaptureRigidBatch(Samplers, SampleId, 2, Pair, Error))
		{
			Test->AddError(Error);
			bChecksPassed = false;
			return;
		}
		Record(Test->TestEqual(TEXT("Rigid pair has two snapshots"), Pair.Num(), 2));
		if (Pair.Num() != 2) { return; }
		Record(Test->TestEqual(TEXT("Shared acquisition"), Pair[0]->Data().AcquiredSeconds, Pair[1]->Data().AcquiredSeconds));
		Record(Test->TestEqual(TEXT("Shared engine frame"), Pair[0]->Data().FrameId, Pair[1]->Data().FrameId));
		Record(Test->TestTrue(TEXT("Completion follows acquisition"), Pair[1]->Data().CompletedSeconds >= Pair[1]->Data().AcquiredSeconds));
		Record(Test->TestTrue(TEXT("Each snapshot retains its own completion stamp"),
			Pair[0]->Data().CompletedSeconds != Pair[1]->Data().CompletedSeconds));
		Record(Test->TestEqual(TEXT("Fixed identity is literal"), Pair[0]->Data().Enrollment.ComponentId, FString(TEXT("fixed-part"))));
		Record(Test->TestEqual(TEXT("Moving identity is literal"), Pair[1]->Data().Enrollment.ComponentId, FString(TEXT("moving-part"))));
		Record(Test->TestEqual(TEXT("Fixed subject is literal"), Pair[0]->Data().Enrollment.SubjectId, FString(TEXT("fixed-part"))));
		Record(Test->TestEqual(TEXT("Moving subject is literal"), Pair[1]->Data().Enrollment.SubjectId, FString(TEXT("moving-part"))));
		Record(Test->TestEqual(TEXT("Assembly stream is literal"), Pair[0]->Data().Enrollment.StreamId, FString(TEXT("assembly"))));
		Record(Test->TestEqual(TEXT("Engine cube has 24 vertices"), Pair[0]->Data().Positions.Num(), 24));
		Record(Test->TestEqual(TEXT("Engine cube has 12 triangles"), Pair[0]->Data().Indices.Num(), 36));
		Record(Test->TestEqual(TEXT("Moving engine cube has 24 vertices"), Pair[1]->Data().Positions.Num(), 24));
		Record(Test->TestEqual(TEXT("Moving engine cube has 12 triangles"), Pair[1]->Data().Indices.Num(), 36));
		Record(Test->TestTrue(TEXT("Fixed transform is frozen at exact fixture scale and center"),
			Pair[0]->Data().ComponentToWorld.Equals(
				FTransform(FRotator::ZeroRotator, FVector(0, 0, 10000), FVector(AssemblyCubeScale)).ToMatrixWithScale(), 1.e-8)));
		Record(Test->TestTrue(TEXT("Moving transform is frozen at exact fixture scale and center"),
			Pair[1]->Data().ComponentToWorld.Equals(
				FTransform(FRotator::ZeroRotator, FVector(XPositions[SampleIndex], 0, 10000), FVector(AssemblyCubeScale)).ToMatrixWithScale(), 1.e-8)));
		for (const auto& Snapshot : Pair)
		{
			FVector3d LocalMin(TNumericLimits<double>::Max());
			FVector3d LocalMax(TNumericLimits<double>::Lowest());
			FVector3d WorldOffsetMin(TNumericLimits<double>::Max());
			FVector3d WorldOffsetMax(TNumericLimits<double>::Lowest());
			const FVector3d Center = FVector3d(Snapshot->Data().ComponentToWorld.GetOrigin());
			for (const auto& Position : Snapshot->Data().Positions)
			{
				LocalMin.X = FMath::Min(LocalMin.X, Position.X);
				LocalMin.Y = FMath::Min(LocalMin.Y, Position.Y);
				LocalMin.Z = FMath::Min(LocalMin.Z, Position.Z);
				LocalMax.X = FMath::Max(LocalMax.X, Position.X);
				LocalMax.Y = FMath::Max(LocalMax.Y, Position.Y);
				LocalMax.Z = FMath::Max(LocalMax.Z, Position.Z);
				const FVector3d Offset = FVector3d(
					Snapshot->Data().ComponentToWorld.TransformPosition(FVector(Position))) - Center;
				WorldOffsetMin.X = FMath::Min(WorldOffsetMin.X, Offset.X);
				WorldOffsetMin.Y = FMath::Min(WorldOffsetMin.Y, Offset.Y);
				WorldOffsetMin.Z = FMath::Min(WorldOffsetMin.Z, Offset.Z);
				WorldOffsetMax.X = FMath::Max(WorldOffsetMax.X, Offset.X);
				WorldOffsetMax.Y = FMath::Max(WorldOffsetMax.Y, Offset.Y);
				WorldOffsetMax.Z = FMath::Max(WorldOffsetMax.Z, Offset.Z);
			}
			Record(Test->TestTrue(TEXT("Engine cube local minimum X is exactly -128 cm"), LocalMin.X == -128.0));
			Record(Test->TestTrue(TEXT("Engine cube local minimum Y is exactly -128 cm"), LocalMin.Y == -128.0));
			Record(Test->TestTrue(TEXT("Engine cube local minimum Z is exactly -128 cm"), LocalMin.Z == -128.0));
			Record(Test->TestTrue(TEXT("Engine cube local maximum X is exactly 128 cm"), LocalMax.X == 128.0));
			Record(Test->TestTrue(TEXT("Engine cube local maximum Y is exactly 128 cm"), LocalMax.Y == 128.0));
			Record(Test->TestTrue(TEXT("Engine cube local maximum Z is exactly 128 cm"), LocalMax.Z == 128.0));
			Record(Test->TestTrue(TEXT("Scaled fixture offset minimum X is exactly -50 cm"), WorldOffsetMin.X == -50.0));
			Record(Test->TestTrue(TEXT("Scaled fixture offset minimum Y is exactly -50 cm"), WorldOffsetMin.Y == -50.0));
			Record(Test->TestTrue(TEXT("Scaled fixture offset minimum Z is exactly -50 cm"), WorldOffsetMin.Z == -50.0));
			Record(Test->TestTrue(TEXT("Scaled fixture offset maximum X is exactly 50 cm"), WorldOffsetMax.X == 50.0));
			Record(Test->TestTrue(TEXT("Scaled fixture offset maximum Y is exactly 50 cm"), WorldOffsetMax.Y == 50.0));
			Record(Test->TestTrue(TEXT("Scaled fixture offset maximum Z is exactly 50 cm"), WorldOffsetMax.Z == 50.0));
		}

		const FString FixedBundle = SampleId + TEXT("-fixed");
		const FString MovingBundle = SampleId + TEXT("-moving");
		if (!AnimationCaptureMeshReplay::Write(Directory, FixedBundle, *Pair[0], Error)
			|| !AnimationCaptureMeshReplay::Write(Directory, MovingBundle, *Pair[1], Error))
		{
			Test->AddError(Error);
			bChecksPassed = false;
			return;
		}
		auto Sample = MakeShared<FJsonObject>();
		Sample->SetStringField(TEXT("sample_id"), SampleId);
		Sample->SetStringField(TEXT("fixed_bundle"), FixedBundle);
		Sample->SetStringField(TEXT("moving_bundle"), MovingBundle);
		Sample->SetNumberField(TEXT("acquired_seconds"), Pair[0]->Data().AcquiredSeconds);
		Sample->SetNumberField(TEXT("frame_id"), double(Pair[0]->Data().FrameId));
		Sample->SetNumberField(TEXT("fixed_revision"), double(Pair[0]->Data().PoseRevision));
		Sample->SetNumberField(TEXT("moving_revision"), double(Pair[1]->Data().PoseRevision));
		Samples.Add(MakeShared<FJsonValueObject>(Sample));
		AcquisitionTimes.Add(Pair[0]->Data().AcquiredSeconds);
		Retained.Append(Pair);
		++SampleIndex;
	}

	TSharedRef<FJsonObject> Role(const FAnimationMeshData& Data, const TCHAR* Region) const
	{
		auto Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("component_id"), Data.Enrollment.ComponentId);
		Value->SetNumberField(TEXT("component_generation"), double(Data.Enrollment.ComponentGeneration));
		Value->SetStringField(TEXT("configuration_id"), Data.ConfigurationId);
		Value->SetStringField(TEXT("subject_id"), Data.Enrollment.SubjectId);
		Value->SetStringField(TEXT("stream_id"), Data.Enrollment.StreamId);
		Value->SetStringField(TEXT("region_id"), Region);
		Value->SetStringField(TEXT("topology_id"), Data.TopologyId);
		Value->SetArrayField(TEXT("triangle_ids"), IntegerArray(12));
		return Value;
	}

	void Finish()
	{
		Record(Test->TestEqual(TEXT("Five rigid pairs remain charged"), Fixture.Budget->LiveSnapshots(), 10));
		Record(Test->TestTrue(TEXT("Output retention charges byte capacity"), Fixture.Budget->LiveBytes() > 0));
		Record(Test->TestEqual(TEXT("Ten retained snapshots are available"), Retained.Num(), 10));
		if (Retained.Num() != 10) { return; }
		const FAnimationMeshData FixedData = Retained[0]->Data();
		const FAnimationMeshData MovingData = Retained[1]->Data();
		for (int32 Index = 2; Index < Retained.Num(); Index += 2)
		{
			Record(Test->TestEqual(TEXT("Fixed topology is stable"), Retained[Index]->Data().TopologyId, FixedData.TopologyId));
			Record(Test->TestEqual(TEXT("Moving topology is stable"), Retained[Index + 1]->Data().TopologyId, MovingData.TopologyId));
		}
		for (int32 Index = 1; Index < AcquisitionTimes.Num(); ++Index)
		{
			const double Gap = AcquisitionTimes[Index] - AcquisitionTimes[Index - 1];
			Record(Test->TestTrue(TEXT("Latent samples are at least 0.1 seconds apart"), Gap >= .1));
			Record(Test->TestTrue(TEXT("Acquisition gap does not exceed fixed 1.0 second criterion"), Gap <= 1.0));
		}

		const int64 PeakReservedBytes = Fixture.Budget->PeakBytes();
		Fixture.Stop();
		Record(Test->TestEqual(TEXT("Replay remains after fixture retirement"),
			IFileManager::Get().FileSize(*(Directory / TEXT("step-00-fixed") / TEXT("complete.json"))) > 0, true));
		Retained.Reset();
		Record(Test->TestEqual(TEXT("Releasing outputs releases admission"), Fixture.Budget->LiveBytes(), int64(0)));

		auto Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(TEXT("format"), TEXT("neutral_assembly_capture"));
		Manifest->SetNumberField(TEXT("schema_version"), 1);
		auto Roles = MakeShared<FJsonObject>();
		Roles->SetObjectField(TEXT("fixed"), Role(FixedData, TEXT("fixed-surface")));
		Roles->SetObjectField(TEXT("moving"), Role(MovingData, TEXT("moving-surface")));
		Manifest->SetObjectField(TEXT("roles"), Roles);
		Manifest->SetArrayField(TEXT("samples"), Samples);
		auto Controls = MakeShared<FJsonObject>();
		Controls->SetBoolField(TEXT("checks_passed"), bChecksPassed);
		Controls->SetNumberField(TEXT("peak_reserved_bytes"), double(PeakReservedBytes));
		Manifest->SetObjectField(TEXT("controls"), Controls);

		FString Json;
		FJsonSerializer::Serialize(Manifest, TJsonWriterFactory<>::Create(&Json));
		if (Json.Len() > 64 * 1024)
		{
			Test->AddError(TEXT("assembly.json exceeds 64 KiB"));
			bChecksPassed = false;
			return;
		}
		if (!FFileHelper::SaveStringToFile(Json, *(Directory / TEXT("assembly.json"))))
		{
			Test->AddError(TEXT("Could not publish assembly.json"));
			bChecksPassed = false;
			return;
		}
	}

	FAutomationTestBase* Test;
	double Started;
	double LastSampleSeconds = 0;
	bool bInitialized = false;
	bool bChecksPassed = true;
	int32 SampleIndex = 0;
	FString Directory;
	FRigidAssemblyFixture Fixture;
	TArray<TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe>> Retained;
	TArray<double> AcquisitionTimes;
	TArray<TSharedPtr<FJsonValue>> Samples;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRigidAssemblyTest, "AnimationAnalysis.Capture.Mesh.RigidAssembly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRigidAssemblyTest::RunTest(const FString&)
{
	if (!FApp::CanEverRender())
	{
		AddError(TEXT("Rigid assembly fixture requires rendered host initialization"));
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Engine/Maps/Entry")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FRigidAssemblyControls(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	return true;
}
