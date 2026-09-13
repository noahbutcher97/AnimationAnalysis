#include "AnimationCapture/AnimationCaptureReadback.h"
#include "GlobalShader.h"
#include "Misc/ScopeLock.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"
#include <limits>

namespace
{
class FAnimationCapturePackRGBCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FAnimationCapturePackRGBCS);
	SHADER_USE_PARAMETER_STRUCT(FAnimationCapturePackRGBCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, OutputSize)
		SHADER_PARAMETER(uint32, RGBIs10Bit)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, RGBTexture)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RGBOutput)
	END_SHADER_PARAMETER_STRUCT()
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{ return Parameters.Platform == SP_PCD3D_SM5; }
};
class FAnimationCapturePackDepthCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FAnimationCapturePackDepthCS);
	SHADER_USE_PARAMETER_STRUCT(FAnimationCapturePackDepthCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, OutputSize)
		SHADER_PARAMETER(FIntPoint, SourceOffset)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, CustomDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, CustomStencilTexture)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, DepthOutput)
	END_SHADER_PARAMETER_STRUCT()
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{ return Parameters.Platform == SP_PCD3D_SM5; }
};
IMPLEMENT_GLOBAL_SHADER(FAnimationCapturePackRGBCS, "/Plugin/AnimationAnalysis/Private/AnimationCaptureReadback.usf", "PackRGBCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FAnimationCapturePackDepthCS, "/Plugin/AnimationAnalysis/Private/AnimationCaptureReadback.usf", "PackDepthCS", SF_Compute);

struct FReadbackSlot
{
	uint64 Serial = 0;
	FAnimationCaptureReadbackResult Result;
	TUniquePtr<FRHIGPUBufferReadback> RGBReadback, DepthReadback;
	TRefCountPtr<FRDGPooledBuffer> RGBOutput, DepthOutput;
	bool bRGBScheduled = false, bDepthScheduled = false, bRGBEnqueued = false, bDepthEnqueued = false;
	bool bRGBDecoded = false, bDepthDecoded = false, bTerminal = false, bCollected = false;
	int64 PackedBytes = 0, DecodedBytes = 0, ReservedBytes = 0;
	TSharedPtr<FAnimationCaptureReservation, ESPMode::ThreadSafe> SharedBudgetReservation;
	bool Retired() const { return !bRGBScheduled && !bDepthScheduled && !RGBReadback && !DepthReadback; }
};
}

struct FAnimationCaptureReadbackState
{
	FCriticalSection Mutex;
	FAnimationCaptureReadbackLimits Limits;
	FAnimationCaptureReadbackStats Stats;
	TSharedPtr<FAnimationCaptureBudget, ESPMode::ThreadSafe> SharedBudget;
	TArray<TSharedPtr<FReadbackSlot, ESPMode::ThreadSafe>> Slots;
	uint64 NextSerial = 1;
	bool bClosed = false, bPollScheduled = false, bShutdownComplete = false;
};

