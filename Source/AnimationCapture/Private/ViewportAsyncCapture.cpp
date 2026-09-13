#include "AnimationCapture/ViewportAsyncCapture.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "LegacyScreenPercentageDriver.h"
#include "Misc/ScopeLock.h"
#include "RenderGraphBuilder.h"
#include "SceneRenderTargetParameters.h"
#include "SceneViewExtension.h"
#include "UnrealClient.h"

namespace
{
struct FViewportReadbackEntry
{
	FAnimationCaptureReadbackTicket Ticket;
	FString Session;
	uint64 Request = 0, DrawFrame = 0;
	bool bDepthQueued = false, bRGBQueued = false;
};
struct FViewportReadbackState
{
	FCriticalSection Mutex;
	bool bClosed = false;
	TArray<FViewportReadbackEntry> Entries;
	FAnimationCaptureReadbackView LastView;
	FTextureRHIRef LastRGB;
	FString ViewError = TEXT("No renderer view has been observed");
};

class FAsyncCaptureViewExtension : public FSceneViewExtensionBase
{
public:
	FAsyncCaptureViewExtension(const FAutoRegister& AutoRegister, FViewport* InViewport,
		TSharedRef<FViewportReadbackState, ESPMode::ThreadSafe> InState, bool Diagnostic)
		: FSceneViewExtensionBase(AutoRegister), Viewport(InViewport), State(InState), bDiagnostic(Diagnostic) {}
	void SetupViewFamily(FSceneViewFamily&) override {}
	void SetupView(FSceneViewFamily&, FSceneView&) override {}
	bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override { return Context.Viewport == Viewport; }
	void BeginRenderViewFamily(FSceneViewFamily& Family) override
	{
		if (Family.RenderTarget != Viewport) { return; }
		FScopeLock Lock(&State->Mutex); if (State->bClosed) { return; }
		for (auto& Entry : State->Entries) { if (!Entry.DrawFrame) { Entry.DrawFrame = GFrameCounter; } }
		if (bDiagnostic)
		{
			const auto* Previous = Family.GetScreenPercentageInterface();
			Family.SetScreenPercentageInterface_Unchecked(new FLegacyScreenPercentageDriver(Family, 1.f)); delete Previous;
			Family.EngineShowFlags.SetScreenPercentage(false); Family.SecondaryViewFraction = 1.f;
		}
	}
	void PostRenderView_RenderThread(FRDGBuilder& Graph, FSceneView& View) override
	{
		if (View.Family->RenderTarget != Viewport) { return; }
		FAnimationCaptureReadbackView Identity;
		Identity.EngineFrame = View.Family->FrameCounter; Identity.RendererFrameNumber = View.Family->FrameNumber;
		Identity.ViewKey = View.GetViewKey(); Identity.Rect = View.UnscaledViewRect;
		Identity.WorldToClip = View.ViewMatrices.GetViewProjectionMatrix(); Identity.DeviceZTransform = View.InvDeviceZToWorldZTransform;
		Identity.WorldTimeSeconds = View.Family->Time.GetWorldTimeSeconds();
		Identity.RealTimeSeconds = View.Family->Time.GetRealTimeSeconds();
		FString Error;
		const FIntRect TargetRect(FIntPoint::ZeroValue, View.Family->RenderTarget->GetSizeXY());
		if (View.Family->Views.Num() != 1 || Identity.Rect != TargetRect || !View.IsPerspectiveProjection()
			|| View.AntiAliasingMethod != AAM_None || View.Family->EngineShowFlags.ScreenPercentage
			|| !FMath::IsNearlyEqual(View.Family->SecondaryViewFraction, 1.f))
		{ Error = TEXT("Unsupported asynchronous view: requires one full-resolution perspective view without AA or scaling"); }
		TArray<FAnimationCaptureReadbackTicket> DepthTickets;
		{
			FScopeLock Lock(&State->Mutex); if (State->bClosed) { return; }
			State->LastView = Identity; State->ViewError = Error;
			// Read the target only during the live renderer callback. Deferred RGB work owns this RHI reference.
			State->LastRGB = View.Family->RenderTarget->GetRenderTargetTexture();
			for (auto& Entry : State->Entries)
			{
				if (Entry.DrawFrame == Identity.EngineFrame && !Entry.bDepthQueued)
				{ Entry.bDepthQueued = true; DepthTickets.Add(Entry.Ticket); }
			}
		}
		if (DepthTickets.IsEmpty()) { return; }
		const auto Textures = CreateSceneTextureUniformBuffer(Graph, View, ESceneTextureSetupMode::All);
		for (const auto& Ticket : DepthTickets)
		{
			if (!Error.IsEmpty()) { FAnimationCaptureReadbackProducer::Fail_RenderThread(Ticket, Error); continue; }
			FString RequestError;
			if (FAnimationCaptureReadbackProducer::BindView_RenderThread(Ticket, Identity, RequestError))
			{
				FAnimationCaptureReadbackProducer::EnqueueDepth_RenderThread(Graph, Ticket,
					Textures->GetContents()->SceneDepthTexture, Textures->GetContents()->CustomDepthTexture,
					Textures->GetContents()->CustomStencilTexture, RequestError);
			}
			if (!RequestError.IsEmpty()) { FAnimationCaptureReadbackProducer::Fail_RenderThread(Ticket, RequestError); }
		}
	}
private:
	FViewport* Viewport;
	TSharedRef<FViewportReadbackState, ESPMode::ThreadSafe> State;
	bool bDiagnostic;
};

void QueueRGB(const TSharedRef<FViewportReadbackState, ESPMode::ThreadSafe>& State,
	const FAnimationCaptureReadbackTicket& Ticket, uint64 Frame, bool BindView)
{
	ENQUEUE_RENDER_COMMAND(AnimationCaptureViewportRGB)([State, Ticket, Frame, BindView](FRHICommandListImmediate& Cmd)
	{
		FAnimationCaptureReadbackView View; FTextureRHIRef RGB; FString Error;
		{
			FScopeLock Lock(&State->Mutex);
			if (State->bClosed) { return; }
			View = State->LastView; RGB = State->LastRGB; Error = State->ViewError;
		}
		if (Error.IsEmpty() && View.EngineFrame != Frame) { Error = TEXT("RGB draw and renderer view frame differ or view is missing"); }
		if (Error.IsEmpty() && BindView) { FAnimationCaptureReadbackProducer::BindView_RenderThread(Ticket, View, Error); }
		if (Error.IsEmpty()) { FAnimationCaptureReadbackProducer::EnqueueRGB_RenderThread(Cmd, Ticket, RGB, Frame, Error); }
		if (!Error.IsEmpty()) { FAnimationCaptureReadbackProducer::Fail_RenderThread(Ticket, Error); }
	});
}
}

