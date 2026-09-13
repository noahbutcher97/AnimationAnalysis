#include "AnimationCapture/AnimationCaptureMeshReference.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRelevance.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "StaticMeshResources.h"

namespace
{
constexpr int64 MaxJsonInteger = 9007199254740991ll;
bool Identity(const FString& S, int32 Max = 256) { return !S.TrimStartAndEnd().IsEmpty() && S.Len() <= Max; }
bool LimitsValid(const FAnimationMeshLimits& L)
{
	return L.MaxSnapshots > 0 && L.MaxVertices > 0 && L.MaxVertices <= MAX_int32 / 24
		&& L.MaxIndices > 0 && L.MaxIndices <= MAX_int32 / 4 && L.MaxSections > 0
		&& L.MaxSections <= 64 && L.MaxBones > 0 && L.MaxBones <= 65536 && L.MaxBytes > 0;
}
struct FPoseQualification
{
	UAnimInstance* Instance = nullptr;
	UObject* Program = nullptr;
	EAnimationMode::Type Mode = EAnimationMode::AnimationCustomMode;
	int16 UpdateCounter = INDEX_NONE;
	uint32 BoneRevision = 0;
};
bool QualifiedPose(USkeletalMeshComponent* Mesh, EAnimationMeshPosePolicy Policy,
	FPoseQualification& Out, FString& Error, bool bRequireReady, bool bAllowPostEvaluation = false)
{
	Out = {};
	if (Policy != EAnimationMeshPosePolicy::SingleNode && Policy != EAnimationMeshPosePolicy::FinalizedAnimation)
	{
		Error = TEXT("unavailable: invalid mesh pose policy");
		return false;
	}
	if (!Mesh || Mesh->GetClass() != USkeletalMeshComponent::StaticClass() || !Mesh->IsRegistered()
		|| Mesh->LeaderPoseComponent.IsValid() || Mesh->GetPostProcessInstance()
		|| Mesh->GetPostProcessAnimBPClassToBeUsed() || Mesh->IsRunningParallelEvaluation()
		|| (!bAllowPostEvaluation && Mesh->IsPostEvaluatingAnimation())
		|| Mesh->IsSimulatingPhysics() || Mesh->bBlendPhysics || Mesh->bForceRefpose || Mesh->GetRefPoseOverride().IsValid()
		|| !static_cast<const USkeletalMeshComponent*>(Mesh)->GetLinkedAnimInstances().IsEmpty())
	{
		Error = TEXT("unavailable: finalized pose requires an ordinary registered component without leader, post-process, linked instances, evaluation work, physics blending or reference-pose override");
		return false;
	}
	Out.Mode = Mesh->GetAnimationMode();
	if (Out.Mode == EAnimationMode::AnimationSingleNode)
	{
		Out.Instance = Mesh->GetSingleNodeInstance();
		Out.Program = Out.Instance ? Cast<UAnimSingleNodeInstance>(Out.Instance)->GetAnimationAsset() : nullptr;
	}
	else if (Policy == EAnimationMeshPosePolicy::FinalizedAnimation
		&& Out.Mode == EAnimationMode::AnimationBlueprint)
	{
		Out.Instance = Mesh->GetAnimInstance();
		Out.Program = Mesh->GetAnimClass();
	}
	else
	{
		Error = TEXT("unavailable: pose policy does not admit the component animation mode");
		return false;
	}
	if (!IsValid(Out.Instance) || !Out.Instance->IsInitialized()
		|| Out.Instance->GetSkelMeshComponent() != Mesh || Out.Instance->IsRunningParallelEvaluation())
	{
		Error = TEXT("unavailable: corresponding initialized animation instance is absent or evaluating");
		return false;
	}
	if (bRequireReady && Out.Instance->NeedsUpdate())
	{
		Error = TEXT("unavailable: animation instance has an outstanding update");
		return false;
	}
	// GetUpdateCounter reaches the proxy and can block. The explicit component and
	// instance parallel-evaluation guards above make this an observation-only read.
	const FGraphTraversalCounter& Counter = Out.Instance->GetUpdateCounter();
	if (bRequireReady && !Counter.HasEverBeenUpdated())
	{
		Error = TEXT("unavailable: animation instance has no completed update witness");
		return false;
	}
	Out.UpdateCounter = Counter.Get();
	Out.BoneRevision = Mesh->GetBoneTransformRevisionNumber();
	return true;
}
void HashString(FMD5& Hash, const FString& S)
{
	FTCHARToUTF8 Utf8(*S); Hash.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	const uint8 Zero = 0; Hash.Update(&Zero, 1);
}
}

struct FAnimationMeshBudget::FState
{
	FAnimationMeshLimits Limits;
	mutable FCriticalSection Mutex;
	int64 Bytes = 0, Peak = 0;
	int32 Count = 0;
};
FAnimationMeshBudget::FAnimationMeshBudget(const FAnimationMeshLimits& L, TSharedPtr<FAnimationCaptureBudget, ESPMode::ThreadSafe> Combined)
	: State(MakeUnique<FState>()), SharedBudget(MoveTemp(Combined)) { State->Limits = L; }