namespace
{
using FState = FAnimationCaptureReadbackState;
using FSlotPtr = TSharedPtr<FReadbackSlot, ESPMode::ThreadSafe>;

FSlotPtr FindSlot(FState& State, uint64 Serial)
{
	for (const auto& Slot : State.Slots) { if (Slot->Serial == Serial) { return Slot; } }
	return nullptr;
}

void Finish(FState& State, FReadbackSlot& Slot, EAnimationCaptureReadbackStatus Status, const FString& Error)
{
	if (Slot.bTerminal) { return; }
	Slot.bTerminal = true;
	Slot.Result.Status = Status;
	// Error text is part of the bounded per-request envelope, not arbitrary producer output.
	Slot.Result.Error = Error.Left(1024);
	Slot.Result.CompletedWallSeconds = FPlatformTime::Seconds();
	Slot.Result.CompletionLatencySeconds = Slot.Result.CompletedWallSeconds - Slot.Result.AdmittedWallSeconds;
	switch (Status)
	{
	case EAnimationCaptureReadbackStatus::Completed: ++State.Stats.Completed; break;
	case EAnimationCaptureReadbackStatus::Failed: ++State.Stats.Failed; break;
	case EAnimationCaptureReadbackStatus::Cancelled: ++State.Stats.Cancelled; break;
	case EAnimationCaptureReadbackStatus::TimedOut: ++State.Stats.TimedOut; break;
	}
}

bool Fail(FState& State, FReadbackSlot& Slot, const FString& Reason, FString& Error)
{
	Error = Reason;
	Finish(State, Slot, EAnimationCaptureReadbackStatus::Failed, Reason);
	return false;
}

void RemoveRetired(FState& State)
{
	for (int32 Index = State.Slots.Num() - 1; Index >= 0; --Index)
	{
		const auto& Slot = State.Slots[Index];
		if (Slot->bCollected && Slot->Retired())
		{
			State.Stats.ReservedBytes -= Slot->ReservedBytes;
			State.Stats.PackedOutputBytes -= Slot->PackedBytes;
			State.Stats.StagingBytes -= Slot->PackedBytes;
			State.Stats.DecodedBytes -= Slot->DecodedBytes;
			State.Slots.RemoveAt(Index);
		}
	}
	State.Stats.PendingRequests = State.Slots.Num();
}

float DecodeDepth(uint32 Bits, const FVector4f& Transform)
{
	float DeviceZ;
	FMemory::Memcpy(&DeviceZ, &Bits, sizeof(DeviceZ));
	const float Denominator = DeviceZ * Transform.Z - Transform.W;
	return FMath::IsFinite(DeviceZ) && DeviceZ > 0 && Denominator > 0
		? DeviceZ * Transform.X + Transform.Y + 1.0f / Denominator : std::numeric_limits<float>::infinity();
}

void PollSlot(FState& State, FReadbackSlot& Slot)
{
	const int32 Pixels = Slot.Result.Request.Size.X * Slot.Result.Request.Size.Y;
	if (Slot.bRGBEnqueued && Slot.RGBReadback && Slot.RGBReadback->IsReady())
	{
		if (!Slot.bTerminal)
		{
			const double Start = FPlatformTime::Seconds();
			const uint32* Bytes = static_cast<const uint32*>(Slot.RGBReadback->Lock(uint32(Pixels) * 4));
			if (Bytes)
			{
				Slot.Result.RGB.SetNumUninitialized(Pixels);
				for (int32 Index = 0; Index < Pixels; ++Index)
				{
					const uint32 Packed = Bytes[Index];
					Slot.Result.RGB[Index] = FColor(uint8(Packed >> 16), uint8(Packed >> 8), uint8(Packed), 255);
				}
				Slot.RGBReadback->Unlock();
				Slot.bRGBDecoded = true;
			}
			else { Finish(State, Slot, EAnimationCaptureReadbackStatus::Failed, TEXT("RGB staging buffer map failed")); }
			const double Elapsed = FPlatformTime::Seconds() - Start;
			Slot.Result.DecodeSeconds += Elapsed;
			State.Stats.DecodeSeconds += Elapsed;
		}
		Slot.RGBReadback.Reset();
		Slot.RGBOutput.SafeRelease();
		Slot.bRGBScheduled = false;
	}
	if (Slot.bDepthEnqueued && Slot.DepthReadback && Slot.DepthReadback->IsReady())
	{
		if (!Slot.bTerminal)
		{
			const double Start = FPlatformTime::Seconds();
			const uint32* Bytes = static_cast<const uint32*>(Slot.DepthReadback->Lock(uint32(Pixels) * 16));
			if (Bytes)
			{
				Slot.Result.SceneDepthCm.SetNumUninitialized(Pixels);
				Slot.Result.LabelDepthCm.SetNumUninitialized(Pixels);
				Slot.Result.Labels.SetNumUninitialized(Pixels);
				for (int32 Index = 0; Index < Pixels; ++Index)
				{
					Slot.Result.SceneDepthCm[Index] = DecodeDepth(Bytes[Index * 4], Slot.Result.View.DeviceZTransform);
					Slot.Result.LabelDepthCm[Index] = DecodeDepth(Bytes[Index * 4 + 1], Slot.Result.View.DeviceZTransform);
					Slot.Result.Labels[Index] = uint8(Bytes[Index * 4 + 2]);
				}
				Slot.DepthReadback->Unlock();
				Slot.bDepthDecoded = true;
			}
			else { Finish(State, Slot, EAnimationCaptureReadbackStatus::Failed, TEXT("Depth staging buffer map failed")); }
			const double Elapsed = FPlatformTime::Seconds() - Start;
			Slot.Result.DecodeSeconds += Elapsed;
			State.Stats.DecodeSeconds += Elapsed;
		}
		Slot.DepthReadback.Reset();
		Slot.DepthOutput.SafeRelease();
		Slot.bDepthScheduled = false;
	}
	if (!Slot.bTerminal && (!EnumHasAnyFlags(Slot.Result.Channels, EAnimationCaptureReadbackChannels::RGB) || Slot.bRGBDecoded)
		&& (!EnumHasAnyFlags(Slot.Result.Channels, EAnimationCaptureReadbackChannels::Depth) || Slot.bDepthDecoded))
	{
		Finish(State, Slot, EAnimationCaptureReadbackStatus::Completed, FString());
	}
}

bool SupportedRHI()
{
	return GDynamicRHI && FString(GDynamicRHI->GetName()) == TEXT("D3D11") && GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM5;
}

FRDGBufferRef CreatePackedBuffer(FRDGBuilder& Graph, uint32 Stride, uint32 Pixels, const TCHAR* Name,
	TRefCountPtr<FRDGPooledBuffer>& Owned)
{
	// Own an exact allocation, rather than an implicitly page-rounded global RDG pool entry.
	FRHICommandListImmediate& Cmd = FRHICommandListImmediate::Get();
	const FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(Stride, Pixels);
	const FRHIBufferCreateDesc CreateDesc = FRHIBufferCreateDesc::Create(Name, Desc.GetSize(), Stride, Desc.Usage)
		.SetInitialState(ERHIAccess::UAVCompute);
	TRefCountPtr<FRHIBuffer> Buffer = Cmd.CreateBuffer(CreateDesc);
	Owned = new FRDGPooledBuffer(Cmd, MoveTemp(Buffer), Desc, Pixels, Name);
	return Graph.RegisterExternalBuffer(Owned);
}

BEGIN_SHADER_PARAMETER_STRUCT(FPackedReadbackParameters, )
	RDG_BUFFER_ACCESS(Packed, ERHIAccess::CopySrc)
END_SHADER_PARAMETER_STRUCT()

void AddPackedCopy(FRDGBuilder& Graph, const TSharedRef<FState, ESPMode::ThreadSafe>& State,
	const FSlotPtr& Slot, FRDGBufferRef Packed, bool bRGB)
{
	auto* Parameters = Graph.AllocParameters<FPackedReadbackParameters>();
	Parameters->Packed = Packed;
	Graph.AddPass(RDG_EVENT_NAME("AnimationCapturePackedReadback"), Parameters, ERDGPassFlags::Readback,
		[State, Slot, Packed, bRGB](FRHICommandList& Cmd)
		{
			FScopeLock Lock(&State->Mutex);
			auto& Readback = bRGB ? Slot->RGBReadback : Slot->DepthReadback;
			// Once packing has been scheduled, fence its GPU use even if cancellation won.
			// Logical completion cannot release a buffer still referenced by queued shader work.
			Readback->EnqueueCopy(Cmd, Packed->GetRHI(), Packed->GetSize());
			if (bRGB) { Slot->bRGBEnqueued = true; Slot->Result.RGBEnqueuedWallSeconds = FPlatformTime::Seconds(); }
			else { Slot->bDepthEnqueued = true; Slot->Result.DepthEnqueuedWallSeconds = FPlatformTime::Seconds(); }
		});
}
}

