#include "AnimationCapture/AnimationCaptureMeshGPU.h"
#include "AnimationCapture/AnimationCaptureMeshReference.h"
#include "AnimationCapture/ViewportAsyncCapture.h"
#include "AnimationCaptureHostFixture.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCompress_BitwiseCompressOnly.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNode_LinkedAnimGraph.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_Slot.h"
#include "AssetCompilingManager.h"
#include "BoneWeights.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Features/IModularFeatures.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Serialization/JsonSerializer.h"
#include "SkeletalMeshAttributes.h"
#include "StaticToSkeletalMeshConverter.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"
#include "UObject/GCObject.h"

namespace
{
const FName FinalizedSlot(TEXT("FinalizedPoseSlot"));
constexpr double CubeScale = 5.0 / 128.0;
constexpr int32 PairCount = 5;
const double AnimationPositions[PairCount] = {.1, .2, .3, .4, .5};
const double PartOffsets[PairCount] = {30, 20, 10, 4, 0};
const double ExpectedDistances[PairCount] = {25, 15, 5, 0, 0};
const bool ExpectedIntersections[PairCount] = {false, false, false, true, true};

FAnimationMeshLimits FinalizedLimits(int32 MaxSnapshots = 24)
{
	return {MaxSnapshots, 256, 2048, 16, 16, 24ll * 1024 * 1024};
}

FAnimationMeshEnrollment Enrollment(UMeshComponent* Component, const TCHAR* Id,
	EAnimationMeshPosePolicy Policy = EAnimationMeshPosePolicy::SingleNode, int32 LOD = 0)
{
	FAnimationMeshEnrollment Result;
	Result.ComponentId = Id;
	Result.ComponentGeneration = 1;
	Result.AssetId = FString(Id) + TEXT("-neutral-asset");
	Result.ConfigurationGeneration = 1;
	Result.ConfigurationId = TEXT("neutral-finalized-pose");
	Result.SubjectId = Id;
	Result.StreamId = TEXT("finalized-pose");
	Result.AnalysisLOD = LOD;
	Result.PosePolicy = Policy;
	for (int32 Index = 0; Index < Component->GetNumMaterials(); ++Index)
	{
		Result.MaterialIds.Add(FString::Printf(TEXT("neutral-material-%d"), Index));
	}
	return Result;
}

USkeletalMesh* MakeNeutralMesh(UObject* Outer)
{
	FReferenceSkeleton ReferenceSkeleton;
	{
		FReferenceSkeletonModifier Modifier(ReferenceSkeleton, nullptr);
		Modifier.Add(FMeshBoneInfo(TEXT("Base"), TEXT("Base"), INDEX_NONE), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(TEXT("Motion"), TEXT("Motion"), 0), FTransform::Identity);
	}
	FMeshDescription Description;
	FSkeletalMeshAttributes Attributes(Description);
	Attributes.Register();
	Attributes.GetVertexInstanceUVs().SetNumChannels(1);
	const FPolygonGroupID Group = Description.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Surface");
	using namespace UE::AnimationCore;
	TArray<FVertexID> Vertices;
	for (const FVector3f Position : {FVector3f(0, -10, -10), FVector3f(0, -10, 10),
		FVector3f(0, 10, -10), FVector3f(0, 10, 10)})
	{
		const FVertexID Vertex = Description.CreateVertex();
		Vertices.Add(Vertex);
		Attributes.GetVertexPositions()[Vertex] = Position;
		Attributes.GetVertexSkinWeights().Set(Vertex, FBoneWeights::Create({FBoneWeight(1, 1.0f)}));
	}
	const int32 Triangles[6] = {0, 1, 2, 2, 1, 3};
	for (int32 Triangle = 0; Triangle < 2; ++Triangle)
	{
		TArray<FVertexInstanceID> Instances;
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertices[Triangles[Triangle * 3 + Corner]]);
			Instances.Add(Instance);
			Attributes.GetVertexInstanceNormals()[Instance] = FVector3f(-1, 0, 0);
			Attributes.GetVertexInstanceTangents()[Instance] = FVector3f(0, 1, 0);
			Attributes.GetVertexInstanceBinormalSigns()[Instance] = 1;
			Attributes.GetVertexInstanceColors()[Instance] = FVector4f(1, 1, 1, 1);
			Attributes.GetVertexInstanceUVs().Set(Instance, 0, FVector2f::ZeroVector);
		}
		Description.CreatePolygon(Group, Instances);
	}
	TArray<const FMeshDescription*> Descriptions{&Description};
	TArray<FSkeletalMaterial> Materials{FSkeletalMaterial(UMaterial::GetDefaultMaterial(MD_Surface))};
	Materials[0].MaterialSlotName = Materials[0].ImportedMaterialSlotName = TEXT("Surface");
	auto* Mesh = NewObject<USkeletalMesh>(Outer, TEXT("NeutralFinalizedPoseMesh"), RF_Public | RF_Standalone | RF_Transient);
	if (!FStaticToSkeletalMeshConverter::InitializeSkeletalMeshFromMeshDescriptions(
		Mesh, Descriptions, Materials, ReferenceSkeleton))
	{
		return nullptr;
	}
	auto* Skeleton = NewObject<USkeleton>(Outer, TEXT("NeutralFinalizedPoseSkeleton"), RF_Public | RF_Standalone | RF_Transient);
	Mesh->SetSkeleton(Skeleton);
	return Skeleton->MergeAllBonesToBoneTree(Mesh) ? Mesh : nullptr;
}

