// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

class UPrimitiveComponent;
class UWorld;
class FViewport;

/** Explicit labels for rendered primitives. No actor discovery or gameplay semantics. */
struct ANIMATIONCAPTURE_API FSurfaceCaptureLabel
{
	uint8 Id = 0;
	FString Name;
	TWeakObjectPtr<UPrimitiveComponent> Component;
};

/** Owns temporary custom-depth labels; validates all inputs before changing any component. */
class ANIMATIONCAPTURE_API FScopedSurfaceCaptureLabels
{
  public:
	FScopedSurfaceCaptureLabels();
	~FScopedSurfaceCaptureLabels();
	bool Apply(UWorld *World, TConstArrayView<FSurfaceCaptureLabel> Labels, FString &Error);
	void Restore();

  private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};

/** Raster data from one actual renderer view. Depth is camera-axis distance in cm; infinity is clear depth. */
struct ANIMATIONCAPTURE_API FViewportSurfaceFrame
{
	uint64 EngineFrame = 0;
	FIntPoint Size = FIntPoint::ZeroValue;
	FMatrix WorldToClip = FMatrix::Identity;
	FVector4f DeviceZTransform = FVector4f(0, 0, 0, 0);
	TArray<float> SceneDepthCm, LabelDepthCm;
	/** Frontmost custom-depth label, including labelled objects behind ordinary scene occluders. */
	TArray<uint8> Labels;
	double ReadbackSeconds = 0;
	FString Error;
};

/**
 * Optional, bounded renderer adapter. No combat dependencies or file writes.
 * The explicit diagnostic-resolution option disables scaling only on the requested view family;
 * it does not mutate a viewport, component, CVar or asset. Otherwise unsupported scaling rejects.
 * Request on the game thread before a draw; Collect in OnViewportRendered for that viewport.
 * Collect flushes the requested readback and RGB screenshot, then rejects a different engine frame.
 * Initial verified backend: D3D11, D32F/S8, one full-resolution perspective view, no AA.
 * This synchronous diagnostic path is deliberately separate from the continuous RGB recorder.
 */
class ANIMATIONCAPTURE_API FViewportSurfaceCapture
{
  public:
	FViewportSurfaceCapture(UWorld *World, FViewport *Viewport, bool bUseDiagnosticResolution = false);
	~FViewportSurfaceCapture();
	bool Request(FString &Error);
	bool Collect(FViewport *DrawnViewport, uint64 ExpectedFrame, FViewportSurfaceFrame &OutFrame,
				 TArray<FColor> &OutRGB, FString &Error);

  private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
