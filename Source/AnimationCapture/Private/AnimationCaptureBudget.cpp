#include "AnimationCapture/AnimationCaptureBudget.h"
#include "Misc/ScopeLock.h"

struct FAnimationCaptureBudgetState
{
	FCriticalSection Mutex;
	FAnimationCaptureBudgetLimits Limits;
	FAnimationCaptureBudgetStats Stats;
};

FAnimationCaptureReservation::FAnimationCaptureReservation(
	TSharedRef<FAnimationCaptureBudgetState, ESPMode::ThreadSafe> InState, int64 InBytes)
	: State(MoveTemp(InState)), ReservedBytes(InBytes) {}

FAnimationCaptureReservation::~FAnimationCaptureReservation()
{
	FScopeLock Lock(&State->Mutex);
	check(State->Stats.LiveReservations > 0 && State->Stats.LiveBytes >= ReservedBytes);
	--State->Stats.LiveReservations;
	State->Stats.LiveBytes -= ReservedBytes;
}

FAnimationCaptureBudget::FAnimationCaptureBudget(const FAnimationCaptureBudgetLimits& Limits)
	: State(MakeShared<FAnimationCaptureBudgetState, ESPMode::ThreadSafe>())
{
	State->Limits = Limits;
}

TSharedPtr<FAnimationCaptureReservation, ESPMode::ThreadSafe> FAnimationCaptureBudget::Reserve(int64 Bytes)
{
	FScopeLock Lock(&State->Mutex);
	if (Bytes <= 0 || State->Limits.MaxReservations <= 0 || State->Limits.MaxBytes <= 0
		|| State->Stats.LiveReservations >= State->Limits.MaxReservations
		|| Bytes > State->Limits.MaxBytes - State->Stats.LiveBytes)
	{
		++State->Stats.Rejected;
		return nullptr;
	}
	++State->Stats.LiveReservations;
	State->Stats.LiveBytes += Bytes;
	State->Stats.PeakReservations = FMath::Max(State->Stats.PeakReservations, State->Stats.LiveReservations);
	State->Stats.PeakBytes = FMath::Max(State->Stats.PeakBytes, State->Stats.LiveBytes);
	++State->Stats.Admitted;
	return TSharedPtr<FAnimationCaptureReservation, ESPMode::ThreadSafe>(
		new FAnimationCaptureReservation(State, Bytes));
}

FAnimationCaptureBudgetStats FAnimationCaptureBudget::GetStats() const
{
	FScopeLock Lock(&State->Mutex);
	return State->Stats;
}
