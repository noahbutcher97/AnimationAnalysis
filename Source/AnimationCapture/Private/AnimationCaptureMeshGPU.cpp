#include "AnimationCapture/AnimationCaptureMeshGPU.h"
#include "CachedGeometry.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GPUSkinCache.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include "RenderGraphBuilder.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "RHIGPUReadback.h"
#include "SceneInterface.h"
#include "SceneViewExtension.h"
#include "SkeletalRenderPublic.h"
#include "UnrealClient.h"

namespace
{
constexpr int64 RequestBytes = 64 * 1024;
class FMeshGPUFamilyData : public ISceneViewFamilyExtentionData
{
public:
	static const TCHAR* GSubclassIdentifier;
	const TCHAR* GetSubclassIdentifier() const override { return GSubclassIdentifier; }
	uint64 Token = 0;
};
const TCHAR* FMeshGPUFamilyData::GSubclassIdentifier = TEXT("AnimationCaptureMeshFamily");
uint64 NextFamilyToken = 0; // Only assigned on the game thread, then copied with the view family.
struct FMeshGPUSlot
{
	uint64 Serial = 0;
	uint64 FamilyToken = 0;
	FString Id;
	FAnimationMeshGPUResult Result;
	TSharedPtr<FAnimationCaptureReservation, ESPMode::ThreadSafe> Envelope, SourceLease;
	TSharedPtr<FAnimationMeshSnapshot, ESPMode::ThreadSafe> Work;
	TArray<FMatrix44f> Matrices;
	const FSkeletalMeshObject* MeshObject = nullptr; // Only consumed in the exact queued view, never in a later poll.
	FBufferRHIRef Source;
	TUniquePtr<FRHIGPUBufferReadback> Readback;
	int64 SourceBytes = 0, CopyBytes = 0;
	bool bPrepared = false, bScheduled = false, bEnqueued = false, bTerminal = false, bCollected = false;
};
}

struct FAnimationMeshGPUState : TSharedFromThis<FAnimationMeshGPUState, ESPMode::ThreadSafe>
{
	FCriticalSection Mutex;
	FAnimationMeshGPULimits Limits;
	FAnimationMeshGPUStats Stats;
	TSharedPtr<FAnimationCaptureBudget, ESPMode::ThreadSafe> Budget;
	TArray<TSharedPtr<FMeshGPUSlot, ESPMode::ThreadSafe>> Slots;
	TWeakObjectPtr<USkeletalMeshComponent> Component;
	FAnimationCaptureMeshReference* Reference = nullptr; // Owned by FImpl; read only on GT and cleared before destruction.
	uint64 NextSerial = 0;
	bool bClosed = false, bPollQueued = false;