FAnimationCaptureReadbackProducer::FAnimationCaptureReadbackProducer(const FAnimationCaptureReadbackLimits& Limits,
	TSharedPtr<FAnimationCaptureBudget, ESPMode::ThreadSafe> SharedBudget)
	: State(MakeShared<FState, ESPMode::ThreadSafe>())
{
	State->Limits = Limits;
	State->SharedBudget = MoveTemp(SharedBudget);
}

FAnimationCaptureReadbackProducer::~FAnimationCaptureReadbackProducer()
{
	Shutdown();
	FScopeLock Lock(&State->Mutex);
	State->Slots.Reset();
}

const TCHAR* FAnimationCaptureReadbackProducer::DecoderIdentity()
{ return TEXT("AnimationCapture.D3D11.PackedBuffer.RGB8_RGB10.D32FS8.v1"); }

bool FAnimationCaptureReadbackProducer::Request(const FAnimationCaptureReadbackRequest& Request,
	EAnimationCaptureReadbackChannels Channels, FAnimationCaptureReadbackTicket& OutTicket, FString& Error)
{
	check(IsInGameThread());
	Error.Reset();
	OutTicket = FAnimationCaptureReadbackTicket();
	FScopeLock Lock(&State->Mutex);
	auto Reject = [&](const TCHAR* Reason) { Error = Reason; ++State->Stats.Rejected; return false; };
	if (State->bClosed) { return Reject(TEXT("Readback producer is shut down")); }
	if (State->Limits.MaxPendingRequests <= 0 || State->Limits.MaxPendingRequests > 256 || State->Limits.MaxReservedBytes <= 0
		|| State->Limits.MaxPixels <= 0 || !FMath::IsFinite(State->Limits.TimeoutSeconds) || State->Limits.TimeoutSeconds <= 0)
	{ return Reject(TEXT("Invalid readback limits")); }
	if (Channels == EAnimationCaptureReadbackChannels::None || (uint8(Channels) & ~uint8(3)) != 0
		|| Request.Size.X <= 0 || Request.Size.Y <= 0 || Request.Size.X > 16384 || Request.Size.Y > 16384)
	{ return Reject(TEXT("Invalid readback channels or dimensions")); }
	if (Request.SessionId.IsEmpty() || Request.ViewportId.IsEmpty() || Request.ClockId.IsEmpty()
		|| Request.SessionId.Len() > 256 || Request.ViewportId.Len() > 256 || Request.ClockId.Len() > 256
		|| Request.PoseRevisions.Num() > 128 || !FMath::IsFinite(Request.AcquisitionTimeSeconds))
	{ return Reject(TEXT("Readback identity is missing, nonfinite or exceeds its envelope bound")); }
	for (const auto& Pose : Request.PoseRevisions)
	{
		if (Pose.SubjectId.IsEmpty() || Pose.SubjectId.Len() > 256) { return Reject(TEXT("Invalid pose identity")); }
	}
	const int64 Pixels = int64(Request.Size.X) * Request.Size.Y;
	if (Pixels > State->Limits.MaxPixels || Pixels > MAX_int32 / 16) { return Reject(TEXT("Readback pixel limit exceeded")); }
	const bool bRGB = EnumHasAnyFlags(Channels, EAnimationCaptureReadbackChannels::RGB);
	const bool bDepth = EnumHasAnyFlags(Channels, EAnimationCaptureReadbackChannels::Depth);
	const int64 Packed = Pixels * ((bRGB ? 4 : 0) + (bDepth ? 16 : 0));
	const int64 Decoded = Pixels * ((bRGB ? 4 : 0) + (bDepth ? 9 : 0));
	const int64 Reserved = Packed * 2 + Decoded;
	if (State->Slots.Num() >= State->Limits.MaxPendingRequests || Reserved > State->Limits.MaxReservedBytes - State->Stats.ReservedBytes)
	{ return Reject(TEXT("Readback request or byte capacity exceeded")); }
	for (const auto& Existing : State->Slots)
	{
		if (Existing->Result.Request.SessionId == Request.SessionId && Existing->Result.Request.RequestId == Request.RequestId)
		{ return Reject(TEXT("Duplicate pending session/request identity")); }
	}
	// The fixed allowance covers bounded request/result strings, pose witnesses and slot bookkeeping.
	const int64 SharedBytes = Reserved + 4096 + int64(Request.PoseRevisions.Num()) * 320;
	TSharedPtr<FAnimationCaptureReservation, ESPMode::ThreadSafe> SharedReservation;
	if (State->SharedBudget && !(SharedReservation = State->SharedBudget->Reserve(SharedBytes)))
	{ return Reject(TEXT("Shared capture budget exhausted")); }
	const auto Slot = MakeShared<FReadbackSlot, ESPMode::ThreadSafe>();
	Slot->Serial = State->NextSerial++;
	Slot->Result.Request = Request;
	Slot->Result.Channels = Channels;
	Slot->Result.AdmittedWallSeconds = FPlatformTime::Seconds();
	Slot->PackedBytes = Packed; Slot->DecodedBytes = Decoded; Slot->ReservedBytes = Reserved;
	Slot->SharedBudgetReservation = SharedReservation;
	Slot->Result.SharedBudgetReservation = SharedReservation;
	State->Slots.Add(Slot);
	State->Stats.ReservedBytes += Reserved;
	State->Stats.PackedOutputBytes += Packed;
	State->Stats.StagingBytes += Packed;
	State->Stats.DecodedBytes += Decoded;
	State->Stats.PendingRequests = State->Slots.Num();
	State->Stats.PeakPendingRequests = FMath::Max(State->Stats.PeakPendingRequests, State->Stats.PendingRequests);
	State->Stats.PeakReservedBytes = FMath::Max(State->Stats.PeakReservedBytes, State->Stats.ReservedBytes);
	++State->Stats.Admitted;
	OutTicket.State = State; OutTicket.Serial = Slot->Serial;
	return true;
}