UAnimSequence* MakeTranslationSequence(USkeletalMesh* Mesh, UObject* Outer)
{
	auto* Sequence = NewObject<UAnimSequence>(Outer, TEXT("NeutralFinalizedPoseSequence"), RF_Public | RF_Standalone | RF_Transient);
	Sequence->SetSkeleton(Mesh->GetSkeleton());
	auto* Compression = NewObject<UAnimBoneCompressionSettings>(Sequence);
	auto* Codec = NewObject<UAnimCompress_BitwiseCompressOnly>(Compression);
	Codec->TranslationCompressionFormat = ACF_None;
	Codec->RotationCompressionFormat = ACF_None;
	Codec->ScaleCompressionFormat = ACF_None;
	Compression->Codecs = {Codec};
	Sequence->BoneCompressionSettings = Compression;
	Sequence->bAllowFrameStripping = false;
	auto& Controller = Sequence->GetController();
	Controller.OpenBracket(FText::FromString(TEXT("Neutral finalized pose motion")), false);
	Controller.InitializeModel();
	Controller.SetFrameRate(FFrameRate(30, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(30), false);
	for (int32 BoneIndex = 0; BoneIndex < 2; ++BoneIndex)
	{
		const FName BoneName = BoneIndex == 0 ? TEXT("Base") : TEXT("Motion");
		Controller.AddBoneCurve(BoneName, false);
		TArray<FVector3f> Positions;
		TArray<FQuat4f> Rotations;
		TArray<FVector3f> Scales;
		for (int32 Frame = 0; Frame <= 30; ++Frame)
		{
			Positions.Add(BoneIndex == 0 ? FVector3f::ZeroVector : FVector3f(float(Frame) / 3.0f, 0, 0));
			Rotations.Add(FQuat4f::Identity);
			Scales.Add(FVector3f::OneVector);
		}
		Controller.SetBoneTrackKeys(BoneName, Positions, Rotations, Scales, false);
	}
	Controller.NotifyPopulated();
	Controller.CloseBracket(false);
	FAssetCompilingManager::Get().FinishAllCompilation();
	return Sequence;
}

UAnimBlueprint* MakeSlotBlueprint(USkeleton* Skeleton, UObject* Outer, FString& Error)
{
	// UE's AnimBlueprint property-access extension dereferences this editor feature
	// during the factory's initial compile, even for an empty animation graph.
	if (!IModularFeatures::Get().IsModularFeatureAvailable(TEXT("PropertyAccessEditor")))
	{
		Error = TEXT("Neutral AnimBlueprint fixture requires the PropertyAccessEditor engine plugin");
		return nullptr;
	}
	// The minimal host does not otherwise load the editor that registers AnimBlueprint
	// creation callbacks before UAnimBlueprintFactory performs its initial compile.
	FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("AnimationBlueprintEditor"));
	auto* Factory = NewObject<UAnimBlueprintFactory>();
	Factory->TargetSkeleton = Skeleton;
	Factory->ParentClass = UAnimInstance::StaticClass();
	Factory->BlueprintType = BPTYPE_Normal;
	const FName Name(TEXT("NeutralFinalizedPoseABP"));
	auto* Blueprint = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(
		UAnimBlueprint::StaticClass(), Outer, Name, RF_Public | RF_Standalone | RF_Transient, nullptr, GWarn));
	if (!Blueprint)
	{
		Error = TEXT("Transient AnimBlueprint creation failed");
		return nullptr;
	}
	UEdGraph* Graph = FindObject<UEdGraph>(Blueprint, TEXT("AnimGraph"));
	UAnimGraphNode_Root* Root = Graph ? FBlueprintEditorUtils::GetAnimGraphRoot(Graph) : nullptr;
	if (!Graph || !Root)
	{
		Error = TEXT("Transient AnimBlueprint did not create an AnimGraph root");
		return nullptr;
	}
	FGraphNodeCreator<UAnimGraphNode_Slot> Creator(*Graph);
	UAnimGraphNode_Slot* Slot = Creator.CreateNode();
	Slot->Node.SlotName = FinalizedSlot;
	Slot->NodeGuid = FGuid::NewGuid();
	Creator.Finalize();
	UEdGraphPin* Output = nullptr;
	UEdGraphPin* Input = nullptr;
	for (UEdGraphPin* Pin : Slot->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && UAnimationGraphSchema::IsLocalSpacePosePin(Pin->PinType))
		{
			Output = Pin;
			break;
		}
	}
	for (UEdGraphPin* Pin : Root->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && UAnimationGraphSchema::IsLocalSpacePosePin(Pin->PinType))
		{
			Input = Pin;
			break;
		}
	}
	if (!Output || !Input || !Graph->GetSchema()->TryCreateConnection(Output, Input))
	{
		Error = TEXT("Transient AnimBlueprint slot could not connect to the pose root");
		return nullptr;
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (Blueprint->Status == BS_Error || !Blueprint->GeneratedClass
		|| !Blueprint->GeneratedClass->IsChildOf(UAnimInstance::StaticClass()))
	{
		Error = TEXT("Transient AnimBlueprint compilation failed");
		return nullptr;
	}
	return Blueprint;
}

TArray<TSharedPtr<FJsonValue>> Vector(const FVector& Value)
{
	return {MakeShared<FJsonValueNumber>(Value.X), MakeShared<FJsonValueNumber>(Value.Y), MakeShared<FJsonValueNumber>(Value.Z)};
}

struct FFinalizedPoseFixture : FGCObject
{
	FAnimationCaptureHostFixture View;
	TUniquePtr<FViewportAsyncCapture> DiagnosticView;
	IConsoleVariable* CacheMode = nullptr;
	int32 PreviousCacheMode = 0;
	TWeakObjectPtr<AActor> BodyActor;
	TWeakObjectPtr<AActor> AttachmentActor;
	USkeletalMeshComponent* Body = nullptr;
	USkeletalMeshComponent* AttachmentParent = nullptr;
	UStaticMeshComponent* AttachedPart = nullptr;
	TObjectPtr<USkeletalMesh> MeshAsset = nullptr;
	TObjectPtr<UAnimSequence> Sequence = nullptr;
	TObjectPtr<UAnimBlueprint> Blueprint = nullptr;
	TObjectPtr<UAnimMontage> Montage = nullptr;
	TObjectPtr<UPackage> AssetPackage = nullptr;
	TSharedRef<FAnimationMeshBudget, ESPMode::ThreadSafe> Budget =
		MakeShared<FAnimationMeshBudget, ESPMode::ThreadSafe>(FinalizedLimits());
	TUniquePtr<FAnimationCaptureMeshReference> BodyReference;
	TUniquePtr<FAnimationCaptureMeshReference> AttachedReference;
	TUniquePtr<FAnimationCaptureMeshReference> DuplicateBodyReference;
	TUniquePtr<FAnimationCaptureMeshReference> MissingLODReference;
	TUniquePtr<FAnimationCaptureMeshGPU> GPU;
	FTransform ParentTransform = FTransform(FRotator(7, 31, -4), FVector(310, -125, 900), FVector::OneVector);

	~FFinalizedPoseFixture() { Stop(); }
	void AddReferencedObjects(FReferenceCollector& Collector) override
	{
		Collector.AddReferencedObject(MeshAsset);
		Collector.AddReferencedObject(Sequence);
		Collector.AddReferencedObject(Blueprint);
		Collector.AddReferencedObject(Montage);
		Collector.AddReferencedObject(AssetPackage);
	}
	FString GetReferencerName() const override { return TEXT("FFinalizedPoseFixture"); }

