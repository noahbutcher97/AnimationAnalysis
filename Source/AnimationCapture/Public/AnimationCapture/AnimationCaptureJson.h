#pragma once
#include "CoreMinimal.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace AnimationCaptureJson
{
inline TArray<TSharedPtr<FJsonValue>> VectorJson(const FVector &V)
{
	return {MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z)};
}

inline TArray<TSharedPtr<FJsonValue>> QuaternionJson(const FQuat &Q)
{
	return {MakeShared<FJsonValueNumber>(Q.X), MakeShared<FJsonValueNumber>(Q.Y), MakeShared<FJsonValueNumber>(Q.Z),
			MakeShared<FJsonValueNumber>(Q.W)};
}

inline FString JsonText(const TSharedRef<FJsonObject> &Object)
{
	FString Text;
	const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Object, Writer);
	return Text;
}

} // namespace AnimationCaptureJson
