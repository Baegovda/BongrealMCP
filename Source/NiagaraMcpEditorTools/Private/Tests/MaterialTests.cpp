#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "MaterialMcpEditorTools.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

/**
 * Material toolset tests: material functions (inputs / outputs / calls), disconnect / reconnect, promote to parameter,
 * validation with and without compiling, material instance batch edits / reparenting / clearing, and a time-varying
 * preview contact sheet. Assets go to /Game/NiagaraMcpTests/Material/Run_<time>.
 *
 * Run: UnrealEditor-Cmd.exe <Project>.uproject -ExecCmds="Automation RunTests NiagaraMcp.Material;Quit" -unattended -nopause
 */
namespace NiagaraMcpMaterialTests
{
	FString NewFolder(const TCHAR* Suffix)
	{
		return FString::Printf(TEXT("/Game/NiagaraMcpTests/Material/Run_%s_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")), Suffix);
	}

	const FMaterialMcpParameter* FindParameter(const FMaterialMcpParameters& Parameters, const TCHAR* Name)
	{
		return Parameters.Parameters.FindByPredicate([Name](const FMaterialMcpParameter& P) { return P.Name == Name; });
	}

	bool HasInput(const FMaterialMcpExpressionInfo& Info, const FString& Prefix)
	{
		return Info.Inputs.ContainsByPredicate([&Prefix](const FString& Input) { return Input.StartsWith(Prefix); });
	}

	const FMaterialMcpExpressionInfo* FindExpressionInfo(const FMaterialMcpGraphInfo& Graph, const FString& Name)
	{
		return Graph.Expressions.FindByPredicate([&Name](const FMaterialMcpExpressionInfo& E) { return E.Name == Name; });
	}

	FString SaveImage(const FToolsetImage& Image, const FString& Name, TArray<uint8>* OutPng = nullptr)
	{
		TArray<uint8> Bytes;
		if (Image.Data.IsEmpty() || !FBase64::Decode(Image.Data, Bytes))
		{
			return FString();
		}
		if (OutPng)
		{
			*OutPng = Bytes;
		}
		const FString File = FPaths::ProjectSavedDir() / TEXT("NiagaraMcpTests") / Name;
		return FFileHelper::SaveArrayToFile(Bytes, *File) ? File : FString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpMaterialFunctionsTest, "NiagaraMcp.Material.Functions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpMaterialFunctionsTest::RunTest(const FString& Parameters)
{
	using namespace NiagaraMcpMaterialTests;
	const FString Folder = NewFolder(TEXT("Fn"));

	// ------------------------------------------------------------------ Material function with two inputs and one output
	const FMaterialMcpGraphInfo Created = UMaterialMcpEditorTools::CreateMaterialFunction(Folder + TEXT("/MF_RadialMask"), TEXT("Radial mask around the UV center"), TEXT("Default"), true, false);
	TestEqual(TEXT("Function asset type"), Created.AssetType, FString(TEXT("MaterialFunction")));
	TestTrue(TEXT("Default function starts with output Result"), Created.FunctionOutputs.Num() == 1 && Created.FunctionOutputs[0] == TEXT("Result"));
	UMaterialFunction* Function = LoadObject<UMaterialFunction>(nullptr, *Created.MaterialPath);
	if (!TestNotNull(TEXT("Function loads"), Function))
	{
		return false;
	}
	TestEqual(TEXT("Unset description"), UMaterialMcpEditorTools::CreateMaterialFunction(Folder + TEXT("/MF_Blend"), TEXT("None"), TEXT("MaterialLayerBlend"), false, false).FunctionInputs.Num(), 2);

	const FMaterialMcpExpressionInfo InUV = UMaterialMcpEditorTools::AddExpression(Function, TEXT("FunctionInput"), -600, -100);
	const FMaterialMcpExpressionInfo InRadius = UMaterialMcpEditorTools::AddExpression(Function, TEXT("FunctionInput"), -600, 100);
	TestEqual(TEXT("First input name"), InUV.Label, FString(TEXT("In")));
	TestEqual(TEXT("Second input gets a unique name"), InRadius.Label, FString(TEXT("In_2")));
	TestEqual(TEXT("Rename input"), UMaterialMcpEditorTools::SetExpressionProperty(Function, InUV.Name, TEXT("InputName"), TEXT("UV")).Label, FString(TEXT("UV")));
	UMaterialMcpEditorTools::SetExpressionProperty(Function, TEXT("UV"), TEXT("InputType"), TEXT("Vector2"));
	UMaterialMcpEditorTools::SetExpressionProperty(Function, InRadius.Name, TEXT("InputName"), TEXT("Radius"));
	UMaterialMcpEditorTools::SetExpressionProperty(Function, TEXT("Radius"), TEXT("InputType"), TEXT("Scalar"));
	UMaterialMcpEditorTools::SetExpressionProperty(Function, TEXT("Radius"), TEXT("PreviewValue"), TEXT("0.4"));
	UMaterialMcpEditorTools::SetExpressionProperty(Function, TEXT("Radius"), TEXT("bUsePreviewValueAsDefault"), TEXT("true"));

	AddExpectedMessagePlain(TEXT("names must be unique"), ELogVerbosity::Warning);
	TestTrue(TEXT("Duplicate input name rejected"), UMaterialMcpEditorTools::SetExpressionProperty(Function, TEXT("Radius"), TEXT("InputName"), TEXT("uv")).Name.IsEmpty());

	const FMaterialMcpExpressionInfo Mask = UMaterialMcpEditorTools::AddExpression(Function, TEXT("Custom"), -300, 0);
	UMaterialMcpEditorTools::SetCustomExpression(Function, Mask.Name, TEXT("return saturate(1.0 - length(UV - 0.5) / max(Radius, 0.001));"), TEXT("Radial mask"), { TEXT("UV"), TEXT("Radius") }, TEXT("Float1"));
	UMaterialMcpEditorTools::ConnectExpressions(Function, TEXT("UV"), TEXT("Auto"), Mask.Name, TEXT("UV"));
	UMaterialMcpEditorTools::ConnectExpressions(Function, TEXT("Radius"), TEXT("Auto"), Mask.Name, TEXT("Radius"));
	const FMaterialMcpExpressionInfo Output = UMaterialMcpEditorTools::ConnectExpressions(Function, Mask.Name, TEXT(""), TEXT("Result"), TEXT("Auto"));
	TestTrue(TEXT("Function output wired"), Output.Inputs.Num() == 1 && Output.Inputs[0].Contains(Mask.Name));

	// SetCustomExpression validates OutputType before it changes anything.
	AddExpectedMessagePlain(TEXT("Unknown OutputType"), ELogVerbosity::Warning);
	UMaterialMcpEditorTools::SetCustomExpression(Function, Mask.Name, TEXT("return 0;"), TEXT("Broken"), { TEXT("X") }, TEXT("Float7"));
	const FMaterialMcpGraphInfo FunctionGraph = UMaterialMcpEditorTools::GetMaterialGraph(Function);
	const FMaterialMcpExpressionInfo* MaskAfter = FindExpressionInfo(FunctionGraph, Mask.Name);
	TestTrue(TEXT("Failed SetCustomExpression left the node unchanged"), MaskAfter && MaskAfter->Code.Contains(TEXT("saturate")) && MaskAfter->Inputs.Num() == 2);
	TestEqual(TEXT("Function inputs listed"), FunctionGraph.FunctionInputs.Num(), 2);
	TestTrue(TEXT("UV input is Vector2"), FunctionGraph.FunctionInputs.Num() > 0 && FunctionGraph.FunctionInputs[0].StartsWith(TEXT("UV")));

	AddExpectedMessagePlain(TEXT("only exist in material functions"), ELogVerbosity::Warning);
	AddExpectedMessagePlain(TEXT("Connect to a FunctionOutput node"), ELogVerbosity::Warning);
	UMaterialMcpEditorTools::ConnectToMaterialProperty(Function, Mask.Name, TEXT("Auto"), TEXT("EmissiveColor"));

	const FMaterialMcpValidation FunctionValidation = UMaterialMcpEditorTools::ValidateMaterial(Function, false);
	for (const FMaterialMcpIssue& Issue : FunctionValidation.Issues)
	{
		AddInfo(FString::Printf(TEXT("MF issue: %s %s (%s)"), *Issue.Severity, *Issue.Message, *Issue.Expression));
	}
	TestTrue(TEXT("Function validates"), FunctionValidation.bOk);
	TestTrue(TEXT("Function saved"), UMaterialMcpEditorTools::CompileMaterial(Function, true).bSaved);

	// ------------------------------------------------------------------ Material calling the function
	const FMaterialMcpGraphInfo MaterialInfo = UMaterialMcpEditorTools::CreateMaterial(Folder + TEXT("/M_UsesMask"), false);
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialInfo.MaterialPath);
	if (!TestNotNull(TEXT("Material loads"), Material))
	{
		return false;
	}
	UMaterialMcpEditorTools::SetMaterialProperty(Material, TEXT("ShadingModel"), TEXT("Unlit"), false);
	UMaterialMcpEditorTools::AddExpression(Material, TEXT("FunctionInput"), 0, 0); // rejected (expected message above)

	const FMaterialMcpExpressionInfo Call = UMaterialMcpEditorTools::AddExpression(Material, TEXT("MaterialFunctionCall"), -400, 0);
	const FMaterialMcpExpressionInfo CallSet = UMaterialMcpEditorTools::SetExpressionProperty(Material, Call.Name, TEXT("MaterialFunction"), Function->GetPathName());
	AddInfo(FString::Printf(TEXT("Call pins: in [%s] out [%s]"), *FString::Join(CallSet.Inputs, TEXT(", ")), *FString::Join(CallSet.Outputs, TEXT(", "))));
	TestEqual(TEXT("Call references the function"), CallSet.MaterialFunction, Function->GetPathName());
	TestTrue(TEXT("Call has the function inputs"), CallSet.Inputs.Num() == 2 && CallSet.Inputs[0] == TEXT("UV") && CallSet.Inputs[1] == TEXT("Radius"));
	TestTrue(TEXT("Call has the function output"), CallSet.Outputs.Contains(TEXT("Result")));

	const FMaterialMcpExpressionInfo TexCoord = UMaterialMcpEditorTools::AddExpression(Material, TEXT("TextureCoordinate"), -700, -100);
	const FMaterialMcpExpressionInfo RadiusConst = UMaterialMcpEditorTools::AddExpression(Material, TEXT("Constant"), -700, 100);
	UMaterialMcpEditorTools::SetExpressionProperty(Material, RadiusConst.Name, TEXT("R"), TEXT("0.35"));
	const FMaterialMcpExpressionInfo TintConst = UMaterialMcpEditorTools::AddExpression(Material, TEXT("Constant3Vector"), -400, 250);
	UMaterialMcpEditorTools::SetExpressionProperty(Material, TintConst.Name, TEXT("Constant"), TEXT("1, 0.5, 0.2, 1"));
	const FMaterialMcpExpressionInfo Multiply = UMaterialMcpEditorTools::AddExpression(Material, TEXT("Multiply"), -150, 0);

	UMaterialMcpEditorTools::ConnectExpressions(Material, TexCoord.Name, TEXT("Auto"), Call.Name, TEXT("UV"));
	UMaterialMcpEditorTools::ConnectExpressions(Material, RadiusConst.Name, TEXT("Auto"), Call.Name, TEXT("Radius"));
	UMaterialMcpEditorTools::ConnectExpressions(Material, Call.Name, TEXT("Result"), Multiply.Name, TEXT("A"));
	UMaterialMcpEditorTools::ConnectExpressions(Material, TintConst.Name, TEXT("Auto"), Multiply.Name, TEXT("B"));
	TestEqual(TEXT("Emissive wired"), UMaterialMcpEditorTools::ConnectToMaterialProperty(Material, Multiply.Name, TEXT("Auto"), TEXT("EmissiveColor")).PropertyConnections.Num(), 1);

	// Disconnect / reconnect.
	const FMaterialMcpExpressionInfo Cut = UMaterialMcpEditorTools::DisconnectExpressions(Material, Call.Name, TEXT("UV"));
	TestTrue(TEXT("UV input disconnected"), Cut.Inputs.Num() == 2 && Cut.Inputs[0] == TEXT("UV") && Cut.Inputs[1].Contains(TEXT("<-")));
	const FMaterialMcpValidation MissingInput = UMaterialMcpEditorTools::ValidateMaterial(Material, false);
	TestTrue(TEXT("Required function input reported"), MissingInput.Issues.ContainsByPredicate([](const FMaterialMcpIssue& I) { return I.Severity == TEXT("Error") && I.Message.Contains(TEXT("'UV'")); }));
	AddExpectedMessagePlain(TEXT("is not connected."), ELogVerbosity::Warning);
	TestTrue(TEXT("Disconnecting an open input fails"), UMaterialMcpEditorTools::DisconnectExpressions(Material, Call.Name, TEXT("UV")).Name.IsEmpty());
	TestTrue(TEXT("UV reconnected"), HasInput(UMaterialMcpEditorTools::ConnectExpressions(Material, TexCoord.Name, TEXT("Auto"), Call.Name, TEXT("UV")), TEXT("UV <- ")));
	TestEqual(TEXT("Emissive disconnected"), UMaterialMcpEditorTools::DisconnectMaterialProperty(Material, TEXT("EmissiveColor")).PropertyConnections.Num(), 0);
	TestEqual(TEXT("Emissive reconnected"), UMaterialMcpEditorTools::ConnectToMaterialProperty(Material, Multiply.Name, TEXT("Auto"), TEXT("EmissiveColor")).PropertyConnections.Num(), 1);

	// Promote constants to parameters, keeping values and links.
	const FMaterialMcpExpressionInfo RadiusParam = UMaterialMcpEditorTools::PromoteToParameter(Material, RadiusConst.Name, TEXT("Mask Radius"), false);
	TestEqual(TEXT("Constant became ScalarParameter"), RadiusParam.Class, FString(TEXT("ScalarParameter")));
	TestEqual(TEXT("Parameter name"), RadiusParam.Label, FString(TEXT("Mask Radius")));
	const FMaterialMcpExpressionInfo TintParam = UMaterialMcpEditorTools::PromoteToParameter(Material, TintConst.Name, TEXT("Tint"), false);
	TestEqual(TEXT("Constant3Vector became VectorParameter"), TintParam.Class, FString(TEXT("VectorParameter")));
	const FMaterialMcpGraphInfo Promoted = UMaterialMcpEditorTools::GetMaterialGraph(Material);
	const FMaterialMcpExpressionInfo* CallAfter = FindExpressionInfo(Promoted, Call.Name);
	const FMaterialMcpExpressionInfo* MultiplyAfter = FindExpressionInfo(Promoted, Multiply.Name);
	TestTrue(TEXT("Radius link kept"), CallAfter && HasInput(*CallAfter, FString::Printf(TEXT("Radius <- %s"), *RadiusParam.Name)));
	TestTrue(TEXT("Tint link kept"), MultiplyAfter && HasInput(*MultiplyAfter, FString::Printf(TEXT("B <- %s"), *TintParam.Name)));
	TestNull(TEXT("Old constant removed"), FindExpressionInfo(Promoted, RadiusConst.Name));
	const FMaterialMcpParameters MaterialParameters = UMaterialMcpEditorTools::GetMaterialParameters(Material);
	const FMaterialMcpParameter* RadiusValue = FindParameter(MaterialParameters, TEXT("Mask Radius"));
	const FMaterialMcpParameter* TintValue = FindParameter(MaterialParameters, TEXT("Tint"));
	TestTrue(TEXT("Radius value kept"), RadiusValue && FMath::IsNearlyEqual(FCString::Atof(*RadiusValue->Value), 0.35f, 1.e-4f));
	TestTrue(TEXT("Tint value kept"), TintValue && TintValue->Value.StartsWith(TEXT("1, 0.5, 0.2")));

	const FMaterialMcpCompileResult Compile = UMaterialMcpEditorTools::CompileMaterial(Material, true);
	for (const FString& Error : Compile.Errors)
	{
		AddError(FString::Printf(TEXT("Material compile: %s"), *Error));
	}
	TestTrue(TEXT("Material using the function compiles"), Compile.bSucceeded && Compile.bSaved);

	// Recompiling the function recompiles the material that uses it.
	const FMaterialMcpCompileResult FunctionCompile = UMaterialMcpEditorTools::CompileMaterial(Function, true);
	TestTrue(TEXT("Function compile lists the dependent material"), FunctionCompile.DependentMaterials.Contains(Material->GetPathName()));
	TestTrue(TEXT("Function compile has no errors"), FunctionCompile.bSucceeded);
	UMaterialMcpEditorTools::CompileMaterial(Material, true);

	// ValidateMaterial without compiling must not touch the package.
	UPackage* Package = Material->GetOutermost();
	TestFalse(TEXT("Package clean after save"), Package->IsDirty());
	const FMaterialMcpValidation ReadOnly = UMaterialMcpEditorTools::ValidateMaterial(Material, false);
	for (const FMaterialMcpIssue& Issue : ReadOnly.Issues)
	{
		AddInfo(FString::Printf(TEXT("M issue: %s %s (%s)"), *Issue.Severity, *Issue.Message, *Issue.Expression));
	}
	TestTrue(TEXT("Material valid (no compile)"), ReadOnly.bOk);
	TestFalse(TEXT("ValidateMaterial(bCompile=false) leaves the package clean"), Package->IsDirty());
	TestTrue(TEXT("Material valid (compiled)"), UMaterialMcpEditorTools::ValidateMaterial(Material, true).bOk);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpMaterialInstancesTest, "NiagaraMcp.Material.Instances",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpMaterialInstancesTest::RunTest(const FString& Parameters)
{
	using namespace NiagaraMcpMaterialTests;
	const FString Folder = NewFolder(TEXT("MI"));

	// Base material: Speed (scalar), Tint (vector), Noise (texture), UseTint (static switch).
	UMaterial* Base = LoadObject<UMaterial>(nullptr, *UMaterialMcpEditorTools::CreateMaterial(Folder + TEXT("/M_Base"), false).MaterialPath);
	if (!TestNotNull(TEXT("Base material"), Base))
	{
		return false;
	}
	UMaterialMcpEditorTools::SetMaterialProperty(Base, TEXT("ShadingModel"), TEXT("Unlit"), false);
	const FMaterialMcpExpressionInfo Speed = UMaterialMcpEditorTools::AddExpression(Base, TEXT("ScalarParameter"), -800, 0);
	UMaterialMcpEditorTools::SetExpressionProperty(Base, Speed.Name, TEXT("ParameterName"), TEXT("Speed"));
	UMaterialMcpEditorTools::SetExpressionProperty(Base, TEXT("Speed"), TEXT("DefaultValue"), TEXT("1"));
	const FMaterialMcpExpressionInfo Tint = UMaterialMcpEditorTools::AddExpression(Base, TEXT("VectorParameter"), -800, 200);
	UMaterialMcpEditorTools::SetExpressionProperty(Base, Tint.Name, TEXT("ParameterName"), TEXT("Tint"));
	const FMaterialMcpExpressionInfo Noise = UMaterialMcpEditorTools::AddExpression(Base, TEXT("TextureSampleParameter2D"), -800, 400);
	UMaterialMcpEditorTools::SetExpressionProperty(Base, Noise.Name, TEXT("ParameterName"), TEXT("Noise"));
	UMaterialMcpEditorTools::SetExpressionProperty(Base, TEXT("Noise"), TEXT("Texture"), TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
	const FMaterialMcpExpressionInfo Switch = UMaterialMcpEditorTools::AddExpression(Base, TEXT("StaticSwitchParameter"), -200, 0);
	UMaterialMcpEditorTools::SetExpressionProperty(Base, Switch.Name, TEXT("ParameterName"), TEXT("UseTint"));
	UMaterialMcpEditorTools::SetExpressionProperty(Base, TEXT("UseTint"), TEXT("DefaultValue"), TEXT("true"));
	const FMaterialMcpExpressionInfo Mul1 = UMaterialMcpEditorTools::AddExpression(Base, TEXT("Multiply"), -500, 200);
	const FMaterialMcpExpressionInfo Mul2 = UMaterialMcpEditorTools::AddExpression(Base, TEXT("Multiply"), -350, 100);
	UMaterialMcpEditorTools::ConnectExpressions(Base, TEXT("Tint"), TEXT("Auto"), Mul1.Name, TEXT("A"));
	UMaterialMcpEditorTools::ConnectExpressions(Base, TEXT("Noise"), TEXT("Auto"), Mul1.Name, TEXT("B"));
	UMaterialMcpEditorTools::ConnectExpressions(Base, Mul1.Name, TEXT("Auto"), Mul2.Name, TEXT("A"));
	UMaterialMcpEditorTools::ConnectExpressions(Base, TEXT("Speed"), TEXT("Auto"), Mul2.Name, TEXT("B"));
	UMaterialMcpEditorTools::ConnectExpressions(Base, Mul2.Name, TEXT("Auto"), TEXT("UseTint"), TEXT("True"));
	UMaterialMcpEditorTools::ConnectExpressions(Base, TEXT("Noise"), TEXT("Auto"), TEXT("UseTint"), TEXT("False"));
	UMaterialMcpEditorTools::ConnectToMaterialProperty(Base, TEXT("UseTint"), TEXT("Auto"), TEXT("EmissiveColor"));
	TestTrue(TEXT("Base compiles"), UMaterialMcpEditorTools::CompileMaterial(Base, true).bSucceeded);

	// Alternative parent with Speed and Tint only.
	UMaterial* Alt = LoadObject<UMaterial>(nullptr, *UMaterialMcpEditorTools::CreateMaterial(Folder + TEXT("/M_Alt"), false).MaterialPath);
	if (!TestNotNull(TEXT("Alt material"), Alt))
	{
		return false;
	}
	UMaterialMcpEditorTools::SetMaterialProperty(Alt, TEXT("ShadingModel"), TEXT("Unlit"), false);
	const FMaterialMcpExpressionInfo AltSpeed = UMaterialMcpEditorTools::AddExpression(Alt, TEXT("ScalarParameter"), -500, 0);
	UMaterialMcpEditorTools::SetExpressionProperty(Alt, AltSpeed.Name, TEXT("ParameterName"), TEXT("Speed"));
	const FMaterialMcpExpressionInfo AltTint = UMaterialMcpEditorTools::AddExpression(Alt, TEXT("VectorParameter"), -500, 200);
	UMaterialMcpEditorTools::SetExpressionProperty(Alt, AltTint.Name, TEXT("ParameterName"), TEXT("Tint"));
	const FMaterialMcpExpressionInfo AltMul = UMaterialMcpEditorTools::AddExpression(Alt, TEXT("Multiply"), -250, 100);
	UMaterialMcpEditorTools::ConnectExpressions(Alt, TEXT("Tint"), TEXT("Auto"), AltMul.Name, TEXT("A"));
	UMaterialMcpEditorTools::ConnectExpressions(Alt, TEXT("Speed"), TEXT("Auto"), AltMul.Name, TEXT("B"));
	UMaterialMcpEditorTools::ConnectToMaterialProperty(Alt, AltMul.Name, TEXT("Auto"), TEXT("EmissiveColor"));
	TestTrue(TEXT("Alt compiles"), UMaterialMcpEditorTools::CompileMaterial(Alt, true).bSucceeded);

	const FMaterialMcpParameters Created = UMaterialMcpEditorTools::CreateMaterialInstance(Base, Folder + TEXT("/MI_Base"), true);
	UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *Created.Path);
	if (!TestNotNull(TEXT("Instance"), Instance))
	{
		return false;
	}
	TestEqual(TEXT("Instance has 4 parameters"), Created.Parameters.Num(), 4);

	// Several values in one call.
	TArray<FMaterialMcpParameterValue> Values;
	Values.Add({ TEXT("Speed"), TEXT("2.5") });
	Values.Add({ TEXT("tint"), TEXT("0, 0.3, 1, 1") });
	Values.Add({ TEXT("UseTint"), TEXT("false") });
	Values.Add({ TEXT("Noise"), TEXT("/Engine/EngineResources/Black") });
	const FMaterialMcpParameters Set = UMaterialMcpEditorTools::SetMaterialInstanceParameters(Instance, Values, true);
	for (const FMaterialMcpParameter& P : Set.Parameters)
	{
		AddInfo(FString::Printf(TEXT("MI %s (%s) = %s overridden=%d"), *P.Name, *P.Type, *P.Value, P.bOverridden ? 1 : 0));
	}
	const FMaterialMcpParameter* SpeedValue = FindParameter(Set, TEXT("Speed"));
	const FMaterialMcpParameter* TintValue = FindParameter(Set, TEXT("Tint"));
	const FMaterialMcpParameter* SwitchValue = FindParameter(Set, TEXT("UseTint"));
	const FMaterialMcpParameter* NoiseValue = FindParameter(Set, TEXT("Noise"));
	TestTrue(TEXT("Speed set"), SpeedValue && SpeedValue->Value == TEXT("2.5") && SpeedValue->bOverridden);
	TestTrue(TEXT("Tint set (vector setter read back)"), TintValue && TintValue->Value.StartsWith(TEXT("0, 0.3, 1")) && TintValue->bOverridden);
	TestTrue(TEXT("UseTint set"), SwitchValue && SwitchValue->Value == TEXT("false") && SwitchValue->bOverridden);
	TestTrue(TEXT("Noise set"), NoiseValue && NoiseValue->Value.Contains(TEXT("Black")) && NoiseValue->bOverridden);

	// One invalid entry: nothing is applied.
	TArray<FMaterialMcpParameterValue> Bad;
	Bad.Add({ TEXT("Speed"), TEXT("7") });
	Bad.Add({ TEXT("UseTint"), TEXT("maybe") });
	Bad.Add({ TEXT("Spead"), TEXT("1") });
	AddExpectedMessagePlain(TEXT("Nothing was changed"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	TestTrue(TEXT("Invalid batch rejected"), UMaterialMcpEditorTools::SetMaterialInstanceParameters(Instance, Bad, false).Path.IsEmpty());
	const FMaterialMcpParameters AfterBad = UMaterialMcpEditorTools::GetMaterialParameters(Instance);
	TestTrue(TEXT("Speed unchanged after rejected batch"), FindParameter(AfterBad, TEXT("Speed")) && FindParameter(AfterBad, TEXT("Speed"))->Value == TEXT("2.5"));
	TestTrue(TEXT("Static switch rejects unparseable text"), UMaterialMcpEditorTools::SetMaterialInstanceParameter(Instance, TEXT("UseTint"), TEXT("maybe"), false).Path.IsEmpty());

	// Clear one override.
	const FMaterialMcpParameters Cleared = UMaterialMcpEditorTools::ClearMaterialInstanceParameters(Instance, { TEXT("Speed") }, false);
	const FMaterialMcpParameter* SpeedCleared = FindParameter(Cleared, TEXT("Speed"));
	TestTrue(TEXT("Speed inherits again"), SpeedCleared && !SpeedCleared->bOverridden && SpeedCleared->Value == TEXT("1.0"));
	TestTrue(TEXT("Tint still overridden"), FindParameter(Cleared, TEXT("Tint")) && FindParameter(Cleared, TEXT("Tint"))->bOverridden);

	// Reparent.
	const FMaterialMcpParameters Reparented = UMaterialMcpEditorTools::SetMaterialInstanceParent(Instance, Alt, false);
	TestEqual(TEXT("New parent"), Reparented.Parent, Alt->GetPathName());
	TestEqual(TEXT("Parameters of the new parent"), Reparented.Parameters.Num(), 2);
	const FMaterialMcpParameter* TintReparented = FindParameter(Reparented, TEXT("Tint"));
	TestTrue(TEXT("Tint override kept"), TintReparented && TintReparented->bOverridden && TintReparented->Value.StartsWith(TEXT("0, 0.3, 1")));
	AddExpectedMessagePlain(TEXT("cannot be its own ancestor"), ELogVerbosity::Warning);
	TestTrue(TEXT("Self parent rejected"), UMaterialMcpEditorTools::SetMaterialInstanceParent(Instance, Instance, false).Path.IsEmpty());

	// Clear everything.
	const FMaterialMcpParameters AllCleared = UMaterialMcpEditorTools::ClearMaterialInstanceParameters(Instance, {}, true);
	TestFalse(TEXT("No overrides left"), AllCleared.Parameters.ContainsByPredicate([](const FMaterialMcpParameter& P) { return P.bOverridden; }));
	TestTrue(TEXT("Instance saved"), AllCleared.bSaved);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpMaterialPreviewTest, "NiagaraMcp.Material.PreviewOverTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpMaterialPreviewTest::RunTest(const FString& Parameters)
{
	using namespace NiagaraMcpMaterialTests;
	const FString Folder = NewFolder(TEXT("Preview"));

	// Emissive = frac(Time * 0.5): a uniform gray that brightens over time.
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *UMaterialMcpEditorTools::CreateMaterial(Folder + TEXT("/M_TimePulse"), false).MaterialPath);
	if (!TestNotNull(TEXT("Material"), Material))
	{
		return false;
	}
	UMaterialMcpEditorTools::SetMaterialProperty(Material, TEXT("ShadingModel"), TEXT("Unlit"), false);
	const FMaterialMcpExpressionInfo Time = UMaterialMcpEditorTools::AddExpression(Material, TEXT("Time"), -600, 0);
	const FMaterialMcpExpressionInfo Scale = UMaterialMcpEditorTools::AddExpression(Material, TEXT("Multiply"), -400, 0);
	UMaterialMcpEditorTools::SetExpressionProperty(Material, Scale.Name, TEXT("ConstB"), TEXT("0.5"));
	const FMaterialMcpExpressionInfo Frac = UMaterialMcpEditorTools::AddExpression(Material, TEXT("Frac"), -200, 0);
	UMaterialMcpEditorTools::ConnectExpressions(Material, Time.Name, TEXT("Auto"), Scale.Name, TEXT("A"));
	UMaterialMcpEditorTools::ConnectExpressions(Material, Scale.Name, TEXT("Auto"), Frac.Name, TEXT("Auto"));
	UMaterialMcpEditorTools::ConnectToMaterialProperty(Material, Frac.Name, TEXT("Auto"), TEXT("EmissiveColor"));
	TestTrue(TEXT("Time material compiles"), UMaterialMcpEditorTools::CompileMaterial(Material, true).bSucceeded);

	constexpr int32 FrameSize = 64;
	const FToolsetImage Sheet = UMaterialMcpEditorTools::CapturePreview(Material, TEXT("Plane"), FrameSize, 3, 0.5f);
	TArray<uint8> Png;
	AddInfo(FString::Printf(TEXT("Time preview saved: %s"), *SaveImage(Sheet, TEXT("material_time_preview.png"), &Png)));
	if (!TestTrue(TEXT("Contact sheet returned"), Png.Num() > 0))
	{
		return false;
	}
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	TArray64<uint8> Raw;
	if (!TestTrue(TEXT("PNG decodes"), Wrapper.IsValid() && Wrapper->SetCompressed(Png.GetData(), Png.Num()) && Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw)))
	{
		return false;
	}
	const int64 Width = Wrapper->GetWidth();
	TestEqual(TEXT("Sheet width = 3 frames"), Width, static_cast<int64>(3 * FrameSize));
	TestEqual(TEXT("Sheet height = 1 row"), Wrapper->GetHeight(), static_cast<int64>(FrameSize));
	auto CenterBrightness = [&Raw, Width](int32 FrameIndex)
	{
		const int64 X = FrameIndex * FrameSize + FrameSize / 2;
		const int64 Y = FrameSize / 2;
		const uint8* Pixel = &Raw[(Y * Width + X) * 4];
		return (static_cast<int32>(Pixel[0]) + Pixel[1] + Pixel[2]) / 3;
	};
	const int32 B0 = CenterBrightness(0);
	const int32 B1 = CenterBrightness(1);
	const int32 B2 = CenterBrightness(2);
	AddInfo(FString::Printf(TEXT("Frame brightness at t=0/0.5/1.0: %d %d %d"), B0, B1, B2));
	TestTrue(TEXT("Frame 1 differs from frame 0 (time advanced)"), FMath::Abs(B1 - B0) > 5);
	TestTrue(TEXT("Frame 2 differs from frame 1 (time advanced)"), FMath::Abs(B2 - B1) > 5);
	TestTrue(TEXT("Brightness grows with time"), B0 < B1 && B1 < B2);

	AddExpectedMessagePlain(TEXT("FrameCount must be 1-16"), ELogVerbosity::Warning);
	TestTrue(TEXT("FrameCount 0 rejected"), UMaterialMcpEditorTools::CapturePreview(Material, TEXT("Plane"), FrameSize, 0, 0.5f).Data.IsEmpty());
	return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
