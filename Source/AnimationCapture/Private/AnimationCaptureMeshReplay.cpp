#include "AnimationCapture/AnimationCaptureMeshReference.h"

#include "HAL/Platform.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/Char.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
constexpr int32 MaxMetadataBytes = 64 * 1024;

class FSha256
{
public:
	void Update(const uint8* Data, uint64 Size)
	{
		TotalBytes += Size;
		while (Size)
		{
			const uint32 Copy = static_cast<uint32>(FMath::Min<uint64>(Size, 64 - Buffered));
			FMemory::Memcpy(Block + Buffered, Data, Copy);
			Buffered += Copy; Data += Copy; Size -= Copy;
			if (Buffered == 64) { Transform(); Buffered = 0; }
		}
	}

	FString Final()
	{
		const uint64 BitCount = TotalBytes * 8;
		Block[Buffered++] = 0x80;
		if (Buffered > 56) { FMemory::Memzero(Block + Buffered, 64 - Buffered); Transform(); Buffered = 0; }
		FMemory::Memzero(Block + Buffered, 56 - Buffered);
		for (int32 Index = 0; Index < 8; ++Index) Block[56 + Index] = static_cast<uint8>(BitCount >> (56 - Index * 8));
		Transform();
		FString Result;
		for (uint32 Value : State) Result += FString::Printf(TEXT("%08x"), Value);
		return Result;
	}

private:
	static uint32 Rotate(uint32 Value, uint32 Bits) { return (Value >> Bits) | (Value << (32 - Bits)); }
	void Transform()
	{
		static constexpr uint32 K[64] = {
			0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
			0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
			0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
			0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
			0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
			0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
			0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
			0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
		uint32 Words[64];
		for (int32 I = 0; I < 16; ++I) Words[I] = uint32(Block[I*4]) << 24 | uint32(Block[I*4+1]) << 16 | uint32(Block[I*4+2]) << 8 | Block[I*4+3];
		for (int32 I = 16; I < 64; ++I)
		{
			const uint32 S0 = Rotate(Words[I-15],7) ^ Rotate(Words[I-15],18) ^ (Words[I-15] >> 3);
			const uint32 S1 = Rotate(Words[I-2],17) ^ Rotate(Words[I-2],19) ^ (Words[I-2] >> 10);
			Words[I] = Words[I-16] + S0 + Words[I-7] + S1;
		}
		uint32 A=State[0],B=State[1],C=State[2],D=State[3],E=State[4],F=State[5],G=State[6],H=State[7];
		for (int32 I = 0; I < 64; ++I)
		{
			const uint32 S1=Rotate(E,6)^Rotate(E,11)^Rotate(E,25), Ch=(E&F)^((~E)&G), T1=H+S1+Ch+K[I]+Words[I];
			const uint32 S0=Rotate(A,2)^Rotate(A,13)^Rotate(A,22), Maj=(A&B)^(A&C)^(B&C), T2=S0+Maj;
			H=G; G=F; F=E; E=D+T1; D=C; C=B; B=A; A=T1+T2;
		}
		State[0]+=A; State[1]+=B; State[2]+=C; State[3]+=D; State[4]+=E; State[5]+=F; State[6]+=G; State[7]+=H;
	}
	uint32 State[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
	uint8 Block[64] = {}; uint32 Buffered = 0; uint64 TotalBytes = 0;
};

class FAsciiJson
{
public:
	void Raw(const ANSICHAR* Text)
	{
		while (*Text)
		{
			Bytes.Add(static_cast<uint8>(*Text++));
		}
	}

	void Character(ANSICHAR Value) { Bytes.Add(static_cast<uint8>(Value)); }

	void String(const FString& Value)
	{
		Character('"');
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			const uint16 CharacterValue = static_cast<uint16>(Value[Index]);
			switch (CharacterValue)
			{
			case '"': Raw("\\\""); break;
			case '\\': Raw("\\\\"); break;
			case '\b': Raw("\\b"); break;
			case '\f': Raw("\\f"); break;
			case '\n': Raw("\\n"); break;
			case '\r': Raw("\\r"); break;
			case '\t': Raw("\\t"); break;
			default:
				if (CharacterValue >= 0x20 && CharacterValue < 0x7f)
				{
					Bytes.Add(static_cast<uint8>(CharacterValue));
				}
				else
				{
					static constexpr ANSICHAR Digits[] = "0123456789abcdef";
					Raw("\\u");
					Bytes.Add(Digits[(CharacterValue >> 12) & 0xf]);
					Bytes.Add(Digits[(CharacterValue >> 8) & 0xf]);
					Bytes.Add(Digits[(CharacterValue >> 4) & 0xf]);
					Bytes.Add(Digits[CharacterValue & 0xf]);
				}
				break;
			}
		}
		Character('"');
	}

	void Integer(int64 Value)
	{
		const FTCHARToUTF8 Converted(*LexToString(Value));
		for (int32 Index = 0; Index < Converted.Length(); ++Index)
		{
			Bytes.Add(static_cast<uint8>(Converted.Get()[Index]));
		}
	}

	void Unsigned(uint64 Value)
	{
		const FTCHARToUTF8 Converted(*LexToString(Value));
		for (int32 Index = 0; Index < Converted.Length(); ++Index)
		{
			Bytes.Add(static_cast<uint8>(Converted.Get()[Index]));
		}
	}

	void Number(double Value)
	{
		// Match Unreal's JSON print policy so every finite double round-trips
		// through a locale-independent JSON number without six-digit truncation.
		const FString Text = FString::Printf(TEXT("%.17g"), Value);
		const FTCHARToUTF8 Converted(*Text);
		for (int32 Index = 0; Index < Converted.Length(); ++Index)
		{
			Bytes.Add(static_cast<uint8>(Converted.Get()[Index]));
		}
	}

	TArray<uint8> Bytes;
};

bool IsBoundedText(const FString& Value, bool bAllowEmpty = false)
{
	if (Value.Len() > 256 || (!bAllowEmpty && Value.TrimStartAndEnd().IsEmpty())) return false;
	for (int32 Index = 0; Index < Value.Len(); ++Index)
	{
		const uint16 Unit = static_cast<uint16>(Value[Index]);
		if (Unit >= 0xd800 && Unit <= 0xdbff)
		{
			if (++Index >= Value.Len()) return false;
			const uint16 Low = static_cast<uint16>(Value[Index]);
			if (Low < 0xdc00 || Low > 0xdfff) return false;
		}
		else if (Unit >= 0xdc00 && Unit <= 0xdfff) return false;
	}
	return true;
}

void AddLittleEndian32(TArray<uint8>& Bytes, uint32 Value)
{
	Bytes.Add(static_cast<uint8>(Value));
	Bytes.Add(static_cast<uint8>(Value >> 8));
	Bytes.Add(static_cast<uint8>(Value >> 16));
	Bytes.Add(static_cast<uint8>(Value >> 24));
}

void AddLittleEndian64(TArray<uint8>& Bytes, uint64 Value)
{
	for (int32 Shift = 0; Shift < 64; Shift += 8)
	{
		Bytes.Add(static_cast<uint8>(Value >> Shift));
	}
}

FString HashBytes(const TArray<uint8>& Bytes)
{
	FSha256 Hash;
	Hash.Update(Bytes.GetData(), Bytes.Num());
	return Hash.Final();
}

void WriteTopologyJson(const FAnimationMeshData& Data, FAsciiJson& Json)
{
	Json.Raw("{\"asset_id\":"); Json.String(Data.Enrollment.AssetId);
	Json.Raw(",\"configuration_generation\":"); Json.Integer(Data.Enrollment.ConfigurationGeneration);
	Json.Raw(",\"index_encoding\":\"uint32-le-global\",\"lod\":"); Json.Integer(Data.Enrollment.AnalysisLOD);
	Json.Raw(",\"sections\":[");
	for (int32 Index = 0; Index < Data.Sections.Num(); ++Index)
	{
		if (Index) Json.Character(',');
		const FAnimationMeshSection& Section = Data.Sections[Index];
		Json.Raw("{\"first_index\":"); Json.Unsigned(Section.FirstIndex);
		Json.Raw(",\"index_count\":"); Json.Unsigned(Section.IndexCount);
		Json.Raw(",\"material_id\":");
		if (Section.MaterialId.IsEmpty()) Json.Raw("null"); else Json.String(Section.MaterialId);
		Json.Raw(",\"section_id\":"); Json.String(Section.Id); Json.Character('}');
	}
	Json.Raw("],\"vertex_count\":"); Json.Integer(Data.Positions.Num()); Json.Character('}');
}

void WriteClock(FAsciiJson& Json, double Seconds)
{
	Json.Raw("{\"domain\":\"unreal-monotonic\",\"seconds\":"); Json.Number(Seconds); Json.Character('}');
}

void WritePose(FAsciiJson& Json, const FAnimationMeshData& Data)
{
	Json.Raw("{\"frame_id\":"); Json.Integer(Data.FrameId);
	Json.Raw(",\"revision\":"); Json.Integer(Data.PoseRevision);
	Json.Raw(",\"stream_id\":"); Json.String(Data.Enrollment.StreamId);
	Json.Raw(",\"subject_id\":"); Json.String(Data.Enrollment.SubjectId); Json.Character('}');
}

void WriteDescriptor(FAsciiJson& Json, const FString& Hash, int64 Size)
{
	Json.Raw("{\"sha256\":"); Json.String(Hash); Json.Raw(",\"size_bytes\":"); Json.Integer(Size); Json.Character('}');
}

bool ValidateData(const FAnimationMeshData& Data, FString& Error)
{
	constexpr int64 MaxJsonInteger = 9007199254740991ll;
	auto Fail = [&Error](const TCHAR* Message) { Error = Message; return false; };
	if (Data.Positions.IsEmpty() || Data.Indices.IsEmpty() || Data.Sections.IsEmpty()) return Fail(TEXT("Mesh snapshot requires nonempty geometry and sections"));
	if (Data.Indices.Num() % 3 != 0) return Fail(TEXT("Mesh indices must contain complete triangles"));
	if (Data.Coverage.Num() > 64) return Fail(TEXT("Mesh coverage exceeds schema limit"));
	if (Data.Enrollment.ComponentGeneration < 0 || Data.Enrollment.ComponentGeneration > MaxJsonInteger ||
		Data.Enrollment.ConfigurationGeneration < 0 || Data.Enrollment.ConfigurationGeneration > MaxJsonInteger ||
		Data.Enrollment.AnalysisLOD < 0 || Data.FrameId < 0 || Data.FrameId > MaxJsonInteger ||
		Data.PoseRevision < 0 || Data.PoseRevision > MaxJsonInteger)
		return Fail(TEXT("Mesh identities require bounded nonnegative integers"));
	const FString* Required[] = {&Data.Enrollment.ComponentId, &Data.Enrollment.AssetId, &Data.Enrollment.SubjectId,
		&Data.Enrollment.StreamId, &Data.RequestId, &Data.ConfigurationId, &Data.ProducerId};
	for (const FString* Value : Required) if (!IsBoundedText(*Value)) return Fail(TEXT("Mesh identity text is empty or exceeds 256 characters"));
	if (!FMath::IsFinite(Data.AcquiredSeconds) || !FMath::IsFinite(Data.CompletedSeconds) || Data.CompletedSeconds < Data.AcquiredSeconds)
		return Fail(TEXT("Mesh clocks must be finite and completion cannot precede acquisition"));
	uint64 ExpectedFirst = 0;
	TSet<FString> SectionIds;
	for (const FAnimationMeshSection& Section : Data.Sections)
	{
		if (!IsBoundedText(Section.Id) || (!Section.MaterialId.IsEmpty() && !IsBoundedText(Section.MaterialId)) || SectionIds.Contains(Section.Id))
			return Fail(TEXT("Mesh section identities are invalid or duplicated"));
		if (Section.FirstIndex != ExpectedFirst || Section.IndexCount < 3 || Section.FirstIndex % 3 || Section.IndexCount % 3)
			return Fail(TEXT("Mesh sections must partition complete triangles in order"));
		ExpectedFirst += Section.IndexCount;
		SectionIds.Add(Section.Id);
	}
	if (ExpectedFirst != static_cast<uint64>(Data.Indices.Num())) return Fail(TEXT("Mesh sections do not cover the index buffer"));
	for (uint32 Index : Data.Indices) if (Index >= static_cast<uint32>(Data.Positions.Num())) return Fail(TEXT("Mesh index is outside the vertex buffer"));
	for (const FVector3d& Position : Data.Positions)
		if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y) || !FMath::IsFinite(Position.Z)) return Fail(TEXT("Mesh positions must be finite"));
	for (int32 Row = 0; Row < 4; ++Row) for (int32 Column = 0; Column < 4; ++Column)
		if (!FMath::IsFinite(Data.ComponentToWorld.M[Row][Column])) return Fail(TEXT("Mesh transform must be finite"));
	if (Data.ComponentToWorld.M[0][3] != 0 || Data.ComponentToWorld.M[1][3] != 0 || Data.ComponentToWorld.M[2][3] != 0 || Data.ComponentToWorld.M[3][3] != 1)
		return Fail(TEXT("Mesh transform must be affine for row-vector convention"));
	TSet<FString> Features;
	for (const FAnimationMeshFeature& Feature : Data.Coverage)
	{
		if (!IsBoundedText(Feature.Feature) || !IsBoundedText(Feature.ProducerId) || !IsBoundedText(Feature.EvidenceId) ||
			!IsBoundedText(Feature.Reason, Feature.State == TEXT("observed")) || Features.Contains(Feature.Feature) ||
			!(Feature.State == TEXT("observed") || Feature.State == TEXT("inactive") || Feature.State == TEXT("unsupported") ||
				Feature.State == TEXT("unknown") || Feature.State == TEXT("excluded")))
			return Fail(TEXT("Mesh feature coverage is invalid"));
		Features.Add(Feature.Feature);
	}
	return true;
}