struct FViewportAsyncCapture::FImpl
{
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<UGameViewportClient> Client;
	FViewport* Viewport;
	FIntPoint InitialSize;
	FAnimationCaptureReadbackProducer Producer;
	TSharedRef<FViewportReadbackState, ESPMode::ThreadSafe> State = MakeShared<FViewportReadbackState, ESPMode::ThreadSafe>();
	TSharedPtr<FAsyncCaptureViewExtension, ESPMode::ThreadSafe> Extension;
	FDelegateHandle DrawHandle, CleanupHandle;
	bool bClosed = false;
	uint64 PreflightRejected = 0;
	FImpl(UWorld* InWorld, FViewport* InViewport, bool Diagnostic, const FAnimationCaptureReadbackLimits& Limits,
		TSharedPtr<FAnimationCaptureBudget, ESPMode::ThreadSafe> SharedBudget)
		: World(InWorld), Client(InWorld ? InWorld->GetGameViewport() : nullptr), Viewport(InViewport),
		InitialSize(InViewport ? InViewport->GetRenderTargetTextureSizeXY() : FIntPoint::ZeroValue),
		Producer(Limits, MoveTemp(SharedBudget))
	{
		if (InWorld && InViewport)
		{
			Extension = FSceneViewExtensions::NewExtension<FAsyncCaptureViewExtension>(Viewport, State, Diagnostic);
			DrawHandle = UGameViewportClient::OnViewportRendered().AddRaw(this, &FImpl::Draw);
			CleanupHandle = FWorldDelegates::OnWorldCleanup.AddLambda([this](UWorld* Cleaned, bool, bool)
			{ if (Cleaned == World.Get()) { Close(); } });
		}
	}
	bool Validate(FString& Error)
	{
		if (bClosed || !World.IsValid() || !Client.IsValid() || Client->GetWorld() != World.Get()
			|| Client->Viewport != Viewport || !Viewport)
		{ Error = TEXT("Asynchronous capture viewport/world was destroyed or replaced"); Close(); return false; }
		if (Viewport->GetRenderTargetTextureSizeXY() != InitialSize)
		{ Error = TEXT("Asynchronous capture viewport resized; create a new adapter for its new generation"); Close(); return false; }
		if (!GDynamicRHI || FString(GDynamicRHI->GetName()) != TEXT("D3D11"))
		{ Error = TEXT("Asynchronous capture requires D3D11"); return false; }
		return true;
	}
	void Draw(FViewport* Drawn)
	{
		if (Drawn != Viewport || bClosed) { return; }
		FString Error; if (!Validate(Error)) { return; }
		TArray<FAnimationCaptureReadbackTicket> Tickets;
		{
			FScopeLock Lock(&State->Mutex);
			for (auto& Entry : State->Entries)
			{
				if (Entry.DrawFrame == GFrameCounter && !Entry.bRGBQueued)
				{ Entry.bRGBQueued = true; Tickets.Add(Entry.Ticket); }
			}
		}
		for (const auto& Ticket : Tickets) { QueueRGB(State, Ticket, GFrameCounter, false); }
	}
	void Close(bool bCancelPending = true)
	{
		if (bClosed) { return; } bClosed = true;
		UGameViewportClient::OnViewportRendered().Remove(DrawHandle); FWorldDelegates::OnWorldCleanup.Remove(CleanupHandle);
		if (bCancelPending) { FScopeLock Lock(&State->Mutex); State->bClosed = true; }
		Extension.Reset(); Producer.Shutdown(bCancelPending);
		// Shutdown has retired queued commands and GPU work; no renderer callback can retain the target afterward.
		FScopeLock Lock(&State->Mutex); State->bClosed = true; State->Entries.Reset(); State->LastRGB.SafeRelease();
	}
};