void FAnimationCaptureReadbackProducer::Pump()
{
	check(IsInGameThread());
	FScopeLock Lock(&State->Mutex);
	const double Now = FPlatformTime::Seconds();
	for (const auto& Slot : State->Slots)
	{
		if (!Slot->bTerminal && Now - Slot->Result.AdmittedWallSeconds >= State->Limits.TimeoutSeconds)
		{ Finish(*State, *Slot, EAnimationCaptureReadbackStatus::TimedOut, TEXT("Readback completion deadline exceeded; missing results remain unknown")); }
	}
	RemoveRetired(*State);
	if (State->bPollScheduled || State->bShutdownComplete || State->Slots.IsEmpty()) { return; }
	State->bPollScheduled = true;
	ENQUEUE_RENDER_COMMAND(AnimationCapturePollReadbacks)([Shared = State](FRHICommandListImmediate&)
	{
		FScopeLock PollLock(&Shared->Mutex);
		for (const auto& Slot : Shared->Slots) { PollSlot(*Shared, *Slot); }
		RemoveRetired(*Shared);
		Shared->bPollScheduled = false;
	});
}

bool FAnimationCaptureReadbackProducer::Collect(FAnimationCaptureReadbackResult& OutResult)
{
	check(IsInGameThread());
	FScopeLock Lock(&State->Mutex);
	for (const auto& Slot : State->Slots)
	{
		if (Slot->bTerminal && !Slot->bCollected)
		{
			Slot->bCollected = true;
			// Keep the small immutable descriptor available while cancelled GPU resources retire.
			auto RetainedRequest = Slot->Result.Request;
			const auto RetainedView = Slot->Result.View;
			const bool bRetainedView = Slot->Result.bHasView;
			const auto RetainedChannels = Slot->Result.Channels;
			OutResult = MoveTemp(Slot->Result);
			Slot->Result.Request = MoveTemp(RetainedRequest);
			Slot->Result.View = RetainedView;
			Slot->Result.bHasView = bRetainedView;
			Slot->Result.Channels = RetainedChannels;
			OutResult.CollectedWallSeconds = FPlatformTime::Seconds();
			RemoveRetired(*State);
			return true;
		}
	}
	return false;
}