FAnimationMeshBudget::~FAnimationMeshBudget() = default;
const FAnimationMeshLimits& FAnimationMeshBudget::Limits() const { return State->Limits; }
int64 FAnimationMeshBudget::LiveBytes() const { FScopeLock Lock(&State->Mutex); return State->Bytes; }
int64 FAnimationMeshBudget::PeakBytes() const { FScopeLock Lock(&State->Mutex); return State->Peak; }
int32 FAnimationMeshBudget::LiveSnapshots() const { FScopeLock Lock(&State->Mutex); return State->Count; }
bool FAnimationMeshBudget::Acquire(int64 Bytes)
{
	FScopeLock Lock(&State->Mutex);
	if (!LimitsValid(State->Limits) || Bytes <= 0 || State->Count >= State->Limits.MaxSnapshots
		|| Bytes > State->Limits.MaxBytes - State->Bytes) { return false; }
	State->Bytes += Bytes; ++State->Count; State->Peak = FMath::Max(State->Peak, State->Bytes); return true;
}
void FAnimationMeshBudget::Release(int64 Bytes)
{
	FScopeLock Lock(&State->Mutex); State->Bytes -= Bytes; --State->Count;
}
FAnimationMeshSnapshot::FAnimationMeshSnapshot(TSharedRef<FAnimationMeshBudget, ESPMode::ThreadSafe> B, int64 N)
	: Budget(B), Bytes(N) {}
FAnimationMeshSnapshot::~FAnimationMeshSnapshot() { Budget->Release(Bytes); }

struct FAnimationCaptureMeshReference::FState
{
	TWeakObjectPtr<UMeshComponent> Component;
	TWeakObjectPtr<UObject> Asset;
	TWeakObjectPtr<USkeletalMeshComponent> PoseSource;
	TWeakObjectPtr<UObject> PoseAsset;
	TWeakObjectPtr<UAnimInstance> PoseInstance;
	TWeakObjectPtr<UObject> PoseProgram;
	TWeakObjectPtr<USceneComponent> Parent;
	FName Socket;
	TArray<TWeakObjectPtr<UMaterialInterface>> Materials;
	TSharedPtr<FAnimationMeshBudget, ESPMode::ThreadSafe> Budget;
	FAnimationMeshEnrollment Enrollment;
	FDelegateHandle Finalization;
	uint64 Serial = 0, Frame = 0;
	uint32 BoneRevision = 0;
	int16 UpdateCounter = INDEX_NONE;
	EAnimationMode::Type PoseMode = EAnimationMode::AnimationCustomMode;
	double WorldTime = -1;
};
FAnimationCaptureMeshReference::FAnimationCaptureMeshReference() : State(MakeUnique<FState>()) {}
FAnimationCaptureMeshReference::~FAnimationCaptureMeshReference()
{
	check(IsInGameThread());
	if (auto* Pose = State->PoseSource.Get()) { Pose->UnregisterOnBoneTransformsFinalizedDelegate(State->Finalization); }
}
TUniquePtr<FAnimationCaptureMeshReference> FAnimationCaptureMeshReference::Create(UMeshComponent* Component,
	const FAnimationMeshEnrollment& E, TSharedRef<FAnimationMeshBudget, ESPMode::ThreadSafe> Budget, FString& Error)
{
	Error.Reset();
	if (!IsInGameThread()) { Error = TEXT("unavailable: enrollment requires the game thread"); return nullptr; }
	if (!LimitsValid(Budget->Limits()) || !Identity(E.ComponentId) || !Identity(E.AssetId) || !Identity(E.ConfigurationId, 128)
		|| !Identity(E.SubjectId) || !Identity(E.StreamId) || E.ComponentGeneration < 0 || E.ComponentGeneration > MaxJsonInteger
		|| E.ConfigurationGeneration < 0 || E.ConfigurationGeneration > MaxJsonInteger || E.AnalysisLOD < 0
		|| E.MaterialIds.Num() > 64 || (E.PosePolicy != EAnimationMeshPosePolicy::SingleNode
			&& E.PosePolicy != EAnimationMeshPosePolicy::FinalizedAnimation))
	{ Error = TEXT("unavailable: invalid explicit enrollment or limits"); return nullptr; }
	for (const auto& Material : E.MaterialIds) if (Material.Len() > 256)
	{ Error = TEXT("unavailable: material identity exceeds 256 characters"); return nullptr; }
	if (!IsValid(Component) || !Component->IsRegistered() || !Component->GetWorld()
		|| (Component->GetClass() != USkeletalMeshComponent::StaticClass() && Component->GetClass() != UStaticMeshComponent::StaticClass()))
	{ Error = TEXT("unavailable: enroll a registered ordinary skeletal or static mesh component"); return nullptr; }
	auto* Skeletal = Cast<USkeletalMeshComponent>(Component);
	auto* Static = Cast<UStaticMeshComponent>(Component);
	UObject* Asset = Skeletal ? static_cast<UObject*>(Skeletal->GetSkeletalMeshAsset()) : static_cast<UObject*>(Static->GetStaticMesh());
	if (!Asset || Component->GetNumMaterials() != E.MaterialIds.Num())
	{ Error = TEXT("unavailable: asset and explicit material-slot identities are required"); return nullptr; }
	USkeletalMeshComponent* Pose = Skeletal ? Skeletal : Cast<USkeletalMeshComponent>(Component->GetAttachParent());
	FPoseQualification Qualification;
	if (Pose && !QualifiedPose(Pose, E.PosePolicy, Qualification, Error, false)) { return nullptr; }
	if (!Pose && E.PosePolicy != EAnimationMeshPosePolicy::SingleNode)
	{ Error = TEXT("unavailable: finalized-animation policy requires a skeletal pose source"); return nullptr; }
	// Only a direct ordinary skeletal attachment is qualified. Arbitrary parent chains
	// may have later transform producers and need their own ordering evidence.
	if (Static && Component->GetAttachParent() && !Pose)
	{ Error = TEXT("unavailable: rigid attachment parent ordering is unqualified"); return nullptr; }
	auto Result = TUniquePtr<FAnimationCaptureMeshReference>(new FAnimationCaptureMeshReference());
	auto& S = *Result->State;
	S.Component = Component; S.Asset = Asset; S.PoseSource = Pose; S.Parent = Component->GetAttachParent();
	S.PoseAsset = Pose ? Pose->GetSkeletalMeshAsset() : nullptr;
	S.PoseInstance = Qualification.Instance; S.PoseProgram = Qualification.Program; S.PoseMode = Qualification.Mode;
	S.Socket = Component->GetAttachSocketName(); S.Enrollment = E; S.Budget = Budget;
	for (int32 I = 0; I < E.MaterialIds.Num(); ++I) { S.Materials.Add(Component->GetMaterial(I)); }
	if (Pose)
	{
		S.Finalization = Pose->RegisterOnBoneTransformsFinalizedDelegate(FOnBoneTransformsFinalizedMultiCast::FDelegate::CreateLambda([State = &S]
		{
			USkeletalMeshComponent* Source = State->PoseSource.Get();
			FPoseQualification Witness;
			FString Ignored;
			if (!Source || !QualifiedPose(Source, State->Enrollment.PosePolicy, Witness, Ignored, true, true)
				|| Witness.Instance != State->PoseInstance.Get() || Witness.Program != State->PoseProgram.Get()
				|| Witness.Mode != State->PoseMode)
			{
				return;
			}
			++State->Serial; State->Frame = GFrameCounter;
			State->BoneRevision = Witness.BoneRevision; State->UpdateCounter = Witness.UpdateCounter;
			const auto* C = State->Component.Get(); State->WorldTime = C && C->GetWorld() ? C->GetWorld()->GetTimeSeconds() : -1;
		}));
	}
	return Result;
}

TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe> FAnimationCaptureMeshReference::Capture(const FString& RequestId, FString& Error)
{
	return Prepare(RequestId, Error, true);
}

bool FAnimationCaptureMeshReference::CaptureRigidBatch(
	TConstArrayView<FAnimationCaptureMeshReference*> Samplers,
	const FString& RequestId, int32 MaxComponents,
	TArray<TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe>>& OutSnapshots,
	FString& Error)
{
	Error.Reset();
	OutSnapshots.Reset();
	auto Fail = [&Error](const TCHAR* Reason)
	{
		Error = Reason;
		return false;
	};
	if (!IsInGameThread())
	{
		return Fail(TEXT("unavailable: rigid batch capture requires the game thread"));
	}
	if (!Identity(RequestId) || MaxComponents < 1 || MaxComponents > 64
		|| Samplers.IsEmpty() || Samplers.Num() > MaxComponents)
	{
		return Fail(TEXT("unavailable: invalid rigid batch request or component bound"));
	}

	TSet<const FAnimationCaptureMeshReference*> UniqueSamplers;
	TSet<UMeshComponent*> UniqueComponents;
	UWorld* World = nullptr;
	for (const FAnimationCaptureMeshReference* Sampler : Samplers)
	{
		if (!Sampler || UniqueSamplers.Contains(Sampler))
		{
			return Fail(TEXT("unavailable: rigid batch observers must be non-null and unique"));
		}
		UniqueSamplers.Add(Sampler);
		UMeshComponent* Component = Sampler->State->Component.Get();
		if (!Component || !Component->IsRegistered() || !Component->GetWorld()
			|| Component->GetWorld()->bInTick || Component->GetClass() != UStaticMeshComponent::StaticClass()
			|| Component->GetAttachParent() || Component->IsSimulatingPhysics())
		{
			return Fail(TEXT("unavailable: rigid batch requires registered ordinary unparented static components outside world tick without physics"));
		}
		if (UniqueComponents.Contains(Component))
		{
			return Fail(TEXT("unavailable: rigid batch components must be unique"));
		}
		UniqueComponents.Add(Component);
		if (!World)
		{
			World = Component->GetWorld();
		}
		else if (Component->GetWorld() != World)
		{
			return Fail(TEXT("unavailable: rigid batch components must share one world"));
		}
	}

	const double AcquiredSeconds = FPlatformTime::Seconds();
	const uint64 AcquisitionFrame = GFrameCounter;
	if (!FMath::IsFinite(AcquiredSeconds) || AcquisitionFrame > MaxJsonInteger)
	{
		return Fail(TEXT("unavailable: rigid batch acquisition identity is invalid"));
	}
	TArray<TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe>> Prepared;
	Prepared.Reserve(Samplers.Num());
	for (FAnimationCaptureMeshReference* Sampler : Samplers)
	{
		auto Snapshot = Sampler->Prepare(RequestId, Error, true, AcquiredSeconds);
		if (!Snapshot)
		{
			return false;
		}
		Prepared.Add(MoveTemp(Snapshot));
	}
	if (GFrameCounter != AcquisitionFrame || !World || World->bInTick)
	{
		return Fail(TEXT("unavailable: rigid batch world or frame changed before publication"));
	}
	for (int32 Index = 0; Index < Samplers.Num(); ++Index)
	{
		UMeshComponent* Component = Samplers[Index]->State->Component.Get();
		const auto& Data = Prepared[Index]->Data();
		if (!Component || Component->GetWorld() != World || !Component->IsRegistered()
			|| Component->GetAttachParent() || Component->IsSimulatingPhysics()
			|| Data.FrameId != int64(AcquisitionFrame) || Data.AcquiredSeconds != AcquiredSeconds)
		{
			return Fail(TEXT("unavailable: rigid batch validity changed before publication"));
		}
	}
	OutSnapshots = MoveTemp(Prepared);
	return true;
}

