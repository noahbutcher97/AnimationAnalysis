// Copyright Noah Butcher. All Rights Reserved.
#include "AnimationCapture/AnimationCaptureSession.h"
#include "AnimationCapture/AnimationCaptureJson.h"
#include "AnimationCapture/AnimationCaptureImageWriter.h"
#include "AnimationCapture/ViewportAsyncCapture.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "AssetCompilingManager.h"
#include "Containers/Ticker.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Camera/PlayerCameraManager.h"
#include "SceneView.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ShaderCompiler.h"
#include "UnrealClient.h"

namespace
{
using namespace AnimationCaptureJson;

bool ValidRole(const FString &Role)
{
	if (Role.IsEmpty() || Role.Len() > 80)
	{
		return false;
	}
	for (TCHAR C : Role)
	{
		if (!FChar::IsAlnum(C) && C != TEXT('_') && C != TEXT('-'))
		{
			return false;
		}
	}
	return true;
}
} // namespace

struct FAnimationCaptureSession::FImpl
{
	FAnimationCaptureSession *Owner;
	TWeakObjectPtr<UWorld> World;
	FAnimationCaptureSettings Settings;
	TArray<FAnimationCaptureSubject> Participants;
	TArray<FString> ActorPaths, MeshPaths;
	FString Directory, Map, WorldPath, StartedUtc;
	FString StopReason;
	int64 DataBytes = 0;
	bool bDataLimit = false;
	TArray<FString> Errors;
	TUniquePtr<IFileHandle> Samples, Frames, Markers, ReadbackEvents;
	FAnimationCaptureImageWriter ImageWriter;
	TUniquePtr<FViewportAsyncCapture> Readback;
	TMap<uint64, TSharedRef<FJsonObject>> PendingReadbacks;
	TArray<TSharedRef<FJsonObject>> PendingFrames;
	int32 SubmittedFrames = 0, RejectedFrames = 0, FailedFrames = 0, PeakPendingFrames = 0;
	int64 PeakPipelineBytes = 0;
	double ImageFlushSeconds = 0, CaptureWallSeconds = 0;
	FDelegateHandle TickHandle, DrawHandle, CleanupHandle;
	FTSTicker::FDelegateHandle WatchdogHandle;
	bool bRecording = false;
	bool bSaved = false;
	bool bRenderAvailable = false;
	double StartSimulation = 0, StartWall = 0, LastSample = -1, LastFrame = -1;
	int32 SampleCount = 0, FrameCount = 0, MarkerCount = 0, DrawCount = 0, ReadyDraws = 0;
	FString CurrentMarker;
	TSharedPtr<IAnimationCaptureExtension> Extension;
	struct FPoseWitness
	{
		FDelegateHandle Handle;
		uint64 Serial = 0, EngineFrame = 0;
		double SimulationTime = -1;
	};
	TArray<FPoseWitness> Poses;

	explicit FImpl(FAnimationCaptureSession *InOwner) : Owner(InOwner)
	{
	}

	void Error(const FString &Message)
	{
		Errors.AddUnique(Message);
	}

	void MergeFields(const TSharedRef<FJsonObject> &Record, const TSharedPtr<FJsonObject> &Extra)
	{
		if (!Extra)
		{
			return;
		}
		for (const auto &Field : Extra->Values)
		{
			if (Record->HasField(Field.Key))
			{
				Error(TEXT("Extension attempted to replace recorder field: ") + Field.Key);
			}
			else
			{
				Record->SetField(Field.Key, Field.Value);
			}
		}
	}

	void WriteLine(IFileHandle *File, const TSharedRef<FJsonObject> &Object)
	{
		const FTCHARToUTF8 Utf8(*(JsonText(Object) + TEXT("\n")));
		if (DataBytes + ImageWriter.GetReservedFileBytes() + PendingReadbackFileBytes() + Utf8.Length() > Settings.MaxDataBytes)
		{
			bDataLimit = true;
			Error(TEXT("Data byte budget exhausted; evidence is incomplete"));
			return;
		}
		if (!File || !File->Write(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length()))
		{
			Error(TEXT("Stream write failed; evidence is incomplete"));
		}
		else
		{
			DataBytes += Utf8.Length();
		}
	}

	void WriteTextArtifact(const FString &Name, const FString &Text)
	{
		bool bSafeCharacters =
			!Name.IsEmpty() && Name.Len() <= 160 && !Name.StartsWith(TEXT(".")) && !Name.EndsWith(TEXT("."));
		for (TCHAR Character : Name)
		{
			bSafeCharacters &=
				FChar::IsAlnum(Character) || Character == TEXT('_') || Character == TEXT('-') || Character == TEXT('.');
		}
		FString Device = Name;
		int32 Dot;
		if (Name.FindChar(TEXT('.'), Dot))
		{
			Device = Name.Left(Dot);
		}
		Device.ToUpperInline();
		const bool bDeviceName =
			Device == TEXT("CON") || Device == TEXT("PRN") || Device == TEXT("AUX") || Device == TEXT("NUL") ||
			(Device.Len() == 4 && (Device.StartsWith(TEXT("COM")) || Device.StartsWith(TEXT("LPT"))) &&
			 Device[3] >= TEXT('1') && Device[3] <= TEXT('9'));
		if (!bSafeCharacters || bDeviceName || Name.Contains(TEXT("..")) || Name.Contains(TEXT("/")) ||
			Name.Contains(TEXT("\\")) || Name.Contains(TEXT(":")) ||
			Name.Equals(TEXT("session.json"), ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("samples.jsonl"), ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("frames.jsonl"), ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("markers.jsonl"), ESearchCase::IgnoreCase))
		{
			Error(TEXT("Extension artifact must be a separate relative filename"));
			return;
		}
		if (IFileManager::Get().FileExists(*(Directory / Name)))
		{
			Error(TEXT("Extension artifact cannot overwrite an existing file"));
			return;
		}
		const FTCHARToUTF8 Utf8(*Text);
		if (DataBytes + Utf8.Length() > Settings.MaxDataBytes)
		{
			bDataLimit = true;
			Error(TEXT("Extension artifact exceeds data budget"));
			return;
		}
		if (!FFileHelper::SaveStringToFile(Text, *(Directory / Name),
										   FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Error(TEXT("Extension artifact write failed: ") + Name);
			return;
		}
		DataBytes += Utf8.Length();
	}

	bool Watchdog(float)
	{
		if (!bRecording)
		{
			return false;
		}
		CollectFrames();
		if (FailedFrames > 0)
		{
			FString Unused;
			Owner->Stop(TEXT("image_write_failed"), Unused);
			return false;
		}
		if (bDataLimit || FPlatformTime::Seconds() - StartWall >= Settings.MaxWallSeconds)
		{
			FString Unused;
			Owner->Stop(bDataLimit ? TEXT("data_limit_reached") : TEXT("wall_time_limit_reached"), Unused);
			return false;
		}
		return true;
	}

	TSharedRef<FJsonObject> Observation() const
	{
		auto Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("simulation_time_s"), World.IsValid() ? World->GetTimeSeconds() : LastSample);
		Row->SetNumberField(TEXT("wall_elapsed_s"), FPlatformTime::Seconds() - StartWall);
		Row->SetNumberField(TEXT("engine_frame"), static_cast<double>(GFrameCounter));
		Row->SetStringField(TEXT("marker"), CurrentMarker);
		return Row;
	}

