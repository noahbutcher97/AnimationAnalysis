#include "Misc/AutomationTest.h"

#if __has_include("AnimationCapture/AnimationCaptureReadback.h")
#include "AnimationCapture/AnimationCaptureReadback.h"
#include "AnimationCapture/ViewportAsyncCapture.h"
#include "DynamicRHI.h"
#include "Misc/ScopeLock.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#define ANIMATION_CAPTURE_HAS_READBACK 1
#else
#define ANIMATION_CAPTURE_HAS_READBACK 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationCaptureReadbackAdmissionTest,
	"AnimationAnalysis.Capture.Portability.ReadbackAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAnimationCaptureReadbackAdmissionTest::RunTest(const FString&)
{
#if ANIMATION_CAPTURE_HAS_READBACK
	FAnimationCaptureReadbackLimits Limits;
	Limits.MaxPendingRequests = 1;
	Limits.MaxReservedBytes = 53 * 16;
	FAnimationCaptureReadbackProducer Producer(Limits);
	FAnimationCaptureReadbackRequest Request;
	Request.SessionId = TEXT("AdmissionControl");
	Request.RequestId = 7;
	Request.ViewportId = TEXT("ExplicitViewport");
	Request.ClockId = TEXT("FixtureClock");
	Request.AcquisitionTimeSeconds = 1.25;
	Request.Size = FIntPoint(4, 4);
	FAnimationCaptureReadbackTicket Ticket, Rejected;
	FString Error;
	const auto Channels = EAnimationCaptureReadbackChannels::RGB | EAnimationCaptureReadbackChannels::Depth;
	TestTrue(TEXT("One request fits exact combined reservation"), Producer.Request(Request, Channels, Ticket, Error));
	TestEqual(TEXT("Packed output, staging and decoded arrays stay charged"), Producer.GetStats().ReservedBytes, int64(53 * 16));
	Request.RequestId = 8;
	TestFalse(TEXT("Capacity rejects before a second acquisition"), Producer.Request(Request, Channels, Rejected, Error));
	TestEqual(TEXT("Rejection is counted"), Producer.GetStats().Rejected, uint64(1));
	TestTrue(TEXT("Pending admission cancels"), Producer.Cancel(Ticket, TEXT("fixture_cancelled")));
	TestFalse(TEXT("Cancellation happens once"), Producer.Cancel(Ticket, TEXT("duplicate")));
	FAnimationCaptureReadbackResult Result;
	TestTrue(TEXT("Cancelled request has a terminal result"), Producer.Collect(Result));
	TestEqual(TEXT("Request metadata was copied at admission"), Result.Request.RequestId, uint64(7));
	TestTrue(TEXT("Cancellation is distinct from failure"), Result.Status == EAnimationCaptureReadbackStatus::Cancelled);
	TestFalse(TEXT("Terminal result is delivered once"), Producer.Collect(Result));
	TestEqual(TEXT("Unsubmitted collected cancellation releases reservation"), Producer.GetStats().ReservedBytes, int64(0));
	TestTrue(TEXT("Capacity is reusable after retirement"), Producer.Request(Request, Channels, Ticket, Error));
	{
		FViewportAsyncCapture MissingViewport(nullptr, nullptr);
		Rejected = Ticket;
		TestFalse(TEXT("Missing viewport rejects surface admission"), MissingViewport.Request(Request, Rejected, Error));
		TestFalse(TEXT("Surface preflight rejection clears an earlier ticket"), Rejected.IsValid());
		TestEqual(TEXT("Surface preflight rejection is counted"), MissingViewport.GetStats().Rejected, uint64(1));
		Rejected = Ticket;
		TestFalse(TEXT("Missing viewport rejects RGB admission"), MissingViewport.CaptureRGB(Request, Rejected, Error));
		TestFalse(TEXT("RGB preflight rejection clears an earlier ticket"), Rejected.IsValid());
		TestEqual(TEXT("Both preflight rejections are counted once"), MissingViewport.GetStats().Rejected, uint64(2));
		TestEqual(TEXT("Preflight rejection acquires no payload capacity"), MissingViewport.GetStats().ReservedBytes, int64(0));
		TestEqual(TEXT("Clearing a caller ticket does not cancel its prior owner"), Producer.GetStats().Cancelled, uint64(1));
		TestEqual(TEXT("Prior owner's pending admission remains charged"), Producer.GetStats().PendingRequests, 1);
	}
	Producer.Shutdown();
	TestTrue(TEXT("Shutdown emits cancellation"), Producer.Collect(Result));
	TestEqual(TEXT("Shutdown releases its resource reservation after collection"), Producer.GetStats().ReservedBytes, int64(0));
	TestFalse(TEXT("Shutdown rejects new work"), Producer.Request(Request, Channels, Rejected, Error));
#else
	AddError(TEXT("The bounded reusable AnimationCaptureReadback producer is not implemented"));
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationCaptureReadbackByteLimitTest,
	"AnimationAnalysis.Capture.Portability.ReadbackByteLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAnimationCaptureReadbackByteLimitTest::RunTest(const FString&)
{
#if ANIMATION_CAPTURE_HAS_READBACK
	FAnimationCaptureReadbackLimits Limits;
	Limits.MaxPendingRequests = 8;
	Limits.MaxReservedBytes = 53 * 16 - 1;
	FAnimationCaptureReadbackProducer Producer(Limits);
	FAnimationCaptureReadbackRequest Request;
	Request.SessionId = TEXT("ByteControl");
	Request.RequestId = 1;
	Request.ViewportId = TEXT("ExplicitViewport");
	Request.ClockId = TEXT("FixtureClock");
	Request.Size = FIntPoint(4, 4);
	FAnimationCaptureReadbackTicket Ticket;
	FString Error;
	TestFalse(TEXT("One byte under the required reservation rejects"), Producer.Request(Request,
		EAnimationCaptureReadbackChannels::RGB | EAnimationCaptureReadbackChannels::Depth, Ticket, Error));
	TestEqual(TEXT("Rejected allocation uses no bytes"), Producer.GetStats().ReservedBytes, int64(0));
	Request.Size = FIntPoint(MAX_int32, MAX_int32);
	TestFalse(TEXT("Overflow-sized dimensions reject before multiplication"), Producer.Request(Request,
		EAnimationCaptureReadbackChannels::RGB, Ticket, Error));
	Request.Size = FIntPoint::ZeroValue;
	TestFalse(TEXT("Empty dimensions reject"), Producer.Request(Request, EAnimationCaptureReadbackChannels::RGB, Ticket, Error));
#else
	AddError(TEXT("The bounded reusable AnimationCaptureReadback producer is not implemented"));
#endif
	return !HasAnyErrors();
}

#if ANIMATION_CAPTURE_HAS_READBACK
namespace
{
class FReadbackTimeoutControl : public IAutomationLatentCommand
{
public:
	explicit FReadbackTimeoutControl(FAutomationTestBase* InTest) : Test(InTest) {}
	bool Update() override
	{
		if (!Producer)
		{
			FAnimationCaptureReadbackLimits Limits;
			Limits.TimeoutSeconds = 0.005;
			Limits.MaxPendingRequests = 1;
			Limits.MaxReservedBytes = 12 * 16;
			Producer = MakeUnique<FAnimationCaptureReadbackProducer>(Limits);
			FAnimationCaptureReadbackRequest Request;
			Request.SessionId = TEXT("MissingDrawControl");
			Request.RequestId = 1;
			Request.ViewportId = TEXT("NeverDrawn");
			Request.ClockId = TEXT("FixtureClock");
			Request.Size = FIntPoint(4, 4);
			FString Error;
			Test->TestTrue(TEXT("Missing-draw request admitted"), Producer->Request(Request,
				EAnimationCaptureReadbackChannels::RGB, Ticket, Error));
			Started = FPlatformTime::Seconds();
			return false;
		}
		Producer->Pump();
		FAnimationCaptureReadbackResult Result;
		if (Producer->Collect(Result))
		{
			Test->TestTrue(TEXT("Missing acquisition is explicitly timed out"), Result.Status == EAnimationCaptureReadbackStatus::TimedOut);
			Test->TestFalse(TEXT("No renderer identity is invented"), Result.bHasView);
			Test->TestTrue(TEXT("No image is invented"), Result.RGB.IsEmpty());
			Test->TestEqual(TEXT("Timeout count is exact"), Producer->GetStats().TimedOut, uint64(1));
			Test->TestEqual(TEXT("Unsubmitted timeout releases payload budget"), Producer->GetStats().ReservedBytes, int64(0));
			Test->TestEqual(TEXT("Peak reservation stayed within the bound"), Producer->GetStats().PeakReservedBytes, int64(12 * 16));
			Test->TestFalse(TEXT("Timeout is delivered once"), Producer->Collect(Result));
			Test->TestFalse(TEXT("Cancellation cannot replace timeout"), Producer->Cancel(Ticket, TEXT("late_cancel")));
			Producer->Shutdown();
			return true;
		}
		if (FPlatformTime::Seconds() - Started > 2)
		{
			Test->AddError(TEXT("Missing-draw timeout was not delivered"));
			Producer->Shutdown();
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TUniquePtr<FAnimationCaptureReadbackProducer> Producer;
	FAnimationCaptureReadbackTicket Ticket;
	double Started = 0;
};

struct FReadbackRGBReference
{
	FCriticalSection Mutex;
	TArray<FColor> Pixels;
};

class FReadbackRGBControl : public IAutomationLatentCommand
{
public:
	explicit FReadbackRGBControl(FAutomationTestBase* InTest, bool bInTenBit = false)
		: Test(InTest), Started(FPlatformTime::Seconds()), bTenBit(bInTenBit) {}
	bool Update() override
	{
		if (!Producer)
		{
			Producer = MakeUnique<FAnimationCaptureReadbackProducer>();
			FAnimationCaptureReadbackRequest Request;
			Request.SessionId = TEXT("RGBByteControl");
			Request.RequestId = 101;
			Request.ViewportId = TEXT("SyntheticTexture");
			Request.ClockId = TEXT("FixtureClock");
			Request.AcquisitionTimeSeconds = 12.5;
			Request.ExpectedEngineFrame = GFrameCounter;
			Request.Size = bTenBit ? FIntPoint(1024, 1) : FIntPoint(3, 2);
			FAnimationCaptureReadbackTicket Ticket;
			FString Error;
			if (!Test->TestTrue(TEXT("RGB request admitted"), Producer->Request(Request, EAnimationCaptureReadbackChannels::RGB, Ticket, Error))) { return true; }
			FAnimationCaptureReadbackView View;
			View.EngineFrame = Request.ExpectedEngineFrame;
			View.RendererFrameNumber = 23;
			View.ViewKey = 45;
			View.WorldTimeSeconds = 5.5; View.RealTimeSeconds = 6.5;
			View.Rect = FIntRect(FIntPoint::ZeroValue, Request.Size);
			ExpectedFrame = View.EngineFrame;
			ENQUEUE_RENDER_COMMAND(AnimationCaptureRGBFixture)([Ticket, View, TenBit = bTenBit, Reference = Reference](FRHICommandListImmediate& Cmd)
			{
				const FColor Pixels[] = {FColor(1, 2, 3), FColor(64, 128, 192), FColor(255, 0, 1),
					FColor(17, 31, 63), FColor(127, 128, 129), FColor(254, 253, 252)};
				const FIntPoint Size = View.Rect.Size();
				const auto Desc = FRHITextureCreateDesc::Create2D(TEXT("AnimationCaptureRGBFixture"), Size,
					TenBit ? PF_A2B10G10R10 : PF_B8G8R8A8)
					.SetFlags(ETextureCreateFlags::ShaderResource | (TenBit ? ETextureCreateFlags::None : ETextureCreateFlags::SRGB))
					.SetInitialState(ERHIAccess::SRVCompute);
				FTextureRHIRef Texture = Cmd.CreateTexture(Desc);
				TArray<uint32> Packed10;
				if (TenBit)
				{
					Packed10.SetNumUninitialized(1024);
					// Exercise every ten-bit value in every channel, including all rounding boundaries.
					for (uint32 Index = 0; Index < 1024; ++Index)
					{ Packed10[Index] = Index | (((Index * 37) % 1024) << 10) | ((1023 - Index) << 20) | ((Index % 4) << 30); }
				}
				const uint8* SourceBytes = TenBit ? reinterpret_cast<const uint8*>(Packed10.GetData()) : reinterpret_cast<const uint8*>(Pixels);
				Cmd.UpdateTexture2D(Texture, 0, FUpdateTextureRegion2D(0, 0, 0, 0, Size.X, Size.Y), Size.X * 4, SourceBytes);
				if (TenBit)
				{
					TArray<FColor> Expected;
					// Explicit synchronous engine reference belongs only to this correctness control.
					Cmd.Transition(FRHITransitionInfo(Texture, ERHIAccess::SRVCompute, ERHIAccess::CopySrc));
					Cmd.ReadSurfaceData(Texture, View.Rect, Expected, FReadSurfaceDataFlags());
					Cmd.Transition(FRHITransitionInfo(Texture, ERHIAccess::CopySrc, ERHIAccess::SRVCompute));
					for (auto& Color : Expected) { Color.A = 255; }
					FScopeLock Lock(&Reference->Mutex); Reference->Pixels = MoveTemp(Expected);
				}
				FString Failure;
				if (FAnimationCaptureReadbackProducer::BindView_RenderThread(Ticket, View, Failure))
				{
					FAnimationCaptureReadbackProducer::EnqueueRGB_RenderThread(Cmd, Ticket, Texture, View.EngineFrame, Failure);
				}
			});
			return false;
		}
		// Withhold polling for several real engine ticks; completion cannot refresh the original identity.
		if (++Ticks < 4) { return false; }
		Producer->Pump();
		FAnimationCaptureReadbackResult Result;
		if (Producer->Collect(Result))
		{
			Test->TestTrue(*FString::Printf(TEXT("RGB completes: %s"), *Result.Error), Result.Status == EAnimationCaptureReadbackStatus::Completed);
			Test->TestEqual(TEXT("Delayed completion retains renderer frame"), Result.View.EngineFrame, ExpectedFrame);
			Test->TestEqual(TEXT("Delayed completion retains supplied clock time"), Result.Request.AcquisitionTimeSeconds, 12.5);
			Test->TestEqual(TEXT("Renderer world time remains separate and immutable"), Result.View.WorldTimeSeconds, 5.5);
			Test->TestEqual(TEXT("Renderer real time remains separate and immutable"), Result.View.RealTimeSeconds, 6.5);
			if (bTenBit)
			{
				FScopeLock Lock(&Reference->Mutex);
				Test->TestEqual(TEXT("Engine reference covers all 1024 values"), Reference->Pixels.Num(), 1024);
				Test->TestTrue(TEXT("All RGB10A2 values match engine D3D11 screenshot quantization"), Result.RGB == Reference->Pixels);
				Test->TestTrue(TEXT("Source format is reported truthfully"), Result.RGBSourceFormat == PF_A2B10G10R10);
			}
			else
			{
				const TArray<FColor> Expected = {FColor(1, 2, 3), FColor(64, 128, 192), FColor(255, 0, 1),
					FColor(17, 31, 63), FColor(127, 128, 129), FColor(254, 253, 252)};
				Test->TestTrue(TEXT("Raw BGRA bytes survive sRGB source and GPU packing"), Result.RGB == Expected);
			}
			Test->TestEqual(TEXT("Collected result releases reservation"), Producer->GetStats().ReservedBytes, int64(0));
			Producer->Shutdown();
			return true;
		}
		if (FPlatformTime::Seconds() - Started > 10)
		{
			Test->AddError(TEXT("RGB readback did not complete within ten seconds"));
			Producer->Shutdown();
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TUniquePtr<FAnimationCaptureReadbackProducer> Producer;
	double Started;
	bool bTenBit;
	TSharedRef<FReadbackRGBReference, ESPMode::ThreadSafe> Reference = MakeShared<FReadbackRGBReference, ESPMode::ThreadSafe>();
	uint64 ExpectedFrame = 0;
	int32 Ticks = 0;
};
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationCaptureReadbackTimeoutTest,
	"AnimationAnalysis.Capture.Portability.ReadbackTimeout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAnimationCaptureReadbackTimeoutTest::RunTest(const FString&)
{
#if ANIMATION_CAPTURE_HAS_READBACK
	ADD_LATENT_AUTOMATION_COMMAND(FReadbackTimeoutControl(this));
#else
	AddError(TEXT("The bounded reusable AnimationCaptureReadback producer is not implemented"));
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationCaptureReadbackRGBTest,
	"AnimationAnalysis.Capture.Rendered.ReadbackRGBBytes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAnimationCaptureReadbackRGBTest::RunTest(const FString&)
{
#if ANIMATION_CAPTURE_HAS_READBACK
	if (!GDynamicRHI || FString(GDynamicRHI->GetName()) != TEXT("D3D11"))
	{
		AddError(TEXT("ReadbackRGBBytes requires rendered D3D11; a headless run is not a rendered pass"));
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FReadbackRGBControl(this));
#else
	AddError(TEXT("The bounded reusable AnimationCaptureReadback producer is not implemented"));
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationCaptureReadbackRGB10BitTest,
	"AnimationAnalysis.Capture.Rendered.ReadbackRGB10Bit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAnimationCaptureReadbackRGB10BitTest::RunTest(const FString&)
{
#if ANIMATION_CAPTURE_HAS_READBACK
	if (!GDynamicRHI || FString(GDynamicRHI->GetName()) != TEXT("D3D11"))
	{
		AddError(TEXT("ReadbackRGB10Bit requires rendered D3D11; a headless run is not a rendered pass"));
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FReadbackRGBControl(this, true));
#else
	AddError(TEXT("The bounded reusable AnimationCaptureReadback producer is not implemented"));
#endif
	return !HasAnyErrors();
}
