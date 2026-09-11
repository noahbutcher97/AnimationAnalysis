// Copyright Epic Games, Inc. All Rights Reserved.
#include "AnimationCapture/ViewportSurfaceCapture.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "LegacyScreenPercentageDriver.h"
#include "Misc/ScopeLock.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "SceneRenderTargetParameters.h"
#include "SceneViewExtension.h"
#include "UObject/UObjectIterator.h"
#include "UnrealClient.h"
#include <limits>

namespace { TSet<TWeakObjectPtr<UPrimitiveComponent>> OwnedLabelComponents; }

struct FScopedSurfaceCaptureLabels::FImpl
{
	struct FPrevious { TWeakObjectPtr<UPrimitiveComponent> Component; bool Enabled; int32 Value; ERendererStencilMask Mask; };
	TArray<FPrevious> Previous;
};
FScopedSurfaceCaptureLabels::FScopedSurfaceCaptureLabels() : Impl(MakeUnique<FImpl>()) {}
FScopedSurfaceCaptureLabels::~FScopedSurfaceCaptureLabels() { Restore(); }
bool FScopedSurfaceCaptureLabels::Apply(UWorld* World, TConstArrayView<FSurfaceCaptureLabel> Labels, FString& Error)
{
	Error.Reset();
	if (!World || Labels.IsEmpty() || !Impl->Previous.IsEmpty()) { Error = TEXT("Labels require a world, subjects and an unused scope"); return false; }
	TSet<uint8> Ids; TSet<FString> Names; TSet<UPrimitiveComponent*> Components;
	for (const auto& Label : Labels)
	{
		UPrimitiveComponent* Component = Label.Component.Get();
		if (!Component || Component->GetWorld() != World || !Component->IsRegistered() || Label.Id == 0
			|| Label.Name.IsEmpty() || Ids.Contains(Label.Id) || Names.Contains(Label.Name) || Components.Contains(Component)
			|| OwnedLabelComponents.Contains(Component))
		{ Error = TEXT("Labels require unique nonzero IDs, names and registered components in the same world"); return false; }
		Ids.Add(Label.Id); Names.Add(Label.Name); Components.Add(Component);
	}
	// Existing labels can alias a subject in the raster. Reject the collision instead of relabelling unrelated objects.
	for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
	{
		if (It->GetWorld() == World && It->bRenderCustomDepth && !Components.Contains(*It) && Ids.Contains(It->CustomDepthStencilValue))
		{ Error = TEXT("A requested label is already used by another primitive"); return false; }
	}
	for (const auto& Label : Labels)
	{
		UPrimitiveComponent* Component = Label.Component.Get();
		Impl->Previous.Add({Component, bool(Component->bRenderCustomDepth), Component->CustomDepthStencilValue, Component->CustomDepthStencilWriteMask});
		OwnedLabelComponents.Add(Component);
		Component->SetCustomDepthStencilValue(Label.Id);
		Component->SetCustomDepthStencilWriteMask(ERendererStencilMask::ERSM_Default);
		Component->SetRenderCustomDepth(true);
	}
	return true;
}
void FScopedSurfaceCaptureLabels::Restore()
{
	for (const auto& Previous : Impl->Previous)
	{
		OwnedLabelComponents.Remove(Previous.Component);
		if (UPrimitiveComponent* Component = Previous.Component.Get())
		{
			Component->SetRenderCustomDepth(Previous.Enabled);
			Component->SetCustomDepthStencilValue(Previous.Value);
			Component->SetCustomDepthStencilWriteMask(Previous.Mask);
		}
	}
	Impl->Previous.Reset();
}

