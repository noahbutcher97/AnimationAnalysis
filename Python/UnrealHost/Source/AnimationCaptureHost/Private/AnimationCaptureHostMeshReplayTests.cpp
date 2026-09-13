#include "AnimationCapture/AnimationCaptureMeshReference.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationCaptureMeshTopologyIdentityTest,
	"AnimationAnalysis.Capture.MeshReplay.PythonCanonicalTopology",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAnimationCaptureMeshTopologyIdentityTest::RunTest(const FString&)
{
	FAnimationMeshData Data;
	Data.Enrollment.AssetId = FString(TEXT("m\u00e9sH-\u96ea")) + TCHAR(0x7f);
	Data.Enrollment.ConfigurationGeneration = 7;
	Data.Enrollment.AnalysisLOD = 2;
	Data.Positions.SetNum(4);
	Data.Indices = {0, 1, 2, 0, 2, 3};
	Data.Sections = {
		{TEXT("body"), 0, 3, TEXT("mat-\u00e9")},
		{TEXT("edge"), 3, 3, TEXT("")},
	};

	TestEqual(TEXT("Unicode and multiple sections use Python's canonical topology identity"),
		AnimationCaptureMeshReplay::TopologyIdentity(Data),
		FString(TEXT("c476d3bf54c795a8234f19bafacc2065058f91792a9d199d69523a366c5a6814")));
	return true;
}
