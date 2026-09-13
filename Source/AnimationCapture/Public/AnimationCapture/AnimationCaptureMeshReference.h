#pragma once

#include "CoreMinimal.h"
#include "AnimationCapture/AnimationCaptureBudget.h"

class UMeshComponent;
class FAnimationCaptureMeshReference;

/** Explicit limits; zero is invalid. Bytes are reserved payload/work capacity, not RSS.
 * Engine-owned buffers, allocator overhead and caller copies are outside this budget. */
struct ANIMATIONCAPTURE_API FAnimationMeshLimits
{
	int32 MaxSnapshots = 0;
	int32 MaxVertices = 0;
	int32 MaxIndices = 0;
	int32 MaxSections = 0;
	int32 MaxBones = 0;
	int64 MaxBytes = 0;
};

/** Local mesh limits, optionally constrained by the same admission used by imagery. */
class ANIMATIONCAPTURE_API FAnimationMeshBudget : public TSharedFromThis<FAnimationMeshBudget, ESPMode::ThreadSafe>
{
public:
	explicit FAnimationMeshBudget(const FAnimationMeshLimits& InLimits,
		TSharedPtr<FAnimationCaptureBudget, ESPMode::ThreadSafe> SharedBudget = nullptr);
	~FAnimationMeshBudget();
	const FAnimationMeshLimits& Limits() const;
	int64 LiveBytes() const;
	int64 PeakBytes() const;
	int32 LiveSnapshots() const;
private:
	struct FState;
	TUniquePtr<FState> State;
	friend class FAnimationCaptureMeshReference;
	friend class FAnimationMeshSnapshot;
	TSharedPtr<FAnimationCaptureBudget, ESPMode::ThreadSafe> SharedBudget;
	bool Acquire(int64 Bytes);
	void Release(int64 Bytes);
};

struct ANIMATIONCAPTURE_API FAnimationMeshSection
{
	FString Id;
	uint32 FirstIndex = 0;
	uint32 IndexCount = 0;
	/** Empty means unknown, serialized as null. */
	FString MaterialId;
};

struct ANIMATIONCAPTURE_API FAnimationMeshFeature
{
	FString Feature;
	FString State;
	FString ProducerId;
	FString EvidenceId;
	FString Reason;
};

/** Explicit pose-source enrollment. Existing callers default to the SingleNode policy. */
enum class EAnimationMeshPosePolicy : uint8
{
	SingleNode,
	FinalizedAnimation
};

/** Caller supplies portable identities, not paths inferred from a consuming project. */
struct ANIMATIONCAPTURE_API FAnimationMeshEnrollment
{
	FString ComponentId;
	int64 ComponentGeneration = 0;
	FString AssetId;
	int64 ConfigurationGeneration = 0;
	FString ConfigurationId;
	FString SubjectId;
	FString StreamId;
	int32 AnalysisLOD = 0;
	EAnimationMeshPosePolicy PosePolicy = EAnimationMeshPosePolicy::SingleNode;
	/** One opaque identifier per material slot; empty entries mean unknown. */
	TArray<FString> MaterialIds;
};

struct ANIMATIONCAPTURE_API FAnimationMeshData
{
	FAnimationMeshEnrollment Enrollment;
	FString RequestId;
	FString ConfigurationId;
	FString ProducerId;
	FString TopologyId;
	int64 FrameId = 0;
	int64 PoseRevision = 0;
	double AcquiredSeconds = 0;
	double CompletedSeconds = 0;
	/** Both stamps are FPlatformTime::Seconds, domain "unreal-monotonic". */
	FMatrix ComponentToWorld = FMatrix::Identity;
	TArray<FVector3d> Positions;
	TArray<uint32> Indices;
	TArray<FAnimationMeshSection> Sections;
	TArray<FAnimationMeshFeature> Coverage;
};

/** Owned immutable result. No UObject references; safe after sampler/world retirement.
 * Retaining shared pointers retains admission. Copying Data() creates caller-owned data. */
class ANIMATIONCAPTURE_API FAnimationMeshSnapshot
{
public:
	~FAnimationMeshSnapshot();
	const FAnimationMeshData& Data() const { return Value; }
	FAnimationMeshSnapshot(const FAnimationMeshSnapshot&) = delete;
	FAnimationMeshSnapshot& operator=(const FAnimationMeshSnapshot&) = delete;
private:
	friend class FAnimationCaptureMeshReference;
	friend struct FAnimationMeshGPUState;
	FAnimationMeshSnapshot(TSharedRef<FAnimationMeshBudget, ESPMode::ThreadSafe> InBudget, int64 InBytes);
	TSharedRef<FAnimationMeshBudget, ESPMode::ThreadSafe> Budget;
	int64 Bytes;
	TSharedPtr<FAnimationCaptureReservation, ESPMode::ThreadSafe> SharedReservation;
	FAnimationMeshData Value;
};

/** Synchronous game-thread observer. Create after component registration and before
 * its next pose finalization. Supports ordinary static mesh and finalized skeletal
 * bone references under the explicit enrollment policy (SingleNode by default).
 * No component/LOD/pose/rendering changes are made. */
class ANIMATIONCAPTURE_API FAnimationCaptureMeshReference
{
public:
	static TUniquePtr<FAnimationCaptureMeshReference> Create(UMeshComponent* Component,
		const FAnimationMeshEnrollment& Enrollment,
		TSharedRef<FAnimationMeshBudget, ESPMode::ThreadSafe> Budget, FString& Error);
	/** Capture a bounded ordered set of ordinary unparented static components during one
	 * stable synchronous game-thread operation. Failure publishes no snapshots. */
	static bool CaptureRigidBatch(
		TConstArrayView<FAnimationCaptureMeshReference*> Samplers,
		const FString& RequestId, int32 MaxComponents,
		TArray<TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe>>& OutSnapshots,
		FString& Error);
	/** Capture a bounded ordered set of distinct enrolled skeletal components, direct
	 * rigid attachments and ordinary unparented rigid components in one synchronous
	 * acquisition. Failure publishes no snapshots. */
	static bool CaptureBatch(
		TConstArrayView<FAnimationCaptureMeshReference*> Samplers,
		const FString& RequestId, int32 MaxComponents,
		TArray<TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe>>& OutSnapshots,
		FString& Error);
	~FAnimationCaptureMeshReference();
	/** Failure contains no geometry; Error gives unavailable/admission reason.
	 * Repeated captures retain the same witnessed skeletal revision until finalized again. */
	TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe> Capture(const FString& RequestId, FString& Error);
private:
	friend struct FAnimationMeshGPUState;
	/** Internal preparation never exposes unfinished GPU position storage. */
	TSharedPtr<FAnimationMeshSnapshot, ESPMode::ThreadSafe> Prepare(const FString& RequestId, FString& Error,
		bool bComputePositions, TOptional<double> NativeAcquiredSeconds = {});
	FAnimationCaptureMeshReference();
	struct FState;
	TUniquePtr<FState> State;
};

namespace AnimationCaptureMeshReplay
{
	/** Canonical schema-1 topology identity, lowercase SHA-256. */
	ANIMATIONCAPTURE_API FString TopologyIdentity(const FAnimationMeshData& Data);
	/** Windows, synchronous game thread, caller-owned stable trusted existing root. RelativeName is
	 * one portable ASCII component. Refuses reuse. No UObject access or discovery.
	 * Encoded metadata is bounded to 64 KiB total. No concurrent path replacement claim. */
	ANIMATIONCAPTURE_API bool Write(const FString& Root, const FString& RelativeName,
		const FAnimationMeshSnapshot& Snapshot, FString& Error);
}
