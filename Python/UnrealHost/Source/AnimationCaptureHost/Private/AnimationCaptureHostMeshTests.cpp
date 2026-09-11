#include "AnimationCapture/AnimationCaptureMeshReference.h"
#include "AnimationCaptureHostFixture.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCompress_BitwiseCompressOnly.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/Skeleton.h"
#include "AssetCompilingManager.h"
#include "BoneWeights.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MeshDescription.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/OutputDeviceNull.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "Serialization/JsonSerializer.h"
#include "SkeletalMeshAttributes.h"
#include "StaticToSkeletalMeshConverter.h"
#include "StaticMeshResources.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
void Grid(FMeshDescription& D, int32 Cells)
{
	FSkeletalMeshAttributes A(D); A.Register();
	A.GetVertexInstanceUVs().SetNumChannels(1);
	auto Left = D.CreatePolygonGroup(), Right = D.CreatePolygonGroup();
	A.GetPolygonGroupMaterialSlotNames()[Left] = TEXT("Left"); A.GetPolygonGroupMaterialSlotNames()[Right] = TEXT("Right");
	TArray<FVertexID> Vertices;
	using namespace UE::AnimationCore;
	for (int32 Z = 0; Z <= Cells; ++Z) for (int32 Y = 0; Y <= Cells; ++Y)
	{
		auto V = D.CreateVertex(); Vertices.Add(V);
		const float Weight = float(Y) / Cells;
		A.GetVertexPositions()[V] = FVector3f(0, -60 + 120 * Weight, -30 + 60 * float(Z) / Cells);
		A.GetVertexSkinWeights().Set(V, FBoneWeights::Create({FBoneWeight(0, 1 - Weight), FBoneWeight(1, Weight)}));
	}
	for (int32 Z = 0; Z < Cells; ++Z) for (int32 Y = 0; Y < Cells; ++Y)
	{
		const int32 I = Z * (Cells + 1) + Y;
		const int32 Corners[] = {I, I + Cells + 1, I + 1, I + 1, I + Cells + 1, I + Cells + 2};
		for (int32 T = 0; T < 2; ++T)
		{
			TArray<FVertexInstanceID> Instances;
			for (int32 C = 0; C < 3; ++C)
			{
				auto VI = D.CreateVertexInstance(Vertices[Corners[T * 3 + C]]); Instances.Add(VI);
				A.GetVertexInstanceNormals()[VI] = FVector3f(-1, 0, 0);
				A.GetVertexInstanceTangents()[VI] = FVector3f(0, 1, 0);
				A.GetVertexInstanceBinormalSigns()[VI] = 1;
				A.GetVertexInstanceColors()[VI] = FVector4f(1, 1, 1, 1);
				const auto P = A.GetVertexPositions()[Vertices[Corners[T * 3 + C]]];
				A.GetVertexInstanceUVs().Set(VI, 0, FVector2f((P.Y + 60) / 120, (P.Z + 30) / 60));
			}
			Instances.Swap(1, 2); // Front-facing winding for the UE diagnostic view.
			D.CreatePolygon(Y < Cells / 2 ? Left : Right, Instances);
		}
	}
}
USkeletalMesh* MakeMesh()
{
	FReferenceSkeleton Ref;
	{
	FReferenceSkeletonModifier Mod(Ref, nullptr);
	Mod.Add(FMeshBoneInfo(TEXT("Base"), TEXT("Base"), INDEX_NONE), FTransform::Identity);
	Mod.Add(FMeshBoneInfo(TEXT("Bend"), TEXT("Bend"), 0), FTransform(FVector(0, 0, 20)));
	} // Modifier rebuilds the final reference skeleton when its scope ends.
	FMeshDescription Fine, Coarse; Grid(Fine, 100); Grid(Coarse, 50);
	TArray<const FMeshDescription*> Descriptions{&Fine, &Coarse};
	TArray<FSkeletalMaterial> Materials{FSkeletalMaterial(UMaterial::GetDefaultMaterial(MD_Surface)), FSkeletalMaterial(UMaterial::GetDefaultMaterial(MD_Surface))};
	Materials[0].MaterialSlotName = Materials[0].ImportedMaterialSlotName = TEXT("Left");
	Materials[1].MaterialSlotName = Materials[1].ImportedMaterialSlotName = TEXT("Right");
	auto* Mesh = NewObject<USkeletalMesh>(GetTransientPackage());
	if (!FStaticToSkeletalMeshConverter::InitializeSkeletalMeshFromMeshDescriptions(Mesh, Descriptions, Materials, Ref)) { return nullptr; }
	auto* Skeleton = NewObject<USkeleton>(Mesh);
	Mesh->SetSkeleton(Skeleton);
	if (!Skeleton->MergeAllBonesToBoneTree(Mesh)) { return nullptr; }
	return Mesh;
}

