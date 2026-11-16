#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "MaterialMcpEditorTools.h"
#include "Materials/Material.h"
#include "NiagaraMcpEditorTools.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitterHandle.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

/**
 * End-to-end check of the toolsets without an MCP client: builds the chain-strike effect from the demo
 * (ribbon material with Custom HLSL, a module script with Map Get -> Custom Hlsl -> Map Set, and a three
 * step emitter setup) under /Game/NiagaraMcpTests and compiles everything.
 *
 * Run: Session Frontend > Automation > NiagaraMcp, or
 *   UnrealEditor-Cmd.exe <Project>.uproject -ExecCmds="Automation RunTests NiagaraMcp;Quit" -unattended -nopause
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpEndToEndTest, "NiagaraMcp.EndToEnd.ChainStrike",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

namespace NiagaraMcpTests
{
	const FNiagaraMcpGraphNode* FindNodeByClass(const FNiagaraMcpGraphInfo& Graph, const TCHAR* ClassName)
	{
		return Graph.Nodes.FindByPredicate([ClassName](const FNiagaraMcpGraphNode& Node) { return Node.Class == ClassName; });
	}

	const FNiagaraMcpGraphPin* FindPin(const FNiagaraMcpGraphNode& Node, const TCHAR* Direction, const TCHAR* Type)
	{
		return Node.Pins.FindByPredicate([Direction, Type](const FNiagaraMcpGraphPin& Pin) { return Pin.Direction == Direction && Pin.Type == Type; });
	}

	FNiagaraMcpHlslPin Pin(const TCHAR* Name, const TCHAR* Type)
	{
		FNiagaraMcpHlslPin Result;
		Result.Name = Name;
		Result.Type = Type;
		return Result;
	}
}

bool FNiagaraMcpEndToEndTest::RunTest(const FString& Parameters)
{
	using namespace NiagaraMcpTests;

	const FString Folder = FString::Printf(TEXT("/Game/NiagaraMcpTests/Run_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));

	// ------------------------------------------------------------------ Ribbon material (Custom HLSL)
	const FMaterialMcpGraphInfo MaterialInfo = UMaterialMcpEditorTools::CreateMaterial(Folder + TEXT("/M_ChainRibbon"), true);
	if (!TestFalse(TEXT("CreateMaterial returns a path"), MaterialInfo.MaterialPath.IsEmpty()))
	{
		return false;
	}
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialInfo.MaterialPath);
	TestNotNull(TEXT("Material loads"), Material);

	TestEqual(TEXT("BlendMode"), UMaterialMcpEditorTools::SetMaterialProperty(Material, TEXT("BlendMode"), TEXT("Masked"), false).Value, FString(TEXT("BLEND_Masked")));
	TestEqual(TEXT("ShadingModel"), UMaterialMcpEditorTools::SetMaterialProperty(Material, TEXT("ShadingModel"), TEXT("Unlit"), false).Value, FString(TEXT("MSM_Unlit")));
	UMaterialMcpEditorTools::SetMaterialProperty(Material, TEXT("TwoSided"), TEXT("true"), false);
	UMaterialMcpEditorTools::SetMaterialProperty(Material, TEXT("bUsedWithNiagaraRibbons"), TEXT("true"), false);

	const FMaterialMcpExpressionInfo TexCoord = UMaterialMcpEditorTools::AddExpression(Material, TEXT("TextureCoordinate"), -700, 0);
	const FMaterialMcpExpressionInfo Rings = UMaterialMcpEditorTools::AddExpression(Material, TEXT("Custom"), -350, -150);
	const FMaterialMcpExpressionInfo Steel = UMaterialMcpEditorTools::AddExpression(Material, TEXT("Custom"), -350, 150);
	TestFalse(TEXT("Expressions created"), TexCoord.Name.IsEmpty() || Rings.Name.IsEmpty() || Steel.Name.IsEmpty());

	const FString RingCommon =
		TEXT("float x = frac(UV.x * 13.0 + 0.5) - 0.5;\n")
		TEXT("float y = UV.y - 0.5;\n")
		TEXT("float a = length(float2(max(abs(x) - 0.23, 0.0) / 0.45, y)) - 0.39;\n")
		TEXT("float b = length(float2(max(0.50 - abs(x) - 0.14, 0.0) / 0.68, y)) - 0.195;\n");
	const FMaterialMcpExpressionInfo RingsAfter = UMaterialMcpEditorTools::SetCustomExpression(Material, Rings.Name,
		RingCommon + TEXT("float link = abs(a) - 0.062;\nfloat edgeLink = abs(b) - 0.049;\nreturn 1.0 - step(0.0, min(link, edgeLink));"),
		TEXT("Alternating interlocking rings"), { TEXT("UV") }, TEXT("Float1"));
	TestEqual(TEXT("Custom input count"), RingsAfter.Inputs.Num(), 1);
	UMaterialMcpEditorTools::SetCustomExpression(Material, Steel.Name,
		RingCommon +
		TEXT("float rim = saturate(1.0 - min(abs(a) / 0.062, abs(b) / 0.049));\n")
		TEXT("float upper = saturate(0.55 - y * 1.1);\n")
		TEXT("float3 metal = float3(0.080,0.018,0.022) + float3(0.37,0.062,0.048) * rim * upper;\n")
		TEXT("float tip = pow(saturate(UV.x), 9.0);\n")
		TEXT("return metal + float3(2.5,0.17,0.021) * tip * (0.30 + 0.70 * rim);"),
		TEXT("Dark steel and hot red tip"), { TEXT("UV") }, TEXT("Float3"));

	UMaterialMcpEditorTools::ConnectExpressions(Material, TexCoord.Name, TEXT(""), Rings.Name, TEXT("UV"));
	UMaterialMcpEditorTools::ConnectExpressions(Material, TexCoord.Name, TEXT(""), Steel.Name, TEXT("UV"));
	UMaterialMcpEditorTools::ConnectToMaterialProperty(Material, Rings.Name, TEXT(""), TEXT("OpacityMask"));
	const FMaterialMcpGraphInfo Wired = UMaterialMcpEditorTools::ConnectToMaterialProperty(Material, Steel.Name, TEXT(""), TEXT("EmissiveColor"));
	TestEqual(TEXT("Two material outputs wired"), Wired.PropertyConnections.Num(), 2);

	const FMaterialMcpCompileResult MaterialCompile = UMaterialMcpEditorTools::CompileMaterial(Material, true);
	for (const FString& CompileError : MaterialCompile.Errors)
	{
		AddError(FString::Printf(TEXT("Material compile: %s"), *CompileError));
	}

	// ------------------------------------------------------------------ Module script (Map Get -> Custom Hlsl -> Map Set)
	const FNiagaraMcpGraphInfo NewModule = UNiagaraMcpEditorTools::CreateModuleScript(Folder + TEXT("/NM_ChainStrikeMotion"),
		TEXT("Drops a ribbon chain, shakes it on impact and pulls it back."), { TEXT("ParticleSpawn"), TEXT("ParticleUpdate") }, true);
	if (!TestFalse(TEXT("CreateModuleScript returns a path"), NewModule.ScriptPath.IsEmpty()))
	{
		return false;
	}
	TestTrue(TEXT("Module allowed in ParticleUpdate"), NewModule.AllowedStacks.Contains(TEXT("ParticleUpdateScript")));
	UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, *NewModule.ScriptPath);
	TestNotNull(TEXT("Module script loads"), ModuleScript);

	for (const FNiagaraMcpGraphNode& Node : NewModule.Nodes)
	{
		AddInfo(FString::Printf(TEXT("Template node: %s (%s) pins=%d"), *Node.Title, *Node.Class, Node.Pins.Num()));
	}

	// Template: Input Map -> Map Set -> Output Module. Rewire it as Input Map -> Map Get / Map Set -> Output.
	const FNiagaraMcpGraphNode* InputNode = FindNodeByClass(NewModule, TEXT("NiagaraNodeInput"));
	const FNiagaraMcpGraphNode* SetNodeInfo = FindNodeByClass(NewModule, TEXT("NiagaraNodeParameterMapSet"));
	if (!TestNotNull(TEXT("Template has an input map node"), InputNode) || !TestNotNull(TEXT("Template has a Map Set node"), SetNodeInfo))
	{
		return false;
	}
	const FString InputId = InputNode->NodeId;
	const FString SetId = SetNodeInfo->NodeId;
	const FNiagaraMcpGraphPin* InputMapOut = FindPin(*InputNode, TEXT("Output"), TEXT("NiagaraParameterMap"));
	TestNotNull(TEXT("Input map output pin"), InputMapOut);

	FString GetId;
	if (const FNiagaraMcpGraphNode* ExistingGet = FindNodeByClass(NewModule, TEXT("NiagaraNodeParameterMapGet")))
	{
		GetId = ExistingGet->NodeId;
	}
	else
	{
		const FNiagaraMcpGraphNode GetNode = UNiagaraMcpEditorTools::AddGraphNode(ModuleScript, TEXT("MapGet"), -300, 200);
		GetId = GetNode.NodeId;
		const FNiagaraMcpGraphPin* GetMapIn = FindPin(GetNode, TEXT("Input"), TEXT("NiagaraParameterMap"));
		if (InputMapOut && GetMapIn)
		{
			UNiagaraMcpEditorTools::ConnectPins(ModuleScript, InputId, FName(*InputMapOut->Name), GetId, FName(*GetMapIn->Name));
		}
	}
	TestFalse(TEXT("Map Get exists"), GetId.IsEmpty());

	struct FGetPin { const TCHAR* Name; const TCHAR* Type; const TCHAR* Default; };
	const FGetPin GetPins[] =
	{
		{ TEXT("Particles.RibbonLinkOrder"), TEXT("float"), nullptr },
		{ TEXT("Particles.Age"), TEXT("float"), nullptr },
		{ TEXT("Module.LandingX"), TEXT("float"), TEXT("0.0") },
		{ TEXT("Module.LandingY"), TEXT("float"), TEXT("0.0") },
		{ TEXT("Module.Lean"), TEXT("float"), TEXT("-72.0") },
		{ TEXT("Module.Length"), TEXT("float"), TEXT("305.0") },
	};
	for (const FGetPin& GetPin : GetPins)
	{
		const FNiagaraMcpGraphNode After = UNiagaraMcpEditorTools::AddParameterPin(ModuleScript, GetId, GetPin.Name, GetPin.Type);
		TestTrue(FString::Printf(TEXT("Map Get pin %s"), GetPin.Name), After.Pins.ContainsByPredicate([&GetPin](const FNiagaraMcpGraphPin& P) { return P.Direction == TEXT("Output") && P.Name == GetPin.Name; }));
		if (GetPin.Default)
		{
			const FNiagaraMcpGraphNode WithDefault = UNiagaraMcpEditorTools::SetPinDefaultValue(ModuleScript, GetId, GetPin.Name, GetPin.Default);
			const FNiagaraMcpGraphPin* Output = WithDefault.Pins.FindByPredicate([&GetPin](const FNiagaraMcpGraphPin& P) { return P.Direction == TEXT("Output") && P.Name == GetPin.Name; });
			TestTrue(FString::Printf(TEXT("Default of %s"), GetPin.Name), Output && FMath::IsNearlyEqual(FCString::Atof(*Output->DefaultValue), FCString::Atof(GetPin.Default)));
		}
	}
	UNiagaraMcpEditorTools::AddParameterPin(ModuleScript, SetId, TEXT("Particles.Position"), TEXT("position"));
	const FNiagaraMcpGraphNode SetAfter = UNiagaraMcpEditorTools::AddParameterPin(ModuleScript, SetId, TEXT("Particles.RibbonWidth"), TEXT("float"));
	TestTrue(TEXT("Map Set pins"), SetAfter.Pins.ContainsByPredicate([](const FNiagaraMcpGraphPin& P) { return P.Direction == TEXT("Input") && P.Name == TEXT("Particles.RibbonWidth"); }));

	const FNiagaraMcpGraphNode HlslNode = UNiagaraMcpEditorTools::AddGraphNode(ModuleScript, TEXT("CustomHlsl"), 0, 200);
	const FString HlslId = HlslNode.NodeId;
	const FNiagaraMcpGraphNode HlslAfter = UNiagaraMcpEditorTools::SetCustomHlsl(ModuleScript, HlslId,
		TEXT("float u = saturate(U);\n")
		TEXT("float descent = saturate(Age / 0.34);\n")
		TEXT("float fall = (1.0 - descent * descent) * 440.0;\n")
		TEXT("float impact = max(Age - 0.34, 0.0);\n")
		TEXT("float shake = step(0.34, Age) * exp(-6.7 * impact);\n")
		TEXT("float body = sin(u * 3.14159265);\n")
		TEXT("float whip = body * (sin(u * 9.0 - Age * 17.0) * 17.0 * (1.0 - descent) + sin(impact * 26.0 - u * 10.0) * 27.0 * shake);\n")
		TEXT("float depth = body * sin(impact * 19.0 - u * 8.0) * 15.0 * shake;\n")
		TEXT("float lift = step(0.34, Age) * abs(sin(impact * 18.0)) * exp(-11.0 * impact) * 16.0;\n")
		TEXT("float r = saturate((Age - 0.91) / 0.33);\n")
		TEXT("float retract = r * r * (3.0 - 2.0 * r);\n")
		TEXT("Position = float3(LandingX + (1.0 - u) * Lean + whip, LandingY + depth + (1.0 - u) * Lean * 0.29, 5.0 + fall + (1.0 - u) * max(Length, 1.0) + u * lift + retract * 370.0);\n")
		TEXT("float f = saturate((Age - 1.06) / 0.22);\n")
		TEXT("Width = 18.0 * (1.0 - f * f * (3.0 - 2.0 * f));"),
		{ Pin(TEXT("U"), TEXT("float")), Pin(TEXT("Age"), TEXT("float")), Pin(TEXT("LandingX"), TEXT("float")), Pin(TEXT("LandingY"), TEXT("float")), Pin(TEXT("Lean"), TEXT("float")), Pin(TEXT("Length"), TEXT("float")) },
		{ Pin(TEXT("Position"), TEXT("position")), Pin(TEXT("Width"), TEXT("float")) });
	TestTrue(TEXT("Custom Hlsl has the U input"), HlslAfter.Pins.ContainsByPredicate([](const FNiagaraMcpGraphPin& P) { return P.Direction == TEXT("Input") && P.Name == TEXT("U"); }));
	TestFalse(TEXT("Custom Hlsl code stored"), HlslAfter.CustomHlsl.IsEmpty());

	struct FLink { FString From; const TCHAR* FromPin; FString To; const TCHAR* ToPin; };
	const FLink Links[] =
	{
		{ GetId, TEXT("Particles.RibbonLinkOrder"), HlslId, TEXT("U") },
		{ GetId, TEXT("Particles.Age"), HlslId, TEXT("Age") },
		{ GetId, TEXT("Module.LandingX"), HlslId, TEXT("LandingX") },
		{ GetId, TEXT("Module.LandingY"), HlslId, TEXT("LandingY") },
		{ GetId, TEXT("Module.Lean"), HlslId, TEXT("Lean") },
		{ GetId, TEXT("Module.Length"), HlslId, TEXT("Length") },
		{ HlslId, TEXT("Position"), SetId, TEXT("Particles.Position") },
		{ HlslId, TEXT("Width"), SetId, TEXT("Particles.RibbonWidth") },
	};
	for (const FLink& Link : Links)
	{
		const FNiagaraMcpGraphNode After = UNiagaraMcpEditorTools::ConnectPins(ModuleScript, Link.From, Link.FromPin, Link.To, Link.ToPin);
		TestFalse(FString::Printf(TEXT("Connect %s -> %s"), Link.FromPin, Link.ToPin), After.NodeId.IsEmpty());
	}
	UNiagaraMcpEditorTools::ApplyScriptChanges(ModuleScript, true);

	// ------------------------------------------------------------------ System with three ribbon chains
	const FNiagaraMcpAssetResult SystemAsset = UNiagaraMcpEditorTools::CreateSystem(Folder + TEXT("/NS_ChainStrike"), true);
	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemAsset.Path);
	if (!TestNotNull(TEXT("System created"), System))
	{
		return false;
	}

	struct FChain { const TCHAR* Name; const TCHAR* SpawnTime; const TCHAR* LandingX; const TCHAR* Lean; };
	const FChain Chains[] =
	{
		{ TEXT("ChainLeft"), TEXT("0.02"), TEXT("-60"), TEXT("-30") },
		{ TEXT("Chain"), TEXT("0.32"), TEXT("6"), TEXT("-42") },
		{ TEXT("ChainRight"), TEXT("0.62"), TEXT("70"), TEXT("-55") },
	};
	for (const FChain& Chain : Chains)
	{
		const FNiagaraMcpEmitterInfo Emitter = UNiagaraMcpEditorTools::AddEmitter(System, Chain.Name, TEXT(""), false);
		TestEqual(TEXT("Emitter name"), Emitter.Name, FName(Chain.Name));

		const FNiagaraMcpStackInfo Before = UNiagaraMcpEditorTools::GetStack(System, Chain.Name, false);
		for (const FNiagaraMcpModuleInfo& Module : Before.Modules)
		{
			AddInfo(FString::Printf(TEXT("%s: %s [%s] v%s"), Chain.Name, *Module.DisplayName, *Module.Stack, *Module.ScriptVersion));
		}

		if (Before.Modules.ContainsByPredicate([](const FNiagaraMcpModuleInfo& Module) { return Module.DisplayName == TEXT("Spawn Rate"); }))
		{
			const FNiagaraMcpStackInfo AfterRemove = UNiagaraMcpEditorTools::RemoveModule(System, Chain.Name, TEXT("Spawn Rate"), TEXT(""), false);
			TestEqual(TEXT("Spawn Rate removed"), AfterRemove.Modules.Num(), Before.Modules.Num() - 1);
		}
		UNiagaraMcpEditorTools::AddModule(System, Chain.Name, TEXT("EmitterUpdate"), TEXT("/Niagara/Modules/Beams/BeamEmitterSetup.BeamEmitterSetup"), -1, false);
		const FNiagaraMcpModuleInfo Burst = UNiagaraMcpEditorTools::AddModule(System, Chain.Name, TEXT("EmitterUpdate"), TEXT("/Niagara/Modules/Emitter/SpawnBurst_Instantaneous.SpawnBurst_Instantaneous"), -1, false);
		TestFalse(TEXT("Burst added"), Burst.ModuleId.IsEmpty());
		UNiagaraMcpEditorTools::AddModule(System, Chain.Name, TEXT("ParticleSpawn"), TEXT("/Niagara/Modules/Beams/SpawnBeam.SpawnBeam"), -1, false);
		const FNiagaraMcpModuleInfo Motion = UNiagaraMcpEditorTools::AddModule(System, Chain.Name, TEXT("ParticleUpdate"), NewModule.ScriptPath, -1, false);
		TestTrue(TEXT("Custom module exposes Length"), Motion.Inputs.ContainsByPredicate([](const FNiagaraMcpInputInfo& Input) { return Input.Name == TEXT("Length"); }));

		const FNiagaraMcpInputInfo Count = UNiagaraMcpEditorTools::SetModuleInput(System, Chain.Name, Burst.ModuleId, TEXT("Spawn Count"), TEXT("40"), TEXT(""), false);
		TestEqual(TEXT("Spawn Count"), Count.Value, FString(TEXT("40")));
		UNiagaraMcpEditorTools::SetModuleInput(System, Chain.Name, Burst.ModuleId, TEXT("Spawn Time"), Chain.SpawnTime, TEXT(""), false);
		// Lifetime is a child of the Lifetime Mode switch: switch to Direct Set first, as in the demo.
		const FNiagaraMcpInputInfo LifetimeMode = UNiagaraMcpEditorTools::SetModuleInput(System, Chain.Name, TEXT("Initialize Particle"), TEXT("Lifetime Mode"), TEXT("Direct Set"), TEXT(""), false);
		TestTrue(TEXT("Lifetime Mode is static"), LifetimeMode.bStatic);
		TestEqual(TEXT("Lifetime Mode shows its display name"), LifetimeMode.Value, FString(TEXT("Direct Set")));
		const FNiagaraMcpInputInfo Lifetime = UNiagaraMcpEditorTools::SetModuleInput(System, Chain.Name, TEXT("Initialize Particle"), TEXT("Lifetime"), TEXT("1.35"), TEXT(""), false);
		TestTrue(TEXT("Lifetime set"), FMath::IsNearlyEqual(FCString::Atof(*Lifetime.Value), 1.35f, 0.001f));
		UNiagaraMcpEditorTools::SetModuleInput(System, Chain.Name, Motion.ModuleId, TEXT("LandingX"), Chain.LandingX, TEXT(""), false);
		const FNiagaraMcpInputInfo Lean = UNiagaraMcpEditorTools::SetModuleInput(System, Chain.Name, Motion.ModuleId, TEXT("Lean"), Chain.Lean, TEXT(""), false);
		TestEqual(TEXT("Lean"), FCString::Atof(*Lean.Value), FCString::Atof(Chain.Lean));
		UNiagaraMcpEditorTools::SetModuleInput(System, Chain.Name, TEXT("Emitter State"), TEXT("Loop Duration"), TEXT("2.1"), TEXT(""), false);

		const FNiagaraMcpNoteInfo Note = UNiagaraMcpEditorTools::SetModuleNote(System, Chain.Name, Motion.ModuleId, TEXT("AI generated"), TEXT("Drop, impact shake and retract of the chain."), false, TEXT(""), false);
		TestEqual(TEXT("Note stored"), Note.Header, FString(TEXT("AI generated")));

		// Replace the template's sprite renderer with a ribbon.
		const FNiagaraMcpRendererInfo Ribbon = UNiagaraMcpEditorTools::AddRenderer(System, Chain.Name, TEXT("Ribbon"), MaterialInfo.MaterialPath, false);
		TestEqual(TEXT("Ribbon renderer class"), Ribbon.Class, FString(TEXT("NiagaraRibbonRendererProperties")));
		const FNiagaraMcpEmitterInfo AfterRenderer = UNiagaraMcpEditorTools::RemoveRenderer(System, Chain.Name, 0, false);
		TestEqual(TEXT("One renderer left"), AfterRenderer.Renderers.Num(), 1);
	}

	const FNiagaraMcpPropertyResult LocalSpace = UNiagaraMcpEditorTools::SetEmitterProperty(System, TEXT("Chain"), TEXT("bLocalSpace"), TEXT("true"), false);
	TestEqual(TEXT("Local space"), LocalSpace.Value, FString(TEXT("True")));

	const FNiagaraMcpStackInfo Stack = UNiagaraMcpEditorTools::GetStack(System, TEXT("Chain"), true);
	TestTrue(TEXT("Stack has the motion module with a note"), Stack.Modules.ContainsByPredicate([](const FNiagaraMcpModuleInfo& Module) { return Module.bHasNote; }));

	const FNiagaraMcpCompileResult Compile = UNiagaraMcpEditorTools::CompileSystem(System, true, true);
	for (const FString& Status : Compile.ScriptStatus)
	{
		AddInfo(Status);
	}
	for (const FNiagaraMcpCompileMessage& Message : Compile.Messages)
	{
		if (Message.Severity == TEXT("Error"))
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Message.Script, *Message.Message));
		}
		else
		{
			AddInfo(FString::Printf(TEXT("%s %s: %s"), *Message.Severity, *Message.Script, *Message.Message));
		}
	}
	TestTrue(TEXT("System compiled without errors"), Compile.bSucceeded);

	const FNiagaraMcpSystemSummary Summary = UNiagaraMcpEditorTools::GetSystemSummary(System);
	TestEqual(TEXT("Three emitters"), Summary.Emitters.Num(), 3);
	return !HasAnyErrors();
}

/** Parameters, a texture object sampled in Custom HLSL and a vector parameter, as in the mimic-eye material. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpMaterialParametersTest, "NiagaraMcp.EndToEnd.MaterialParameters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraMcpMaterialParametersTest::RunTest(const FString& Parameters)
{
	const FString Path = FString::Printf(TEXT("/Game/NiagaraMcpTests/Run_%s/M_ParamEye"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	const FMaterialMcpGraphInfo Created = UMaterialMcpEditorTools::CreateMaterial(Path, true);
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *Created.MaterialPath);
	if (!TestNotNull(TEXT("Material created"), Material))
	{
		return false;
	}
	UMaterialMcpEditorTools::SetMaterialProperty(Material, TEXT("ShadingModel"), TEXT("Unlit"), false);

	const FMaterialMcpExpressionInfo Time = UMaterialMcpEditorTools::AddExpression(Material, TEXT("Time"), -900, -200);
	const FMaterialMcpExpressionInfo UV = UMaterialMcpEditorTools::AddExpression(Material, TEXT("TextureCoordinate"), -900, 0);
	const FMaterialMcpExpressionInfo Rate = UMaterialMcpEditorTools::AddExpression(Material, TEXT("ScalarParameter"), -900, 150);
	const FMaterialMcpExpressionInfo Noise = UMaterialMcpEditorTools::AddExpression(Material, TEXT("TextureObjectParameter"), -900, 300);
	const FMaterialMcpExpressionInfo Tint = UMaterialMcpEditorTools::AddExpression(Material, TEXT("VectorParameter"), -900, 500);
	const FMaterialMcpExpressionInfo Eye = UMaterialMcpEditorTools::AddExpression(Material, TEXT("Custom"), -400, 0);

	TestEqual(TEXT("Scalar parameter name"), UMaterialMcpEditorTools::SetExpressionProperty(Material, Rate.Name, TEXT("ParameterName"), TEXT("Gaze Changes Per Second")).Label, FString(TEXT("Gaze Changes Per Second")));
	UMaterialMcpEditorTools::SetExpressionProperty(Material, TEXT("Gaze Changes Per Second"), TEXT("DefaultValue"), TEXT("2.3"));
	UMaterialMcpEditorTools::SetExpressionProperty(Material, Noise.Name, TEXT("ParameterName"), TEXT("Blink Noise"));
	const FMaterialMcpExpressionInfo NoiseAfter = UMaterialMcpEditorTools::SetExpressionProperty(Material, TEXT("Blink Noise"), TEXT("Texture"), TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
	TestEqual(TEXT("Texture parameter"), NoiseAfter.Label, FString(TEXT("Blink Noise")));
	UMaterialMcpEditorTools::SetExpressionProperty(Material, Tint.Name, TEXT("ParameterName"), TEXT("Eye Color"));
	UMaterialMcpEditorTools::SetExpressionProperty(Material, TEXT("Eye Color"), TEXT("DefaultValue"), TEXT("1, 0.32, 0.035, 1"));

	UMaterialMcpEditorTools::SetCustomExpression(Material, Eye.Name,
		TEXT("float n = Texture2DSample(Noise, NoiseSampler, UV + frac(Seconds * Rate * 0.1)).r;\nreturn Tint.rgb * n;"),
		TEXT("Noise tint"), { TEXT("UV"), TEXT("Seconds"), TEXT("Rate"), TEXT("Noise"), TEXT("Tint") }, TEXT("Float3"));
	UMaterialMcpEditorTools::ConnectExpressions(Material, UV.Name, TEXT(""), Eye.Name, TEXT("UV"));
	UMaterialMcpEditorTools::ConnectExpressions(Material, Time.Name, TEXT(""), Eye.Name, TEXT("Seconds"));
	UMaterialMcpEditorTools::ConnectExpressions(Material, TEXT("Gaze Changes Per Second"), TEXT(""), Eye.Name, TEXT("Rate"));
	UMaterialMcpEditorTools::ConnectExpressions(Material, TEXT("Blink Noise"), TEXT(""), Eye.Name, TEXT("Noise"));
	const FMaterialMcpExpressionInfo Wired = UMaterialMcpEditorTools::ConnectExpressions(Material, TEXT("Eye Color"), TEXT(""), Eye.Name, TEXT("Tint"));
	TestEqual(TEXT("All custom inputs connected"), Wired.Inputs.FilterByPredicate([](const FString& Input) { return Input.Contains(TEXT("<-")); }).Num(), 5);
	UMaterialMcpEditorTools::ConnectToMaterialProperty(Material, Eye.Name, TEXT(""), TEXT("EmissiveColor"));
	UMaterialMcpEditorTools::LayoutMaterial(Material);

	const FMaterialMcpCompileResult Compile = UMaterialMcpEditorTools::CompileMaterial(Material, true);
	for (const FString& CompileError : Compile.Errors)
	{
		AddError(FString::Printf(TEXT("Material compile: %s"), *CompileError));
	}
	TestTrue(TEXT("Material compiled"), Compile.bSucceeded);
	return !HasAnyErrors();
}

/** Saves a tool image as PNG under Saved/NiagaraMcpTests so the result can be looked at. */
static FString SaveToolImage(const FToolsetImage& Image, const FString& Name)
{
	TArray<uint8> Bytes;
	if (Image.Data.IsEmpty() || !FBase64::Decode(Image.Data, Bytes))
	{
		return FString();
	}
	const FString File = FPaths::ProjectSavedDir() / TEXT("NiagaraMcpTests") / Name;
	return FFileHelper::SaveArrayToFile(Bytes, *File) ? File : FString();
}