bool FAnimationCaptureMeshReference::CaptureBatch(
	TConstArrayView<FAnimationCaptureMeshReference*> Samplers,
	const FString& RequestId, int32 MaxComponents,
	TArray<TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe>>& OutSnapshots,
	FString& Error)
{
	Error.Reset();
	OutSnapshots.Reset();
	auto Fail = [&Error](const TCHAR* Reason)
	{
		Error = Reason;
		return false;
	};
	if (!IsInGameThread())
	{
		return Fail(TEXT("unavailable: component batch capture requires the game thread"));
	}
	if (!Identity(RequestId) || MaxComponents < 1 || MaxComponents > 64
		|| Samplers.IsEmpty() || Samplers.Num() > MaxComponents)
	{
		return Fail(TEXT("unavailable: invalid component batch request or component bound"));
	}

	TSet<const FAnimationCaptureMeshReference*> UniqueSamplers;
	TSet<UMeshComponent*> UniqueComponents;
	UWorld* World = nullptr;
	for (const FAnimationCaptureMeshReference* Sampler : Samplers)
	{
		if (!Sampler || UniqueSamplers.Contains(Sampler))
		{
			return Fail(TEXT("unavailable: component batch observers must be non-null and unique"));
		}
		UniqueSamplers.Add(Sampler);
		UMeshComponent* Component = Sampler->State->Component.Get();
		if (!Component || !Component->IsRegistered() || !Component->GetWorld() || Component->GetWorld()->bInTick
			|| (Component->GetClass() != USkeletalMeshComponent::StaticClass()
				&& Component->GetClass() != UStaticMeshComponent::StaticClass())
			|| Component->IsSimulatingPhysics())
		{
			return Fail(TEXT("unavailable: component batch requires registered supported components outside world tick without physics"));
		}
		if (UniqueComponents.Contains(Component))
		{
			return Fail(TEXT("unavailable: component batch components must be unique"));
		}
		UniqueComponents.Add(Component);
		if (!World) { World = Component->GetWorld(); }
		else if (Component->GetWorld() != World)
		{
			return Fail(TEXT("unavailable: component batch components must share one world"));
		}
	}

	const double AcquisitionWorldTime = World->GetTimeSeconds();
	const double AcquiredSeconds = FPlatformTime::Seconds();
	const uint64 AcquisitionFrame = GFrameCounter;
	if (!FMath::IsFinite(AcquiredSeconds) || AcquisitionFrame > MaxJsonInteger)
	{
		return Fail(TEXT("unavailable: component batch acquisition identity is invalid"));
	}
	TArray<TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe>> Prepared;
	Prepared.Reserve(Samplers.Num());
	for (FAnimationCaptureMeshReference* Sampler : Samplers)
	{
		auto Snapshot = Sampler->Prepare(RequestId, Error, true, AcquiredSeconds);
		if (!Snapshot) { return false; }
		Prepared.Add(MoveTemp(Snapshot));
	}
	if (GFrameCounter != AcquisitionFrame || !World || World->bInTick
		|| World->GetTimeSeconds() != AcquisitionWorldTime)
	{
		return Fail(TEXT("unavailable: component batch world or frame changed before publication"));
	}
	for (int32 Index = 0; Index < Samplers.Num(); ++Index)
	{
		const FState& State = *Samplers[Index]->State;
		UMeshComponent* Component = State.Component.Get();
		const FAnimationMeshData& Data = Prepared[Index]->Data();
		if (!Component || Component->GetWorld() != World || !Component->IsRegistered()
			|| Component->IsSimulatingPhysics() || Component->GetAttachParent() != State.Parent.Get()
			|| Component->GetAttachSocketName() != State.Socket || Data.FrameId != int64(AcquisitionFrame)
			|| Data.AcquiredSeconds != AcquiredSeconds
			|| !Data.ComponentToWorld.Equals(Component->GetComponentTransform().ToMatrixWithScale(), 0))
		{
			return Fail(TEXT("unavailable: component batch participant changed before publication"));
		}
		USkeletalMeshComponent* Pose = Cast<USkeletalMeshComponent>(Component);
		if (!Pose) { Pose = Cast<USkeletalMeshComponent>(Component->GetAttachParent()); }
		if (Pose)
		{
			FPoseQualification Qualification;
			if (!QualifiedPose(Pose, State.Enrollment.PosePolicy, Qualification, Error, true)
				|| Pose != State.PoseSource.Get() || Pose->GetSkeletalMeshAsset() != State.PoseAsset.Get()
				|| Qualification.Instance != State.PoseInstance.Get() || Qualification.Program != State.PoseProgram.Get()
				|| Qualification.Mode != State.PoseMode || State.Serial != uint64(Data.PoseRevision)
				|| State.Frame != AcquisitionFrame || State.WorldTime != World->GetTimeSeconds()
				|| State.BoneRevision != Qualification.BoneRevision
				|| State.UpdateCounter != Qualification.UpdateCounter)
			{
				if (Error.IsEmpty()) { Error = TEXT("unavailable: component batch finalized-pose witness changed before publication"); }
				return false;
			}
		}
	}
	OutSnapshots = MoveTemp(Prepared);
	return true;
}