UAnimSequence* MakeAnimation(USkeletalMesh* Mesh)
{
    auto* Animation = NewObject<UAnimSequence>(Mesh);
    Animation->SetSkeleton(Mesh->GetSkeleton());
    auto* Compression = NewObject<UAnimBoneCompressionSettings>(Animation);
    auto* Codec = NewObject<UAnimCompress_BitwiseCompressOnly>(Compression);
    Codec->TranslationCompressionFormat = ACF_None;
    Codec->RotationCompressionFormat = ACF_None;
    Codec->ScaleCompressionFormat = ACF_None;
    Compression->Codecs = {Codec};
    Animation->BoneCompressionSettings = Compression;
    Animation->bAllowFrameStripping = false;
    auto& C = Animation->GetController();
    C.OpenBracket(FText::FromString(TEXT("Neutral animation probe")), false);
    C.InitializeModel(); C.SetFrameRate(FFrameRate(30, 1), false); C.SetNumberOfFrames(FFrameNumber(30), false);
    for (int32 Bone = 0; Bone < 2; ++Bone)
    {
        const FName Name = Bone == 0 ? TEXT("Base") : TEXT("Bend");
        C.AddBoneCurve(Name, false);
        TArray<FVector3f> P, S; TArray<FQuat4f> R;
        for (int32 I = 0; I <= 30; ++I)
        {
            P.Add(Bone ? FVector3f(0, 0, 20) : FVector3f::ZeroVector); S.Add(FVector3f::OneVector);
            R.Add(FQuat4f(FVector3f(1, 0, 0), Bone ? FMath::DegreesToRadians(float(I) * 3) : 0));
        }
        C.SetBoneTrackKeys(Name, P, R, S, false);
    }
    C.NotifyPopulated(); C.CloseBracket(false);
    FAssetCompilingManager::Get().FinishAllCompilation();
    return Animation;
}

FAnimationMeshLimits ReferenceLimits()
{
	return {4, 20000, 100000, 16, 256, 16ll * 1024 * 1024};
}
FAnimationMeshEnrollment Enrollment(UMeshComponent* Mesh, const TCHAR* Id, int32 LOD = 0)
{
	FAnimationMeshEnrollment E;
	E.ComponentId = Id; E.ComponentGeneration = 1; E.AssetId = FString(Id) + TEXT("-asset");
	E.ConfigurationGeneration = 1; E.ConfigurationId = TEXT("neutral-reference");
	E.SubjectId = TEXT("assembly-\u00e9"); E.StreamId = TEXT("inspection"); E.AnalysisLOD = LOD;
	for (int32 I = 0; I < Mesh->GetNumMaterials(); ++I) { E.MaterialIds.Add(FString::Printf(TEXT("material-%d"), I)); }
	return E;
}
struct FMeshFixture
{
	FAnimationCaptureHostFixture View;
	TWeakObjectPtr<AActor> Actor;
	USkeletalMeshComponent* Mesh = nullptr;
	UStaticMeshComponent* Prop = nullptr;
	TSharedRef<FAnimationMeshBudget, ESPMode::ThreadSafe> Budget = MakeShared<FAnimationMeshBudget, ESPMode::ThreadSafe>(ReferenceLimits());
	TUniquePtr<FAnimationCaptureMeshReference> Bone, Coarse, Rigid;
	~FMeshFixture() { Stop(); }
	bool Start(UWorld* World, FString& Error)
	{
		if (!View.Start(World, Error)) { return false; }
		for (auto& M : View.Meshes) { M->SetWorldLocation(FVector(5000, 5000, 5000)); }
		auto* Asset = MakeMesh(); if (!Asset) { Error = TEXT("Procedural mesh build failed"); return false; }
		auto* Animation = MakeAnimation(Asset);
		Actor = World->SpawnActor<AActor>(); Mesh = NewObject<USkeletalMeshComponent>(Actor.Get()); Actor->SetRootComponent(Mesh);
		Mesh->SetSkeletalMesh(Asset); Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		Mesh->RegisterComponent(); Mesh->PlayAnimation(Animation, true); Mesh->SetForcedLOD(1); // Fixture-owned fixed benchmark workload.
		Mesh->SetWorldTransform(FTransform(FRotator(0, 12, 0), FVector(400, 0, 10000), FVector(1.2, .8, 1.1)));
		Prop = NewObject<UStaticMeshComponent>(Actor.Get());
		Prop->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		Prop->SetupAttachment(Mesh, TEXT("Bend")); Prop->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Prop->SetRelativeLocation(FVector(0, 75, 0)); Prop->SetRelativeScale3D(FVector(.1, .1, .4)); Prop->RegisterComponent();
		Bone = FAnimationCaptureMeshReference::Create(Mesh, Enrollment(Mesh, TEXT("panel")), Budget, Error);
		if (!Bone) { return false; }
		Coarse = FAnimationCaptureMeshReference::Create(Mesh, Enrollment(Mesh, TEXT("panel"), 1), Budget, Error);
		if (!Coarse) { return false; }
		Rigid = FAnimationCaptureMeshReference::Create(Prop, Enrollment(Prop, TEXT("tool")), Budget, Error);
		return Rigid.IsValid();
	}
	void Stop()
	{
		Bone.Reset(); Coarse.Reset(); Rigid.Reset();
		if (Actor.IsValid()) { Actor->Destroy(); } Actor.Reset(); Mesh = nullptr; Prop = nullptr; View.Stop();
	}
};