	void Finish(FMeshGPUSlot& S, EAnimationMeshGPUStatus Status, const FString& Error)
	{
		if (S.bTerminal) { return; }
		S.bTerminal = true; S.Result.Status = Status; S.Result.Error = Error.Left(1024);
		S.Result.CompletedSeconds = FPlatformTime::Seconds();
		switch (Status)
		{
		case EAnimationMeshGPUStatus::Completed: ++Stats.Completed; break;
		case EAnimationMeshGPUStatus::Unavailable: ++Stats.Unavailable; break;
		case EAnimationMeshGPUStatus::Cancelled: ++Stats.Cancelled; break;
		case EAnimationMeshGPUStatus::TimedOut: ++Stats.TimedOut; break;
		default: ++Stats.Failed; break;
		}
		if (Status == EAnimationMeshGPUStatus::Completed)
		{
			S.Work->Value.CompletedSeconds = S.Result.CompletedSeconds; S.Result.Snapshot = S.Work;
		}
		if (!S.bScheduled) { S.Work.Reset(); S.Matrices.Reset(); S.MeshObject = nullptr; }
	}
	void RemoveRetired()
	{
		Slots.RemoveAll([](const auto& S) { return S->bCollected && !S->bScheduled; });
		Stats.PendingRequests = Slots.Num();
	}
	void PrepareGT(FSceneViewFamily& Family)
	{
		check(IsInGameThread());
		FScopeLock Lock(&Mutex);
		if (bClosed || !Reference) { return; }
		for (const auto& S : Slots)
		{
			if (S->bTerminal || S->bPrepared) { continue; }
			const double Before = FPlatformTime::Seconds(); S->bPrepared = true;
			auto* FamilyData = Family.GetOrCreateExtentionData<FMeshGPUFamilyData>();
			if (!FamilyData->Token) { FamilyData->Token = ++NextFamilyToken; }
			S->FamilyToken = FamilyData->Token;
			USkeletalMeshComponent* Mesh = Component.Get(); FString Error;
			if (!Mesh || !Mesh->IsRegistered()) { Finish(*S, EAnimationMeshGPUStatus::Unavailable, TEXT("Skeletal component retired")); continue; }
			const int32 LOD = S->Result.Enrollment.AnalysisLOD;
			bool ActiveMorph = false;
			for (float Weight : Mesh->MorphTargetWeights) { ActiveMorph |= Weight != 0; }
			const bool ExternalMorph = Mesh->IsValidExternalMorphSetLODIndex(LOD) && !Mesh->GetExternalMorphSets(LOD).IsEmpty();
			if (ActiveMorph || ExternalMorph || Mesh->GetMeshDeformerInstanceForLOD(LOD) || Mesh->IsSkinWeightProfilePending() || Mesh->GetCPUSkinningEnabled())
			{ Finish(*S, EAnimationMeshGPUStatus::Unavailable, TEXT("Cached bone subset excludes active/external morphs, deformers, pending weights and CPU rendering")); continue; }
			S->Work = Reference->Prepare(S->Id, Error, false);
			if (!S->Work) { Finish(*S, EAnimationMeshGPUStatus::Unavailable, Error); continue; }
			const auto& RenderLOD = Mesh->GetSkeletalMeshAsset()->GetResourceForRendering()->LODRenderData[LOD];
			bool Cloth = false; for (const auto& Section : RenderLOD.RenderSections) { Cloth |= Section.HasClothingData(); }
			if (Cloth) { Finish(*S, EAnimationMeshGPUStatus::Unavailable, TEXT("Cached bone subset excludes cloth-mapped sections")); continue; }
			auto& D = S->Work->Value;
			D.ProducerId = TEXT("unreal-skin-cache-bone-v1"); D.CompletedSeconds = 0;
			for (auto& Feature : D.Coverage)
			{
				Feature.ProducerId = D.ProducerId;
				if (Feature.Feature == TEXT("bone")) { Feature.Reason = TEXT("Renderer Skin Cache positions, matched pose and selected render LOD, before material effects"); }
				if (Feature.Feature == TEXT("pose_ordering"))
				{
					Feature.Reason = D.Enrollment.PosePolicy == EAnimationMeshPosePolicy::FinalizedAnimation
						? TEXT("Current finalized animation instance/update/bone witnesses and matching renderer reference-to-local matrices in the acquired view")
						: TEXT("Current single-node finalization and matching renderer reference-to-local matrices in the acquired view");
				}
			}
			S->MeshObject = Mesh->GetMeshObject();
			Mesh->GetCurrentRefToLocalMatrices(S->Matrices, LOD); // O(bones); never computes CPU-skinned vertices.
			S->Result.bHasAcquisition = true; S->Result.AcquisitionFrame = D.FrameId; S->Result.PoseRevision = D.PoseRevision;
			S->Result.AcquiredSeconds = D.AcquiredSeconds; S->Result.ConfigurationId = D.ConfigurationId;
			S->Result.PrepareSeconds = FPlatformTime::Seconds() - Before; Stats.PrepareSeconds += S->Result.PrepareSeconds;
		}
	}
	void CaptureRT(FRDGBuilder& Graph, FSceneView& View)
	{
		check(IsInRenderingThread());
		FScopeLock Lock(&Mutex); if (bClosed) { return; }
		const auto* FamilyData = View.Family->GetExtentionData<FMeshGPUFamilyData>();
		if (!FamilyData) { return; }
		TArray<TSharedPtr<FMeshGPUSlot, ESPMode::ThreadSafe>> Copies;
		for (const auto& S : Slots)
		{
			if (S->bTerminal || !S->bPrepared || S->bScheduled || S->FamilyToken != FamilyData->Token) { continue; }
			const double CaptureStart = FPlatformTime::Seconds();
			ON_SCOPE_EXIT
			{
				S->Result.CaptureSeconds = FPlatformTime::Seconds() - CaptureStart; Stats.CaptureSeconds += S->Result.CaptureSeconds;
			};
			auto Unavailable = [&](const FString& Why) { Finish(*S, EAnimationMeshGPUStatus::Unavailable, Why); };
			// Before any mesh-object dereference: objects belong only to their enqueued frame.
			if (S->Result.AcquisitionFrame != View.Family->FrameCounter)
			{ Unavailable(TEXT("Acquired pose and renderer view frame differ")); continue; }
			const FIntRect Full(FIntPoint::ZeroValue, View.Family->RenderTarget->GetSizeXY());
			if (View.Family->Views.Num() != 1 || View.UnscaledViewRect != Full || !View.IsPerspectiveProjection()
				|| View.AntiAliasingMethod != AAM_None || View.Family->EngineShowFlags.ScreenPercentage
				|| !FMath::IsNearlyEqual(View.Family->SecondaryViewFraction, 1.f))
			{
				Unavailable(FString::Printf(TEXT("Cached mesh requires one full-resolution perspective view without AA or scaling: views=%d rect=%s target=%s perspective=%d aa=%d scaling=%d secondary=%g"),
					View.Family->Views.Num(), *View.UnscaledViewRect.ToString(), *Full.ToString(), View.IsPerspectiveProjection(), int32(View.AntiAliasingMethod),
					int32(View.Family->EngineShowFlags.ScreenPercentage), View.Family->SecondaryViewFraction)); continue;
			}
			auto& Identity = S->Result.View;
			Identity.EngineFrame = View.Family->FrameCounter; Identity.RendererFrameNumber = View.Family->FrameNumber;
			Identity.ViewKey = View.GetViewKey(); Identity.Rect = View.UnscaledViewRect;
			Identity.WorldToClip = View.ViewMatrices.GetViewProjectionMatrix(); Identity.DeviceZTransform = View.InvDeviceZToWorldZTransform;
			Identity.WorldTimeSeconds = View.Family->Time.GetWorldTimeSeconds(); Identity.RealTimeSeconds = View.Family->Time.GetRealTimeSeconds();
			S->Result.bHasView = true;
			if (!S->MeshObject || !S->MeshObject->IsGPUSkinMesh() || S->MeshObject->IsNaniteMesh())
			{ Unavailable(TEXT("Ordinary GPU skeletal render object is unavailable")); continue; }
			// UE 5.6 query can access Skin Cache setup before its task finishes (UE-334281).
			// The public cache wait is a CPU setup dependency, not a GPU fence/readback wait.
			const double Before = FPlatformTime::Seconds();
			if (View.Family->Scene) { if (auto* Cache = View.Family->Scene->GetGPUSkinCache()) { Cache->AddAsyncComputeWait(Graph); } }
			S->Result.SetupWaitSeconds = FPlatformTime::Seconds() - Before; Stats.SetupWaitSeconds += S->Result.SetupWaitSeconds;
			FCachedGeometry Geometry;
			const bool Available = S->MeshObject->GetCachedGeometry(Graph, Geometry);
			if (!Available) { Unavailable(TEXT("Skin Cache geometry or renderer dynamic data is unavailable")); continue; }
			const auto& Actual = S->MeshObject->GetReferenceToLocalMatrices(); bool PoseMatches = Actual.Num() == S->Matrices.Num();
			if (PoseMatches) for (int32 I = 0; I < Actual.Num(); ++I) { if (!Actual[I].Equals(S->Matrices[I], 1.e-5f)) { PoseMatches = false; break; } }
			if (!PoseMatches) { Unavailable(TEXT("Renderer bone matrices differ from the acquired pose")); continue; }
			S->MeshObject = nullptr; S->Matrices.Reset();
			const auto& D = S->Work->Value;
			if (!Available || Geometry.LODIndex != D.Enrollment.AnalysisLOD || Geometry.Sections.Num() != D.Sections.Num()
				|| !Geometry.LocalToWorld.ToMatrixWithScale().Equals(D.ComponentToWorld, 1.e-4))
			{ Unavailable(TEXT("Skin Cache is unavailable or LOD, transform or sections do not match acquisition")); continue; }
			FRHIBuffer* Buffer = nullptr; uint64 NextVertex = 0; bool Layout = true;
			for (int32 I = 0; I < Geometry.Sections.Num(); ++I)
			{
				const auto& Section = Geometry.Sections[I]; const auto& Expected = D.Sections[I];
				if (!Section.PositionBuffer || Section.RDGPositionBuffer || Section.SectionIndex != uint32(I)
					|| Section.LODIndex != D.Enrollment.AnalysisLOD || Section.TotalVertexCount != uint32(D.Positions.Num())
					|| Section.TotalIndexCount != uint32(D.Indices.Num()) || Section.IndexBaseIndex != Expected.FirstIndex
					|| uint64(Section.NumPrimitives) * 3 != Expected.IndexCount || Section.VertexBaseIndex != NextVertex || !Section.NumVertices)
				{ Layout = false; break; }
				const auto& Desc = Section.PositionBuffer->GetDesc();
				if (Desc.Common.ViewType != FRHIViewDesc::EViewType::BufferSRV || Desc.Common.Format != PF_R32_FLOAT
					|| Desc.Buffer.SRV.BufferType != FRHIViewDesc::EBufferType::Typed || Desc.Buffer.SRV.OffsetInBytes != 0
					|| (Desc.Buffer.SRV.NumElements != 0 && uint64(Desc.Buffer.SRV.NumElements) < uint64(D.Positions.Num()) * 3))
				{ Layout = false; break; }
				FRHIBuffer* Candidate = Section.PositionBuffer->GetBuffer();
				if (!Candidate || (Buffer && Candidate != Buffer)) { Layout = false; break; }
				Buffer = Candidate; NextVertex += Section.NumVertices;
			}
			const int64 CopyBytes = int64(D.Positions.Num()) * sizeof(FVector3f);
			if (!Layout || !Buffer || NextVertex != uint64(D.Positions.Num()) || Buffer->GetSize() < CopyBytes)
			{ Unavailable(TEXT("Unsupported Skin Cache layout; requires exhaustive sections over one float3 position buffer")); continue; }
			if (Buffer->GetSize() > Limits.MaxSourceBytes - Stats.SourceBytes || CopyBytes > Limits.MaxCopyBytes - Stats.StagingBytes)
			{ Unavailable(TEXT("Skin Cache allocation exceeds explicit source/copy limits")); continue; }
			S->SourceLease = Budget->Reserve(int64(Buffer->GetSize()) + CopyBytes);
			if (!S->SourceLease) { Unavailable(TEXT("Shared admission cannot retain Skin Cache source and staging allocation")); continue; }
			S->Source = Buffer; S->SourceBytes = Buffer->GetSize(); S->CopyBytes = CopyBytes;
			Stats.SourceBytes += S->SourceBytes; Stats.StagingBytes += S->CopyBytes;
			Stats.PeakSourceBytes = FMath::Max(Stats.PeakSourceBytes, Stats.SourceBytes); Stats.PeakStagingBytes = FMath::Max(Stats.PeakStagingBytes, Stats.StagingBytes);
			S->Readback = MakeUnique<FRHIGPUBufferReadback>(TEXT("AnimationCaptureMesh")); S->bScheduled = true;
			Copies.Add(S);
		}
		Lock.Unlock(); // RDG immediate mode may execute AddPass; Copies owns a bounded, stable list.
		for (const auto& S : Copies)
		{
			auto State = AsShared(); const FBufferRHIRef OwnedSource = S->Source;
			Graph.AddPass(RDG_EVENT_NAME("AnimationCaptureMeshCopy"), ERDGPassFlags::None,
				[State, S, OwnedSource](FRHICommandListImmediate& Cmd)
				{
					FScopeLock PassLock(&State->Mutex);
					const double EnqueueStart = FPlatformTime::Seconds();
					// Even a collected cancellation must enqueue its retirement fence after scheduling.
					Cmd.Transition(FRHITransitionInfo(OwnedSource, ERHIAccess::Unknown, ERHIAccess::CopySrc));
					S->Readback->EnqueueCopy(Cmd, OwnedSource, uint32(S->CopyBytes));
					Cmd.Transition(FRHITransitionInfo(OwnedSource, ERHIAccess::CopySrc, ERHIAccess::SRVMask | ERHIAccess::VertexOrIndexBuffer));
					S->bEnqueued = true;
					const double Enqueued = FPlatformTime::Seconds(), Work = Enqueued - EnqueueStart;
					State->Stats.EnqueueWorkSeconds += Work;
					if (!S->bCollected) { S->Result.EnqueuedSeconds = Enqueued; S->Result.EnqueueWorkSeconds = Work; }
				});
		}
	}
	void Retire(FMeshGPUSlot& S)
	{
		Stats.SourceBytes -= S.SourceBytes; Stats.StagingBytes -= S.CopyBytes;
		S.SourceBytes = S.CopyBytes = 0; S.Readback.Reset(); S.Source.SafeRelease(); S.SourceLease.Reset();
		S.bScheduled = false; S.Work.Reset(); S.Matrices.Reset(); S.MeshObject = nullptr;
	}
	void PollRT()
	{
		check(IsInRenderingThread());
		for (const auto& S : Slots)
		{
			if (!S->bEnqueued || !S->Readback || !S->Readback->IsReady()) { continue; }
			if (!S->bTerminal)
			{
				const double Before = FPlatformTime::Seconds(); bool Valid = true;
				const auto* Positions = static_cast<const FVector3f*>(S->Readback->Lock(uint32(S->CopyBytes)));
				if (!Positions) { Valid = false; }
				else
				{
					for (int32 I = 0; I < S->Work->Value.Positions.Num(); ++I)
					{
						const auto& P = Positions[I]; if (!FMath::IsFinite(P.X) || !FMath::IsFinite(P.Y) || !FMath::IsFinite(P.Z)) { Valid = false; break; }
						S->Work->Value.Positions[I] = FVector3d(P);
					}
					S->Readback->Unlock();
				}
				S->Result.DecodeSeconds = FPlatformTime::Seconds() - Before; Stats.DecodeSeconds += S->Result.DecodeSeconds;
				Finish(*S, Valid ? EAnimationMeshGPUStatus::Completed : EAnimationMeshGPUStatus::Failed,
					Valid ? FString() : TEXT("Skin Cache readback failed or returned nonfinite positions"));
			}
			Retire(*S);
		}
		RemoveRetired();
	}
};

