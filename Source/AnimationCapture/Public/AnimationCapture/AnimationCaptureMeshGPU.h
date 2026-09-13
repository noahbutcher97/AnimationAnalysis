#pragma once

#include "CoreMinimal.h"
#include "AnimationCapture/AnimationCaptureMeshReference.h"
#include "AnimationCapture/AnimationCaptureReadback.h"

class UWorld;
class FViewport;
class USkeletalMeshComponent;
struct FAnimationMeshGPUState;

enum class EAnimationMeshGPUStatus : uint8 { Completed, Unavailable, Failed, Cancelled, TimedOut };
struct ANIMATIONCAPTURE_API FAnimationMeshGPULimits
{
	int32 MaxPendingRequests = 4;
	/** Owner total of whole retained engine position allocations, including cancelled copies. */
	int64 MaxSourceBytes = 16ll * 1024 * 1024;
	int64 MaxCopyBytes = 4ll * 1024 * 1024;
	double TimeoutSeconds = 5;
};
struct ANIMATIONCAPTURE_API FAnimationMeshGPUStats
{
	int32 PendingRequests = 0, PeakPendingRequests = 0;
	int64 SourceBytes = 0, StagingBytes = 0, PeakSourceBytes = 0, PeakStagingBytes = 0;
	uint64 Admitted = 0, Rejected = 0, Completed = 0, Unavailable = 0, Failed = 0, Cancelled = 0, TimedOut = 0;
	double PrepareSeconds = 0, CaptureSeconds = 0, SetupWaitSeconds = 0, EnqueueWorkSeconds = 0, DecodeSeconds = 0, ShutdownSeconds = 0;
};
struct ANIMATIONCAPTURE_API FAnimationMeshGPUResult
{
	FString RequestId, Error;
	FAnimationMeshEnrollment Enrollment;
	EAnimationMeshGPUStatus Status = EAnimationMeshGPUStatus::Failed;
	/** Both false if acquisition never happened. Completion never replaces acquisition. */
	bool bHasAcquisition = false, bHasView = false;
	uint64 AcquisitionFrame = 0, PoseRevision = 0;
	FString ConfigurationId;
	double RequestedSeconds = 0, AcquiredSeconds = 0, CompletedSeconds = 0, CollectedSeconds = 0;
	double EnqueuedSeconds = 0, PrepareSeconds = 0, CaptureSeconds = 0, SetupWaitSeconds = 0, EnqueueWorkSeconds = 0, DecodeSeconds = 0;
	FAnimationCaptureReadbackView View;
	TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe> Snapshot;
	/** Keeps the bounded request/result envelope charged after collection. Extra value copies are caller-owned. */
	TSharedPtr<FAnimationCaptureReservation, ESPMode::ThreadSafe> Reservation;
};
class ANIMATIONCAPTURE_API FAnimationMeshGPUTicket
{
public:
	bool IsValid() const { return State.IsValid() && Serial != 0; }
private:
	TSharedPtr<FAnimationMeshGPUState, ESPMode::ThreadSafe> State;
	uint64 Serial = 0;
	friend class FAnimationCaptureMeshGPU;
};

/** UE5.6 D3D11, one unscaled perspective view without AA; explicit single-node bone
 * subset. Captures Skin Cache positions before material effects. Does not CPU-skin,
 * tick, force LOD, enable Skin Cache or silently fall back. Needs resident topology
 * and weights. All instance calls are game-thread only; immutable results may outlive
 * the component/world. Pump queues at most one nonblocking render poll. Collected
 * cancellations retain GPU resources/admission until their copy fence retires.
 * Shutdown is the sole measured device-wide drain; device hangs follow UE policy. */
class ANIMATIONCAPTURE_API FAnimationCaptureMeshGPU
{
public:
	static TUniquePtr<FAnimationCaptureMeshGPU> Create(UWorld* World, FViewport* Viewport,
		USkeletalMeshComponent* Component, const FAnimationMeshEnrollment& Enrollment,
		const FAnimationMeshLimits& MeshLimits, const FAnimationMeshGPULimits& Limits,
		TSharedRef<FAnimationCaptureBudget, ESPMode::ThreadSafe> SharedBudget, FString& Error);
	~FAnimationCaptureMeshGPU();
	bool Request(const FString& RequestId, FAnimationMeshGPUTicket& Ticket, FString& Error);
	void Pump();
	bool Collect(FAnimationMeshGPUResult& Result);
	bool Cancel(const FAnimationMeshGPUTicket& Ticket, const FString& Reason);
	void Shutdown();
	FAnimationMeshGPUStats GetStats() const;
private:
	FAnimationCaptureMeshGPU();
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
