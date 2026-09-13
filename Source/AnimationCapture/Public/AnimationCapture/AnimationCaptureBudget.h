#pragma once

#include "CoreMinimal.h"

struct FAnimationCaptureBudgetState;

struct ANIMATIONCAPTURE_API FAnimationCaptureBudgetLimits
{
	int32 MaxReservations = 8;
	int64 MaxBytes = 128ll * 1024 * 1024;
};

struct ANIMATIONCAPTURE_API FAnimationCaptureBudgetStats
{
	int32 LiveReservations = 0;
	int32 PeakReservations = 0;
	int64 LiveBytes = 0;
	int64 PeakBytes = 0;
	uint64 Admitted = 0;
	uint64 Rejected = 0;
};

/** One admitted allocation envelope. Destruction releases it safely from any thread. */
class ANIMATIONCAPTURE_API FAnimationCaptureReservation
{
public:
	~FAnimationCaptureReservation();
	int64 Bytes() const { return ReservedBytes; }
private:
	FAnimationCaptureReservation(TSharedRef<FAnimationCaptureBudgetState, ESPMode::ThreadSafe> InState, int64 InBytes);
	TSharedRef<FAnimationCaptureBudgetState, ESPMode::ThreadSafe> State;
	int64 ReservedBytes = 0;
	friend class FAnimationCaptureBudget;
};

/** Explicit shared admission object. It counts reservations rather than logical requests. */
class ANIMATIONCAPTURE_API FAnimationCaptureBudget
{
public:
	explicit FAnimationCaptureBudget(const FAnimationCaptureBudgetLimits& Limits);
	TSharedPtr<FAnimationCaptureReservation, ESPMode::ThreadSafe> Reserve(int64 Bytes);
	FAnimationCaptureBudgetStats GetStats() const;
private:
	TSharedRef<FAnimationCaptureBudgetState, ESPMode::ThreadSafe> State;
};
