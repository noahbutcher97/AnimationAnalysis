// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "AnimationCapture/AnimationCaptureBudget.h"

/** Bounded PNG export. Called only from the game thread; workers own pixels and paths,
 * never actors, viewports or session state. Results are collected in submission order.
 * Destruction drains outstanding work before releasing the writer. */
class ANIMATIONCAPTURE_API FAnimationCaptureImageWriter
{
  public:
	struct FResult
	{
		FString File;
		FString Error;
		int64 BytesWritten = 0;
		double EncodeSeconds = 0, WriteSeconds = 0;
	};
	static constexpr int32 MaxPendingFrames = 4;
	static constexpr int64 MaxPendingBytes = 64ll * 1024 * 1024;

	explicit FAnimationCaptureImageWriter(TSharedPtr<FAnimationCaptureBudget, ESPMode::ThreadSafe> SharedBudget = nullptr);
	~FAnimationCaptureImageWriter();
	// Conservative reservation, checked again against the actual compressed size.
	static int64 FileByteReservation(FIntPoint Size);
	bool CanEnqueue(FIntPoint Size, FString &OutError) const;
	bool Enqueue(const FString &File, FIntPoint Size, TArray<FColor> &&Pixels, FString &OutError);
	bool Collect(FResult &OutResult, bool bWait = false);
	int32 GetPendingCount() const;
	int64 GetReservedFileBytes() const;
	/** May only change admission while the writer has no queued work. */
	bool SetSharedBudget(TSharedPtr<FAnimationCaptureBudget, ESPMode::ThreadSafe> SharedBudget);

  private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