namespace
{
class FMeshGPUViewExtension : public FSceneViewExtensionBase
{
public:
	FMeshGPUViewExtension(const FAutoRegister& R, FViewport* V, TSharedRef<FAnimationMeshGPUState, ESPMode::ThreadSafe> S)
		: FSceneViewExtensionBase(R), Viewport(V), State(S) {}
	void SetupViewFamily(FSceneViewFamily&) override {}
	void SetupView(FSceneViewFamily&, FSceneView&) override {}
	bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& C) const override { return C.Viewport == Viewport; }
	void BeginRenderViewFamily(FSceneViewFamily& F) override { if (F.RenderTarget == Viewport) { State->PrepareGT(F); } }
	void PostRenderView_RenderThread(FRDGBuilder& G, FSceneView& V) override { if (V.Family->RenderTarget == Viewport) { State->CaptureRT(G, V); } }
private:
	FViewport* Viewport;
	TSharedRef<FAnimationMeshGPUState, ESPMode::ThreadSafe> State;
};
}

struct FAnimationCaptureMeshGPU::FImpl
{
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<UGameViewportClient> Client;
	FViewport* Viewport = nullptr;
	FIntPoint Size;
	TSharedRef<FAnimationMeshGPUState, ESPMode::ThreadSafe> State = MakeShared<FAnimationMeshGPUState, ESPMode::ThreadSafe>();
	TUniquePtr<FAnimationCaptureMeshReference> Reference;
	FAnimationMeshEnrollment Enrollment;
	TSharedPtr<FMeshGPUViewExtension, ESPMode::ThreadSafe> Extension;
	FDelegateHandle Cleanup;
	bool Validate(FString& Error)
	{
		if (State->bClosed || !World.IsValid() || !Client.IsValid() || Client->GetWorld() != World.Get()
			|| Client->Viewport != Viewport || !Viewport || !State->Component.IsValid() || !State->Component->IsRegistered())
		{ Error = TEXT("Mesh GPU enrollment was closed, destroyed or replaced"); return false; }
		if (Viewport->GetRenderTargetTextureSizeXY() != Size) { Error = TEXT("Mesh GPU viewport resized; reenroll its new generation"); return false; }
		return true;
	}
	void Close()
	{
		check(IsInGameThread()); const double Before = FPlatformTime::Seconds();
		{
			FScopeLock Lock(&State->Mutex); if (State->bClosed) { return; }
			State->bClosed = true; State->Reference = nullptr;
			for (const auto& S : State->Slots) { State->Finish(*S, EAnimationMeshGPUStatus::Cancelled, TEXT("Mesh GPU enrollment shutdown")); }
		}
		FWorldDelegates::OnWorldCleanup.Remove(Cleanup); Extension.Reset(); Reference.Reset();
		ENQUEUE_RENDER_COMMAND(AnimationCaptureMeshShutdown)([S = State](FRHICommandListImmediate& Cmd)
		{
			FScopeLock Lock(&S->Mutex); bool Submitted = false;
			for (const auto& Slot : S->Slots) { Submitted |= Slot->bScheduled; }
			if (Submitted)
			{
				Cmd.SubmitAndBlockUntilGPUIdle();
				Cmd.ImmediateFlush(EImmediateFlushType::FlushRHIThread); // Refresh D3D11 cached fence readiness.
			}
			S->PollRT();
			for (const auto& Slot : S->Slots) { if (Slot->bScheduled) { S->Retire(*Slot); } }
			S->RemoveRetired();
		});
		FlushRenderingCommands();
		FScopeLock Lock(&State->Mutex); State->Stats.ShutdownSeconds += FPlatformTime::Seconds() - Before;
	}
};