namespace
{
constexpr int64 MaximumSurfacePixels = 1920ll * 1080;
struct FSurfaceReadState
{
	FCriticalSection Mutex;
	bool Requested = false, InFlight = false;
	TUniquePtr<FViewportSurfaceFrame> Result;
};
BEGIN_SHADER_PARAMETER_STRUCT(FSurfaceReadParameters, )
	RDG_TEXTURE_ACCESS(SceneDepth, ERHIAccess::CopySrc)
	RDG_TEXTURE_ACCESS(LabelDepth, ERHIAccess::CopySrc)
END_SHADER_PARAMETER_STRUCT()

// Explicitly decode the locally supported D3D11 D32F/S8 layout. Never apply RHI screenshot depth normalization.
bool ReadDepth(FRHICommandListImmediate& Cmd, FRHITexture* Texture, FIntRect Rect, FVector4f Transform,
	TArray<float>& Depth, TArray<uint8>* Labels)
{
	FRHIGPUTextureReadback Readback(TEXT("ViewportSurfaceObservation"));
	Readback.EnqueueCopy(Cmd, Texture);
	Cmd.BlockUntilGPUIdle();
	int32 Pitch = 0, Height = 0;
	const uint8* Bytes = static_cast<const uint8*>(Readback.Lock(Pitch, &Height));
	if (!Bytes || Pitch < Rect.Max.X || Height < Rect.Max.Y) { if (Bytes) { Readback.Unlock(); } return false; }
	Depth.SetNumUninitialized(Rect.Area());
	if (Labels) { Labels->SetNumUninitialized(Rect.Area()); }
	for (int32 Y = 0; Y < Rect.Height(); ++Y)
	{
		for (int32 X = 0; X < Rect.Width(); ++X)
		{
			const uint8* Pixel = Bytes + (int64(Y + Rect.Min.Y) * Pitch + X + Rect.Min.X) * 8;
			float DeviceZ; FMemory::Memcpy(&DeviceZ, Pixel, sizeof(float));
			const float Denominator = DeviceZ * Transform.Z - Transform.W;
			Depth[Y * Rect.Width() + X] = DeviceZ > 0 && Denominator > 0
				? DeviceZ * Transform.X + Transform.Y + 1.0f / Denominator : std::numeric_limits<float>::infinity();
			if (Labels) { (*Labels)[Y * Rect.Width() + X] = Pixel[4]; }
		}
	}
	Readback.Unlock(); return true;
}

class FSurfaceViewExtension : public FSceneViewExtensionBase
{
public:
	FSurfaceViewExtension(const FAutoRegister& AutoRegister, FViewport* InViewport,
		TSharedRef<FSurfaceReadState, ESPMode::ThreadSafe> InState, bool bInDiagnosticResolution)
		: FSceneViewExtensionBase(AutoRegister), Viewport(InViewport), State(InState), bDiagnosticResolution(bInDiagnosticResolution) {}
	void SetupViewFamily(FSceneViewFamily&) override {}
	void SetupView(FSceneViewFamily&, FSceneView&) override {}
	void BeginRenderViewFamily(FSceneViewFamily& Family) override
	{
		FScopeLock Lock(&State->Mutex);
		if (bDiagnosticResolution && State->Requested && Family.RenderTarget == Viewport)
		{
			// PIE forces the screen-percentage flag after client setup. Apply the caller's
			// explicit diagnostic policy on this view family only, before renderer setup.
			// Replace the family-owned driver as well: forking a scaled legacy driver
			// after disabling its flag violates the driver's constructor contract.
			const ISceneViewFamilyScreenPercentage* Previous = Family.GetScreenPercentageInterface();
			Family.SetScreenPercentageInterface_Unchecked(new FLegacyScreenPercentageDriver(Family, 1.0f));
			delete Previous;
			Family.EngineShowFlags.SetScreenPercentage(false);
			Family.SecondaryViewFraction = 1;
		}
	}
	bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override { return Context.Viewport == Viewport; }
	void PostRenderView_RenderThread(FRDGBuilder& Graph, FSceneView& View) override
	{
		if (View.Family->RenderTarget != Viewport) { return; }
		{
			FScopeLock Lock(&State->Mutex);
			if (!State->Requested) { return; }
			State->Requested = false;
		}
		auto Frame = MakeShared<FViewportSurfaceFrame, ESPMode::ThreadSafe>();
		Frame->EngineFrame = View.Family->FrameCounter;
		Frame->Size = View.UnscaledViewRect.Size();
		Frame->WorldToClip = View.ViewMatrices.GetViewProjectionMatrix();
		Frame->DeviceZTransform = View.InvDeviceZToWorldZTransform;
		const FIntRect Rect(FIntPoint::ZeroValue, View.Family->RenderTarget->GetSizeXY());
		if (View.Family->Views.Num() != 1 || View.UnscaledViewRect != Rect || Rect.Area() > MaximumSurfacePixels
			|| Rect.Area() <= 0 || !View.IsPerspectiveProjection() || View.AntiAliasingMethod != AAM_None
			|| View.Family->EngineShowFlags.ScreenPercentage || !FMath::IsNearlyEqual(View.Family->SecondaryViewFraction, 1.0f))
		{
			Frame->Error = FString::Printf(TEXT("Unsupported surface view: views=%d view=%s target=%s perspective=%d AA=%d screen_percentage=%d secondary_fraction=%g"),
				View.Family->Views.Num(), *View.UnscaledViewRect.ToString(), *Rect.ToString(), View.IsPerspectiveProjection(),
				int32(View.AntiAliasingMethod), bool(View.Family->EngineShowFlags.ScreenPercentage), View.Family->SecondaryViewFraction);
			Complete(*Frame); return;
		}
		const auto Textures = CreateSceneTextureUniformBuffer(Graph, View, ESceneTextureSetupMode::All);
		FRDGTextureRef SceneDepth = Textures->GetContents()->SceneDepthTexture;
		FRDGTextureRef LabelDepth = Textures->GetContents()->CustomDepthTexture;
		for (FRDGTextureRef Texture : {SceneDepth, LabelDepth})
		{
			// D3D11 reports five logical bytes for D32F/S8, while staging rows use
			// the physical eight-byte R32G8X24 layout (D3D11Device/RenderTarget.cpp).
			if (!Texture || Texture->Desc.Format != PF_DepthStencil || GPixelFormats[Texture->Desc.Format].BlockBytes != 5
				|| GPixelFormats[Texture->Desc.Format].bIs24BitUnormDepthStencil
				|| Texture->Desc.NumSamples != 1 || Texture->Desc.Extent.X < Rect.Max.X || Texture->Desc.Extent.Y < Rect.Max.Y
				|| int64(Texture->Desc.Extent.X) * Texture->Desc.Extent.Y > MaximumSurfacePixels + 65536)
			{
				Frame->Error = Texture ? FString::Printf(TEXT("Unsupported surface texture %s: format=%s block_bytes=%d samples=%d extent=%s"),
					Texture == SceneDepth ? TEXT("scene") : TEXT("labels"), GPixelFormats[Texture->Desc.Format].Name,
					GPixelFormats[Texture->Desc.Format].BlockBytes, Texture->Desc.NumSamples, *Texture->Desc.Extent.ToString())
					: TEXT("Missing surface texture");
				Complete(*Frame); return;
			}
		}
		auto* Parameters = Graph.AllocParameters<FSurfaceReadParameters>();
		Parameters->SceneDepth = SceneDepth; Parameters->LabelDepth = LabelDepth;
		const auto SharedState = State;
		Graph.AddPass(RDG_EVENT_NAME("ViewportSurfaceReadback"), Parameters, ERDGPassFlags::Readback,
			[Frame, SharedState, SceneDepth, LabelDepth, Rect](FRHICommandListImmediate& Cmd)
			{
				const double Begin = FPlatformTime::Seconds();
				if (!ReadDepth(Cmd, SceneDepth->GetRHI(), Rect, Frame->DeviceZTransform, Frame->SceneDepthCm, nullptr)
					|| !ReadDepth(Cmd, LabelDepth->GetRHI(), Rect, Frame->DeviceZTransform, Frame->LabelDepthCm, &Frame->Labels))
				{ Frame->Error = TEXT("Raw surface readback failed"); }
				Frame->ReadbackSeconds = FPlatformTime::Seconds() - Begin;
				FScopeLock Lock(&SharedState->Mutex);
				SharedState->Result = MakeUnique<FViewportSurfaceFrame>(MoveTemp(*Frame));
			});
	}
private:
	void Complete(FViewportSurfaceFrame& Frame)
	{
		FScopeLock Lock(&State->Mutex); State->Result = MakeUnique<FViewportSurfaceFrame>(MoveTemp(Frame));
	}
	FViewport* Viewport;
	TSharedRef<FSurfaceReadState, ESPMode::ThreadSafe> State;
	bool bDiagnosticResolution;
};
}