	bool PrepareAssets(FString& Error)
	{
		const FString PackageName = FString::Printf(TEXT("/Temp/AnimationCaptureHost/FinalizedPose_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		AssetPackage = CreatePackage(*PackageName);
		AssetPackage->SetFlags(RF_Transient);
		AssetPackage->AddToRoot();
		MeshAsset = MakeNeutralMesh(AssetPackage);
		Sequence = MeshAsset ? MakeTranslationSequence(MeshAsset, AssetPackage) : nullptr;
		Blueprint = MeshAsset ? MakeSlotBlueprint(MeshAsset->GetSkeleton(), AssetPackage, Error) : nullptr;
		Montage = Sequence ? UAnimMontage::CreateSlotAnimationAsDynamicMontage(
			Sequence, FinalizedSlot, 0, 0, 1, 1, -1) : nullptr;
		if (!MeshAsset || !Sequence || !Blueprint || !Montage)
		{
			if (Error.IsEmpty()) { Error = TEXT("Neutral finalized-pose assets could not be constructed"); }
			return false;
		}
		return true;
	}

	bool Start(UWorld* World, FAutomationTestBase* Test, FString& Error)
	{
		if (!IsValid(MeshAsset) || !IsValid(Sequence) || !IsValid(Blueprint) || !IsValid(Montage))
		{
			Error = TEXT("Neutral finalized-pose assets were not prepared before PIE");
			return false;
		}
		Test->TestTrue(TEXT("Generated assets survive map loading and PIE setup"),
			IsValid(MeshAsset) && IsValid(Sequence) && IsValid(Blueprint) && IsValid(Montage));
		if (!View.Start(World, Error)) { return false; }
		for (auto& Component : View.Meshes) { Component->SetWorldLocation(FVector(5000)); }
		World->GetFirstPlayerController()->GetViewTarget()->SetActorLocation(FVector(10, -125, 900));
		DiagnosticView = MakeUnique<FViewportAsyncCapture>(World, View.Viewport, true);
		CacheMode = IConsoleManager::Get().FindConsoleVariable(TEXT("r.SkinCache.Mode"));
		if (!CacheMode) { Error = TEXT("Fixture Skin Cache mode is unavailable"); return false; }
		PreviousCacheMode = CacheMode->GetInt();
		CacheMode->SetWithCurrentPriority(1);
		BodyActor = World->SpawnActor<AActor>();
		AttachmentActor = World->SpawnActor<AActor>();
		Body = NewObject<USkeletalMeshComponent>(BodyActor.Get());
		AttachmentParent = NewObject<USkeletalMeshComponent>(AttachmentActor.Get());
		for (USkeletalMeshComponent* Component : {Body, AttachmentParent})
		{
			Component->SetSkeletalMesh(MeshAsset);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
			Component->SetAnimationMode(EAnimationMode::AnimationBlueprint);
			Component->SetAnimInstanceClass(Blueprint->GeneratedClass);
			Component->SkinCacheUsage.Init(ESkinCacheUsage::Enabled,
				MeshAsset->GetResourceForRendering()->LODRenderData.Num());
		}
		for (USkeletalMeshComponent* Component : {Body, AttachmentParent})
		{
			auto* Root = NewObject<USceneComponent>(Component->GetOwner());
			Component->GetOwner()->SetRootComponent(Root);
			Root->RegisterComponent();
			Root->SetWorldTransform(ParentTransform);
			Component->SetupAttachment(Root);
			Component->RegisterComponent();
		}
		Test->TestTrue(TEXT("Graph components inherit ordinary transformed scene parents"),
			Body->GetAttachParent() && AttachmentParent->GetAttachParent()
			&& Body->GetComponentTransform().Equals(ParentTransform)
			&& AttachmentParent->GetComponentTransform().Equals(ParentTransform));
		AttachedPart = NewObject<UStaticMeshComponent>(AttachmentActor.Get());
		AttachedPart->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/EngineMeshes/Cube.Cube")));
		AttachedPart->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		AttachedPart->SetupAttachment(AttachmentParent, TEXT("Motion"));
		AttachedPart->SetRelativeTransform(FTransform(FRotator::ZeroRotator, FVector(PartOffsets[0], 0, 0), FVector(CubeScale)));
		AttachedPart->RegisterComponent();
		if (!AttachedPart->GetStaticMesh() || !Body->GetAnimInstance() || !AttachmentParent->GetAnimInstance())
		{
			Error = TEXT("Neutral graph components did not initialize");
			return false;
		}
		for (UAnimInstance* Instance : {Body->GetAnimInstance(), AttachmentParent->GetAnimInstance()})
		{
			if (Instance->Montage_Play(Montage, 1.0f) <= 0)
			{
				Error = TEXT("Neutral montage did not start through the compiled slot graph");
				return false;
			}
			Instance->Montage_Pause(Montage);
		}
		Test->TestEqual(TEXT("Body uses AnimationBlueprint mode"), Body->GetAnimationMode(), EAnimationMode::AnimationBlueprint);
		Test->TestEqual(TEXT("Attachment parent uses AnimationBlueprint mode"), AttachmentParent->GetAnimationMode(), EAnimationMode::AnimationBlueprint);
		Test->TestTrue(TEXT("Body instance is generated AnimBlueprint class"), Body->GetAnimInstance()->GetClass() == Blueprint->GeneratedClass);
		Test->TestTrue(TEXT("Montage is active through the compiled slot"), Body->GetAnimInstance()->GetCurrentActiveMontage() == Montage);

		auto Default = FAnimationCaptureMeshReference::Create(Body, Enrollment(Body, TEXT("default-graph")), Budget, Error);
		Test->TestFalse(TEXT("Default policy rejects AnimationBlueprint graph"), Default.IsValid());
		auto InvalidEnrollment = Enrollment(Body, TEXT("invalid-policy"), static_cast<EAnimationMeshPosePolicy>(255));
		auto Invalid = FAnimationCaptureMeshReference::Create(Body, InvalidEnrollment, Budget, Error);
		Test->TestFalse(TEXT("Invalid pose policy rejects enrollment"), Invalid.IsValid());

		BodyReference = FAnimationCaptureMeshReference::Create(
			Body, Enrollment(Body, TEXT("animated-body"), EAnimationMeshPosePolicy::FinalizedAnimation), Budget, Error);
		AttachedReference = FAnimationCaptureMeshReference::Create(
			AttachedPart, Enrollment(AttachedPart, TEXT("attached-part"), EAnimationMeshPosePolicy::FinalizedAnimation), Budget, Error);
		DuplicateBodyReference = FAnimationCaptureMeshReference::Create(
			Body, Enrollment(Body, TEXT("animated-body-duplicate"), EAnimationMeshPosePolicy::FinalizedAnimation), Budget, Error);
		MissingLODReference = FAnimationCaptureMeshReference::Create(
			Body, Enrollment(Body, TEXT("missing-lod"), EAnimationMeshPosePolicy::FinalizedAnimation, 1), Budget, Error);
		if (!BodyReference || !AttachedReference || !DuplicateBodyReference || !MissingLODReference)
		{
			return false;
		}
		Test->TestFalse(TEXT("Opt-in enrollment before finalization cannot capture"),
			BodyReference->Capture(TEXT("before-finalization"), Error).IsValid());
		if (World->GetGameViewport() && World->GetGameViewport()->Viewport)
		{
			FAnimationMeshGPULimits GPULimits{2, 8ll * 1024 * 1024, 1024 * 1024, 20};
			const FAnimationCaptureBudgetLimits SharedLimits{16, 64ll * 1024 * 1024};
			auto SharedBudget = MakeShared<FAnimationCaptureBudget, ESPMode::ThreadSafe>(SharedLimits);
			GPU = FAnimationCaptureMeshGPU::Create(World, World->GetGameViewport()->Viewport, Body,
				Enrollment(Body, TEXT("animated-body-gpu"), EAnimationMeshPosePolicy::FinalizedAnimation),
				FinalizedLimits(4), GPULimits, SharedBudget, Error);
		}
		if (!GPU) { Error = TEXT("Finalized graph GPU enrollment failed: ") + Error; return false; }
		return true;
	}

	void Finalize(double PositionSeconds, double PartOffset)
	{
		AttachedPart->SetRelativeTransform(FTransform(FRotator::ZeroRotator, FVector(PartOffset, 0, 0), FVector(CubeScale)));
		for (USkeletalMeshComponent* Component : {Body, AttachmentParent})
		{
			UAnimInstance* Instance = Component->GetAnimInstance();
			Instance->Montage_SetPosition(Montage, PositionSeconds);
			Component->TickAnimation(0, false);
			Component->RefreshBoneTransforms();
		}
	}

	FVector ExpectedPartOrigin(double PositionSeconds, double PartOffset) const
	{
		const double Pitch = FMath::DegreesToRadians(7.0), Yaw = FMath::DegreesToRadians(31.0);
		return FVector(310, -125, 900) + (PartOffset + 10.0 * PositionSeconds)
			* FVector(FMath::Cos(Pitch) * FMath::Cos(Yaw), FMath::Cos(Pitch) * FMath::Sin(Yaw), FMath::Sin(Pitch));
	}

	void Stop()
	{
		GPU.Reset();
		DiagnosticView.Reset();
		BodyReference.Reset();
		AttachedReference.Reset();
		DuplicateBodyReference.Reset();
		MissingLODReference.Reset();
		if (BodyActor.IsValid()) { BodyActor->Destroy(); }
		if (AttachmentActor.IsValid()) { AttachmentActor->Destroy(); }
		BodyActor.Reset();
		AttachmentActor.Reset();
		Body = nullptr;
		AttachmentParent = nullptr;
		AttachedPart = nullptr;
		View.Stop();
		if (CacheMode) { CacheMode->SetWithCurrentPriority(PreviousCacheMode); CacheMode = nullptr; }
		if (AssetPackage && AssetPackage->IsRooted()) { AssetPackage->RemoveFromRoot(); }
		AssetPackage = nullptr;
	}
};

class FFinalizedPoseControls : public IAutomationLatentCommand
{
public:
	FFinalizedPoseControls(FAutomationTestBase* InTest, TSharedRef<FFinalizedPoseFixture> InFixture)
		: Test(InTest), Started(FPlatformTime::Seconds()), Fixture(MoveTemp(InFixture)) {}
	~FFinalizedPoseControls() { Fixture->Stop(); }

	bool Update() override
	{
		if (FPlatformTime::Seconds() - Started > 90)
		{
			Test->AddError(TEXT("Finalized-pose control deadline exceeded"));
			return true;
		}
		FString Error;
		if (!bStarted)
		{
			UWorld* World = nullptr;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::PIE) { World = Context.World(); break; }
			}
			if (!World) { return false; }
			if (!Fixture->Start(World, Test, Error)) { Test->AddError(Error); return true; }
			OutputDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Observations") /
				(TEXT("FinalizedPose-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			if (!IFileManager::Get().MakeDirectory(*OutputDirectory, true))
			{
				Test->AddError(TEXT("Could not create finalized-pose evidence directory"));
				return true;
			}
			bStarted = true;
			return false;
		}
		if (!bBoundaryControlsRun)
		{
			Fixture->Finalize(AnimationPositions[0], PartOffsets[0]);
			RunBoundaryControls();
			if (HasErrors()) { return true; }
			bBoundaryControlsRun = true;
			Fixture->Body->SetComponentTickEnabled(false);
			StaleFrame = GFrameCounter;
			return false;
		}
		if (!bStaleControlRun)
		{
			if (GFrameCounter == StaleFrame) { return false; }
			Test->TestFalse(TEXT("Previous-frame finalization witness is unavailable"),
				Fixture->BodyReference->Capture(TEXT("previous-frame"), Error).IsValid());
			Fixture->Body->SetComponentTickEnabled(true);
			bStaleControlRun = true;
			return false;
		}
		if (SampleIndex < PairCount)
		{
			CapturePair();
			return HasErrors();
		}
		if (!bGPURequested)
		{
			if (Fixture->GPU)
			{
				Fixture->Finalize(AnimationPositions[PairCount - 1], PartOffsets[PairCount - 1]);
				if (!Fixture->GPU->Request(TEXT("delayed-finalized-body"), GPUTicket, Error))
				{
					Test->AddError(Error);
					return true;
				}
			}
			bGPURequested = true;
			return false;
		}
		if (Fixture->GPU && !bGPUAdvanced)
		{
			Fixture->GPU->Pump();
			const auto Stats = Fixture->GPU->GetStats();
			if (Stats.Unavailable || Stats.Failed || Stats.Cancelled || Stats.TimedOut)
			{
				FAnimationMeshGPUResult Failed;
				if (Fixture->GPU->Collect(Failed)) { Test->AddError(TEXT("Finalized graph GPU: ") + Failed.Error); }
				return true;
			}
			if (Fixture->GPU->GetStats().Completed < 1) { return false; }
			Fixture->Finalize(.9, 0);
			auto Advanced = Fixture->BodyReference->Capture(TEXT("advanced-after-gpu"), Error);
			if (!Advanced) { Test->AddError(Error); return true; }
			AdvancedBodyX = Advanced->Data().Positions[0].X;
			AdvancedAcquiredSeconds = Advanced->Data().AcquiredSeconds;
			Fixture->BodyActor->Destroy();
			bGPUAdvanced = true;
			return false; // Deliberately collect on a later frame after source retirement.
		}
		if (Fixture->GPU && !bGPUCollected)
		{
			FAnimationMeshGPUResult Result;
			if (!Fixture->GPU->Collect(Result)) { return false; }
			Test->TestEqual(TEXT("Delayed graph GPU result completed"), Result.Status, EAnimationMeshGPUStatus::Completed);
			if (Result.Snapshot)
			{
				Test->TestTrue(TEXT("Delayed GPU geometry retains its acquisition pose"),
					FMath::Abs(Result.Snapshot->Data().Positions[0].X - 5.0) < .001);
				Test->TestTrue(TEXT("Delayed GPU collection does not relabel advanced pose"),
					FMath::Abs(AdvancedBodyX - Result.Snapshot->Data().Positions[0].X) > 1.0);
				Test->TestEqual(TEXT("GPU result preserves observer revision"),
					int64(Result.PoseRevision), Result.Snapshot->Data().PoseRevision);
				Test->TestEqual(TEXT("GPU result preserves acquired frame"),
					int64(Result.AcquisitionFrame), Result.Snapshot->Data().FrameId);
				Test->TestEqual(TEXT("GPU result preserves acquisition timestamp"),
					Result.AcquiredSeconds, Result.Snapshot->Data().AcquiredSeconds);
				Test->TestTrue(TEXT("GPU acquisition preceded advance and delayed collection"),
					Result.AcquiredSeconds < AdvancedAcquiredSeconds && Result.AcquisitionFrame < GFrameCounter
					&& Result.CompletedSeconds <= AdvancedAcquiredSeconds && Result.CollectedSeconds > AdvancedAcquiredSeconds);
				Test->TestTrue(TEXT("GPU source is retired before collection"), !Fixture->BodyActor.IsValid());
				if (!AnimationCaptureMeshReplay::Write(OutputDirectory, TEXT("delayed-gpu-body"), *Result.Snapshot, Error))
				{
					Test->AddError(Error);
				}
				GPUEvidence = MakeShared<FJsonObject>();
				GPUEvidence->SetNumberField(TEXT("acquired_seconds"), Result.AcquiredSeconds);
				GPUEvidence->SetNumberField(TEXT("completed_seconds"), Result.CompletedSeconds);
				GPUEvidence->SetNumberField(TEXT("collected_seconds"), Result.CollectedSeconds);
				GPUEvidence->SetNumberField(TEXT("advanced_acquired_seconds"), AdvancedAcquiredSeconds);
				GPUEvidence->SetNumberField(TEXT("prepare_ms"), Result.PrepareSeconds * 1000);
				GPUEvidence->SetNumberField(TEXT("capture_ms"), Result.CaptureSeconds * 1000);
				GPUEvidence->SetNumberField(TEXT("setup_wait_ms"), Result.SetupWaitSeconds * 1000);
				GPUEvidence->SetNumberField(TEXT("decode_ms"), Result.DecodeSeconds * 1000);
				GPUEvidence->SetNumberField(TEXT("pose_revision"), double(Result.PoseRevision));
			}
			else { Test->AddError(TEXT("Completed GPU result has no owned geometry")); }
			bGPUCollected = true;
		}
		PublishManifest();
		return true;
	}

private:
	bool HasErrors() const { return Test->HasAnyErrors(); }

	void RunBoundaryControls()
	{
		FString Error;
		TArray<TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe>> Results;
		TArray<FAnimationCaptureMeshReference*> Valid{Fixture->BodyReference.Get(), Fixture->AttachedReference.Get()};
		if (!FAnimationCaptureMeshReference::CaptureBatch(Valid, TEXT("boundary-positive"), 2, Results, Error))
		{
			Test->AddError(TEXT("Positive control before negative boundaries: ") + Error);
			return;
		}
		Test->TestEqual(TEXT("Positive boundary batch has both participants"), Results.Num(), 2);
		Results.Reset();
		Test->TestFalse(TEXT("Narrow rigid batch still rejects animated participants"),
			FAnimationCaptureMeshReference::CaptureRigidBatch(Valid, TEXT("rigid-only"), 2, Results, Error));
		UWorld* CaptureWorld = Fixture->Body->GetWorld();
		CaptureWorld->bInTick = true;
		Test->TestFalse(TEXT("Mixed batch rejects world-tick acquisition"),
			FAnimationCaptureMeshReference::CaptureBatch(Valid, TEXT("during-world-tick"), 2, Results, Error));
		CaptureWorld->bInTick = false;
		TArray<FAnimationCaptureMeshReference*> Empty;
		Test->TestFalse(TEXT("Empty mixed batch is unavailable"),
			FAnimationCaptureMeshReference::CaptureBatch(Empty, TEXT("empty"), 2, Results, Error));
		Test->TestFalse(TEXT("Zero mixed batch maximum is unavailable"),
			FAnimationCaptureMeshReference::CaptureBatch(Valid, TEXT("zero"), 0, Results, Error));
		Test->TestFalse(TEXT("Mixed batch maximum above 64 is unavailable"),
			FAnimationCaptureMeshReference::CaptureBatch(Valid, TEXT("large"), 65, Results, Error));
		Test->TestFalse(TEXT("Mixed batch count above supplied maximum is unavailable"),
			FAnimationCaptureMeshReference::CaptureBatch(Valid, TEXT("over-count"), 1, Results, Error));
		TArray<FAnimationCaptureMeshReference*> DuplicateObserver{Fixture->BodyReference.Get(), Fixture->BodyReference.Get()};
		Test->TestFalse(TEXT("Duplicate mixed batch observer is unavailable"),
			FAnimationCaptureMeshReference::CaptureBatch(DuplicateObserver, TEXT("duplicate-observer"), 2, Results, Error));
		TArray<FAnimationCaptureMeshReference*> DuplicateComponent{Fixture->BodyReference.Get(), Fixture->DuplicateBodyReference.Get()};
		Test->TestFalse(TEXT("Duplicate mixed batch component is unavailable"),
			FAnimationCaptureMeshReference::CaptureBatch(DuplicateComponent, TEXT("duplicate-component"), 2, Results, Error));
		Test->TestFalse(TEXT("Absent analysis LOD is unavailable"),
			Fixture->MissingLODReference->Capture(TEXT("missing-lod"), Error).IsValid());

		const TArray<FBoneIndexType> RequiredBones = Fixture->Body->RequiredBones;
		Fixture->Body->RequiredBones.Remove(1);
		Test->TestFalse(TEXT("Missing evaluated body bone is unavailable"),
			Fixture->BodyReference->Capture(TEXT("missing-body-bone"), Error).IsValid());
		Fixture->Body->RequiredBones = RequiredBones;
		const TArray<FBoneIndexType> ParentRequiredBones = Fixture->AttachmentParent->RequiredBones;
		Fixture->AttachmentParent->RequiredBones.Remove(1);
		Test->TestFalse(TEXT("Missing evaluated attachment bone is unavailable"),
			Fixture->AttachedReference->Capture(TEXT("missing-attachment-bone"), Error).IsValid());
		Fixture->AttachmentParent->RequiredBones = ParentRequiredBones;

		Fixture->AttachedPart->SetUsingAbsoluteLocation(true);
		Test->TestFalse(TEXT("Absolute attachment transform is unavailable"),
			Fixture->AttachedReference->Capture(TEXT("absolute-attachment"), Error).IsValid());
		Fixture->AttachedPart->SetUsingAbsoluteLocation(false);
		const FTransform SynchronizedTransform = Fixture->AttachedPart->GetComponentTransform();
		FTransform UnsynchronizedTransform = SynchronizedTransform;
		UnsynchronizedTransform.AddToTranslation(FVector(1, 0, 0));
		Fixture->AttachedPart->SetComponentToWorld(UnsynchronizedTransform);
		Test->TestFalse(TEXT("Unsynchronized cached attachment transform is unavailable"),
			Fixture->AttachedReference->Capture(TEXT("unsynchronized-attachment"), Error).IsValid());
		Fixture->AttachedPart->SetComponentToWorld(SynchronizedTransform);
		Fixture->Body->LeaderPoseComponent = Fixture->AttachmentParent;
		Test->TestFalse(TEXT("Graph leader pose remains unsupported"),
			Fixture->BodyReference->Capture(TEXT("leader-pose"), Error).IsValid());
		Fixture->Body->LeaderPoseComponent.Reset();
		Fixture->Body->bBlendPhysics = true;
		Test->TestFalse(TEXT("Graph physics blending remains unsupported"),
			Fixture->BodyReference->Capture(TEXT("physics-blending"), Error).IsValid());
		Fixture->Body->bBlendPhysics = false;
		Fixture->Body->SetForceRefPose(true);
		Test->TestFalse(TEXT("Forced reference pose remains unsupported"),
			Fixture->BodyReference->Capture(TEXT("forced-reference"), Error).IsValid());
		Fixture->Body->SetForceRefPose(false);
		FAnimNode_LinkedAnimGraph LinkedGraph;
		LinkedGraph.SetAnimClass(Fixture->Blueprint->GeneratedClass.Get(), Fixture->Body->GetAnimInstance());
		Test->TestEqual(TEXT("Engine linked graph registers one real instance"),
			static_cast<const USkeletalMeshComponent*>(Fixture->Body)->GetLinkedAnimInstances().Num(), 1);
		Test->TestFalse(TEXT("Graph linked instances remain unsupported"),
			Fixture->BodyReference->Capture(TEXT("linked-instance"), Error).IsValid());
		LinkedGraph.SetAnimClass(nullptr, Fixture->Body->GetAnimInstance());
		Fixture->Body->SetOverridePostProcessAnimBP(Fixture->Blueprint->GeneratedClass.Get(), false);
		Test->TestFalse(TEXT("Graph post-process class remains unsupported"),
			Fixture->BodyReference->Capture(TEXT("post-process-class"), Error).IsValid());
		Fixture->Body->SetOverridePostProcessAnimBP(nullptr, false);
		Fixture->AttachmentParent->SetRefPoseOverride(Fixture->MeshAsset->GetRefSkeleton().GetRefBonePose());
		Test->TestFalse(TEXT("Reference-pose override remains unsupported for graph parent"),
			Fixture->AttachedReference->Capture(TEXT("unsupported-parent"), Error).IsValid());
		Fixture->AttachmentParent->ClearRefPoseOverride();
		Fixture->Finalize(AnimationPositions[0], PartOffsets[0]);
		bool bFinalizationCallbackRan = false;
		const FDelegateHandle DuringPostEvaluation = Fixture->Body->RegisterOnBoneTransformsFinalizedDelegate(
			FOnBoneTransformsFinalizedMultiCast::FDelegate::CreateLambda([&]
			{
				bFinalizationCallbackRan = true;
				Test->TestTrue(TEXT("Fixture callback occurs during post evaluation"), Fixture->Body->IsPostEvaluatingAnimation());
				Test->TestFalse(TEXT("Acquisition during post evaluation is unavailable"),
					Fixture->BodyReference->Capture(TEXT("during-post-evaluation"), Error).IsValid());
			}));
		Fixture->Finalize(AnimationPositions[0], PartOffsets[0]);
		Fixture->Body->UnregisterOnBoneTransformsFinalizedDelegate(DuringPostEvaluation);
		Test->TestTrue(TEXT("Post-evaluation boundary was exercised"), bFinalizationCallbackRan);

		Fixture->Body->TickAnimation(0, false);
		Test->TestFalse(TEXT("Pending same-frame graph update is unavailable"),
			Fixture->BodyReference->Capture(TEXT("pending-update"), Error).IsValid());
		Fixture->Finalize(AnimationPositions[0], PartOffsets[0]);

		auto TinyBudget = MakeShared<FAnimationMeshBudget, ESPMode::ThreadSafe>(FinalizedLimits(1));
		auto TinyBody = FAnimationCaptureMeshReference::Create(Fixture->Body,
			Enrollment(Fixture->Body, TEXT("tiny-body"), EAnimationMeshPosePolicy::FinalizedAnimation), TinyBudget, Error);
		auto TinyPart = FAnimationCaptureMeshReference::Create(Fixture->AttachedPart,
			Enrollment(Fixture->AttachedPart, TEXT("tiny-part"), EAnimationMeshPosePolicy::FinalizedAnimation), TinyBudget, Error);
		if (!TinyBody || !TinyPart) { Test->AddError(TEXT("Budget control enrollment: ") + Error); return; }
		Fixture->Finalize(AnimationPositions[0], PartOffsets[0]);
		TArray<FAnimationCaptureMeshReference*> Tiny{TinyBody.Get(), TinyPart.Get()};
		Test->TestFalse(TEXT("Mixed batch admission is transactional"),
			FAnimationCaptureMeshReference::CaptureBatch(Tiny, TEXT("budget"), 2, Results, Error));
		Test->TestEqual(TEXT("Failed mixed batch publishes no snapshots"), Results.Num(), 0);
		Test->TestEqual(TEXT("Failed mixed batch releases bytes"), TinyBudget->LiveBytes(), int64(0));
		Test->TestEqual(TEXT("Failed mixed batch releases count"), TinyBudget->LiveSnapshots(), 0);

		UWorld* OtherWorld = UWorld::CreateWorld(EWorldType::Game, false);
		FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
		Context.SetCurrentWorld(OtherWorld);
		AActor* OtherActor = OtherWorld->SpawnActor<AActor>();
		auto* OtherComponent = NewObject<UStaticMeshComponent>(OtherActor);
		OtherActor->SetRootComponent(OtherComponent);
		OtherComponent->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/EngineMeshes/Cube.Cube")));
		OtherComponent->RegisterComponent();
		auto OtherBudget = MakeShared<FAnimationMeshBudget, ESPMode::ThreadSafe>(FinalizedLimits(2));
		auto OtherReference = FAnimationCaptureMeshReference::Create(OtherComponent,
			Enrollment(OtherComponent, TEXT("other-world")), OtherBudget, Error);
		Test->TestTrue(TEXT("Cross-world participant is validly enrolled"), OtherReference.IsValid());
		TArray<FAnimationCaptureMeshReference*> CrossWorld{Fixture->BodyReference.Get(), OtherReference.Get()};
		Test->TestFalse(TEXT("Cross-world mixed batch is unavailable"),
			FAnimationCaptureMeshReference::CaptureBatch(CrossWorld, TEXT("cross-world"), 2, Results, Error));
		OtherReference.Reset();
		GEngine->DestroyWorldContext(OtherWorld);
		OtherWorld->DestroyWorld(false);

		UStaticMesh* OriginalRigidAsset = Fixture->AttachedPart->GetStaticMesh();
		Fixture->AttachedPart->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		Test->TestFalse(TEXT("One-side asset replacement invalidates old enrollment"),
			Fixture->AttachedReference->Capture(TEXT("replaced-asset"), Error).IsValid());
		Fixture->AttachedReference = FAnimationCaptureMeshReference::Create(Fixture->AttachedPart,
			Enrollment(Fixture->AttachedPart, TEXT("attached-part"), EAnimationMeshPosePolicy::FinalizedAnimation), Fixture->Budget, Error);
		if (!Fixture->AttachedReference) { Test->AddError(Error); return; }
		Test->TestFalse(TEXT("Reenrolled asset replacement requires new finalization"),
			Fixture->AttachedReference->Capture(TEXT("asset-before-finalization"), Error).IsValid());
		Fixture->Finalize(AnimationPositions[0], PartOffsets[0]);
		Test->TestTrue(TEXT("Reenrolled asset replacement captures after finalization"),
			Fixture->AttachedReference->Capture(TEXT("asset-after-finalization"), Error).IsValid());
		Fixture->AttachedPart->SetStaticMesh(OriginalRigidAsset);
		Fixture->AttachedReference = FAnimationCaptureMeshReference::Create(Fixture->AttachedPart,
			Enrollment(Fixture->AttachedPart, TEXT("attached-part"), EAnimationMeshPosePolicy::FinalizedAnimation), Fixture->Budget, Error);
		if (!Fixture->AttachedReference) { Test->AddError(Error); return; }
		Fixture->Finalize(AnimationPositions[0], PartOffsets[0]);
		Test->TestTrue(TEXT("Restored asset is eligible before custom-mode control"),
			Fixture->AttachedReference->Capture(TEXT("before-custom-mode"), Error).IsValid());
		Fixture->AttachmentParent->SetAnimationMode(EAnimationMode::AnimationCustomMode);
		Test->TestFalse(TEXT("Custom animation mode remains unsupported"),
			Fixture->AttachedReference->Capture(TEXT("custom-mode"), Error).IsValid());
		Fixture->AttachmentParent->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		UAnimInstance* OriginalInstance = Fixture->AttachmentParent->GetAnimInstance();
		// UE 5.6 intentionally treats setting the existing class as a no-op.
		Fixture->AttachmentParent->SetAnimInstanceClass(nullptr);
		Fixture->AttachmentParent->SetAnimInstanceClass(Fixture->Blueprint->GeneratedClass);
		Test->TestNotEqual(TEXT("Fixture replaced only the attachment-parent instance"),
			Fixture->AttachmentParent->GetAnimInstance(), OriginalInstance);
		Test->TestFalse(TEXT("One-side instance replacement invalidates old enrollment"),
			Fixture->AttachedReference->Capture(TEXT("replaced-instance"), Error).IsValid());
		Fixture->AttachedReference = FAnimationCaptureMeshReference::Create(Fixture->AttachedPart,
			Enrollment(Fixture->AttachedPart, TEXT("attached-part"), EAnimationMeshPosePolicy::FinalizedAnimation),
			Fixture->Budget, Error);
		Test->TestTrue(TEXT("Reenrollment after replacement succeeds before finalization"), Fixture->AttachedReference.IsValid());
		Test->TestFalse(TEXT("Reenrolled replacement still requires a new finalization"),
			Fixture->AttachedReference->Capture(TEXT("replacement-before-finalization"), Error).IsValid());
		UAnimInstance* NewInstance = Fixture->AttachmentParent->GetAnimInstance();
		Test->TestTrue(TEXT("Replacement montage starts"), NewInstance->Montage_Play(Fixture->Montage, 1.0f) > 0);
		NewInstance->Montage_Pause(Fixture->Montage);
		Fixture->Finalize(AnimationPositions[0], PartOffsets[0]);
	}

	void CapturePair()
	{
		FString Error;
		Fixture->Finalize(AnimationPositions[SampleIndex], PartOffsets[SampleIndex]);
		if (SampleIndex == 0)
		{
			// One extra finalization on only the body proves observer-local revisions need
			// not match even when acquisition and geometry are coherent.
			Fixture->Body->TickAnimation(0, false);
			Fixture->Body->RefreshBoneTransforms();
		}
		TArray<FAnimationCaptureMeshReference*> Samplers{Fixture->BodyReference.Get(), Fixture->AttachedReference.Get()};
		TArray<TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe>> Pair;
		const FString PairId = FString::Printf(TEXT("finalized-pair-%02d"), SampleIndex);
		const uint32 BodyBoneRevision = Fixture->Body->GetBoneTransformRevisionNumber();
		const uint32 ParentBoneRevision = Fixture->AttachmentParent->GetBoneTransformRevisionNumber();
		const double BeforeCapture = FPlatformTime::Seconds();
		if (!FAnimationCaptureMeshReference::CaptureBatch(Samplers, PairId, 2, Pair, Error))
		{
			Test->AddError(Error);
			return;
		}
		BatchMilliseconds.Add(MakeShared<FJsonValueNumber>((FPlatformTime::Seconds() - BeforeCapture) * 1000));
		Test->TestEqual(TEXT("Batch does not refresh body pose"), Fixture->Body->GetBoneTransformRevisionNumber(), BodyBoneRevision);
		Test->TestEqual(TEXT("Batch does not refresh attachment-parent pose"), Fixture->AttachmentParent->GetBoneTransformRevisionNumber(), ParentBoneRevision);
		Test->TestEqual(TEXT("Batch does not switch animation mode"), Fixture->Body->GetAnimationMode(), EAnimationMode::AnimationBlueprint);
		Test->TestEqual(TEXT("Finalized pair has two participants"), Pair.Num(), 2);
		if (Pair.Num() != 2) { return; }
		const auto& BodyData = Pair[0]->Data();
		const auto& PartData = Pair[1]->Data();
		Test->TestEqual(TEXT("Pair preserves shared acquisition"), BodyData.AcquiredSeconds, PartData.AcquiredSeconds);
		Test->TestEqual(TEXT("Pair preserves shared engine frame"), BodyData.FrameId, PartData.FrameId);
		Test->TestTrue(TEXT("Pair preserves actual separate completions"), BodyData.CompletedSeconds != PartData.CompletedSeconds);
		if (SampleIndex == 0)
		{
			Test->TestNotEqual(TEXT("Coherent batch preserves distinct observer-local revisions"),
				BodyData.PoseRevision, PartData.PoseRevision);
		}
		Test->TestEqual(TEXT("Animated body has four vertices"), BodyData.Positions.Num(), 4);
		Test->TestEqual(TEXT("Animated body has two triangles"), BodyData.Indices.Num(), 6);
		Test->TestEqual(TEXT("Attached Engine cube has 24 vertices"), PartData.Positions.Num(), 24);
		Test->TestEqual(TEXT("Attached Engine cube has 12 triangles"), PartData.Indices.Num(), 36);
		for (const FVector3d& Position : BodyData.Positions)
		{
			Test->TestTrue(TEXT("Animated plane retains independently authored Y/Z coordinates"),
				FMath::Abs(FMath::Abs(Position.Y) - 10.0) < .001 && FMath::Abs(FMath::Abs(Position.Z) - 10.0) < .001);
			Test->TestTrue(TEXT("Animated body matches authored linear translation within 0.001 cm"),
				FMath::Abs(Position.X - 10.0 * AnimationPositions[SampleIndex]) < .001);
		}
		const FVector ExpectedOrigin = Fixture->ExpectedPartOrigin(AnimationPositions[SampleIndex], PartOffsets[SampleIndex]);
		Test->TestTrue(TEXT("Attached part matches independently composed transform within 0.001 cm"),
			FVector::Distance(FVector(PartData.ComponentToWorld.GetOrigin()), ExpectedOrigin) < .001);

		const FString BodyBundle = PairId + TEXT("-body");
		const FString PartBundle = PairId + TEXT("-attached-part");
		if (!AnimationCaptureMeshReplay::Write(OutputDirectory, BodyBundle, *Pair[0], Error)
			|| !AnimationCaptureMeshReplay::Write(OutputDirectory, PartBundle, *Pair[1], Error))
		{
			Test->AddError(Error);
			return;
		}
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("pair_id"), PairId);
		Json->SetStringField(TEXT("body_bundle"), BodyBundle);
		Json->SetStringField(TEXT("attached_part_bundle"), PartBundle);
		Json->SetNumberField(TEXT("frame_id"), double(BodyData.FrameId));
		Json->SetNumberField(TEXT("acquired_seconds"), BodyData.AcquiredSeconds);
		auto Participant = [](const FAnimationMeshData& Data)
		{
			auto Value = MakeShared<FJsonObject>();
			Value->SetNumberField(TEXT("pose_revision"), double(Data.PoseRevision));
			Value->SetStringField(TEXT("configuration_id"), Data.ConfigurationId);
			Value->SetStringField(TEXT("topology_id"), Data.TopologyId);
			Value->SetNumberField(TEXT("completed_seconds"), Data.CompletedSeconds);
			return Value;
		};
		Json->SetObjectField(TEXT("body"), Participant(BodyData));
		Json->SetObjectField(TEXT("attached_part"), Participant(PartData));
		auto Expected = MakeShared<FJsonObject>();
		Expected->SetNumberField(TEXT("animation_position_seconds"), AnimationPositions[SampleIndex]);
		Expected->SetArrayField(TEXT("body_position_offset_cm"), Vector(FVector(10.0 * AnimationPositions[SampleIndex], 0, 0)));
		Expected->SetArrayField(TEXT("attached_part_world_origin_cm"), Vector(ExpectedOrigin));
		Expected->SetNumberField(TEXT("minimum_distance_cm"), ExpectedDistances[SampleIndex]);
		Expected->SetBoolField(TEXT("surface_intersection"), ExpectedIntersections[SampleIndex]);
		Json->SetObjectField(TEXT("expected"), Expected);
		Pairs.Add(MakeShared<FJsonValueObject>(Json));
		AcquisitionTimes.Add(BodyData.AcquiredSeconds);
		Retained.Append(Pair);
		++SampleIndex;
	}

	TSharedRef<FJsonObject> Role(const FAnimationMeshData& Data) const
	{
		auto Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("component_id"), Data.Enrollment.ComponentId);
		Value->SetNumberField(TEXT("component_generation"), double(Data.Enrollment.ComponentGeneration));
		Value->SetStringField(TEXT("subject_id"), Data.Enrollment.SubjectId);
		Value->SetStringField(TEXT("stream_id"), Data.Enrollment.StreamId);
		return Value;
	}