TUniquePtr<FMeshFixture> Inspection;
IConsoleObject* InspectCommand = nullptr;
IConsoleObject* SampleCommand = nullptr;
IConsoleObject* StopCommand = nullptr;
FDelegateHandle CleanupHandle;

double AnalyticError(const FAnimationMeshData& D, USkeletalMeshComponent* Mesh)
{
	const auto& LOD = Mesh->GetSkeletalMeshAsset()->GetResourceForRendering()->LODRenderData[D.Enrollment.AnalysisLOD];
	const auto* Weights = Mesh->GetSkinWeightBuffer(D.Enrollment.AnalysisLOD);
	const double Key = Mesh->GetSingleNodeInstance()->GetCurrentTime() * 30;
	const double Alpha = Key - FMath::FloorToDouble(Key), H0 = FMath::DegreesToRadians(FMath::FloorToDouble(Key) * 3) * .5;
	const double H1 = H0 + FMath::DegreesToRadians(3.0) * .5;
	const double QX = FMath::Lerp(FMath::Sin(H0), FMath::Sin(H1), Alpha), QW = FMath::Lerp(FMath::Cos(H0), FMath::Cos(H1), Alpha);
	const double Norm = QX * QX + QW * QW, C = 1 - 2 * QX * QX / Norm, S = 2 * QX * QW / Norm;
	double Error = 0;
	for (const auto& Section : LOD.RenderSections) for (uint32 I = Section.BaseVertexIndex; I < Section.BaseVertexIndex + Section.NumVertices; ++I)
	{
		const FVector3d P(LOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(I)); double W0 = 0, W1 = 0;
		for (uint32 J = 0; J < Weights->GetMaxBoneInfluences(); ++J)
		{
			const double W = Weights->GetBoneWeight(I, J) / 65535.0;
			if (Section.BoneMap[Weights->GetBoneIndex(I, J)] == 0) { W0 += W; } else { W1 += W; }
		}
		const FVector3d Bent(P.X, C * P.Y - S * (P.Z - 20), S * P.Y + C * (P.Z - 20) + 20);
		Error = FMath::Max(Error, FVector3d::Distance(P * W0 + Bent * W1, D.Positions[I]));
	}
	return Error;
}

