// Copyright Epic Games, Inc. All Rights Reserved.
#include "AnimationCapture/AnimationCaptureImageWriter.h"
#include "Async/Async.h"
#include "ImageCore.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"

struct FAnimationCaptureImageWriter::FImpl
{
	struct FPending
	{
		TFuture<FResult> Future;
		int64 FileBytes = 0, MemoryBytes = 0;
	};
	TArray<FPending> Pending;
	int64 ReservedFileBytes = 0, ReservedMemoryBytes = 0;
	IImageWrapperModule* Encoder = nullptr;
};

FAnimationCaptureImageWriter::FAnimationCaptureImageWriter() : Impl(MakeUnique<FImpl>()) {}
FAnimationCaptureImageWriter::~FAnimationCaptureImageWriter()
{
	FResult Unused;
	while (Collect(Unused, true)) {}
}

int64 FAnimationCaptureImageWriter::FileByteReservation(FIntPoint Size)
{
	// Reject dimensions before multiplication/allocation. The queue's byte bound
	// provides the tighter practical limit, including raw and encoded buffers.
	if (Size.X <= 0 || Size.Y <= 0 || Size.X > 16384 || Size.Y > 16384) { return 0; }
	return static_cast<int64>(Size.X) * Size.Y * 8 + 65536;
}

bool FAnimationCaptureImageWriter::CanEnqueue(FIntPoint Size, FString& OutError) const
{
	OutError.Reset();
	const int64 FileBytes = FileByteReservation(Size);
	if (FileBytes == 0) { OutError = TEXT("Invalid PNG dimensions"); return false; }
	const int64 MemoryBytes = FileBytes + static_cast<int64>(Size.X) * Size.Y * 4;
	if (MemoryBytes > MaxPendingBytes)
	{
		OutError = TEXT("Viewport dimensions exceed the PNG buffer budget"); return false;
	}
	if (Impl->Pending.Num() >= MaxPendingFrames || Impl->ReservedMemoryBytes + MemoryBytes > MaxPendingBytes)
	{
		OutError = TEXT("PNG queue exhausted; a requested frame was not captured"); return false;
	}
	return true;
}

bool FAnimationCaptureImageWriter::Enqueue(const FString& File, FIntPoint Size, TArray<FColor>&& Pixels, FString& OutError)
{
	check(IsInGameThread());
	if (!CanEnqueue(Size, OutError)) { return false; }
	if (Pixels.Num() != static_cast<int64>(Size.X) * Size.Y)
	{
		OutError = TEXT("PNG pixel count does not match viewport dimensions"); return false;
	}
	// Module loading is restricted to the game thread. Compression uses independent
	// wrapper instances; the queued work is drained before the owning module unloads.
	if (!Impl->Encoder) { Impl->Encoder = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper")); }
	const int64 FileBytes = FileByteReservation(Size);
	auto& Pending = Impl->Pending.AddDefaulted_GetRef();
	Pending.FileBytes = FileBytes;
	Pending.MemoryBytes = FileBytes + static_cast<int64>(Size.X) * Size.Y * 4;
	Impl->ReservedFileBytes += Pending.FileBytes;
	Impl->ReservedMemoryBytes += Pending.MemoryBytes;
	Pending.Future = Async(EAsyncExecution::ThreadPool,
		[File, Size, FileBytes, Encoder = Impl->Encoder, Pixels = MoveTemp(Pixels)]()
		{
			FResult Result; Result.File = File;
			TArray64<uint8> Compressed;
			const double EncodeStart = FPlatformTime::Seconds();
			const bool bEncoded = Encoder->CompressImage(Compressed, EImageFormat::PNG, FImageView(Pixels.GetData(), Size.X, Size.Y), 0);
			Result.EncodeSeconds = FPlatformTime::Seconds() - EncodeStart;
			if (!bEncoded || Compressed.Num() > FileBytes)
			{
				Result.Error = TEXT("PNG encoding failed or exceeded reserved bytes"); return Result;
			}
			const double WriteStart = FPlatformTime::Seconds();
			const bool bWritten = FFileHelper::SaveArrayToFile(Compressed, *File);
			Result.WriteSeconds = FPlatformTime::Seconds() - WriteStart;
			if (bWritten) { Result.BytesWritten = Compressed.Num(); }
			else { Result.Error = TEXT("PNG file write failed"); }
			return Result;
		});
	return true;
}

bool FAnimationCaptureImageWriter::Collect(FResult& OutResult, bool bWait)
{
	if (Impl->Pending.IsEmpty() || (!bWait && !Impl->Pending[0].Future.IsReady())) { return false; }
	auto& Pending = Impl->Pending[0];
	OutResult = Pending.Future.Get();
	Impl->ReservedFileBytes -= Pending.FileBytes;
	Impl->ReservedMemoryBytes -= Pending.MemoryBytes;
	Impl->Pending.RemoveAt(0);
	return true;
}

int32 FAnimationCaptureImageWriter::GetPendingCount() const { return Impl->Pending.Num(); }
int64 FAnimationCaptureImageWriter::GetReservedFileBytes() const { return Impl->ReservedFileBytes; }