	void Tick(UWorld *TickedWorld, ELevelTick, float)
	{
		if (!bRecording || TickedWorld != World.Get())
		{
			return;
		}
		CollectFrames();
		if (Extension)
		{
			Extension->Collect();
		}
		if (SampleCount >= Settings.MaxSamples || bDataLimit)
		{
			FString Unused;
			Owner->Stop(bDataLimit ? TEXT("data_limit_reached") : TEXT("sample_limit_reached"), Unused);
			return;
		}
		const double Now = TickedWorld->GetTimeSeconds();
		if (LastSample >= 0 && Now - LastSample + UE_SMALL_NUMBER < 1.0 / Settings.SampleHz)
		{
			return;
		}
		LastSample = Now;
		auto Row = Observation();
		Row->SetNumberField(TEXT("index"), ++SampleCount);
		Row->SetNumberField(TEXT("world_time_dilation"), TickedWorld->GetWorldSettings()->GetEffectiveTimeDilation());
		Row->SetBoolField(TEXT("world_paused"), TickedWorld->IsPaused());
		TArray<TSharedPtr<FJsonValue>> Actors;
		for (int32 ParticipantIndex = 0; ParticipantIndex < Participants.Num(); ++ParticipantIndex)
		{
			const auto &Participant = Participants[ParticipantIndex];
			auto Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("role"), Participant.Id);
			AActor *Actor = Participant.Actor.Get();
			Entry->SetBoolField(TEXT("valid"), IsValid(Actor));
			if (Actor)
			{
				Entry->SetArrayField(TEXT("position_cm"), VectorJson(Actor->GetActorLocation()));
				Entry->SetArrayField(TEXT("rotation_deg"), VectorJson(Actor->GetActorRotation().Euler()));
				Entry->SetArrayField(TEXT("velocity_cm_s"), VectorJson(Actor->GetVelocity()));
				Entry->SetNumberField(TEXT("custom_time_dilation"), Actor->CustomTimeDilation);
				auto Points = MakeShared<FJsonObject>();
				USkeletalMeshComponent *Mesh = Participant.Mesh.Get();
				for (const FName Point : Participant.Points)
				{
					const auto *Override = Participant.PointSources.Find(Point);
					USceneComponent *Source = Override ? Override->Get() : Mesh;
					if (!Mesh || !Source || !Source->DoesSocketExist(Point))
					{
						Points->SetField(Point.ToString(), MakeShared<FJsonValueNull>());
						continue;
					}
					auto Position = MakeShared<FJsonObject>();
					const FTransform Socket = Source->GetSocketTransform(Point);
					const FTransform InMesh = Socket.GetRelativeTransform(Mesh->GetComponentTransform());
					Position->SetArrayField(TEXT("world_cm"), VectorJson(Socket.GetLocation()));
					Position->SetArrayField(TEXT("component_cm"), VectorJson(InMesh.GetLocation()));
					Position->SetArrayField(
						TEXT("actor_cm"),
						VectorJson(Actor->GetActorTransform().InverseTransformPosition(Socket.GetLocation())));
					Position->SetArrayField(TEXT("world_rotation_xyzw"), QuaternionJson(Socket.GetRotation()));
					Position->SetArrayField(TEXT("component_rotation_xyzw"), QuaternionJson(InMesh.GetRotation()));
					Position->SetStringField(TEXT("source_component"), Source->GetPathName());
					Position->SetStringField(TEXT("attachment_socket"), Source->GetAttachSocketName().ToString());
					Position->SetStringField(TEXT("attach_parent"), GetPathNameSafe(Source->GetAttachParent()));
					if (const auto *StaticMesh = Cast<UStaticMeshComponent>(Source))
					{
						Position->SetStringField(TEXT("source_asset"), GetPathNameSafe(StaticMesh->GetStaticMesh()));
					}
					Points->SetObjectField(Point.ToString(), Position);
				}
				Entry->SetObjectField(TEXT("points"), Points);
				if (Mesh)
				{
					const auto &Pose = Poses[ParticipantIndex];
					Entry->SetNumberField(TEXT("pose_evaluation_serial"), static_cast<double>(Pose.Serial));
					Entry->SetNumberField(TEXT("pose_engine_frame"), static_cast<double>(Pose.EngineFrame));
					Entry->SetNumberField(TEXT("pose_simulation_time_s"), Pose.SimulationTime);
					Entry->SetBoolField(TEXT("pose_finalized_this_frame"),
										Pose.Serial > 0 && Pose.EngineFrame == GFrameCounter);
					Entry->SetStringField(TEXT("mesh_component"), Mesh->GetPathName());
					Entry->SetStringField(TEXT("mesh_asset"), GetPathNameSafe(Mesh->GetSkinnedAsset()));
					Entry->SetBoolField(TEXT("recently_rendered"), Mesh->WasRecentlyRendered());
					Entry->SetNumberField(TEXT("visibility_tick_option"),
										  static_cast<int32>(Mesh->VisibilityBasedAnimTickOption));
					UAnimInstance *Anim = Mesh->GetAnimInstance();
					UAnimMontage *Montage = Anim ? Anim->GetCurrentActiveMontage() : nullptr;
					TArray<TSharedPtr<FJsonValue>> Montages;
					if (Anim)
					{
						for (const FAnimMontageInstance *Instance : Anim->MontageInstances)
						{
							if (!Instance || !Instance->Montage)
							{
								continue;
							}
							auto M = MakeShared<FJsonObject>();
							M->SetStringField(TEXT("asset"), Instance->Montage->GetPathName());
							M->SetNumberField(TEXT("instance_id"), Instance->GetInstanceID());
							M->SetNumberField(TEXT("position_s"), Instance->GetPosition());
							M->SetNumberField(TEXT("weight"), Instance->GetWeight());
							M->SetNumberField(TEXT("play_rate"), Instance->GetPlayRate());
							Montages.Add(MakeShared<FJsonValueObject>(M));
						}
					}
					Entry->SetArrayField(TEXT("montages"), Montages);
					Entry->SetStringField(TEXT("active_montage"), GetPathNameSafe(Montage));
					if (Montage)
					{
						Entry->SetNumberField(TEXT("montage_position_s"), Anim->Montage_GetPosition(Montage));
						Entry->SetNumberField(TEXT("montage_rate"), Anim->Montage_GetPlayRate(Montage));
					}
				}
				if (Extension)
				{
					MergeFields(Entry, Extension->ObserveSubject(ParticipantIndex));
				}
			}
			Actors.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Row->SetArrayField(TEXT("actors"), Actors);
		WriteLine(Samples.Get(), Row);
	}