void BuildPayloads(const FAnimationMeshData& Data, TArray<uint8>& Positions, TArray<uint8>& Indices)
{
	Positions.Reserve(Data.Positions.Num() * 24);
	for (const FVector3d& Position : Data.Positions)
	{
		for (double Coordinate : {Position.X, Position.Y, Position.Z})
		{
			uint64 Bits;
			FMemory::Memcpy(&Bits, &Coordinate, sizeof(Bits));
			AddLittleEndian64(Positions, Bits);
		}
	}
	Indices.Reserve(Data.Indices.Num() * 4);
	for (uint32 Index : Data.Indices) AddLittleEndian32(Indices, Index);
}

void BuildRecordJson(const FAnimationMeshData& Data, const FString& TopologyId,
	const FString& PositionsHash, int64 PositionsSize, const FString& IndicesHash, int64 IndicesSize, FAsciiJson& Json)
{
	Json.Raw("{\"buffers\":{\"indices.bin\":"); WriteDescriptor(Json, IndicesHash, IndicesSize);
	Json.Raw(",\"positions.bin\":"); WriteDescriptor(Json, PositionsHash, PositionsSize);
	Json.Raw("},\"completed\":"); WriteClock(Json, Data.CompletedSeconds);
	Json.Raw(",\"format\":\"mesh_observation\",\"observation\":{\"acquired\":"); WriteClock(Json, Data.AcquiredSeconds);
	Json.Raw(",\"component_generation\":"); Json.Integer(Data.Enrollment.ComponentGeneration);
	Json.Raw(",\"component_id\":"); Json.String(Data.Enrollment.ComponentId);
	Json.Raw(",\"component_to_world\":[");
	for (int32 Row = 0; Row < 4; ++Row) for (int32 Column = 0; Column < 4; ++Column)
	{
		if (Row || Column) Json.Character(',');
		Json.Number(Data.ComponentToWorld.M[Row][Column]);
	}
	Json.Raw("],\"configuration_id\":"); Json.String(Data.ConfigurationId);
	Json.Raw(",\"coordinate_system\":\"unreal-left-handed-z-up\",\"coverage\":[");
	for (int32 Index = 0; Index < Data.Coverage.Num(); ++Index)
	{
		if (Index) Json.Character(',');
		const FAnimationMeshFeature& Feature = Data.Coverage[Index];
		Json.Raw("{\"evidence_id\":"); Json.String(Feature.EvidenceId);
		Json.Raw(",\"feature\":"); Json.String(Feature.Feature);
		Json.Raw(",\"producer_id\":"); Json.String(Feature.ProducerId);
		Json.Raw(",\"reason\":"); Json.String(Feature.Reason);
		Json.Raw(",\"state\":"); Json.String(Feature.State); Json.Character('}');
	}
	Json.Raw("],\"pose\":"); WritePose(Json, Data);
	Json.Raw(",\"position_encoding\":\"xyz-float64-le\",\"producer_id\":"); Json.String(Data.ProducerId);
	Json.Raw(",\"topology_id\":"); Json.String(TopologyId);
	Json.Raw(",\"units\":\"centimetres\",\"vector_convention\":\"row\"},\"reason\":\"\",\"request\":{\"acquired\":");
	WriteClock(Json, Data.AcquiredSeconds);
	Json.Raw(",\"component_generation\":"); Json.Integer(Data.Enrollment.ComponentGeneration);
	Json.Raw(",\"component_id\":"); Json.String(Data.Enrollment.ComponentId);
	Json.Raw(",\"configuration_id\":"); Json.String(Data.ConfigurationId);
	Json.Raw(",\"pose\":"); WritePose(Json, Data);
	Json.Raw(",\"request_id\":"); Json.String(Data.RequestId);
	Json.Raw(",\"topology_id\":"); Json.String(TopologyId);
	Json.Raw("},\"schema_version\":1,\"status\":\"completed\",\"topology\":");
	WriteTopologyJson(Data, Json); Json.Character('}');
}