FAnimationCaptureMeshGPU::FAnimationCaptureMeshGPU() : Impl(MakeUnique<FImpl>()) {}
TUniquePtr<FAnimationCaptureMeshGPU> FAnimationCaptureMeshGPU::Create(UWorld* World, FViewport* Viewport,
	USkeletalMeshComponent* Component, const FAnimationMeshEnrollment& Enrollment, const FAnimationMeshLimits& MeshLimits,
	const FAnimationMeshGPULimits& Limits, TSharedRef<FAnimationCaptureBudget, ESPMode::ThreadSafe> Budget, FString& Error)
{
	check(IsInGameThread()); Error.Reset();
	if (!World || !World->GetGameViewport() || !Viewport || World->GetGameViewport()->Viewport != Viewport || !Component || Component->GetWorld() != World
		|| Limits.MaxPendingRequests <= 0 || Limits.MaxPendingRequests > 64 || Limits.MaxSourceBytes <= 0 || Limits.MaxSourceBytes > MAX_uint32
		|| Limits.MaxCopyBytes <= 0 || Limits.MaxCopyBytes > MAX_uint32 || !FMath::IsFinite(Limits.TimeoutSeconds) || Limits.TimeoutSeconds <= 0
		|| !GDynamicRHI || FString(GDynamicRHI->GetName()) != TEXT("D3D11") || GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5)
	{ Error = TEXT("Mesh GPU requires a live matching viewport/component, bounded limits and D3D11 SM5"); return nullptr; }
	auto MeshBudget = MakeShared<FAnimationMeshBudget, ESPMode::ThreadSafe>(MeshLimits, Budget);
	auto Reference = FAnimationCaptureMeshReference::Create(Component, Enrollment, MeshBudget, Error); if (!Reference) { return nullptr; }
	TUniquePtr<FAnimationCaptureMeshGPU> Owner(new FAnimationCaptureMeshGPU()); auto& I = *Owner->Impl;
	I.World = World; I.Client = World->GetGameViewport(); I.Viewport = Viewport; I.Size = Viewport->GetRenderTargetTextureSizeXY();
	I.Enrollment = Enrollment; I.Reference = MoveTemp(Reference); I.State->Reference = I.Reference.Get(); I.State->Component = Component;
	I.State->Budget = Budget; I.State->Limits = Limits;
	I.Extension = FSceneViewExtensions::NewExtension<FMeshGPUViewExtension>(Viewport, I.State);
	I.Cleanup = FWorldDelegates::OnWorldCleanup.AddLambda([Impl = Owner->Impl.Get()](UWorld* Cleaned, bool, bool) { if (Impl->World.Get() == Cleaned) { Impl->Close(); } });
	return Owner;
}
FAnimationCaptureMeshGPU::~FAnimationCaptureMeshGPU()
{
	Shutdown(); FScopeLock Lock(&Impl->State->Mutex);
	Impl->State->Slots.Reset(); Impl->State->Stats.PendingRequests = 0; // Stale tickets retain no retired payload.
}
bool FAnimationCaptureMeshGPU::Request(const FString& Id, FAnimationMeshGPUTicket& Ticket, FString& Error)
{
	check(IsInGameThread()); Ticket = {}; Error.Reset();
	const bool Valid = Impl->Validate(Error);
	FScopeLock Lock(&Impl->State->Mutex); auto& S = *Impl->State;
	if (!Valid || Id.IsEmpty() || Id.Len() > 256 || S.Slots.Num() >= S.Limits.MaxPendingRequests
		|| S.Slots.ContainsByPredicate([&](const auto& Slot) { return Slot->Id == Id; }))
	{ if (Error.IsEmpty()) { Error = TEXT("Mesh GPU request identity or pending limit rejected"); } ++S.Stats.Rejected; return false; }
	auto Lease = S.Budget->Reserve(RequestBytes);
	if (!Lease) { Error = TEXT("Shared admission cannot retain mesh request envelope"); ++S.Stats.Rejected; return false; }
	auto Slot = MakeShared<FMeshGPUSlot, ESPMode::ThreadSafe>(); Slot->Serial = ++S.NextSerial; Slot->Id = Id;
	Slot->Envelope = Lease; Slot->Result.Reservation = MoveTemp(Lease); Slot->Result.RequestId = Id; Slot->Result.Enrollment = Impl->Enrollment;
	Slot->Result.RequestedSeconds = FPlatformTime::Seconds(); S.Slots.Add(Slot); ++S.Stats.Admitted;
	S.Stats.PendingRequests = S.Slots.Num(); S.Stats.PeakPendingRequests = FMath::Max(S.Stats.PeakPendingRequests, S.Stats.PendingRequests);
	Ticket.State = Impl->State; Ticket.Serial = Slot->Serial; return true;
}
void FAnimationCaptureMeshGPU::Pump()
{
	check(IsInGameThread()); FString Error; const bool Valid = Impl->Validate(Error);
	auto S = Impl->State; FScopeLock Lock(&S->Mutex);
	const double Now = FPlatformTime::Seconds();
	for (const auto& Slot : S->Slots)
	{
		if (Slot->bTerminal) { continue; }
		if (!Valid) { S->Finish(*Slot, EAnimationMeshGPUStatus::Cancelled, Error); }
		else if (Now - Slot->Result.RequestedSeconds >= S->Limits.TimeoutSeconds) { S->Finish(*Slot, EAnimationMeshGPUStatus::TimedOut, TEXT("Mesh GPU request deadline elapsed")); }
	}
	S->RemoveRetired();
	if (S->bClosed || S->bPollQueued || S->Slots.IsEmpty()) { return; }
	S->bPollQueued = true;
	ENQUEUE_RENDER_COMMAND(AnimationCaptureMeshPoll)([S](FRHICommandListImmediate&) { FScopeLock PollLock(&S->Mutex); S->bPollQueued = false; S->PollRT(); });
}
bool FAnimationCaptureMeshGPU::Collect(FAnimationMeshGPUResult& Result)
{
	check(IsInGameThread()); FScopeLock Lock(&Impl->State->Mutex);
	for (const auto& Slot : Impl->State->Slots) if (Slot->bTerminal && !Slot->bCollected)
	{
		Slot->bCollected = true; Slot->Result.CollectedSeconds = FPlatformTime::Seconds(); Result = MoveTemp(Slot->Result);
		Impl->State->RemoveRetired(); return true;
	}
	return false;
}
bool FAnimationCaptureMeshGPU::Cancel(const FAnimationMeshGPUTicket& Ticket, const FString& Reason)
{
	check(IsInGameThread()); if (Ticket.State != Impl->State || !Ticket.Serial) { return false; }
	FScopeLock Lock(&Impl->State->Mutex);
	for (const auto& Slot : Impl->State->Slots) if (Slot->Serial == Ticket.Serial && !Slot->bTerminal)
	{ Impl->State->Finish(*Slot, EAnimationMeshGPUStatus::Cancelled, Reason); return true; }
	return false;
}
void FAnimationCaptureMeshGPU::Shutdown() { check(IsInGameThread()); Impl->Close(); }
FAnimationMeshGPUStats FAnimationCaptureMeshGPU::GetStats() const { check(IsInGameThread()); FScopeLock Lock(&Impl->State->Mutex); return Impl->State->Stats; }