	int64 PendingReadbackFileBytes() const
	{
		int64 Bytes = 0;
		for (const auto& Pair : PendingReadbacks)
		{
			Bytes += FAnimationCaptureImageWriter::FileByteReservation(FIntPoint(
				int32(Pair.Value->GetNumberField(TEXT("width"))), int32(Pair.Value->GetNumberField(TEXT("height")))));
		}
		return Bytes;
	}
	void PixelEvidence(const TSharedRef<FJsonObject>& Row, const TArray<FColor>& Pixels)
	{
		if (Pixels.IsEmpty()) { Row->SetBoolField(TEXT("nontrivial_pixels"), false); return; }
		int32 MinChannel = 255, MaxChannel = 0, Varied = 0;
		const FColor First = Pixels[0];
		for (int32 I = 0; I < Pixels.Num(); I += FMath::Max(1, Pixels.Num() / 8192))
		{
			MinChannel = FMath::Min(MinChannel, int32(FMath::Min3(Pixels[I].R, Pixels[I].G, Pixels[I].B)));
			MaxChannel = FMath::Max(MaxChannel, int32(FMath::Max3(Pixels[I].R, Pixels[I].G, Pixels[I].B)));
			Varied += FMath::Abs(int32(Pixels[I].R) - First.R) > 8 || FMath::Abs(int32(Pixels[I].G) - First.G) > 8 || FMath::Abs(int32(Pixels[I].B) - First.B) > 8;
		}
		Row->SetBoolField(TEXT("nontrivial_pixels"), Varied >= 8 && MaxChannel - MinChannel >= 8);
	}
	void CollectReadbacks()
	{
		if (!Readback) { return; }
		FAnimationCaptureReadbackResult Result;
		while (Readback->Poll(Result))
		{
			const auto* Pending = PendingReadbacks.Find(Result.Request.RequestId);
			if (!Pending) { Error(TEXT("Readback result has no admitted frame")); continue; }
			const auto Row = *Pending; PendingReadbacks.Remove(Result.Request.RequestId);
			const TCHAR* Status = Result.Status == EAnimationCaptureReadbackStatus::Completed ? TEXT("completed") :
				Result.Status == EAnimationCaptureReadbackStatus::Cancelled ? TEXT("cancelled") :
				Result.Status == EAnimationCaptureReadbackStatus::TimedOut ? TEXT("timed_out") : TEXT("failed");
			auto Event = MakeShared<FJsonObject>();
			Event->SetNumberField(TEXT("request_id"), double(Result.Request.RequestId));
			Event->SetStringField(TEXT("session_id"), Result.Request.SessionId); Event->SetStringField(TEXT("status"), Status);
			Event->SetStringField(TEXT("error"), Result.Error);
			Event->SetNumberField(TEXT("acquisition_wall_elapsed_s"), Row->GetNumberField(TEXT("wall_elapsed_s")));
			Event->SetNumberField(TEXT("completed_wall_elapsed_s"), Result.CompletedWallSeconds - StartWall);
			if (Result.bHasView) { Event->SetNumberField(TEXT("engine_frame"), double(Result.View.EngineFrame)); }
			if (Result.Status != EAnimationCaptureReadbackStatus::Completed)
			{
				WriteLine(ReadbackEvents.Get(), Event);
				++FailedFrames; Error(FString::Printf(TEXT("RGB request %llu %s: %s"), Result.Request.RequestId, Status, *Result.Error)); continue;
			}
			if (!Result.bHasView || Result.View.EngineFrame != uint64(Row->GetNumberField(TEXT("engine_frame"))))
			{ WriteLine(ReadbackEvents.Get(), Event); ++FailedFrames; Error(TEXT("RGB acquisition frame differs from its immutable row")); continue; }
			Row->SetStringField(TEXT("readback_mode"), TEXT("asynchronous"));
			Row->SetStringField(TEXT("readback_session_id"), Result.Request.SessionId);
			Row->SetNumberField(TEXT("readback_request_id"), double(Result.Request.RequestId));
			Row->SetStringField(TEXT("readback_decoder"), FAnimationCaptureReadbackProducer::DecoderIdentity());
			Row->SetNumberField(TEXT("renderer_frame_number"), Result.View.RendererFrameNumber);
			Row->SetNumberField(TEXT("renderer_view_key"), Result.View.ViewKey);
			Row->SetNumberField(TEXT("renderer_world_time_s"), Result.View.WorldTimeSeconds);
			Row->SetNumberField(TEXT("renderer_real_time_s"), Result.View.RealTimeSeconds);
			Row->SetStringField(TEXT("renderer_clock"), TEXT("unreal.view_family.world_time"));
			Row->SetStringField(TEXT("readback_viewport_id"), Result.Request.ViewportId);
			Row->SetNumberField(TEXT("readback_viewport_generation"), double(Result.Request.ViewportGeneration));
			Row->SetNumberField(TEXT("readback_completed_wall_elapsed_s"), Result.CompletedWallSeconds - StartWall);
			Row->SetNumberField(TEXT("readback_collected_wall_elapsed_s"), Result.CollectedWallSeconds - StartWall);
			Row->SetNumberField(TEXT("readback_collected_engine_frame"), double(GFrameCounter));
			Row->SetNumberField(TEXT("readback_latency_s"), Result.CompletionLatencySeconds);
			Row->SetNumberField(TEXT("readback_decode_wall_s"), Result.DecodeSeconds);
			TArray<TSharedPtr<FJsonValue>> Matrix;
			for (int32 R = 0; R < 4; ++R) { for (int32 C = 0; C < 4; ++C) { Matrix.Add(MakeShared<FJsonValueNumber>(Result.View.WorldToClip.M[R][C])); } }
			Row->SetArrayField(TEXT("renderer_world_to_clip_row_major"), Matrix);
			PixelEvidence(Row, Result.RGB);
			FString QueueError;
			if (!ImageWriter.Enqueue(Directory / Row->GetStringField(TEXT("file")), Result.Request.Size, MoveTemp(Result.RGB), QueueError))
			{ WriteLine(ReadbackEvents.Get(), Event); ++RejectedFrames; Error(QueueError); continue; }
			// Transfer the PNG reservation to the writer before metadata can spend that budget.
			WriteLine(ReadbackEvents.Get(), Event);
			PendingFrames.Add(Row); PeakPendingFrames = FMath::Max(PeakPendingFrames, ImageWriter.GetPendingCount());
		}
	}
	void CollectFrames(bool bWait = false)
	{
		CollectReadbacks();
		FAnimationCaptureImageWriter::FResult Result;
		while (ImageWriter.Collect(Result, bWait))
		{
			const auto Row = PendingFrames[0];
			PendingFrames.RemoveAt(0);
			if (!Result.Error.IsEmpty())
			{
				++FailedFrames;
				Error(Result.Error + TEXT(": ") + Result.File);
				continue;
			}
			DataBytes += Result.BytesWritten;
			Row->SetNumberField(TEXT("image_encode_wall_s"), Result.EncodeSeconds);
			Row->SetNumberField(TEXT("image_write_wall_s"), Result.WriteSeconds);
			Row->SetNumberField(TEXT("image_collected_wall_elapsed_s"), FPlatformTime::Seconds() - StartWall);
			++FrameCount;
			WriteLine(Frames.Get(), Row);
		}
	}