/** Tools added after 0.2.0: preview, counts, validation, curves, stages, events, local modules, node actions, layout, undo, assets, log, material instances. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpVerificationTest, "NiagaraMcp.EndToEnd.Verification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraMcpVerificationTest::RunTest(const FString& Parameters)
{
	const FString Folder = FString::Printf(TEXT("/Game/NiagaraMcpTests/Verify_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	const FNiagaraMcpAssetResult Created = UNiagaraMcpEditorTools::CreateSystem(Folder + TEXT("/NS_Sparks"), true);
	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *Created.Path);
	if (!TestNotNull(TEXT("System"), System))
	{
		return false;
	}
	UNiagaraMcpEditorTools::AddEmitter(System, TEXT("Sparks"), TEXT(""), false);

	// The Minimal template has no spawn module: the plugin check must say so.
	const FNiagaraMcpValidationResult Before = UNiagaraMcpEditorTools::ValidateSystem(System, true);
	TestTrue(TEXT("Validation flags the missing spawn module"), Before.Issues.ContainsByPredicate([](const FNiagaraMcpIssue& Issue) { return Issue.Message.Contains(TEXT("Spawn")); }));

	const FNiagaraMcpModuleInfo Burst = UNiagaraMcpEditorTools::AddModule(System, TEXT("Sparks"), TEXT("EmitterUpdate"), TEXT("/Niagara/Modules/Emitter/SpawnBurst_Instantaneous.SpawnBurst_Instantaneous"), -1, false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Sparks"), Burst.ModuleId, TEXT("Spawn Count"), TEXT("25"), TEXT(""), false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Sparks"), TEXT("Initialize Particle"), TEXT("Lifetime Mode"), TEXT("Direct Set"), TEXT(""), false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Sparks"), TEXT("Initialize Particle"), TEXT("Lifetime"), TEXT("1.0"), TEXT(""), false);
	UNiagaraMcpEditorTools::AddModule(System, TEXT("Sparks"), TEXT("ParticleSpawn"), TEXT("/Niagara/Modules/Spawn/Location/SphereLocation.SphereLocation"), -1, false);

	// Curves inside a dynamic input.
	UNiagaraMcpEditorTools::AddModule(System, TEXT("Sparks"), TEXT("ParticleUpdate"), TEXT("/Niagara/Modules/Update/Color/Color.Color"), -1, false);
	const FNiagaraMcpInputInfo ColorInput = UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Sparks"), TEXT("Color"), TEXT("Color"), TEXT("Dynamic:/Niagara/DynamicInputs/ValueFromCurve/ColorFromCurve.ColorFromCurve"), TEXT("ParticleUpdate"), false);
	TestEqual(TEXT("Color uses a dynamic input"), ColorInput.Mode, FString(TEXT("Dynamic")));
	TArray<FNiagaraMcpCurveKey> Keys;
	Keys.Add({ 0.0f, TEXT("1, 0.6, 0.1, 1") });
	Keys.Add({ 1.0f, TEXT("0.6, 0.05, 0.02, 0") });
	const FNiagaraMcpCurveInfo Curve = UNiagaraMcpEditorTools::SetCurveKeys(System, TEXT("Sparks"), TEXT("Color"), TEXT("Color/ColorCurve"), Keys, TEXT("Linear"), TEXT("ParticleUpdate"), false);
	TestEqual(TEXT("Color curve has 4 channels"), Curve.Channels.Num(), 4);
	TestEqual(TEXT("Color curve has 2 keys"), Curve.Keys.Num(), 2);
	const FNiagaraMcpCurveInfo CurveBack = UNiagaraMcpEditorTools::GetCurveKeys(System, TEXT("Sparks"), TEXT("Color"), TEXT("Color/ColorCurve"), TEXT("ParticleUpdate"));
	TestTrue(TEXT("Curve read back"), CurveBack.Keys.Num() == 2 && CurveBack.Keys[1].Value.StartsWith(TEXT("0.6")));

	const FNiagaraMcpValidationResult After = UNiagaraMcpEditorTools::ValidateSystem(System, true);
	for (const FNiagaraMcpIssue& Issue : After.Issues)
	{
		AddInfo(FString::Printf(TEXT("%s %s: %s (fixes %d)"), *Issue.Severity, *Issue.Source, *Issue.Message, Issue.Fixes.Num()));
	}
	TestTrue(TEXT("System validates without errors"), After.bOk);

	// Particle counts: 25 alive at 0.3 s, none at 1.5 s (lifetime 1 s).
	const TArray<FNiagaraMcpParticleSample> Samples = UNiagaraMcpEditorTools::GetParticleCounts(System, { 0.3f, 1.5f });
	if (TestEqual(TEXT("Two samples"), Samples.Num(), 2))
	{
		AddInfo(FString::Printf(TEXT("Particles at 0.3 s: %d, at 1.5 s: %d"), Samples[0].TotalParticles, Samples[1].TotalParticles));
		TestEqual(TEXT("25 particles at 0.3 s"), Samples[0].TotalParticles, 25);
		TestTrue(TEXT("Fewer particles at 1.5 s"), Samples[1].TotalParticles < 25);
	}

	// Preview images.
	const FToolsetImage Sheet = UNiagaraMcpEditorTools::CapturePreview(System, { 0.05f, 0.3f, 0.6f, 0.9f }, 256, 0.0f, -15.0f, 0.0f).Image;
	TestTrue(TEXT("Preview is a PNG"), Sheet.MimeType.Contains(TEXT("png")) && Sheet.Data.Len() > 1000);
	AddInfo(FString::Printf(TEXT("Preview saved: %s"), *SaveToolImage(Sheet, TEXT("sparks_preview.png"))));

	// Simulation stage + local module + node actions + layout on a GPU emitter.
	UNiagaraMcpEditorTools::AddEmitter(System, TEXT("GridFx"), TEXT(""), false);
	UNiagaraMcpEditorTools::SetEmitterProperty(System, TEXT("GridFx"), TEXT("SimTarget"), TEXT("GPUComputeSim"), false);
	UNiagaraMcpEditorTools::SetEmitterProperty(System, TEXT("GridFx"), TEXT("CalculateBoundsMode"), TEXT("Fixed"), false);
	const FNiagaraMcpEmitterStages Stages = UNiagaraMcpEditorTools::AddSimulationStage(System, TEXT("GridFx"), TEXT("Advect"), TEXT("Particles"), NAME_None, TEXT(""), -1, false);
	TestEqual(TEXT("One simulation stage"), Stages.SimulationStages.Num(), 1);
	const FNiagaraMcpLocalModuleInfo Local = UNiagaraMcpEditorTools::CreateLocalModule(System, TEXT("GridFx"), TEXT("SimulationStage"), 0, TEXT("AdvectModule"), false);
	TestFalse(TEXT("Local module created"), Local.ScriptPath.IsEmpty());
	TestEqual(TEXT("ListLocalModules"), UNiagaraMcpEditorTools::ListLocalModules(System).Num(), 1);
	if (UNiagaraScript* LocalScript = LoadObject<UNiagaraScript>(nullptr, *Local.ScriptPath))
	{
		const TArray<FNiagaraMcpNodeAction> Actions = UNiagaraMcpEditorTools::ListNodeActions(LocalScript, TEXT("sine"), 10);
		TestTrue(TEXT("Node actions found"), Actions.Num() > 0);
		if (Actions.Num() > 0)
		{
			AddInfo(FString::Printf(TEXT("First action: %s | %s"), *Actions[0].Name, *Actions[0].Category));
			const FNiagaraMcpGraphNode Node = UNiagaraMcpEditorTools::AddGraphNode(LocalScript, FString::Printf(TEXT("Action:%s|%s"), *Actions[0].Name, *Actions[0].Category), 100, 100);
			TestFalse(TEXT("Node created from a menu action"), Node.NodeId.IsEmpty());
		}
		const FNiagaraMcpGraphInfo Laid = UNiagaraMcpEditorTools::LayoutScriptGraph(LocalScript, false);
		TestTrue(TEXT("Layout returns the graph"), Laid.Nodes.Num() >= 3);
		UNiagaraMcpEditorTools::ApplyScriptChanges(LocalScript, false);
	}

	// Event handler on a CPU emitter fed by death events of Sparks.
	UNiagaraMcpEditorTools::AddEmitter(System, TEXT("Trails"), TEXT(""), false);
	const FNiagaraMcpEmitterStages Events = UNiagaraMcpEditorTools::AddEventHandler(System, TEXT("Trails"), TEXT("Sparks"), TEXT("DeathEvent"), TEXT("SpawnedParticles"), 3, 0, false);
	TestEqual(TEXT("One event handler"), Events.EventHandlers.Num(), 1);
	TestEqual(TEXT("Event source"), Events.EventHandlers.Num() ? Events.EventHandlers[0].SourceEmitter : FString(), FString(TEXT("Sparks")));
	UNiagaraMcpEditorTools::CompileSystem(System, true, true);

	// Undo / redo through the editor transaction buffer.
	UNiagaraMcpEditorTools::RenameEmitter(System, TEXT("Trails"), TEXT("TrailsRenamed"), false);
	const FNiagaraMcpUndoResult Undone = UNiagaraMcpEditorTools::Undo(1);
	TestTrue(TEXT("Undo reverted the rename"), Undone.Transactions.Num() == 1 && System->GetEmitterHandles().ContainsByPredicate([](const FNiagaraEmitterHandle& H) { return H.GetName() == TEXT("Trails"); }));
	const FNiagaraMcpUndoResult Redone = UNiagaraMcpEditorTools::Redo(1);
	TestTrue(TEXT("Redo applied the rename again"), Redone.Transactions.Num() == 1 && System->GetEmitterHandles().ContainsByPredicate([](const FNiagaraEmitterHandle& H) { return H.GetName() == TEXT("TrailsRenamed"); }));

	// Assets and log.
	const FNiagaraMcpAssetPage Found = UNiagaraMcpEditorTools::FindAssets(TEXT("NiagaraSystem"), TEXT("NS_Sparks"), Folder, 10, 0);
	TestEqual(TEXT("FindAssets finds the system"), Found.TotalMatched, 1);
	const FNiagaraMcpLogPage Log = UNiagaraMcpEditorTools::ReadOutputLog({ TEXT("LogNiagaraMcp") }, TEXT("Log"), TEXT(""), 0, 50);
	TestTrue(TEXT("Output log captured"), Log.Lines.Num() > 0 && Log.NextCursor > 0);

	// Material: validate, stats, preview, instance.
	const FMaterialMcpGraphInfo Material = UMaterialMcpEditorTools::CreateMaterial(Folder + TEXT("/M_Glow"), true);
	UMaterial* Glow = LoadObject<UMaterial>(nullptr, *Material.MaterialPath);
	if (TestNotNull(TEXT("Material"), Glow))
	{
		UMaterialMcpEditorTools::SetMaterialProperty(Glow, TEXT("ShadingModel"), TEXT("Unlit"), false);
		const FMaterialMcpExpressionInfo Tint = UMaterialMcpEditorTools::AddExpression(Glow, TEXT("VectorParameter"), -400, 0);
		UMaterialMcpEditorTools::SetExpressionProperty(Glow, Tint.Name, TEXT("ParameterName"), TEXT("Tint"));
		UMaterialMcpEditorTools::SetExpressionProperty(Glow, TEXT("Tint"), TEXT("DefaultValue"), TEXT("1, 0.4, 0.1, 1"));
		const FMaterialMcpExpressionInfo Unused = UMaterialMcpEditorTools::AddExpression(Glow, TEXT("ScalarParameter"), -400, 300);
		UMaterialMcpEditorTools::ConnectToMaterialProperty(Glow, TEXT("Tint"), TEXT(""), TEXT("EmissiveColor"));
		const FMaterialMcpValidation Validation = UMaterialMcpEditorTools::ValidateMaterial(Glow);
		TestTrue(TEXT("Material valid"), Validation.bOk);
		TestTrue(TEXT("Unused expression reported"), Validation.Issues.ContainsByPredicate([&Unused](const FMaterialMcpIssue& Issue) { return Issue.Expression == Unused.Name; }));
		const FMaterialMcpStats Stats = UMaterialMcpEditorTools::GetMaterialStats(Glow);
		TestTrue(TEXT("Instruction count"), Stats.PixelShaderInstructions > 0);
		AddInfo(FString::Printf(TEXT("M_Glow: %d PS instructions, %d samplers"), Stats.PixelShaderInstructions, Stats.Samplers));
		const FToolsetImage MaterialImage = UMaterialMcpEditorTools::CapturePreview(Glow, TEXT("Sphere"), 256);
		TestTrue(TEXT("Material preview"), MaterialImage.Data.Len() > 1000);
		AddInfo(FString::Printf(TEXT("Material preview saved: %s"), *SaveToolImage(MaterialImage, TEXT("glow_preview.png"))));

		const FMaterialMcpParameters Instance = UMaterialMcpEditorTools::CreateMaterialInstance(Glow, Folder + TEXT("/MI_Glow_Blue"), true);
		UMaterialInstanceConstant* InstanceAsset = LoadObject<UMaterialInstanceConstant>(nullptr, *Instance.Path);
		if (TestNotNull(TEXT("Material instance"), InstanceAsset))
		{
			const FMaterialMcpParameters Changed = UMaterialMcpEditorTools::SetMaterialInstanceParameter(InstanceAsset, TEXT("Tint"), TEXT("0, 0.3, 1, 1"), true);
			const FMaterialMcpParameter* TintValue = Changed.Parameters.FindByPredicate([](const FMaterialMcpParameter& P) { return P.Name == TEXT("Tint"); });
			AddInfo(FString::Printf(TEXT("Instance Tint = %s"), TintValue ? *TintValue->Value : TEXT("(missing)")));
			TestTrue(TEXT("Instance tint overridden"), TintValue && TintValue->Value.StartsWith(TEXT("0, 0.3")));
		}
	}
	return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