bool FAnimationCaptureReadbackProducer::Cancel(const FAnimationCaptureReadbackTicket& Ticket, const FString& Reason)
{
	check(IsInGameThread());
	if (Ticket.State.Get() != &State.Get()) { return false; }
	FScopeLock Lock(&State->Mutex);
	const auto Slot = FindSlot(*State, Ticket.Serial);
	if (!Slot || Slot->bTerminal) { return false; }
	Finish(*State, *Slot, EAnimationCaptureReadbackStatus::Cancelled, Reason);
	return true;
}

FAnimationCaptureReadbackStats FAnimationCaptureReadbackProducer::GetStats() const
{
	FScopeLock Lock(&State->Mutex);
	return State->Stats;
}

void FAnimationCaptureReadbackProducer::Shutdown(bool bCancelPending)
{
	check(IsInGameThread());
	const double Start = FPlatformTime::Seconds();
	bool bNeedsDrain = false;
	{
		FScopeLock Lock(&State->Mutex);
		if (State->bShutdownComplete) { return; }
		State->bClosed = true;
		for (const auto& Slot : State->Slots)
		{
			if (bCancelPending && !Slot->bTerminal) { Finish(*State, *Slot, EAnimationCaptureReadbackStatus::Cancelled, TEXT("Readback producer shut down")); }
			bNeedsDrain |= !Slot->Retired();
		}
		bNeedsDrain |= State->bPollScheduled;
		bNeedsDrain |= !bCancelPending && !State->Slots.IsEmpty();
	}
	if (bNeedsDrain)
	{
		ENQUEUE_RENDER_COMMAND(AnimationCaptureDrainReadbacks)([Shared = State](FRHICommandListImmediate& Cmd)
		{
			FScopeLock DrainLock(&Shared->Mutex);
			const bool bHasGPUWork = Shared->Slots.ContainsByPredicate([](const FSlotPtr& Slot)
			{ return !Slot->Retired(); });
			if (bHasGPUWork)
			{
				// Teardown only. UE 5.6 D3D11 GPUFence::Wait can stop polling at an earlier
				// owner's unsignalled fence, then wait indefinitely for its own later event.
				// Drain submitted GPU work once, then submit again to refresh D3D11's cached
				// fence events (RHISubmitCommandLists calls PollFences). No per-fence wait.
				Cmd.SubmitAndBlockUntilGPUIdle();
				Cmd.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			}
			for (const auto& Slot : Shared->Slots)
			{
				PollSlot(*Shared, *Slot);
				if (!Slot->bTerminal && ((Slot->bRGBEnqueued && Slot->RGBReadback)
					|| (Slot->bDepthEnqueued && Slot->DepthReadback)))
				{ Finish(*Shared, *Slot, EAnimationCaptureReadbackStatus::Failed, TEXT("Submitted readback fence unavailable after teardown GPU drain")); }
				if (!Slot->bTerminal)
				{ Finish(*Shared, *Slot, EAnimationCaptureReadbackStatus::Cancelled, TEXT("Shutdown before all requested channels were acquired")); }
				Slot->RGBReadback.Reset(); Slot->DepthReadback.Reset();
				Slot->RGBOutput.SafeRelease(); Slot->DepthOutput.SafeRelease();
				Slot->bRGBScheduled = false; Slot->bDepthScheduled = false;
			}
			RemoveRetired(*Shared);
		});
		// Explicit compatibility/teardown drain; never reached by normal acquisition or Pump.
		FlushRenderingCommands();
	}
	FScopeLock Lock(&State->Mutex);
	State->bShutdownComplete = true;
	State->Stats.ShutdownWallSeconds += FPlatformTime::Seconds() - Start;
}