#if PLATFORM_WINDOWS
bool IsSafeBundleName(const FString& Name)
{
	if (Name.IsEmpty() || Name.Len() > 128 || !FChar::IsAlnum(Name[0]) || Name.EndsWith(TEXT("."))) return false;
	for (TCHAR Character : Name) if (!(FChar::IsAlnum(Character) && Character <= 0x7f) && Character != '.' && Character != '_' && Character != '-') return false;
	FString Base;
	if (!Name.Split(TEXT("."), &Base, nullptr)) Base = Name;
	Base.ToUpperInline();
	if (Base == TEXT("CON") || Base == TEXT("PRN") || Base == TEXT("AUX") || Base == TEXT("NUL")) return false;
	for (int32 Number = 1; Number <= 9; ++Number)
		if (Base == FString::Printf(TEXT("COM%d"), Number) || Base == FString::Printf(TEXT("LPT%d"), Number)) return false;
	return true;
}

bool IsExistingPlainDirectory(const FString& Path)
{
	const DWORD Attributes = GetFileAttributesW(*Path);
	return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_DIRECTORY) && !(Attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool RootPathIsPlain(const FString& Root)
{
	FString Full = FPaths::ConvertRelativePathToFull(Root);
	FPaths::NormalizeDirectoryName(Full);
	if (Full.Len() < 3 || Full[1] != ':' || Full[2] != '/') return false;
	if (!IsExistingPlainDirectory(Full.Left(3))) return false;
	int32 Position = 3;
	while (Position < Full.Len())
	{
		int32 Slash = Full.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Position);
		if (Slash == INDEX_NONE) Slash = Full.Len();
		if (!IsExistingPlainDirectory(Full.Left(Slash))) return false;
		Position = Slash + 1;
	}
	return IsExistingPlainDirectory(Full);
}

