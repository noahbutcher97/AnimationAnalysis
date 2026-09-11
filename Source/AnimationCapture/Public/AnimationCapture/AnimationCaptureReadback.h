#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"
#include "RHIResources.h"

class FRHICommandListImmediate;
struct FAnimationCaptureReadbackState;

enum class EAnimationCaptureReadbackChannels : uint8 { None = 0, RGB = 1, Depth = 2 };
ENUM_CLASS_FLAGS(EAnimationCaptureReadbackChannels);
enum class EAnimationCaptureReadbackStatus : uint8 { Completed, Failed, Cancelled, TimedOut };

/** An explicit producer witness, not a request to inspect an object on completion. */
struct ANIMATIONCAPTURE_API FAnimationCapturePoseRevision
{
	FString SubjectId;
	uint64 Revision = 0, EngineFrame = 0;
	bool bMissing = false;
};

/** Copied at admission. Zero ExpectedEngineFrame allows the renderer to supply the frame. */
struct ANIMATIONCAPTURE_API FAnimationCaptureReadbackRequest
{
	FString SessionId, ViewportId, ClockId;
	uint64 RequestId = 0, ViewportGeneration = 0, ExpectedEngineFrame = 0;
	double AcquisitionTimeSeconds = 0;
	FIntPoint Size = FIntPoint::ZeroValue;
	TArray<FAnimationCapturePoseRevision> PoseRevisions;
};

/** Copied once from the actual renderer view. No raw viewport/world/subject pointers. */
struct ANIMATIONCAPTURE_API FAnimationCaptureReadbackView
{
	uint64 EngineFrame = 0;
	uint32 RendererFrameNumber = 0, ViewKey = 0;
	double WorldTimeSeconds = 0, RealTimeSeconds = 0;
	FIntRect Rect;
	FMatrix WorldToClip = FMatrix::Identity;
	FVector4f DeviceZTransform = FVector4f(0, 0, 0, 0);
};

struct ANIMATIONCAPTURE_API FAnimationCaptureReadbackLimits
{
	int32 MaxPendingRequests = 4;
	int64 MaxReservedBytes = 64ll * 1024 * 1024;
	int64 MaxPixels = 1920ll * 1080;
	double TimeoutSeconds = 5;
};

struct ANIMATIONCAPTURE_API FAnimationCaptureReadbackStats
{
	int32 PendingRequests = 0, PeakPendingRequests = 0;
	/** Fixed GPU output + staging + decoded payload capacity. Includes uncollected terminal results. */
	int64 ReservedBytes = 0, PeakReservedBytes = 0;
	int64 PackedOutputBytes = 0, StagingBytes = 0, DecodedBytes = 0;
	uint64 Admitted = 0, Rejected = 0, Completed = 0, Failed = 0, Cancelled = 0, TimedOut = 0;
	double DecodeSeconds = 0, ShutdownWallSeconds = 0;
};

struct ANIMATIONCAPTURE_API FAnimationCaptureReadbackResult
{
	FAnimationCaptureReadbackRequest Request;
	FAnimationCaptureReadbackView View;
	bool bHasView = false;
	EAnimationCaptureReadbackChannels Channels = EAnimationCaptureReadbackChannels::None;
	EAnimationCaptureReadbackStatus Status = EAnimationCaptureReadbackStatus::Failed;
	FString Error;
	TArray<FColor> RGB;
	TArray<float> SceneDepthCm, LabelDepthCm;
	TArray<uint8> Labels;
	EPixelFormat RGBSourceFormat = PF_Unknown;
	bool bRGBSourceSRGB = false;
	double AdmittedWallSeconds = 0, RGBEnqueuedWallSeconds = 0, DepthEnqueuedWallSeconds = 0;
	double CompletedWallSeconds = 0, CollectedWallSeconds = 0, DecodeSeconds = 0;
	/** Acquisition-to-completion wall latency; supplied acquisition clock remains separate. */
	double CompletionLatencySeconds = 0;
};