bool FAnimationCaptureReadbackProducer::BindView_RenderThread(const FAnimationCaptureReadbackTicket& Ticket,
	const FAnimationCaptureReadbackView& View, FString& Error)
{
	check(IsInRenderingThread());
	Error.Reset();
	if (!Ticket.IsValid()) { Error = TEXT("Invalid readback ticket"); return false; }
	FScopeLock Lock(&Ticket.State->Mutex);
	const auto Slot = FindSlot(*Ticket.State, Ticket.Serial);
	if (!Slot || Slot->bTerminal) { Error = TEXT("Readback ticket is terminal or retired"); return false; }
	if (Slot->Result.bHasView) { return Fail(*Ticket.State, *Slot, TEXT("Readback view identity was already bound"), Error); }
	if (View.Rect.Min.X < 0 || View.Rect.Min.Y < 0 || View.Rect.Max.X > 16384 || View.Rect.Max.Y > 16384
		|| View.Rect.Max.X < View.Rect.Min.X || View.Rect.Max.Y < View.Rect.Min.Y || View.Rect.Size() != Slot->Result.Request.Size
		|| (Slot->Result.Request.ExpectedEngineFrame != 0 && View.EngineFrame != Slot->Result.Request.ExpectedEngineFrame)
		|| View.WorldToClip.ContainsNaN() || View.DeviceZTransform.ContainsNaN()
		|| !FMath::IsFinite(View.WorldTimeSeconds) || !FMath::IsFinite(View.RealTimeSeconds))
	{ return Fail(*Ticket.State, *Slot, TEXT("Readback acquisition view/frame/dimensions mismatch"), Error); }
	Slot->Result.View = View;
	Slot->Result.bHasView = true;
	return true;
}

bool FAnimationCaptureReadbackProducer::Fail_RenderThread(const FAnimationCaptureReadbackTicket& Ticket, const FString& Error)
{
	check(IsInRenderingThread());
	if (!Ticket.IsValid()) { return false; }
	FScopeLock Lock(&Ticket.State->Mutex);
	const auto Slot = FindSlot(*Ticket.State, Ticket.Serial);
	if (!Slot || Slot->bTerminal) { return false; }
	Finish(*Ticket.State, *Slot, EAnimationCaptureReadbackStatus::Failed, Error);
	return true;
}