class FMeshReferenceControls : public IAutomationLatentCommand
{
public:
	explicit FMeshReferenceControls(FAutomationTestBase* T) : Test(T), Started(FPlatformTime::Seconds()) {}
	bool Update() override
	{
		if (FPlatformTime::Seconds() - Started > 50) { Test->AddError(TEXT("Mesh fixture deadline")); return true; }
		FString Error;
		if (CommandWarmup > 0)
		{
			if (++CommandWarmup < 8) { return false; }
			Test->TestTrue(TEXT("Interactive route creates the shared mesh fixture"), Inspection && Inspection->Mesh && Inspection->Prop);
			const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Observations"));
			TArray<FString> Before, After;
			IFileManager::Get().FindFiles(Before, *(Root / TEXT("sample-*")), false, true);
			FOutputDeviceNull Output;
			Test->TestTrue(TEXT("Interactive sample route executes"), IConsoleManager::Get().ProcessUserConsoleInput(TEXT("AnimationAnalysis.Host.SampleMesh"), Output, Inspection->View.World.Get()));
			IFileManager::Get().FindFiles(After, *(Root / TEXT("sample-*")), false, true);
			Test->TestEqual(TEXT("Interactive route publishes one replay bundle"), After.Num(), Before.Num() + 1);
			IConsoleManager::Get().ProcessUserConsoleInput(TEXT("AnimationAnalysis.Host.StopMesh"), Output, nullptr);
			IConsoleManager::Get().ProcessUserConsoleInput(TEXT("AnimationAnalysis.Host.StopMesh"), Output, nullptr);
			Test->TestFalse(TEXT("Stop and repeated stop release command fixture"), Inspection.IsValid()); return true;
		}
		if (Retirement)
		{
			Test->TestFalse(TEXT("Skipped animation update produces insufficient pose evidence"), Fixture.Bone->Capture(TEXT("stale"), Error).IsValid());
			Fixture.Actor->Destroy();
			Test->TestFalse(TEXT("Destroyed component is never rebound"), Fixture.Bone->Capture(TEXT("destroyed"), Error).IsValid());
			Test->TestTrue(TEXT("Retained geometry survives source retirement"), Retained->Data().Positions.Num() > 10000);
			Fixture.Stop();
			Test->TestEqual(TEXT("Sampler retirement preserves retained snapshot admission"), Fixture.Budget->LiveSnapshots(), 1);
			Retained.Reset();
			Test->TestEqual(TEXT("Final result release restores all bytes"), Fixture.Budget->LiveBytes(), int64(0));
			FOutputDeviceNull Output;
			Test->TestTrue(TEXT("Interactive inspect route executes"), IConsoleManager::Get().ProcessUserConsoleInput(TEXT("AnimationAnalysis.Host.InspectMesh"), Output, FAnimationCaptureHostFixture::FindWorld()));
			CommandWarmup = 1; return false;
		}
		if (!Initialized)
		{
			auto* World = FAnimationCaptureHostFixture::FindWorld(); if (!World) { return false; }
			if (!Fixture.Start(World, Error)) { Test->AddError(Error); return true; }
			Test->TestFalse(TEXT("Enrollment before finalization has no geometry"), Fixture.Bone->Capture(TEXT("unwitnessed"), Error).IsValid());
			Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Observations") / (TEXT("MeshReference-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			IFileManager::Get().MakeDirectory(*Directory, true); Initialized = true; return false;
		}
		if (++Warmup < 8) { return false; }
		const uint32 Revision = Fixture.Mesh->GetBoneTransformRevisionNumber();
		const int32 ForcedLOD = Fixture.Mesh->GetForcedLOD(); const bool CPUSkinning = Fixture.Mesh->GetCPUSkinningEnabled();
		auto Fine = Fixture.Bone->Capture(TEXT("fine"), Error);
		if (!Fine) { Test->AddError(Error); return true; }
		auto Coarse = Fixture.Coarse->Capture(TEXT("coarse"), Error);
		if (!Coarse) { Test->AddError(Error); return true; }
		auto Rigid = Fixture.Rigid->Capture(TEXT("rigid"), Error);
		if (!Rigid) { Test->AddError(Error); return true; }
		Test->TestEqual(TEXT("Sampler does not finalize bones"), Fixture.Mesh->GetBoneTransformRevisionNumber(), Revision);
		Test->TestEqual(TEXT("Sampler does not force LOD"), Fixture.Mesh->GetForcedLOD(), ForcedLOD);
		Test->TestEqual(TEXT("Sampler does not change CPU rendering"), Fixture.Mesh->GetCPUSkinningEnabled(), CPUSkinning);
		Test->TestEqual(TEXT("Two skeletal sections included"), Fine->Data().Sections.Num(), 2);
		Test->TestNotEqual(TEXT("LOD topology differs"), Fine->Data().TopologyId, Coarse->Data().TopologyId);
		const double FineError = AnalyticError(Fine->Data(), Fixture.Mesh), CoarseError = AnalyticError(Coarse->Data(), Fixture.Mesh);
		Test->TestTrue(TEXT("Both LODs agree with independent scalar animation within 0.001 cm"), FMath::Max(FineError, CoarseError) < .001);
		Test->TestTrue(TEXT("Rigid snapshot preserves acquisition transform"), Rigid->Data().ComponentToWorld.Equals(Fixture.Prop->GetComponentTransform().ToMatrixWithScale(), 1.e-8));
		for (const auto& P : Rigid->Data().Positions)
			Test->TestTrue(TEXT("Engine cube reference remains within declared 50 cm extent"), P.GetAbsMax() <= 50.001);
		for (auto Pair : {TPair<const TCHAR*, const FAnimationMeshSnapshot*>(TEXT("fine"), Fine.Get()), {TEXT("coarse"), Coarse.Get()}, {TEXT("rigid"), Rigid.Get()}})
			if (!Test->TestTrue(TEXT("Native schema bundle written"), AnimationCaptureMeshReplay::Write(Directory, Pair.Key, *Pair.Value, Error))) { Test->AddError(Error); }
		Test->TestFalse(TEXT("Completed bundle cannot be overwritten"), AnimationCaptureMeshReplay::Write(Directory, TEXT("fine"), *Fine, Error));
		Test->TestFalse(TEXT("Traversal cannot be published"), AnimationCaptureMeshReplay::Write(Directory, TEXT("../escaped"), *Fine, Error));
		auto Fourth = Fixture.Bone->Capture(TEXT("fourth"), Error);
		Test->TestTrue(TEXT("Shared admission permits last slot"), Fourth.IsValid());
		Test->TestFalse(TEXT("Different sampler cannot exceed shared snapshot cap"), Fixture.Rigid->Capture(TEXT("full"), Error).IsValid());
		auto Copy = Fourth; Fourth.Reset();
		Test->TestFalse(TEXT("Remaining shared owner retains admission"), Fixture.Bone->Capture(TEXT("still-full"), Error).IsValid());
		Copy.Reset();
		Test->TestEqual(TEXT("Last owner releases one admission"), Fixture.Budget->LiveSnapshots(), 3);
		auto TinyLimits = ReferenceLimits(); TinyLimits.MaxBytes = 100;
		auto TinyBudget = MakeShared<FAnimationMeshBudget, ESPMode::ThreadSafe>(TinyLimits);
		auto Tiny = FAnimationCaptureMeshReference::Create(Fixture.Prop, Enrollment(Fixture.Prop, TEXT("tiny")), TinyBudget, Error);
		Fixture.Mesh->RefreshBoneTransforms(); // Fixture action establishes a new same-frame witness for all observers.
		Test->TestFalse(TEXT("Byte admission rejects before payload allocation"), Tiny->Capture(TEXT("tiny"), Error).IsValid());
		Test->TestEqual(TEXT("Rejected byte admission retains no bytes"), TinyBudget->LiveBytes(), int64(0));
		const auto PreviousPositions = Fine->Data().Positions;
		Fixture.Mesh->SetPosition(Fixture.Mesh->GetSingleNodeInstance()->GetCurrentTime() < .5 ? .8 : .1, false);
		Fixture.Mesh->TickAnimation(0, false); Fixture.Mesh->RefreshBoneTransforms();
		auto Changed = Fixture.Bone->Capture(TEXT("changed"), Error);
		if (!Changed) { Test->AddError(Error); return true; }
		Test->TestTrue(TEXT("Same engine frame can have a distinct finalized pose revision"), Changed->Data().FrameId == Fine->Data().FrameId && Changed->Data().PoseRevision > Fine->Data().PoseRevision);
		double Delta = 0; for (int32 I = 0; I < PreviousPositions.Num(); ++I) { Delta = FMath::Max(Delta, FVector3d::Distance(Changed->Data().Positions[I], PreviousPositions[I])); }
		Test->TestTrue(TEXT("Live pose differs from retained geometry"), Delta > 1);
		Test->TestTrue(TEXT("Retained geometry remains immutable"), PreviousPositions == Fine->Data().Positions); Changed.Reset();
		const auto Required = Fixture.Mesh->RequiredBones; Fixture.Mesh->RequiredBones.Remove(1);
		Test->TestFalse(TEXT("Unevaluated weighted bone cannot produce a complete reference"), Fixture.Bone->Capture(TEXT("missing-bone"), Error).IsValid());
		Test->TestFalse(TEXT("Unevaluated attachment bone cannot qualify rigid pose"), Fixture.Rigid->Capture(TEXT("missing-socket-bone"), Error).IsValid());
		Fixture.Mesh->RequiredBones = Required;
		UMaterialInterface* OriginalMaterial = Fixture.Mesh->GetMaterial(0);
		Fixture.Mesh->SetMaterial(0, UMaterialInstanceDynamic::Create(OriginalMaterial, Fixture.Mesh));
		Test->TestFalse(TEXT("Effective material replacement requires reenrollment"), Fixture.Bone->Capture(TEXT("material-change"), Error).IsValid());
		Fixture.Mesh->SetMaterial(0, OriginalMaterial);
		Fixture.Mesh->SetRefPoseOverride(Fixture.Mesh->GetSkeletalMeshAsset()->GetRefSkeleton().GetRefBonePose());
		Test->TestFalse(TEXT("Reference-pose override is explicitly unqualified"), Fixture.Bone->Capture(TEXT("override-bind"), Error).IsValid());
		Fixture.Mesh->ClearRefPoseOverride();
		TArray<FSkelMeshSkinWeightInfo> OverrideWeights; OverrideWeights.SetNum(Fine->Data().Positions.Num());
		for (auto& W : OverrideWeights) { W.Bones[0] = 0; W.Weights[0] = 255; }
		Fixture.Mesh->SetSkinWeightOverride(0, OverrideWeights);
		Fixture.Mesh->RefreshBoneTransforms(); // Fixture finalizes the changed bind/weight configuration.
		auto Reweighted = Fixture.Bone->Capture(TEXT("reweighted"), Error);
		if (!Reweighted) { Test->AddError(Error); return true; }
		Test->TestNotEqual(TEXT("Effective weight change invalidates configuration identity"), Reweighted->Data().ConfigurationId, Fine->Data().ConfigurationId);
		Test->TestEqual(TEXT("Weight change preserves matching topology"), Reweighted->Data().TopologyId, Fine->Data().TopologyId);
		Reweighted.Reset(); Fixture.Mesh->ClearSkinWeightOverride(0);
		Fixture.Mesh->RefreshBoneTransforms();
		// Freeze the pose and workload for three comparable warmed acquisition batches.
		TArray<TSharedPtr<FJsonValue>> Batches;
		for (int32 Batch = 0; Batch < 3; ++Batch)
		{
			TArray<TSharedPtr<FJsonValue>> Samples;
			for (int32 I = 0; I < 35; ++I)
			{
				const double Before = FPlatformTime::Seconds(); auto Sample = Fixture.Bone->Capture(TEXT("benchmark"), Error);
				const double Ms = (FPlatformTime::Seconds() - Before) * 1000;
				if (!Sample) { Test->AddError(Error); return true; }
				if (I >= 5) { Samples.Add(MakeShared<FJsonValueNumber>(Ms)); }
			}
			Batches.Add(MakeShared<FJsonValueArray>(Samples));
		}
		Fixture.Mesh->LeaderPoseComponent = Fixture.Mesh; // Negative control avoids asking UE to construct a cyclic leader graph.
		Test->TestFalse(TEXT("Leader pose cannot masquerade as qualified bone reference"), Fixture.Bone->Capture(TEXT("leader"), Error).IsValid());
		Fixture.Mesh->LeaderPoseComponent.Reset();
		UStaticMesh* Original = Fixture.Prop->GetStaticMesh();
		Fixture.Prop->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere")));
		Test->TestFalse(TEXT("Asset replacement requires reenrollment"), Fixture.Rigid->Capture(TEXT("replaced"), Error).IsValid());
		Fixture.Prop->SetStaticMesh(Original);
		Fixture.Mesh->SetComponentTickEnabled(false); // Next world tick must not invent a fresh pose.
		Retained = Fine; Fine.Reset(); Coarse.Reset(); Rigid.Reset();
		auto Report = MakeShared<FJsonObject>();
		Report->SetNumberField(TEXT("fine_max_error_cm"), FineError); Report->SetNumberField(TEXT("coarse_max_error_cm"), CoarseError);
		Report->SetNumberField(TEXT("live_change_cm"), Delta); Report->SetNumberField(TEXT("peak_reserved_bytes"), double(Fixture.Budget->PeakBytes()));
		Report->SetNumberField(TEXT("vertices"), Retained->Data().Positions.Num()); Report->SetNumberField(TEXT("indices"), Retained->Data().Indices.Num());
		Report->SetArrayField(TEXT("acquisition_ms_batches"), Batches);
		FString Json; FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json));
		Test->TestTrue(TEXT("Raw CPU timings and oracle evidence saved"), FFileHelper::SaveStringToFile(Json, *(Directory / TEXT("reference-controls.json"))));
		Retirement = true; return false;
	}