	void Draw(FViewport *Viewport)
	{
		if (!bRecording || !bRenderAvailable || Settings.FrameHz <= 0 || !World.IsValid())
		{
			return;
		}
		const UGameViewportClient *Client = World->GetGameViewport();
		if (!Client || Viewport != Client->Viewport ||
			!Owner->IsExpectedViewportClient(World.Get(), Viewport->GetClient(), Client))
		{
			return;
		}
		CollectFrames();
		if (Settings.bUseAsyncReadback && !Readback)
		{
			// Enroll before the next renderer view; this draw cannot supply a witness retroactively.
			Readback = MakeUnique<FViewportAsyncCapture>(World.Get(), Viewport, Settings.bUseAsyncDiagnosticResolution,
				FAnimationCaptureReadbackLimits{}, Settings.SharedBudget); return;
		}
		++DrawCount;
		const bool bResourcesReady = FAssetCompilingManager::Get().GetNumRemainingAssets() == 0 &&
									 !(GShaderCompilingManager && GShaderCompilingManager->IsCompiling());
		// Warm up the initial view. Once recording images, preserve the interaction
		// even if unrelated streaming starts compilation; report readiness per frame.
		if (SubmittedFrames == 0 && !bResourcesReady)
		{
			ReadyDraws = 0;
			return;
		}
		if (SubmittedFrames == 0 && ++ReadyDraws < 2)
		{
			return;
		}
		const double Now = World->GetTimeSeconds();
		if (LastFrame >= 0 && Now - LastFrame + UE_SMALL_NUMBER < 1.0 / Settings.FrameHz)
		{
			return;
		}
		if (SubmittedFrames >= Settings.MaxFrames)
		{
			FString Unused;
			Owner->Stop(TEXT("frame_limit_reached"), Unused);
			return;
		}
		LastFrame = Now;
		const FIntPoint Size = Viewport->GetRenderTargetTextureSizeXY();
		FString QueueError;
		const int64 PerFrameMemory = FAnimationCaptureImageWriter::FileByteReservation(Size) + int64(Size.X) * Size.Y * 4;
		// Collected cancellations/timeouts can still own submitted GPU resources.
		// Count producer reservations until retirement, not only rows awaiting a result.
		const int32 OutstandingFrames = (Readback ? Readback->GetStats().PendingRequests : 0) + ImageWriter.GetPendingCount();
		if (!ImageWriter.CanEnqueue(Size, QueueError) || (Settings.bUseAsyncReadback &&
			(OutstandingFrames >= FAnimationCaptureImageWriter::MaxPendingFrames ||
			 int64(OutstandingFrames + 1) * PerFrameMemory > FAnimationCaptureImageWriter::MaxPendingBytes)))
		{
			if (QueueError.IsEmpty()) { QueueError = TEXT("Combined readback/PNG queue budget exhausted"); }
			++RejectedFrames;
			Error(QueueError);
			FString Unused;
			Owner->Stop(TEXT("image_queue_limit_reached"), Unused);
			return;
		}
		if (DataBytes + ImageWriter.GetReservedFileBytes() + PendingReadbackFileBytes() + FAnimationCaptureImageWriter::FileByteReservation(Size) >
			Settings.MaxDataBytes)
		{
			bDataLimit = true;
			Error(TEXT("Insufficient data budget for next viewport frame"));
			return;
		}
		TSharedPtr<FAnimationCaptureReservation, ESPMode::ThreadSafe> SynchronousPixels;
		if (!Settings.bUseAsyncReadback && Settings.SharedBudget)
		{
			SynchronousPixels = Settings.SharedBudget->Reserve(int64(Size.X) * Size.Y * 4);
			if (!SynchronousPixels)
			{
				++RejectedFrames; Error(TEXT("Shared capture budget exhausted before synchronous viewport readback"));
				FString Unused; Owner->Stop(TEXT("image_queue_limit_reached"), Unused); return;
			}
		}
		TArray<FColor> Pixels;
		// Timestamp the observed viewport, before readback and worker latency. Pose
		// links, camera and sample identity stay on this originating game frame.
		auto Row = Observation();
		const double ReadbackStart = FPlatformTime::Seconds();
		if (!Settings.bUseAsyncReadback && (!GetViewportScreenShot(Viewport, Pixels) || Size.X <= 0 || Size.Y <= 0 ||
			Pixels.Num() != static_cast<int64>(Size.X) * Size.Y))
		{
			Error(TEXT("PIE viewport pixel readback failed"));
			return;
		}
		const double ReadbackSeconds = FPlatformTime::Seconds() - ReadbackStart;
		for (FColor &Pixel : Pixels)
		{
			Pixel.A = 255;
		}
		const FString File = FString::Printf(TEXT("frames/frame_%06d.png"), SubmittedFrames + 1);
		Row->SetNumberField(TEXT("readback_wall_s"), ReadbackSeconds);
		Row->SetNumberField(TEXT("index"), SubmittedFrames + 1);
		Row->SetNumberField(TEXT("sample_index"), SampleCount);
		Row->SetNumberField(TEXT("sample_time_lag_s"), LastSample < 0 ? -1 : Now - LastSample);
		TArray<TSharedPtr<FJsonValue>> PoseLinks;
		for (int32 I = 0; I < Participants.Num(); ++I)
		{
			auto Link = MakeShared<FJsonObject>();
			Link->SetStringField(TEXT("role"), Participants[I].Id);
			Link->SetNumberField(TEXT("pose_evaluation_serial"), static_cast<double>(Poses[I].Serial));
			Link->SetNumberField(TEXT("pose_engine_frame"), static_cast<double>(Poses[I].EngineFrame));
			PoseLinks.Add(MakeShared<FJsonValueObject>(Link));
		}
		Row->SetArrayField(TEXT("pose_links"), PoseLinks);
		Row->SetNumberField(TEXT("pie_draw_index"), DrawCount);
		Row->SetStringField(TEXT("file"), File);
		Row->SetStringField(TEXT("capture_world"), WorldPath);
		Row->SetStringField(TEXT("source"), World->WorldType == EWorldType::PIE ? TEXT("PIEGameViewportAfterDraw")
																				: TEXT("GameViewportAfterDraw"));
		Row->SetBoolField(TEXT("render_resources_ready"), bResourcesReady);
		Row->SetNumberField(TEXT("width"), Size.X);
		Row->SetNumberField(TEXT("height"), Size.Y);
		if (!Settings.bUseAsyncReadback) { PixelEvidence(Row, Pixels); }
		APlayerController *PC = World->GetFirstPlayerController();
		if (PC)
		{
			FVector Location;
			FRotator Rotation;
			PC->GetPlayerViewPoint(Location, Rotation);
			Row->SetArrayField(TEXT("camera_position_cm"), VectorJson(Location));
			Row->SetArrayField(TEXT("camera_rotation_deg"), VectorJson(Rotation.Euler()));
			Row->SetStringField(TEXT("view_target"), GetPathNameSafe(PC->GetViewTarget()));
			if (PC->PlayerCameraManager)
			{
				Row->SetNumberField(TEXT("camera_fov_deg"), PC->PlayerCameraManager->GetFOVAngle());
			}
			FSceneViewProjectionData Projection;
			if (PC->GetLocalPlayer() && PC->GetLocalPlayer()->GetProjectionData(Viewport, Projection))
			{
				// Record the engine's post-draw projection, including aspect constraints.
				// This supports pixel cross-checks; it is not an occlusion/depth test.
				const FMatrix Matrix = Projection.ComputeViewProjectionMatrix();
				TArray<TSharedPtr<FJsonValue>> Values;
				for (int32 R = 0; R < 4; ++R)
				{
					for (int32 C = 0; C < 4; ++C)
					{
						Values.Add(MakeShared<FJsonValueNumber>(Matrix.M[R][C]));
					}
				}
				Row->SetArrayField(TEXT("world_to_clip_row_major"), Values);
				const FIntRect Rect = Projection.GetConstrainedViewRect();
				TArray<TSharedPtr<FJsonValue>> Bounds;
				for (int32 Value : {Rect.Min.X, Rect.Min.Y, Rect.Max.X, Rect.Max.Y})
				{
					Bounds.Add(MakeShared<FJsonValueNumber>(Value));
				}
				Row->SetArrayField(TEXT("projection_view_rect"), Bounds);
				Row->SetStringField(TEXT("projection_source"), TEXT("LocalPlayerAfterDraw"));
			}
		}
		if (Settings.bUseAsyncReadback)
		{
			FAnimationCaptureReadbackRequest Request;
			Request.SessionId = FPaths::GetCleanFilename(Directory); Request.RequestId = SubmittedFrames + 1;
			Request.ViewportId = WorldPath + TEXT("/GameViewport"); Request.ViewportGeneration = 1;
			Request.ExpectedEngineFrame = GFrameCounter; Request.ClockId = TEXT("unreal.world.simulation");
			Request.AcquisitionTimeSeconds = Now; Request.Size = Size;
			for (int32 I = 0; I < Participants.Num(); ++I)
			{ Request.PoseRevisions.Add({Participants[I].Id, Poses[I].Serial, Poses[I].EngineFrame, !Participants[I].Actor.IsValid()}); }
			FAnimationCaptureReadbackTicket Ticket; const double EnqueueStart = FPlatformTime::Seconds();
			if (!Readback->CaptureRGB(Request, Ticket, QueueError))
			{
				++RejectedFrames; Error(QueueError); FString Unused; Owner->Stop(TEXT("readback_admission_failed"), Unused); return;
			}
			Row->SetNumberField(TEXT("readback_wall_s"), FPlatformTime::Seconds() - EnqueueStart);
			Row->SetNumberField(TEXT("readback_enqueue_wall_s"), Row->GetNumberField(TEXT("readback_wall_s")));
			PendingReadbacks.Add(Request.RequestId, Row); ++SubmittedFrames;
			PeakPipelineBytes = FMath::Max(PeakPipelineBytes, int64(OutstandingFrames + 1) * PerFrameMemory);
			PeakPendingFrames = FMath::Max(PeakPendingFrames, OutstandingFrames + 1); return;
		}
		if (!ImageWriter.Enqueue(Directory / File, Size, MoveTemp(Pixels), QueueError))
		{
			++RejectedFrames;
			Error(QueueError);
			FString Unused;
			Owner->Stop(TEXT("image_queue_failed"), Unused);
			return;
		}
		PendingFrames.Add(Row);
		++SubmittedFrames;
		PeakPendingFrames = FMath::Max(PeakPendingFrames, ImageWriter.GetPendingCount());
	}