	void PublishManifest()
	{
		if (HasErrors() || Retained.Num() != PairCount * 2) { return; }
		for (int32 Index = 1; Index < AcquisitionTimes.Num(); ++Index)
		{
			const double Gap = AcquisitionTimes[Index] - AcquisitionTimes[Index - 1];
			Test->TestTrue(TEXT("Actual pair gap is positive"), Gap > 0);
			Test->TestTrue(TEXT("Actual pair gap stays within one-second fixture bound"), Gap <= 1.0);
			Test->TestTrue(TEXT("Pairs use distinct observed frames"),
				Retained[Index * 2]->Data().FrameId > Retained[(Index - 1) * 2]->Data().FrameId);
		}
		auto Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(TEXT("format"), TEXT("neutral_finalized_pose_pairs"));
		Manifest->SetNumberField(TEXT("schema_version"), 1);
		auto Roles = MakeShared<FJsonObject>();
		Roles->SetObjectField(TEXT("body"), Role(Retained[0]->Data()));
		Roles->SetObjectField(TEXT("attached_part"), Role(Retained[1]->Data()));
		Manifest->SetObjectField(TEXT("roles"), Roles);
		Manifest->SetArrayField(TEXT("pairs"), Pairs);
		auto Controls = MakeShared<FJsonObject>();
		Controls->SetBoolField(TEXT("checks_passed"), !HasErrors());
		Controls->SetNumberField(TEXT("peak_reserved_bytes"), double(Fixture->Budget->PeakBytes()));
		Manifest->SetObjectField(TEXT("controls"), Controls);
		FString Json;
		FJsonSerializer::Serialize(Manifest, TJsonWriterFactory<>::Create(&Json));
		Test->TestTrue(TEXT("Finalized-pose manifest is bounded to 64 KiB"), Json.Len() <= 64 * 1024);
		if (!FFileHelper::SaveStringToFile(Json, *(OutputDirectory / TEXT("finalized-pose-pairs.json"))))
		{
			Test->AddError(TEXT("Could not publish finalized-pose pair manifest"));
		}
		auto Performance = MakeShared<FJsonObject>();
		Performance->SetArrayField(TEXT("cpu_pair_capture_ms"), BatchMilliseconds);
		Performance->SetObjectField(TEXT("delayed_gpu"), GPUEvidence.ToSharedRef());
		Performance->SetStringField(TEXT("interpretation"), TEXT("Five local CPU pair samples and one GPU sample; not a real-time or cadence guarantee"));
		FString PerformanceJson;
		FJsonSerializer::Serialize(Performance, TJsonWriterFactory<>::Create(&PerformanceJson));
		if (!FFileHelper::SaveStringToFile(PerformanceJson, *(OutputDirectory / TEXT("finalized-pose-controls.json"))))
		{
			Test->AddError(TEXT("Could not publish finalized-pose timing evidence"));
		}
	}