/** Safe to capture in render commands. A retained stale ticket owns no retired image payload. */
class ANIMATIONCAPTURE_API FAnimationCaptureReadbackTicket
{
public:
	bool IsValid() const { return State.IsValid() && Serial != 0; }
private:
	TSharedPtr<FAnimationCaptureReadbackState, ESPMode::ThreadSafe> State;
	uint64 Serial = 0;
	friend class FAnimationCaptureReadbackProducer;
};

/**
 * Per-owner bounded producer. Request/Pump/Collect/Cancel/Shutdown run on the game thread.
 * Static render entrypoints own only ticket/value/render-resource state; callers enqueue them
 * in acquisition order. Bind the actual view before either channel. RGB is supplied separately
 * after the viewport draw, with that draw's engine frame checked against the bound view.
 *
 * D3D11 SM5 only: single-sample shader-readable BGRA8/RGBA8/RGB10A2 and D32F/S8 depth.
 * RGB10A2 follows FColor::Requantize10to8, exactly as D3D11 screenshot conversion does.
 * GPU packing gives fixed buffer byte sizes (RGB: 4 bytes/pixel; depth: 16 bytes/pixel),
 * independent of physical depth texture stride or staging texture row pitch. Reservations
 * include GPU output, staging and decoded arrays (RGB: 12; depth: 41 bytes/pixel).
 * Identity text is separately bounded to 256 characters per field and 128 pose witnesses.
 * Caller-owned collected arrays and downstream encoding must remain charged by the caller.
 *
 * Pump never waits for the GPU and has at most one pending render poll command. Timeout and
 * cancellation emit one terminal result but keep submitted resources charged until retired.
 * Shutdown is the sole explicit blocking drain: by default it cancels logical requests;
 * Shutdown(false) first delivers fully submitted images and cancels missing branches. Both
 * drain outstanding GPU work with one device-wide idle wait when resources remain, refresh
 * the D3D11 fence cache, and flush the teardown render command. This avoids UE 5.6 D3D11's
 * per-fence wait starvation across owners. Their measured wall time is reported; a failed
 * or hung device remains subject to the engine's GPU-idle policy.
 * Destruction performs Shutdown and discards any uncollected terminal results.
 */
class ANIMATIONCAPTURE_API FAnimationCaptureReadbackProducer
{
public:
	explicit FAnimationCaptureReadbackProducer(const FAnimationCaptureReadbackLimits& Limits = {});
	~FAnimationCaptureReadbackProducer();
	FAnimationCaptureReadbackProducer(const FAnimationCaptureReadbackProducer&) = delete;
	FAnimationCaptureReadbackProducer& operator=(const FAnimationCaptureReadbackProducer&) = delete;
	bool Request(const FAnimationCaptureReadbackRequest& Request, EAnimationCaptureReadbackChannels Channels,
		FAnimationCaptureReadbackTicket& OutTicket, FString& Error);
	void Pump();
	bool Collect(FAnimationCaptureReadbackResult& OutResult);
	bool Cancel(const FAnimationCaptureReadbackTicket& Ticket, const FString& Reason);
	void Shutdown(bool bCancelPending = true);
	FAnimationCaptureReadbackStats GetStats() const;
	static const TCHAR* DecoderIdentity();

	static bool BindView_RenderThread(const FAnimationCaptureReadbackTicket& Ticket,
		const FAnimationCaptureReadbackView& View, FString& Error);
	static bool EnqueueDepth_RenderThread(FRDGBuilder& Graph, const FAnimationCaptureReadbackTicket& Ticket,
		FRDGTextureRef SceneDepth, FRDGTextureRef CustomDepth, FRDGTextureSRVRef CustomStencil, FString& Error);
	static bool EnqueueRGB_RenderThread(FRHICommandListImmediate& Cmd, const FAnimationCaptureReadbackTicket& Ticket,
		const FTextureRHIRef& Texture, uint64 ExpectedEngineFrame, FString& Error);
	static bool Fail_RenderThread(const FAnimationCaptureReadbackTicket& Ticket, const FString& Error);

private:
	TSharedRef<FAnimationCaptureReadbackState, ESPMode::ThreadSafe> State;
};
