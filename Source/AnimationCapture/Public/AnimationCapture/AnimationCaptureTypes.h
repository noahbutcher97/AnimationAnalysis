#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class AActor;
class USkeletalMeshComponent;
class USceneComponent;

/** Explicit, fixed enrollment. Id is an opaque safe label serialized as legacy 'role'. */
struct ANIMATIONCAPTURE_API FAnimationCaptureSubject
{
	FString Id;
	TWeakObjectPtr<AActor> Actor;
	/** No mesh discovery or skeleton defaults. Destroyed enrollment remains missing. */
	TWeakObjectPtr<USkeletalMeshComponent> Mesh;
	TArray<FName> Points;
	TMap<FName, TWeakObjectPtr<USceneComponent>> PointSources;
};

struct ANIMATIONCAPTURE_API FAnimationCaptureSettings
{
	FString Scenario;
	/** Caller-owned output root. Each session creates its own unique child directory. */
	FString OutputRoot;
	double SampleHz = 60.0;
	double FrameHz = 5.0;
	/** Explicit opt-in: D3D11 single perspective view, no AA/scaling. Legacy synchronous default is unchanged. */
	bool bUseAsyncReadback = false;
	/** Async-only opt-in: force full resolution on the enrolled viewport while the session is active.
	 * Does not disable AA; the caller must configure a supported view explicitly. */
	bool bUseAsyncDiagnosticResolution = false;
	double MaxWallSeconds = 60.0;
	int32 MaxSamples = 7200;
	int32 MaxFrames = 600;
	int64 MaxDataBytes = 512ll * 1024 * 1024;
	TMap<FString, FString> Metadata;
};

struct ANIMATIONCAPTURE_API FAnimationCaptureTextArtifact
{
	/** One relative filename; cannot replace the recorder's manifest or streams. */
	FString File;
	FString Text;
};

/** Optional producer-specific observations. Calls are synchronous on the game thread.
 * The session holds a shared reference through finalization. Begin failure must leave
 * no acquired resources; every successful Begin gets exactly one End, including
 * startup manifest failure. End releases resources even when export will fail.
 * Returned fields are additions only: collisions invalidate evidence without replacing
 * recorder fields. Do not retain/mutate returned JSON or reenter the session in a callback.
 */
class ANIMATIONCAPTURE_API IAnimationCaptureExtension
{
  public:
	virtual ~IAnimationCaptureExtension() = default;
	virtual bool Begin(TConstArrayView<FAnimationCaptureSubject> Subjects, FString &Error) = 0;
	virtual void Collect()
	{
	}
	virtual TSharedPtr<FJsonObject> ObserveSubject(int32 Index) const
	{
		return nullptr;
	}
	virtual TSharedPtr<FJsonObject> DescribeSubject(int32 Index) const
	{
		return nullptr;
	}
	virtual TSharedPtr<FJsonObject> DescribeSession() const
	{
		return nullptr;
	}
	virtual void End(TArray<FAnimationCaptureTextArtifact> &Artifacts) = 0;
};
