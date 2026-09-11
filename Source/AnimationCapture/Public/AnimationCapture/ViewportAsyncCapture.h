#pragma once
#include "CoreMinimal.h"
#include "AnimationCapture/AnimationCaptureReadback.h"

class UWorld;
class FViewport;

/**
 * Explicit Game/PIE viewport adapter for the bounded producer. All instance calls are game-thread only.
 * Request admits RGB + scene/custom-depth for the next draw. CaptureRGB is called by a recorder
 * from OnViewportRendered and uses that draw's renderer witness. The caller supplies clock and
 * pose witnesses; they are copied at admission and never refreshed by Poll.
 *
 * One full-resolution perspective view, D3D11, no AA. Diagnostic resolution is scoped to this
 * adapter's view families. Poll accepts no current frame: completed observations retain acquisition
 * identity. Resize/replacement/world cleanup cancels outstanding requests. Destruction performs an
 * explicit measured producer shutdown drain, never a per-frame flush. Labels are separately owned.
 */
class ANIMATIONCAPTURE_API FViewportAsyncCapture
{
public:
	FViewportAsyncCapture(UWorld* World, FViewport* Viewport, bool bDiagnosticResolution = false,
		const FAnimationCaptureReadbackLimits& Limits = {});
	~FViewportAsyncCapture();
	bool Request(const FAnimationCaptureReadbackRequest& Request, FAnimationCaptureReadbackTicket& Ticket, FString& Error);
	bool CaptureRGB(const FAnimationCaptureReadbackRequest& Request, FAnimationCaptureReadbackTicket& Ticket, FString& Error);
	bool Poll(FAnimationCaptureReadbackResult& Result);
	bool Cancel(const FAnimationCaptureReadbackTicket& Ticket, const FString& Reason);
	void Shutdown(bool bCancelPending = true);
	FAnimationCaptureReadbackStats GetStats() const;
private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