TSharedPtr<FAnimationMeshSnapshot, ESPMode::ThreadSafe> FAnimationCaptureMeshReference::Prepare(
	const FString& RequestId, FString& Error, bool bComputePositions, TOptional<double> NativeAcquiredSeconds)
{
	Error.Reset();
	auto Fail = [&Error](const TCHAR* Reason) -> TSharedPtr<FAnimationMeshSnapshot, ESPMode::ThreadSafe>
	{ Error = Reason; return nullptr; };
	if (!IsInGameThread()) { return Fail(TEXT("unavailable: capture requires the game thread")); }
	auto& S = *State; auto* Component = S.Component.Get(); const auto& E = S.Enrollment;
	if (!Identity(RequestId) || GFrameCounter > MaxJsonInteger || S.Serial >= MaxJsonInteger)
	{ return Fail(TEXT("unavailable: invalid request or exhausted identity counter")); }
	if (!Component || !Component->IsRegistered() || !Component->GetWorld() || Component->GetWorld()->bInTick)
	{ return Fail(TEXT("unavailable: component retired, unregistered or world tick still in progress")); }
	if (Component->GetAttachParent() != S.Parent.Get() || Component->GetAttachSocketName() != S.Socket
		|| Component->GetNumMaterials() != S.Materials.Num())
	{ return Fail(TEXT("unavailable: attachment or material mapping changed; enroll a new generation")); }
	for (int32 I = 0; I < S.Materials.Num(); ++I) if (Component->GetMaterial(I) != S.Materials[I].Get())
	{ return Fail(TEXT("unavailable: effective material changed; enroll a new generation")); }
	auto* Skeletal = Cast<USkeletalMeshComponent>(Component);
	auto* Static = Cast<UStaticMeshComponent>(Component);
	USkeletalMeshComponent* Pose = Skeletal ? Skeletal : Cast<USkeletalMeshComponent>(Component->GetAttachParent());
	if (Pose)
	{
		FPoseQualification Qualification;
		if (!QualifiedPose(Pose, E.PosePolicy, Qualification, Error, true)) { return nullptr; }
		if (Pose != S.PoseSource.Get() || Pose->GetSkeletalMeshAsset() != S.PoseAsset.Get() || !S.Serial
			|| Qualification.Instance != S.PoseInstance.Get() || Qualification.Program != S.PoseProgram.Get()
			|| Qualification.Mode != S.PoseMode || S.Frame != GFrameCounter
			|| S.WorldTime != Component->GetWorld()->GetTimeSeconds()
			|| S.BoneRevision != Qualification.BoneRevision || S.UpdateCounter != Qualification.UpdateCounter)
		{ return Fail(TEXT("unavailable: no current finalized pose witness")); }
		if (Static && (Component->IsUsingAbsoluteLocation() || Component->IsUsingAbsoluteRotation() || Component->IsUsingAbsoluteScale()
			|| !(Component->GetRelativeTransform() * Pose->GetSocketTransform(S.Socket)).Equals(Component->GetComponentTransform(), 1.e-5)))
		{ return Fail(TEXT("unavailable: rigid attachment transform is not synchronized with witnessed parent pose")); }
		if (Static && !S.Socket.IsNone())
		{
			const int32 Bone = Pose->GetBoneIndex(Pose->GetSocketBoneName(S.Socket));
			if (!Pose->bRequiredBonesUpToDate || Bone == INDEX_NONE || !Pose->RequiredBones.Contains(Bone))
			{ return Fail(TEXT("unavailable: rigid attachment socket bone lacks current evaluated-pose coverage")); }
		}
	}
	else if (S.Finalization.IsValid()) { return Fail(TEXT("unavailable: enrolled pose source retired")); }
	const FSkeletalMeshLODRenderData* SkelLOD = nullptr;
	const FStaticMeshLODResources* RigidLOD = nullptr;
	const FSkinWeightVertexBuffer* Weights = nullptr;
	const FPositionVertexBuffer* Positions = nullptr;
	int64 Vertices = 0, Indices = 0, Sections = 0, Bones = 0;
	if (Skeletal)
	{
		auto* Asset = Skeletal->GetSkeletalMeshAsset();
		if (!Asset || Asset != S.Asset.Get() || Asset->IsCompiling() || !Asset->IsFullyStreamedIn())
		{ return Fail(TEXT("unavailable: skeletal asset replaced, compiling or streaming")); }
		const auto* Render = Asset->GetResourceForRendering();
		if (!Render || !Render->LODRenderData.IsValidIndex(E.AnalysisLOD)) { return Fail(TEXT("unavailable: analysis LOD absent")); }
		SkelLOD = &Render->LODRenderData[E.AnalysisLOD];
		Positions = &SkelLOD->StaticVertexBuffers.PositionVertexBuffer;
		const auto* Index = SkelLOD->MultiSizeIndexContainer.GetIndexBuffer();
		if (!Index || Index->GetResourceDataSize() <= 0) { return Fail(TEXT("unavailable: CPU indices not resident")); }
		Vertices = Positions->GetNumVertices(); Indices = Index->Num(); Sections = SkelLOD->RenderSections.Num();
		Bones = Asset->GetRefSkeleton().GetNum(); Weights = Skeletal->GetSkinWeightBuffer(E.AnalysisLOD);
		if (!Skeletal->bRequiredBonesUpToDate || E.AnalysisLOD < Skeletal->GetPredictedLODLevel())
		{ return Fail(TEXT("unavailable: selected analysis LOD lacks current evaluated-bone coverage")); }
		if (!Weights || Weights->GetNumVertices() != Vertices || !Weights->GetDataVertexBuffer()->GetWeightData()
			|| Weights->GetVariableBonesPerVertex() || Weights->GetMaxBoneInfluences() > 12)
		{ return Fail(TEXT("unavailable: resident fixed-influence effective weights required (maximum 12 influences)")); }
	}
	else
	{
		UStaticMesh* Asset = Static->GetStaticMesh();
		if (!Asset || Asset != S.Asset.Get() || Asset->IsCompiling() || !Asset->IsFullyStreamedIn() || Asset->HasValidNaniteData())
		{ return Fail(TEXT("unavailable: rigid asset replaced, compiling, streaming or Nanite")); }
		const auto* Render = Asset->GetRenderData();
		if (!Render || !Render->LODResources.IsValidIndex(E.AnalysisLOD)) { return Fail(TEXT("unavailable: analysis LOD absent")); }
		RigidLOD = &Render->LODResources[E.AnalysisLOD]; Positions = &RigidLOD->VertexBuffers.PositionVertexBuffer;
		if (RigidLOD->IndexBuffer.GetIndexDataSize() <= 0) { return Fail(TEXT("unavailable: CPU indices not resident")); }
		Vertices = Positions->GetNumVertices(); Indices = RigidLOD->IndexBuffer.GetNumIndices(); Sections = RigidLOD->Sections.Num();
	}
	const auto& L = S.Budget->Limits();
	if (!Positions->GetVertexData() || Vertices <= 0 || Indices <= 0 || Sections <= 0 || Vertices > L.MaxVertices
		|| Indices > L.MaxIndices || Sections > L.MaxSections || Bones > L.MaxBones)
	{ return Fail(TEXT("unavailable: nonresident geometry or geometry count limit exceeded")); }
	// Reserve before variable-size producer allocations. Keep allowance for simultaneous
	// output, binary serialization/hash work, CPU skin scratch, matrices and bounded JSON.
	const int64 Bytes = 3 * (Vertices * 24 + Indices * 4) + Vertices * 12 + Bones * 128 + 1024 * 1024;
	if (!S.Budget->Acquire(Bytes)) { return Fail(TEXT("unavailable: shared CPU snapshot count or byte admission exhausted")); }
	TSharedPtr<FAnimationMeshSnapshot, ESPMode::ThreadSafe> Snapshot(new FAnimationMeshSnapshot(S.Budget.ToSharedRef(), Bytes));
	if (S.Budget->SharedBudget)
	{
		Snapshot->SharedReservation = S.Budget->SharedBudget->Reserve(Bytes);
		if (!Snapshot->SharedReservation) { return Fail(TEXT("unavailable: combined capture byte or reservation admission exhausted")); }
	}
	auto& D = Snapshot->Value;
	D.Enrollment = E; D.RequestId = RequestId; D.ProducerId = Skeletal ? TEXT("unreal-cpu-bone-reference-v1") : TEXT("unreal-rigid-reference-v1");
	D.AcquiredSeconds = NativeAcquiredSeconds.IsSet() ? NativeAcquiredSeconds.GetValue() : FPlatformTime::Seconds(); D.FrameId = GFrameCounter;
	D.PoseRevision = Pose ? S.Serial : ++S.Serial;
	D.ComponentToWorld = Component->GetComponentTransform().ToMatrixWithScale();
	for (int32 Row = 0; Row < 4; ++Row) for (int32 Col = 0; Col < 4; ++Col)
		if (!FMath::IsFinite(D.ComponentToWorld.M[Row][Col])) { return Fail(TEXT("unavailable: nonfinite acquisition transform")); }
	FMD5 ConfigHash; HashString(ConfigHash, E.ConfigurationId);
	HashString(ConfigHash, FString::FromInt(E.AnalysisLOD));
	// Preserve all existing default hashes; only the explicit opt-in contributes a
	// new configuration discriminator.
	if (E.PosePolicy == EAnimationMeshPosePolicy::FinalizedAnimation)
	{
		HashString(ConfigHash, TEXT("pose-policy:finalized-animation"));
	}
	D.Sections.Reserve(Sections); uint32 NextIndex = 0, NextVertex = 0;
	for (int32 I = 0; I < Sections; ++I)
	{
		const uint32 First = SkelLOD ? SkelLOD->RenderSections[I].BaseIndex : RigidLOD->Sections[I].FirstIndex;
		const uint32 Triangles = SkelLOD ? SkelLOD->RenderSections[I].NumTriangles : RigidLOD->Sections[I].NumTriangles;
		const int32 Material = SkelLOD ? SkelLOD->RenderSections[I].MaterialIndex : RigidLOD->Sections[I].MaterialIndex;
		if (!Triangles || First != NextIndex || int64(Triangles) * 3 > Indices - NextIndex || !E.MaterialIds.IsValidIndex(Material))
		{ return Fail(TEXT("unavailable: incomplete section/index/material mapping")); }
		D.Sections.Add({FString::FromInt(I), First, Triangles * 3, E.MaterialIds[Material]}); NextIndex += Triangles * 3;
		if (SkelLOD)
		{
			const auto& Section = SkelLOD->RenderSections[I];
			if (Section.BaseVertexIndex != NextVertex || Section.NumVertices > Vertices - NextVertex || Section.BoneMap.Num() > Bones)
			{ return Fail(TEXT("unavailable: incomplete section vertex or bone mapping")); }
			for (auto Bone : Section.BoneMap) if (Bone >= Bones) { return Fail(TEXT("unavailable: invalid section bone mapping")); }
			ConfigHash.Update(reinterpret_cast<const uint8*>(Section.BoneMap.GetData()), Section.BoneMap.Num() * sizeof(FBoneIndexType));
			for (uint32 V = NextVertex; V < NextVertex + Section.NumVertices; ++V)
				for (uint32 J = 0; J < Weights->GetMaxBoneInfluences(); ++J)
					if (!Section.BoneMap.IsValidIndex(Weights->GetBoneIndex(V, J)))
					{ return Fail(TEXT("unavailable: effective weights reference an invalid bone")); }
					else if (Weights->GetBoneWeight(V, J) && !Skeletal->RequiredBones.Contains(Section.BoneMap[Weights->GetBoneIndex(V, J)]))
					{ return Fail(TEXT("unavailable: a weighted analysis-LOD bone was not evaluated in the witnessed pose")); }
			NextVertex += Section.NumVertices;
		}
	}
	if (NextIndex != Indices || (SkelLOD && NextVertex != Vertices)) { return Fail(TEXT("unavailable: incomplete geometry ranges")); }
	D.Indices.SetNumUninitialized(Indices);
	for (int32 I = 0; I < Indices; ++I)
	{
		D.Indices[I] = SkelLOD ? SkelLOD->MultiSizeIndexContainer.GetIndexBuffer()->Get(I) : RigidLOD->IndexBuffer.GetIndex(I);
		if (D.Indices[I] >= Vertices) { return Fail(TEXT("unavailable: vertex index outside geometry")); }
	}
	D.Positions.SetNumUninitialized(Vertices);
	if (SkelLOD)
	{
		if (bComputePositions)
		{
		TArray<FMatrix44f> Matrices; Skeletal->GetCurrentRefToLocalMatrices(Matrices, E.AnalysisLOD);
		if (Matrices.Num() != Bones) { return Fail(TEXT("unavailable: reference-to-local mapping incomplete")); }
		TArray<FVector3f> Skinned;
		USkinnedMeshComponent::ComputeSkinnedPositions(Skeletal, Skinned, Matrices, *SkelLOD, *Weights);
		if (Skinned.Num() != Vertices) { return Fail(TEXT("unavailable: CPU skinning returned incomplete geometry")); }
		for (int32 I = 0; I < Vertices; ++I) { D.Positions[I] = FVector3d(Skinned[I]); }
		}
		ConfigHash.Update(Weights->GetDataVertexBuffer()->GetWeightData(), Weights->GetDataVertexBuffer()->GetVertexDataSize());
		HashString(ConfigHash, FString::Printf(TEXT("%u:%u:%u"), Weights->GetMaxBoneInfluences(), Weights->GetBoneIndexByteSize(), Weights->GetBoneWeightByteSize()));
		const auto& InverseBind = Skeletal->GetSkeletalMeshAsset()->GetRefBasesInvMatrix();
		ConfigHash.Update(reinterpret_cast<const uint8*>(InverseBind.GetData()), InverseBind.Num() * sizeof(FMatrix44f));
		const auto& Visibility = Skeletal->GetBoneVisibilityStates(); ConfigHash.Update(Visibility.GetData(), Visibility.Num());
	}
	else for (int32 I = 0; I < Vertices; ++I) { D.Positions[I] = FVector3d(Positions->VertexPosition(I)); }
	if (bComputePositions) for (const auto& P : D.Positions) if (!FMath::IsFinite(P.X) || !FMath::IsFinite(P.Y) || !FMath::IsFinite(P.Z))
	{ return Fail(TEXT("unavailable: nonfinite sampled geometry")); }
	ConfigHash.Update(reinterpret_cast<const uint8*>(Positions->GetVertexData()), Vertices * sizeof(FVector3f));
	D.TopologyId = AnimationCaptureMeshReplay::TopologyIdentity(D);
	if (D.TopologyId.IsEmpty()) { return Fail(TEXT("unavailable: topology identity could not be encoded")); }
	HashString(ConfigHash, D.TopologyId);
	uint8 Digest[16]; ConfigHash.Final(Digest); D.ConfigurationId = E.ConfigurationId + TEXT(":") + BytesToHex(Digest, 16).ToLower();
	auto Feature = [&](const TCHAR* Name, const TCHAR* StateName, const FString& Reason)
	{ D.Coverage.Add({Name, StateName, D.ProducerId, RequestId, Reason}); };
	Feature(Skeletal ? TEXT("bone") : TEXT("rigid"), TEXT("observed"), TEXT("Complete selected analysis LOD reference; not a render-LOD witness"));
	const TCHAR* PoseReason = E.PosePolicy == EAnimationMeshPosePolicy::FinalizedAnimation
		? TEXT("Current finalized animation instance, update counter and bone revision; sampled outside world tick")
		: TEXT("Current single-node finalization; sampled outside world tick");
	Feature(TEXT("pose_ordering"), TEXT("observed"), Pose ? PoseReason : TEXT("Rigid transform copied on game thread outside world tick"));
	const int32 MorphCount = Skeletal ? Skeletal->MorphTargetWeights.Num() : 0;
	int32 ActiveMorphs = 0; if (Skeletal) for (float W : Skeletal->MorphTargetWeights) if (W != 0) { ++ActiveMorphs; }
	Feature(TEXT("morph"), TEXT("excluded"), FString::Printf(TEXT("Reference excludes morphs; effective weight entries=%d, nonzero=%d; not final morph coverage"), MorphCount, ActiveMorphs));
	int32 ClothSections = 0; if (SkelLOD) for (const auto& Section : SkelLOD->RenderSections) if (Section.HasClothingData()) { ++ClothSections; }
	Feature(TEXT("cloth"), TEXT("excluded"), FString::Printf(TEXT("Reference excludes cloth; mapped sections=%d; simulation contribution unknown"), ClothSections));
	Feature(TEXT("mesh_deformer"), TEXT("excluded"), FString::Printf(TEXT("Reference excludes deformer output; effective LOD instance=%s; execution contribution unknown"), Skeletal && Skeletal->GetMeshDeformerInstanceForLOD(E.AnalysisLOD) ? TEXT("present") : TEXT("absent")));
	int32 Missing = 0, WPO = 0, PDO = 0, Masked = 0, Translucent = 0;
	for (int32 I = 0; I < Sections; ++I)
	{
		const int32 Slot = SkelLOD ? SkelLOD->RenderSections[I].MaterialIndex : RigidLOD->Sections[I].MaterialIndex;
		const auto* Material = Component->GetMaterial(Slot);
		if (!Material) { ++Missing; continue; }
		const auto R = Material->GetRelevance_Concurrent(Component->GetWorld()->GetFeatureLevel());
		WPO += R.bUsesWorldPositionOffset; PDO += R.bUsesPixelDepthOffset; Masked += R.bMasked;
		Translucent += R.bNormalTranslucency || R.bSeparateTranslucency || R.bPostMotionBlurTranslucency;
	}
	Feature(TEXT("material_displacement"), TEXT("excluded"), FString::Printf(TEXT("Reference excludes material displacement; section relevance WPO=%d, missing material=%d; runtime displacement unknown"), WPO, Missing));
	Feature(TEXT("raster_visibility"), TEXT("excluded"), FString::Printf(TEXT("Reference excludes raster evidence; section relevance PDO=%d, masked=%d, translucent=%d, missing=%d; visibility unknown"), PDO, Masked, Translucent, Missing));
	D.CompletedSeconds = FPlatformTime::Seconds(); return Snapshot;
}
