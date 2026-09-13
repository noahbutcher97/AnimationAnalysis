#include "Misc/AutomationTest.h"

#include "AnimationCapture/AnimationCaptureBudget.h"
#include "AnimationCapture/AnimationCaptureImageWriter.h"
#include "AnimationCapture/AnimationCaptureReadback.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationCaptureSharedImageBudgetTest,
	"AnimationAnalysis.Capture.Portability.SharedImageBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAnimationCaptureSharedImageBudgetTest::RunTest(const FString&)
{
	FAnimationCaptureBudgetLimits BudgetLimits;
	BudgetLimits.MaxReservations = 1;
	BudgetLimits.MaxBytes = 1024 * 1024;
	auto Budget = MakeShared<FAnimationCaptureBudget, ESPMode::ThreadSafe>(BudgetLimits);
	FAnimationCaptureReadbackLimits ReadbackLimits;
	ReadbackLimits.MaxPendingRequests = 2;
	ReadbackLimits.MaxReservedBytes = 1024 * 1024;
	FAnimationCaptureReadbackProducer Producer(ReadbackLimits, Budget);
	FAnimationCaptureReadbackRequest Request;
	Request.SessionId = TEXT("SharedImageBudget");
	Request.ViewportId = TEXT("ExplicitViewport");
	Request.ClockId = TEXT("FixtureClock");
	Request.RequestId = 1;
	Request.Size = FIntPoint(4, 4);
	FAnimationCaptureReadbackTicket Ticket, Rejected;
	FString Error;
	TestTrue(TEXT("Image request acquires the shared reservation"), Producer.Request(Request,
		EAnimationCaptureReadbackChannels::RGB, Ticket, Error));
	TestEqual(TEXT("One image reservation is live"), Budget->GetStats().LiveReservations, 1);
	Request.RequestId = 2;
	TestFalse(TEXT("Shared reservation capacity rejects another producer admission"), Producer.Request(Request,
		EAnimationCaptureReadbackChannels::RGB, Rejected, Error));
	TestEqual(TEXT("Shared rejection is recorded"), Budget->GetStats().Rejected, uint64(1));
	TestTrue(TEXT("Admitted image can be cancelled"), Producer.Cancel(Ticket, TEXT("fixture cancellation")));
	FAnimationCaptureReadbackResult Result;
	TestTrue(TEXT("Cancellation is collected"), Producer.Collect(Result));
	TestEqual(TEXT("Collected image arrays retain their lease"), Budget->GetStats().LiveReservations, 1);
	Producer.Shutdown();
	TestEqual(TEXT("Producer retirement does not release a result-owned lease"), Budget->GetStats().LiveReservations, 1);
	Result = FAnimationCaptureReadbackResult{};
	TestEqual(TEXT("Releasing the collected result releases shared admission"), Budget->GetStats().LiveReservations, 0);
	TestEqual(TEXT("Peak reservation count is exact"), Budget->GetStats().PeakReservations, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationCaptureSharedPngBudgetTest,
	"AnimationAnalysis.Capture.Portability.SharedPngBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAnimationCaptureSharedPngBudgetTest::RunTest(const FString&)
{
	const FIntPoint Size(2, 2);
	const int64 Bytes = FAnimationCaptureImageWriter::FileByteReservation(Size) + int64(Size.X) * Size.Y * 4;
	FAnimationCaptureBudgetLimits Limits;
	Limits.MaxReservations = 1;
	Limits.MaxBytes = Bytes;
	auto Budget = MakeShared<FAnimationCaptureBudget, ESPMode::ThreadSafe>(Limits);
	FAnimationCaptureImageWriter Writer(Budget);
	TArray<FColor> Pixels;
	Pixels.Init(FColor::Red, Size.X * Size.Y);
	const FString Directory = FPaths::ProjectSavedDir() / TEXT("Observations") /
		(TEXT("SharedPngBudget-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString First = Directory / TEXT("admitted.png");
	FString Error;
	TestTrue(TEXT("PNG reserves before accepting pixel ownership"), Writer.Enqueue(First, Size, MoveTemp(Pixels), Error));
	TestEqual(TEXT("PNG work keeps one shared lease"), Budget->GetStats().LiveReservations, 1);
	TArray<FColor> SecondPixels;
	SecondPixels.Init(FColor::Blue, Size.X * Size.Y);
	TestFalse(TEXT("Concurrent PNG admission is rejected by the shared budget"), Writer.Enqueue(
		Directory / TEXT("rejected.png"), Size, MoveTemp(SecondPixels), Error));
	TestEqual(TEXT("Rejected PNG retains caller pixels"), SecondPixels.Num(), Size.X * Size.Y);
	FAnimationCaptureImageWriter::FResult Result;
	TestTrue(TEXT("PNG async work completes"), Writer.Collect(Result, true));
	TestTrue(TEXT("PNG completion reports no error"), Result.Error.IsEmpty());
	TestEqual(TEXT("Collection releases the PNG lease"), Budget->GetStats().LiveReservations, 0);
	AddInfo(TEXT("SHARED_PNG_BUDGET_OUTPUT=") + Directory);
	return !HasAnyErrors();
}
