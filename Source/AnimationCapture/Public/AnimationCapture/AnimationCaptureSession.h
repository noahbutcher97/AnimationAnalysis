#pragma once

#include "AnimationCapture/AnimationCaptureTypes.h"

class UWorld;
class FViewportClient;

/** Engine-only observational recorder. Game/PIE worlds, explicit subjects and output.
 * All methods run on the game thread. No project discovery, gameplay switches or
 * asset saves. Multiple independent sessions are allowed; extensions own any
 * exclusive producer resources. End-of-world-tick poses and post-draw RGB preserve
 * their original acquisition identities through background image encoding.
 */
class ANIMATIONCAPTURE_API FAnimationCaptureSession
{
  public:
	FAnimationCaptureSession();
	~FAnimationCaptureSession();
	FAnimationCaptureSession(const FAnimationCaptureSession &) = delete;
	FAnimationCaptureSession &operator=(const FAnimationCaptureSession &) = delete;

	bool Start(UWorld *World, const FAnimationCaptureSettings &Settings,
			   TConstArrayView<FAnimationCaptureSubject> Subjects, FString &Error,
			   TSharedPtr<IAnimationCaptureExtension> Extension = nullptr);
	bool Stop(const FString &Reason, FString &Error);
	void Mark(const FString &Label);
	bool IsRecording() const;
	FString GetOutputDirectory() const;
	int32 GetSampleCount() const;
	int32 GetFrameCount() const;
	FString GetStopReason() const;
	static bool IsExpectedViewportClient(const UWorld *World, const FViewportClient *DrawnClient,
										 const FViewportClient *ExpectedClient);

  private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