	void Cleanup(UWorld *CleanedWorld, bool, bool)
	{
		if (CleanedWorld == World.Get())
		{
			FString Unused;
			Owner->Stop(TEXT("world_cleanup"), Unused);
		}
	}

	bool SaveManifest(const FString &Status, const FString &Reason)
	{
		auto Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schema_version"), 2);
		Root->SetStringField(TEXT("status"), Status);
		Root->SetStringField(TEXT("stop_reason"), Reason);
		Root->SetStringField(TEXT("scenario"), Settings.Scenario);
		Root->SetStringField(TEXT("map"), Map);
		Root->SetStringField(TEXT("world"), WorldPath);
		Root->SetStringField(TEXT("started_utc"), StartedUtc);
		Root->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Root->SetStringField(TEXT("coordinate_system"),
							 TEXT("Unreal world/component coordinates; cm, degrees, seconds"));
		Root->SetNumberField(TEXT("start_simulation_time_s"), StartSimulation);
		Root->SetNumberField(TEXT("wall_duration_s"), FPlatformTime::Seconds() - StartWall);
		Root->SetNumberField(TEXT("capture_wall_duration_s"),
							 bRecording ? FPlatformTime::Seconds() - StartWall : CaptureWallSeconds);
		Root->SetStringField(TEXT("image_export_mode"), TEXT("bounded_async_png"));
		Root->SetStringField(TEXT("readback_mode"), Settings.bUseAsyncReadback ? TEXT("asynchronous") : TEXT("synchronous"));
		Root->SetBoolField(TEXT("readback_diagnostic_resolution"), Settings.bUseAsyncReadback && Settings.bUseAsyncDiagnosticResolution);
		if (Settings.bUseAsyncReadback)
		{
			Root->SetNumberField(TEXT("capture_pipeline_peak_frames"), PeakPendingFrames);
			Root->SetNumberField(TEXT("capture_pipeline_peak_reserved_bytes"), double(PeakPipelineBytes));
		}
		if (Readback)
		{
			const auto Stats = Readback->GetStats();
			Root->SetStringField(TEXT("readback_decoder"), FAnimationCaptureReadbackProducer::DecoderIdentity());
			Root->SetNumberField(TEXT("readback_pending_requests"), Stats.PendingRequests);
			Root->SetNumberField(TEXT("readback_peak_requests"), Stats.PeakPendingRequests);
			Root->SetNumberField(TEXT("readback_peak_reserved_bytes"), double(Stats.PeakReservedBytes));
			Root->SetNumberField(TEXT("readback_rejected"), double(Stats.Rejected));
			Root->SetNumberField(TEXT("readback_cancelled"), double(Stats.Cancelled));
			Root->SetNumberField(TEXT("readback_failed"), double(Stats.Failed));
			Root->SetNumberField(TEXT("readback_timed_out"), double(Stats.TimedOut));
			Root->SetNumberField(TEXT("readback_shutdown_wall_s"), Stats.ShutdownWallSeconds);
		}
		Root->SetNumberField(TEXT("image_flush_wall_s"), ImageFlushSeconds);
		Root->SetNumberField(TEXT("image_queue_max_frames"), FAnimationCaptureImageWriter::MaxPendingFrames);
		Root->SetNumberField(TEXT("image_queue_max_buffer_bytes"), FAnimationCaptureImageWriter::MaxPendingBytes);
		Root->SetNumberField(TEXT("image_queue_peak_frames"), PeakPendingFrames);
		Root->SetNumberField(TEXT("image_submitted_frames"), SubmittedFrames);
		Root->SetNumberField(TEXT("image_pending_frames"), ImageWriter.GetPendingCount());
		Root->SetNumberField(TEXT("image_rejected_frames"), RejectedFrames);
		Root->SetNumberField(TEXT("image_failed_frames"), FailedFrames);
		Root->SetNumberField(TEXT("sample_hz"), Settings.SampleHz);
		Root->SetNumberField(TEXT("frame_hz"), Settings.FrameHz);
		Root->SetNumberField(TEXT("max_wall_seconds"), Settings.MaxWallSeconds);
		Root->SetNumberField(TEXT("max_samples"), Settings.MaxSamples);
		Root->SetNumberField(TEXT("max_frames"), Settings.MaxFrames);

		Root->SetNumberField(TEXT("max_data_bytes"), static_cast<double>(Settings.MaxDataBytes));
		Root->SetNumberField(TEXT("data_bytes_written"), static_cast<double>(DataBytes));
		auto Metadata = MakeShared<FJsonObject>();
		for (const auto &Pair : Settings.Metadata)
		{
			Metadata->SetStringField(Pair.Key, Pair.Value);
		}
		Root->SetObjectField(TEXT("metadata"), Metadata);
		Root->SetBoolField(TEXT("render_available"), bRenderAvailable);
		Root->SetNumberField(TEXT("sample_count"), SampleCount);
		Root->SetNumberField(TEXT("frame_count"), FrameCount);
		Root->SetNumberField(TEXT("marker_count"), MarkerCount);
		TArray<TSharedPtr<FJsonValue>> Roles, ErrorValues;
		for (int32 I = 0; I < Participants.Num(); ++I)
		{
			auto Role = MakeShared<FJsonObject>();
			Role->SetStringField(TEXT("role"), Participants[I].Id);
			Role->SetStringField(TEXT("actor"), ActorPaths[I]);
			Role->SetStringField(TEXT("mesh"), MeshPaths[I]);
			TArray<TSharedPtr<FJsonValue>> Points;
			for (FName Point : Participants[I].Points)
			{
				Points.Add(MakeShared<FJsonValueString>(Point.ToString()));
			}
			Role->SetArrayField(TEXT("points"), Points);
			if (Extension)
			{
				MergeFields(Role, Extension->DescribeSubject(I));
			}
			Roles.Add(MakeShared<FJsonValueObject>(Role));
		}
		if (Extension)
		{
			MergeFields(Root, Extension->DescribeSession());
		}
		if (Status == TEXT("complete") && !Errors.IsEmpty())
		{
			Root->SetStringField(TEXT("status"), TEXT("error"));
		}
		for (const auto &Message : Errors)
		{
			ErrorValues.Add(MakeShared<FJsonValueString>(Message));
		}
		Root->SetArrayField(TEXT("participants"), Roles);
		Root->SetArrayField(TEXT("errors"), ErrorValues);
		return FFileHelper::SaveStringToFile(JsonText(Root), *(Directory / TEXT("session.json")),
											 FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
};

FAnimationCaptureSession::FAnimationCaptureSession() : Impl(MakeUnique<FImpl>(this))
{
}
FAnimationCaptureSession::~FAnimationCaptureSession()
{
	FString Unused;
	Stop(TEXT("owner_destroyed"), Unused);
}

bool FAnimationCaptureSession::Start(UWorld *World, const FAnimationCaptureSettings &Settings,
									 TConstArrayView<FAnimationCaptureSubject> Participants, FString &OutError,
									 TSharedPtr<IAnimationCaptureExtension> Extension)
{
	OutError.Reset();
	if (!IsValid(World) || (World->WorldType != EWorldType::PIE && World->WorldType != EWorldType::Game) ||
		IsRecording())
	{
		OutError = TEXT("Capture requires an explicit Game/PIE world and an idle session");
		return false;
	}
	if (Settings.OutputRoot.IsEmpty() || Settings.Scenario.IsEmpty() || Settings.Scenario.Len() > 160 ||
		!FMath::IsFinite(Settings.SampleHz) || Settings.SampleHz <= 0 || Settings.SampleHz > 240 ||
		!FMath::IsFinite(Settings.FrameHz) || Settings.FrameHz < 0 || Settings.FrameHz > 60 ||
		!FMath::IsFinite(Settings.MaxWallSeconds) || Settings.MaxWallSeconds <= 0 || Settings.MaxWallSeconds > 600 ||
		Settings.MaxSamples < 1 || Settings.MaxSamples > 144000 || Settings.MaxFrames < 1 ||
		Settings.MaxFrames > 3600 || Settings.MaxDataBytes < 1 || Settings.MaxDataBytes > 2ll * 1024 * 1024 * 1024 ||
		Settings.Metadata.Num() > 32 || Participants.IsEmpty() || Participants.Num() > 32)
	{
		OutError = TEXT("Invalid capture settings or participant count; see capture guide for limits");
		return false;
	}
	TSet<FString> Roles;
	for (const auto &Pair : Settings.Metadata)
	{
		if (Pair.Key.Len() > 80 || Pair.Value.Len() > 4096)
		{
			OutError = TEXT("Metadata keys/values exceed the 80/4096 character bounds");
			return false;
		}
	}
	for (const auto &Participant : Participants)
	{
		if (!ValidRole(Participant.Id) || Roles.Contains(Participant.Id.ToLower()) || !Participant.Actor.IsValid() ||
			Participant.Actor->GetWorld() != World || Participant.Points.Num() > 64 ||
			(Participant.Mesh.IsValid() && Participant.Mesh->GetOwner() != Participant.Actor.Get()))
		{
			OutError =
				TEXT("Participants require unique safe roles, actors in the capture world, and at most 64 points");
			return false;
		}
		Roles.Add(Participant.Id.ToLower());
		for (const auto &Source : Participant.PointSources)
		{
			if (!Participant.Points.Contains(Source.Key) || !Source.Value.IsValid() ||
				Source.Value->GetOwner() != Participant.Actor.Get())
			{
				OutError = TEXT("Explicit point sources must be owned by the participant and name an enrolled point");
				return false;
			}
		}
	}
	Impl = MakeUnique<FImpl>(this);
	FImpl &S = *Impl;
	S.World = World;
	S.Settings = Settings;
	if (!S.ImageWriter.SetSharedBudget(Settings.SharedBudget))
	{
		OutError = TEXT("Image writer budget cannot change while work is queued");
		return false;
	}
	S.Participants = TArray<FAnimationCaptureSubject>(Participants);
	S.Poses.SetNum(Participants.Num());
	S.StartedUtc = FDateTime::UtcNow().ToIso8601();
	S.StartWall = FPlatformTime::Seconds();
	S.StartSimulation = World->GetTimeSeconds();
	S.WorldPath = World->GetPathName();
	S.Map = World->GetOutermost()->GetName();
	S.Map = UWorld::RemovePIEPrefix(S.Map);
	S.Directory = FPaths::ConvertRelativePathToFull(Settings.OutputRoot /
													(FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%S")) + TEXT("-") +
													 FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*(S.Directory / TEXT("frames")), true);
	auto &Platform = FPlatformFileManager::Get().GetPlatformFile();
	S.Samples.Reset(Platform.OpenWrite(*(S.Directory / TEXT("samples.jsonl"))));
	S.Frames.Reset(Platform.OpenWrite(*(S.Directory / TEXT("frames.jsonl"))));
	S.Markers.Reset(Platform.OpenWrite(*(S.Directory / TEXT("markers.jsonl"))));
	if (Settings.bUseAsyncReadback) { S.ReadbackEvents.Reset(Platform.OpenWrite(*(S.Directory / TEXT("readbacks.jsonl")))); }
	if (!S.Samples || !S.Frames || !S.Markers || (Settings.bUseAsyncReadback && !S.ReadbackEvents))
	{
		S.Samples.Reset();
		S.Frames.Reset();
		S.Markers.Reset();
		S.ReadbackEvents.Reset();
		OutError = TEXT("Could not open capture streams");
		return false;
	}
	if (Extension && !Extension->Begin(S.Participants, OutError))
	{
		S.Samples.Reset();
		S.Frames.Reset();
		S.Markers.Reset();
		S.ReadbackEvents.Reset();
		return false;
	}
	S.Extension = MoveTemp(Extension);
	for (int32 I = 0; I < S.Participants.Num(); ++I)
	{
		auto &P = S.Participants[I];
		S.ActorPaths.Add(GetPathNameSafe(P.Actor.Get()));
		S.MeshPaths.Add(GetPathNameSafe(P.Mesh.Get()));
		if (P.Mesh.IsValid())
		{
			S.Poses[I].Handle = P.Mesh->RegisterOnBoneTransformsFinalizedDelegate(
				FOnBoneTransformsFinalizedMultiCast::FDelegate::CreateLambda([&S, I]() {
					auto &Pose = S.Poses[I];
					++Pose.Serial;
					Pose.EngineFrame = GFrameCounter;
					Pose.SimulationTime = S.World.IsValid() ? S.World->GetTimeSeconds() : -1;
				}));
		}
	}
	S.bRenderAvailable = FApp::CanEverRender();
	S.bRecording = true;
	S.TickHandle = FWorldDelegates::OnWorldTickEnd.AddRaw(&S, &FImpl::Tick);
	S.DrawHandle = UGameViewportClient::OnViewportRendered().AddRaw(&S, &FImpl::Draw);
	S.CleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(&S, &FImpl::Cleanup);
	S.WatchdogHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(&S, &FImpl::Watchdog));
	if (!S.SaveManifest(TEXT("recording"), TEXT("")))
	{
		S.Error(TEXT("Manifest write failed"));
		Stop(TEXT("write_error"), OutError);
		return false;
	}
	Mark(TEXT("capture_started"));
	return true;
}

bool FAnimationCaptureSession::Stop(const FString &Reason, FString &OutError)
{
	OutError.Reset();
	FImpl &S = *Impl;
	if (!S.bRecording)
	{
		if (!S.bSaved)
		{
			OutError = FString::Join(S.Errors, TEXT("; "));
		}
		return S.bSaved;
	}
	S.StopReason = Reason;
	if (S.Extension)
	{
		S.Extension->Collect();
	}
	Mark(TEXT("capture_stopped"));
	S.CaptureWallSeconds = FPlatformTime::Seconds() - S.StartWall;
	S.bRecording = false;
	FWorldDelegates::OnWorldTickEnd.Remove(S.TickHandle);
	UGameViewportClient::OnViewportRendered().Remove(S.DrawHandle);
	FWorldDelegates::OnWorldCleanup.Remove(S.CleanupHandle);
	FTSTicker::GetCoreTicker().RemoveTicker(S.WatchdogHandle);
	// Finish bounded outstanding writes before closing streams or publishing complete.
	// No worker accesses the world, so teardown can safely use the same drain path.
	const double FlushStart = FPlatformTime::Seconds();
	if (S.Readback) { S.Readback->Shutdown(Reason == TEXT("world_cleanup")); }
	S.CollectFrames(true);
	S.ImageFlushSeconds = FPlatformTime::Seconds() - FlushStart;
	S.Samples.Reset();
	S.Frames.Reset();
	S.Markers.Reset();
	S.ReadbackEvents.Reset();
	for (int32 I = 0; I < S.Participants.Num(); ++I)
	{
		if (S.Participants[I].Mesh.IsValid())
		{
			S.Participants[I].Mesh->UnregisterOnBoneTransformsFinalizedDelegate(S.Poses[I].Handle);
		}
	}
	if (S.Extension)
	{
		TArray<FAnimationCaptureTextArtifact> Artifacts;
		S.Extension->End(Artifacts);
		for (const auto &Artifact : Artifacts)
		{
			S.WriteTextArtifact(Artifact.File, Artifact.Text);
		}
	}
	if (!S.SaveManifest(S.Errors.IsEmpty() ? TEXT("complete") : TEXT("error"), Reason))
	{
		S.Error(TEXT("Final manifest write failed"));
	}
	S.bSaved = S.Errors.IsEmpty();
	if (!S.bSaved)
	{
		OutError = TEXT("Capture export incomplete: ") + FString::Join(S.Errors, TEXT("; "));
	}
	return S.bSaved;
}

void FAnimationCaptureSession::Mark(const FString &Label)
{
	if (!Impl->bRecording)
	{
		return;
	}
	if (Impl->MarkerCount >= 10000)
	{
		Impl->Error(TEXT("Marker limit reached"));
		return;
	}
	Impl->CurrentMarker = Label.Left(256);
	auto Row = Impl->Observation();
	Row->SetNumberField(TEXT("index"), ++Impl->MarkerCount);
	Impl->WriteLine(Impl->Markers.Get(), Row);
}
bool FAnimationCaptureSession::IsRecording() const
{
	return Impl->bRecording;
}
FString FAnimationCaptureSession::GetOutputDirectory() const
{
	return Impl->Directory;
}
int32 FAnimationCaptureSession::GetSampleCount() const
{
	return Impl->SampleCount;
}
int32 FAnimationCaptureSession::GetFrameCount() const
{
	return Impl->FrameCount;
}
FString FAnimationCaptureSession::GetStopReason() const
{
	return Impl->StopReason;
}

bool FAnimationCaptureSession::IsExpectedViewportClient(const UWorld *World, const FViewportClient *DrawnClient,
														const FViewportClient *ExpectedClient)
{
	return IsValid(World) && (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game) &&
		   DrawnClient && DrawnClient == ExpectedClient && DrawnClient->GetWorld() == World;
}