	FAutomationTestBase* Test;
	double Started;
	bool bStarted = false;
	bool bBoundaryControlsRun = false;
	bool bStaleControlRun = false;
	bool bGPURequested = false;
	bool bGPUAdvanced = false;
	bool bGPUCollected = false;
	uint64 StaleFrame = 0;
	int32 SampleIndex = 0;
	double AdvancedBodyX = 0;
	double AdvancedAcquiredSeconds = 0;
	TSharedPtr<FJsonObject> GPUEvidence;
	TArray<TSharedPtr<FJsonValue>> BatchMilliseconds;
	FString OutputDirectory;
	TSharedRef<FFinalizedPoseFixture> Fixture;
	FAnimationMeshGPUTicket GPUTicket;
	TArray<TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe>> Retained;
	TArray<TSharedPtr<FJsonValue>> Pairs;
	TArray<double> AcquisitionTimes;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFinalizedPoseTest, "AnimationAnalysis.Capture.Mesh.FinalizedPose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFinalizedPoseTest::RunTest(const FString&)
{
	if (!FApp::CanEverRender())
	{
		AddError(TEXT("Finalized-pose fixture requires rendered host initialization"));
		return false;
	}
	TestNull(TEXT("Transient AnimBlueprint is authored before PIE"), GEditor ? GEditor->PlayWorld : nullptr);
	auto Fixture = MakeShared<FFinalizedPoseFixture>();
	FString Error;
	if (!Fixture->PrepareAssets(Error))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Pre-PIE AnimBlueprint has a generated AnimInstance class"),
		Fixture->Blueprint && Fixture->Blueprint->GeneratedClass
		&& Fixture->Blueprint->GeneratedClass->IsChildOf(UAnimInstance::StaticClass()));
	UEdGraph* AnimGraph = Fixture->Blueprint ? FindObject<UEdGraph>(Fixture->Blueprint, TEXT("AnimGraph")) : nullptr;
	TArray<UAnimGraphNode_Slot*> Slots;
	if (AnimGraph) { AnimGraph->GetNodesOfClass(Slots); }
	TestTrue(TEXT("Pre-PIE AnimBlueprint has the authored slot graph"),
		Slots.Num() == 1 && Slots[0] && Slots[0]->Node.SlotName == FinalizedSlot);
	UE_LOG(LogTemp, Display, TEXT("FinalizedPose fixture compiled AnimBlueprint and slot graph before PIE"));
	ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(TEXT("/Engine/Maps/Entry")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FFinalizedPoseControls(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	return true;
}