bool FAnimationCaptureReadbackProducer::EnqueueDepth_RenderThread(FRDGBuilder& Graph,
	const FAnimationCaptureReadbackTicket& Ticket, FRDGTextureRef SceneDepth, FRDGTextureRef CustomDepth,
	FRDGTextureSRVRef CustomStencil, FString& Error)
{
	check(IsInRenderingThread());
	Error.Reset();
	if (!Ticket.IsValid()) { Error = TEXT("Invalid readback ticket"); return false; }
	FScopeLock Lock(&Ticket.State->Mutex);
	const auto Slot = FindSlot(*Ticket.State, Ticket.Serial);
	if (!Slot || Slot->bTerminal) { Error = TEXT("Readback ticket is terminal or retired"); return false; }
	if (!Slot->Result.bHasView || !EnumHasAnyFlags(Slot->Result.Channels, EAnimationCaptureReadbackChannels::Depth)
		|| Slot->bDepthScheduled || Slot->bDepthDecoded || !SupportedRHI())
	{ return Fail(*Ticket.State, *Slot, TEXT("Depth readback requires a bound view, requested unused channel and D3D11 SM5"), Error); }
	for (FRDGTextureRef Texture : {SceneDepth, CustomDepth})
	{
		if (!Texture || Texture->Desc.Format != PF_DepthStencil || Texture->Desc.NumSamples != 1
			|| Texture->Desc.Dimension != ETextureDimension::Texture2D || GPixelFormats[PF_DepthStencil].BlockBytes != 5
			|| GPixelFormats[PF_DepthStencil].bIs24BitUnormDepthStencil
			|| Texture->Desc.Extent.X < Slot->Result.View.Rect.Max.X || Texture->Desc.Extent.Y < Slot->Result.View.Rect.Max.Y)
		{ return Fail(*Ticket.State, *Slot, TEXT("Unsupported depth source; requires single-sample D32F/S8 covering the bound view"), Error); }
	}
	if (!CustomStencil || CustomStencil->GetParent() != CustomDepth || CustomStencil->Desc.Format != PF_X24_G8)
	{ return Fail(*Ticket.State, *Slot, TEXT("Custom stencil SRV does not belong to the declared custom depth texture"), Error); }
	Slot->bDepthScheduled = true;
	Slot->DepthReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("AnimationCaptureDepth"));
	const uint32 Pixels = uint32(Slot->Result.Request.Size.X) * Slot->Result.Request.Size.Y;
	FRDGBufferRef Packed = CreatePackedBuffer(Graph, 16, Pixels, TEXT("AnimationCaptureDepthOutput"), Slot->DepthOutput);
	auto* Parameters = Graph.AllocParameters<FAnimationCapturePackDepthCS::FParameters>();
	Parameters->OutputSize = Slot->Result.Request.Size;
	Parameters->SourceOffset = Slot->Result.View.Rect.Min;
	Parameters->SceneDepthTexture = SceneDepth;
	Parameters->CustomDepthTexture = CustomDepth;
	Parameters->CustomStencilTexture = CustomStencil;
	Parameters->DepthOutput = Graph.CreateUAV(Packed);
	const TShaderMapRef<FAnimationCapturePackDepthCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("AnimationCapturePackDepth"), Shader, Parameters,
		FComputeShaderUtils::GetGroupCount(Parameters->OutputSize, FIntPoint(8, 8)));
	Lock.Unlock(); // RDG immediate mode may execute the readback callback inside AddPass.
	AddPackedCopy(Graph, Ticket.State.ToSharedRef(), Slot, Packed, false);
	return true;
}