struct FViewportSurfaceCapture::FImpl
{
	TWeakObjectPtr<UWorld> World;
	FViewport* Viewport = nullptr;
	TSharedRef<FSurfaceReadState, ESPMode::ThreadSafe> State = MakeShared<FSurfaceReadState, ESPMode::ThreadSafe>();
	TSharedPtr<FSurfaceViewExtension, ESPMode::ThreadSafe> Extension;
};
FViewportSurfaceCapture::FViewportSurfaceCapture(UWorld* World, FViewport* Viewport, bool bUseDiagnosticResolution) : Impl(MakeUnique<FImpl>())
{
	Impl->World = World; Impl->Viewport = Viewport;
	if (World && Viewport) { Impl->Extension = FSceneViewExtensions::NewExtension<FSurfaceViewExtension>(Viewport, Impl->State, bUseDiagnosticResolution); }
}
FViewportSurfaceCapture::~FViewportSurfaceCapture()
{
	{ FScopeLock Lock(&Impl->State->Mutex); Impl->State->Requested = false; }
	Impl->Extension.Reset(); FlushRenderingCommands();
}
bool FViewportSurfaceCapture::Request(FString& Error)
{
	Error.Reset();
	auto* CustomDepth = IConsoleManager::Get().FindConsoleVariable(TEXT("r.CustomDepth"));
	if (!Impl->World.IsValid() || !Impl->Viewport || !Impl->Viewport->GetClient()
		|| Impl->Viewport->GetClient()->GetWorld() != Impl->World.Get() || !GDynamicRHI
		|| FString(GDynamicRHI->GetName()) != TEXT("D3D11") || !CustomDepth || CustomDepth->GetInt() != 3)
	{ Error = TEXT("Surface request requires its live viewport, D3D11 and r.CustomDepth=3"); return false; }
	FScopeLock Lock(&Impl->State->Mutex);
	if (Impl->State->InFlight) { Error = TEXT("A surface observation is already pending"); return false; }
	Impl->State->InFlight = true; Impl->State->Requested = true; return true;
}
bool FViewportSurfaceCapture::Collect(FViewport* DrawnViewport, uint64 ExpectedFrame, FViewportSurfaceFrame& OutFrame,
	TArray<FColor>& OutRGB, FString& Error)
{
	Error.Reset();
	OutFrame = FViewportSurfaceFrame(); OutRGB.Reset();
	if (DrawnViewport != Impl->Viewport) { Error = TEXT("Surface observation belongs to a different viewport"); return false; }
	{ FScopeLock Lock(&Impl->State->Mutex); if (!Impl->State->InFlight) { return false; } }
	// Screenshot flushes the same draw's queued rendering, including the bounded readback pass.
	if (!GetViewportScreenShot(DrawnViewport, OutRGB)) { Error = TEXT("Surface RGB readback failed"); return false; }
	FScopeLock Lock(&Impl->State->Mutex);
	if (!Impl->State->Result) { return false; }
	OutFrame = MoveTemp(*Impl->State->Result); Impl->State->Result.Reset(); Impl->State->InFlight = false;
	if (!OutFrame.Error.IsEmpty()) { Error = OutFrame.Error; return false; }
	if (OutFrame.EngineFrame != ExpectedFrame || OutFrame.Size != DrawnViewport->GetRenderTargetTextureSizeXY()
		|| OutRGB.Num() != int64(OutFrame.Size.X) * OutFrame.Size.Y)
	{ Error = TEXT("Surface/RGB frame identity or dimensions differ"); return false; }
	for (FColor& Pixel : OutRGB) { Pixel.A = 255; }
	return true;
}