bool WriteExclusiveFile(const FString& Path, const TArray<uint8>& Bytes, FString& Error)
{
	HANDLE File = CreateFileW(*Path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
	if (File == INVALID_HANDLE_VALUE) { Error = FString::Printf(TEXT("Cannot exclusively create replay file (%lu)"), GetLastError()); return false; }
	bool bSucceeded = true;
	int64 Offset = 0;
	while (Offset < Bytes.Num())
	{
		DWORD Written = 0;
		const DWORD Chunk = static_cast<DWORD>(FMath::Min<int64>(Bytes.Num() - Offset, MAX_uint32));
		if (!WriteFile(File, Bytes.GetData() + Offset, Chunk, &Written, nullptr) || Written != Chunk) { bSucceeded = false; break; }
		Offset += Written;
	}
	if (bSucceeded) bSucceeded = FlushFileBuffers(File) != 0;
	const DWORD Failure = bSucceeded ? ERROR_SUCCESS : GetLastError();
	CloseHandle(File);
	if (!bSucceeded) Error = FString::Printf(TEXT("Cannot write and flush replay file (%lu)"), Failure);
	return bSucceeded;
}
#endif
} // namespace

FString AnimationCaptureMeshReplay::TopologyIdentity(const FAnimationMeshData& Data)
{
	FAsciiJson Metadata;
	WriteTopologyJson(Data, Metadata);
	if (Metadata.Bytes.Num() > MaxMetadataBytes || Data.Indices.Num() > MAX_int32 / 4) return FString();
	FSha256 Hash;
	Hash.Update(Metadata.Bytes.GetData(), Metadata.Bytes.Num());
	const uint8 Zero = 0; Hash.Update(&Zero, 1);
	for (uint32 Index : Data.Indices)
	{
		uint8 Encoded[4] = {static_cast<uint8>(Index), static_cast<uint8>(Index >> 8),
			static_cast<uint8>(Index >> 16), static_cast<uint8>(Index >> 24)};
		Hash.Update(Encoded, 4);
	}
	return Hash.Final();
}