FViewportAsyncCapture::FViewportAsyncCapture(UWorld* World, FViewport* Viewport, bool Diagnostic,
	const FAnimationCaptureReadbackLimits& Limits, TSharedPtr<FAnimationCaptureBudget, ESPMode::ThreadSafe> SharedBudget)
	: Impl(MakeUnique<FImpl>(World, Viewport, Diagnostic, Limits, MoveTemp(SharedBudget))) {}
FViewportAsyncCapture::~FViewportAsyncCapture() { Shutdown(); }
bool FViewportAsyncCapture::Request(const FAnimationCaptureReadbackRequest& Request, FAnimationCaptureReadbackTicket& Ticket, FString& Error)
{
	check(IsInGameThread()); Error.Reset(); Ticket = FAnimationCaptureReadbackTicket();
	if (!Impl->Validate(Error)) { ++Impl->PreflightRejected; return false; }
	auto* Depth = IConsoleManager::Get().FindConsoleVariable(TEXT("r.CustomDepth"));
	if (!Depth || Depth->GetInt() != 3) { Error = TEXT("Surface readback requires r.CustomDepth=3"); ++Impl->PreflightRejected; return false; }
	if (Request.Size != Impl->InitialSize) { Error = TEXT("Requested dimensions differ from the enrolled viewport"); ++Impl->PreflightRejected; return false; }
	if (!Impl->Producer.Request(Request, EAnimationCaptureReadbackChannels::RGB | EAnimationCaptureReadbackChannels::Depth, Ticket, Error)) { return false; }
	FScopeLock Lock(&Impl->State->Mutex);
	Impl->State->Entries.Add({Ticket, Request.SessionId, Request.RequestId}); return true;
}
bool FViewportAsyncCapture::CaptureRGB(const FAnimationCaptureReadbackRequest& Request, FAnimationCaptureReadbackTicket& Ticket, FString& Error)
{
	check(IsInGameThread()); Error.Reset(); Ticket = FAnimationCaptureReadbackTicket();
	if (!Impl->Validate(Error)) { ++Impl->PreflightRejected; return false; }
	if (Request.Size != Impl->InitialSize || Request.ExpectedEngineFrame != GFrameCounter)
	{ Error = TEXT("RGB request must identify this post-draw frame and enrolled viewport dimensions"); ++Impl->PreflightRejected; return false; }
	if (!Impl->Producer.Request(Request, EAnimationCaptureReadbackChannels::RGB, Ticket, Error)) { return false; }
	QueueRGB(Impl->State, Ticket, GFrameCounter, true); return true;
}
bool FViewportAsyncCapture::Poll(FAnimationCaptureReadbackResult& Result)
{
	check(IsInGameThread()); FString Error; Impl->Validate(Error); Impl->Producer.Pump();
	if (!Impl->Producer.Collect(Result)) { return false; }
	FScopeLock Lock(&Impl->State->Mutex);
	Impl->State->Entries.RemoveAll([&](const FViewportReadbackEntry& Entry)
	{ return Entry.Session == Result.Request.SessionId && Entry.Request == Result.Request.RequestId; });
	return true;
}
bool FViewportAsyncCapture::Cancel(const FAnimationCaptureReadbackTicket& Ticket, const FString& Reason)
{ check(IsInGameThread()); return Impl->Producer.Cancel(Ticket, Reason); }
void FViewportAsyncCapture::Shutdown(bool bCancelPending) { check(IsInGameThread()); Impl->Close(bCancelPending); }
FAnimationCaptureReadbackStats FViewportAsyncCapture::GetStats() const
{
	check(IsInGameThread());
	auto Stats = Impl->Producer.GetStats();
	Stats.Rejected += Impl->PreflightRejected;
	return Stats;
}