bool FAnimationCaptureReadbackProducer::EnqueueRGB_RenderThread(FRHICommandListImmediate& Cmd,
	const FAnimationCaptureReadbackTicket& Ticket, const FTextureRHIRef& Texture, uint64 ExpectedEngineFrame, FString& Error)
{
	check(IsInRenderingThread());
	Error.Reset();
	if (!Ticket.IsValid()) { Error = TEXT("Invalid readback ticket"); return false; }
	// Release the state mutex before executing RDG, whose copy pass takes it independently.
	{
		FScopeLock Lock(&Ticket.State->Mutex);
		const auto Slot = FindSlot(*Ticket.State, Ticket.Serial);
		if (!Slot || Slot->bTerminal) { Error = TEXT("Readback ticket is terminal or retired"); return false; }
		if (!Slot->Result.bHasView || !EnumHasAnyFlags(Slot->Result.Channels, EAnimationCaptureReadbackChannels::RGB)
			|| Slot->bRGBScheduled || Slot->bRGBDecoded || !SupportedRHI())
		{ return Fail(*Ticket.State, *Slot, TEXT("RGB readback requires a bound view, requested unused channel and D3D11 SM5"), Error); }
		if (Slot->Result.View.EngineFrame != ExpectedEngineFrame || !Texture
			|| Texture->GetDesc().Extent != Slot->Result.Request.Size || Texture->GetDesc().NumSamples != 1
			|| Texture->GetDesc().Dimension != ETextureDimension::Texture2D
			|| !EnumHasAnyFlags(Texture->GetDesc().Flags, ETextureCreateFlags::ShaderResource)
			|| (Texture->GetDesc().Format != PF_B8G8R8A8 && Texture->GetDesc().Format != PF_R8G8B8A8
				&& Texture->GetDesc().Format != PF_A2B10G10R10))
		{
			const FString Detail = Texture ? FString::Printf(
				TEXT("RGB source unsupported: draw_frame=%llu bound_frame=%llu actual_size=%s expected_size=%s samples=%u format=%s flags=0x%llx dimension=%u; requires single-sample shader-readable BGRA8/RGBA8/RGB10A2 Texture2D"),
				ExpectedEngineFrame, Slot->Result.View.EngineFrame, *Texture->GetDesc().Extent.ToString(),
				*Slot->Result.Request.Size.ToString(), uint32(Texture->GetDesc().NumSamples),
				GPixelFormats[Texture->GetDesc().Format].Name, uint64(Texture->GetDesc().Flags), uint32(Texture->GetDesc().Dimension))
				: FString::Printf(TEXT("RGB source is null: draw_frame=%llu bound_frame=%llu expected_size=%s"),
					ExpectedEngineFrame, Slot->Result.View.EngineFrame, *Slot->Result.Request.Size.ToString());
			return Fail(*Ticket.State, *Slot, Detail, Error);
		}
		Slot->bRGBScheduled = true;
		Slot->Result.RGBSourceFormat = Texture->GetDesc().Format;
		Slot->Result.bRGBSourceSRGB = EnumHasAnyFlags(Texture->GetDesc().Flags, ETextureCreateFlags::SRGB);
		Slot->RGBReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("AnimationCaptureRGB"));
	}
	FRDGBuilder Graph(Cmd);
	{
		FScopeLock Lock(&Ticket.State->Mutex);
		const auto Slot = FindSlot(*Ticket.State, Ticket.Serial);
		FRDGTextureRef Source = RegisterExternalTexture(Graph, Texture, TEXT("AnimationCaptureRGBSource"));
		auto SRVDesc = FRDGTextureSRVDesc::CreateForMipLevel(Source, 0);
		SRVDesc.SRGBOverride = SRGBO_ForceDisable;
		const uint32 Pixels = uint32(Slot->Result.Request.Size.X) * Slot->Result.Request.Size.Y;
		FRDGBufferRef Packed = CreatePackedBuffer(Graph, 4, Pixels, TEXT("AnimationCaptureRGBOutput"), Slot->RGBOutput);
		auto* Parameters = Graph.AllocParameters<FAnimationCapturePackRGBCS::FParameters>();
		Parameters->OutputSize = Slot->Result.Request.Size;
		Parameters->RGBIs10Bit = Texture->GetDesc().Format == PF_A2B10G10R10 ? 1u : 0u;
		Parameters->RGBTexture = Graph.CreateSRV(SRVDesc);
		Parameters->RGBOutput = Graph.CreateUAV(Packed);
		const TShaderMapRef<FAnimationCapturePackRGBCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("AnimationCapturePackRGB"), Shader, Parameters,
			FComputeShaderUtils::GetGroupCount(Parameters->OutputSize, FIntPoint(8, 8)));
		Lock.Unlock();
		AddPackedCopy(Graph, Ticket.State.ToSharedRef(), Slot, Packed, true);
	}
	Graph.Execute();
	return true;
}