bool AnimationCaptureMeshReplay::Write(const FString& Root, const FString& RelativeName,
	const FAnimationMeshSnapshot& Snapshot, FString& Error)
{
	Error.Reset();
	if (!IsInGameThread())
	{
		Error = TEXT("Mesh replay serialization requires the game thread");
		return false;
	}
#if !PLATFORM_WINDOWS
	Error = TEXT("Mesh replay publication currently requires Windows exclusive hard-link semantics");
	return false;
#else
	const FAnimationMeshData& Data = Snapshot.Data();
	if (!ValidateData(Data, Error)) return false;
	if (!IsSafeBundleName(RelativeName)) { Error = TEXT("Expected one portable safe ASCII replay directory name"); return false; }
	if (!RootPathIsPlain(Root)) { Error = TEXT("Replay root must be an existing plain stable directory without reparse points"); return false; }
	if (Data.Positions.Num() > MAX_int32 / 24 || Data.Indices.Num() > MAX_int32 / 4) { Error = TEXT("Mesh payload exceeds native serialization bounds"); return false; }

	TArray<uint8> Positions;
	TArray<uint8> Indices;
	BuildPayloads(Data, Positions, Indices);
	const FString PositionsHash = HashBytes(Positions);
	const FString IndicesHash = HashBytes(Indices);
	const FString TopologyId = TopologyIdentity(Data);
	if (PositionsHash.IsEmpty() || IndicesHash.IsEmpty() || TopologyId.IsEmpty()) { Error = TEXT("Cannot hash mesh replay payload"); return false; }
	if (Data.TopologyId != TopologyId) { Error = TEXT("Snapshot topology identity differs from canonical geometry"); return false; }

	FAsciiJson Record;
	BuildRecordJson(Data, TopologyId, PositionsHash, Positions.Num(), IndicesHash, Indices.Num(), Record);
	const FString RecordHash = HashBytes(Record.Bytes);
	FAsciiJson Marker;
	Marker.Raw("{\"files\":{\"indices.bin\":"); WriteDescriptor(Marker, IndicesHash, Indices.Num());
	Marker.Raw(",\"positions.bin\":"); WriteDescriptor(Marker, PositionsHash, Positions.Num());
	Marker.Raw(",\"record.json\":"); WriteDescriptor(Marker, RecordHash, Record.Bytes.Num());
	Marker.Raw("},\"format\":\"mesh_observation_commit\",\"schema_version\":1}");
	if (Record.Bytes.Num() + Marker.Bytes.Num() > MaxMetadataBytes) { Error = TEXT("Mesh replay metadata exceeds combined 64 KiB limit"); return false; }

	FString FullRoot = FPaths::ConvertRelativePathToFull(Root);
	FPaths::NormalizeDirectoryName(FullRoot);
	const FString Folder = FullRoot / RelativeName;
	if (!CreateDirectoryW(*Folder, nullptr)) { Error = FString::Printf(TEXT("Cannot exclusively reserve replay directory (%lu)"), GetLastError()); return false; }
	if (!WriteExclusiveFile(Folder / TEXT("positions.bin"), Positions, Error) ||
		!WriteExclusiveFile(Folder / TEXT("indices.bin"), Indices, Error) ||
		!WriteExclusiveFile(Folder / TEXT("record.json"), Record.Bytes, Error) ||
		!WriteExclusiveFile(Folder / TEXT("complete.pending"), Marker.Bytes, Error)) return false;
	if (!CreateHardLinkW(*(Folder / TEXT("complete.json")), *(Folder / TEXT("complete.pending")), nullptr))
	{
		Error = FString::Printf(TEXT("Cannot exclusively publish replay completion marker (%lu)"), GetLastError());
		return false;
	}
	DeleteFileW(*(Folder / TEXT("complete.pending")));
	return true;
#endif
}