private:
	FAutomationTestBase* Test; double Started; bool Initialized = false, Retirement = false; int32 Warmup = 0, CommandWarmup = 0;
	FMeshFixture Fixture; FString Directory;
	TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe> Retained;
};

}

void RegisterAnimationCaptureMeshHostCommands()
{
	InspectCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("AnimationAnalysis.Host.InspectMesh"),
		TEXT("In PIE: create the neutral animated two-section panel and rigid attachment"), FConsoleCommandDelegate::CreateLambda([]
		{
			Inspection.Reset(); auto Fixture = MakeUnique<FMeshFixture>(); FString Error;
			if (!Fixture->Start(FAnimationCaptureHostFixture::FindWorld(), Error)) { UE_LOG(LogTemp, Warning, TEXT("%s"), *Error); return; }
			Inspection = MoveTemp(Fixture);
		}), ECVF_Default);
	SampleCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("AnimationAnalysis.Host.SampleMesh"),
		TEXT("Record one witnessed CPU bone-reference bundle from the inspection fixture"), FConsoleCommandDelegate::CreateLambda([]
		{
			if (!Inspection) { UE_LOG(LogTemp, Warning, TEXT("Start AnimationAnalysis.Host.InspectMesh first")); return; }
			FString Error; const FString Id = TEXT("sample-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			auto Sample = Inspection->Bone->Capture(Id, Error);
			const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Observations"));
			IFileManager::Get().MakeDirectory(*Root, true);
			if (!Sample || !AnimationCaptureMeshReplay::Write(Root, Id, *Sample, Error)) { UE_LOG(LogTemp, Warning, TEXT("%s"), *Error); return; }
			UE_LOG(LogTemp, Display, TEXT("Mesh reference: %s (%d vertices; selected analysis LOD; later deformation excluded)"), *(Root / Id), Sample->Data().Positions.Num());
		}), ECVF_Default);
	StopCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("AnimationAnalysis.Host.StopMesh"), TEXT("Remove mesh inspection fixture"),
		FConsoleCommandDelegate::CreateLambda([] { Inspection.Reset(); }), ECVF_Default);
	CleanupHandle = FWorldDelegates::OnWorldCleanup.AddLambda([](UWorld* World, bool, bool)
	{ if (Inspection && Inspection->View.World.Get() == World) { Inspection.Reset(); } });
}
void UnregisterAnimationCaptureMeshHostCommands()
{
	Inspection.Reset(); FWorldDelegates::OnWorldCleanup.Remove(CleanupHandle);
	for (auto* Command : {InspectCommand, SampleCommand, StopCommand}) { IConsoleManager::Get().UnregisterConsoleObject(Command); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshReferenceTest, "AnimationAnalysis.Capture.Mesh.Reference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMeshReferenceTest::RunTest(const FString&)
{
	if (!FApp::CanEverRender()) { AddError(TEXT("Mesh reference fixture requires rendered host initialization")); return false; }
	ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Engine/Maps/Entry")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FMeshReferenceControls(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand()); return true;
}
