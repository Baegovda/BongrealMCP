#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "MaterialMcpEditorTools.h"
#include "NiagaraDataChannelAsset.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraMcpEditorTools.h"
#include "NiagaraSystem.h"

/**
 * v0.5 analysis tools, on systems built in the test (/Game/NiagaraMcpTests/Analysis/Run_<time>/...).
 *   UnrealEditor-Cmd.exe <Project>.uproject -ExecCmds="Automation RunTests NiagaraMcp.Analysis;Quit" -unattended -nopause
 */
namespace AnalysisTests
{
	FString RunFolder(const TCHAR* Sub)
	{
		static const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		return FString::Printf(TEXT("/Game/NiagaraMcpTests/Analysis/Run_%s/%s"), *Stamp, Sub);
	}

	/** A system with one Minimal emitter plus Spawn Rate (50/s), Initialize Particle with a 2 s lifetime, sprite renderer. */
	UNiagaraSystem* MakeSystem(const FString& Path, FName Emitter)
	{
		const FNiagaraMcpAssetResult Created = UNiagaraMcpEditorTools::CreateSystem(Path, false);
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *Created.Path);
		if (System && !Emitter.IsNone())
		{
			UNiagaraMcpEditorTools::AddEmitter(System, Emitter, TEXT("Minimal"), false);
			UNiagaraMcpEditorTools::AddModule(System, Emitter, TEXT("EmitterUpdate"), TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate"), -1, false);
			UNiagaraMcpEditorTools::SetModuleInput(System, Emitter, TEXT("Spawn Rate"), TEXT("SpawnRate"), TEXT("50"), TEXT("Any"), false);
			UNiagaraMcpEditorTools::SetModuleInput(System, Emitter, TEXT("Initialize Particle"), TEXT("Lifetime Mode"), TEXT("Direct Set"), TEXT("Any"), false);
			UNiagaraMcpEditorTools::SetModuleInput(System, Emitter, TEXT("Initialize Particle"), TEXT("Lifetime"), TEXT("2"), TEXT("Any"), false);
		}
		return System;
	}

	const FNiagaraMcpEmitterAttributes* FindEmitter(const FNiagaraMcpAttributeSample& Sample, FName Emitter)
	{
		return Sample.Emitters.FindByPredicate([Emitter](const FNiagaraMcpEmitterAttributes& E) { return E.Emitter == Emitter; });
	}

	const FNiagaraMcpAttributeStats* FindStats(const FNiagaraMcpEmitterAttributes* Emitter, const FString& Attribute)
	{
		return Emitter ? Emitter->Attributes.FindByPredicate([&Attribute](const FNiagaraMcpAttributeStats& S) { return S.Attribute == Attribute; }) : nullptr;
	}

	TArray<double> Numbers(const FString& Text)
	{
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","));
		TArray<double> Out;
		for (const FString& Part : Parts)
		{
			Out.Add(FCString::Atod(*Part.TrimStartAndEnd()));
		}
		return Out;
	}

	/** Hue in degrees of a "r, g, b, a" text. */
	float Hue(const FString& Text)
	{
		const TArray<double> V = Numbers(Text);
		return V.Num() >= 3 ? FLinearColor(V[0], V[1], V[2]).LinearRGBToHSV().R : -1.0f;
	}

	float HueDistance(float A, float B)
	{
		const float D = FMath::Fmod(FMath::Abs(A - B), 360.0f);
		return FMath::Min(D, 360.0f - D);
	}

	FString MeanColor(UNiagaraSystem* System, FName Emitter, float Time)
	{
		const TArray<FNiagaraMcpAttributeSample> Samples = UNiagaraMcpEditorTools::GetParticleAttributes(System, { Time }, { TEXT("Color") }, Emitter, 0);
		const FNiagaraMcpAttributeStats* Color = Samples.Num() == 1 ? FindStats(FindEmitter(Samples[0], Emitter), TEXT("Color")) : nullptr;
		return Color ? Color->Mean : FString();
	}
}

// ------------------------------------------------------------------------------------------------ GPU readback

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpAnalysisGpuReadbackTest, "NiagaraMcp.Analysis.GpuReadback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpAnalysisGpuReadbackTest::RunTest(const FString& Parameters)
{
	using namespace AnalysisTests;
	UNiagaraSystem* System = MakeSystem(RunFolder(TEXT("NS_GpuReadback")), TEXT("Gpu"));
	if (!TestNotNull(TEXT("System"), System))
	{
		return false;
	}
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Gpu"), TEXT("Initialize Particle"), TEXT("Lifetime"), TEXT("5"), TEXT("Any"), false);
	UNiagaraMcpEditorTools::AddModule(System, TEXT("Gpu"), TEXT("ParticleSpawn"), TEXT("/Niagara/Modules/Spawn/Location/SphereLocation.SphereLocation"), -1, false);
	UNiagaraMcpEditorTools::SetEmitterProperty(System, TEXT("Gpu"), TEXT("SimTarget"), TEXT("GPUComputeSim"), false);
	UNiagaraMcpEditorTools::SetEmitterProperty(System, TEXT("Gpu"), TEXT("CalculateBoundsMode"), TEXT("Fixed"), false);
	UNiagaraMcpEditorTools::SetEmitterProperty(System, TEXT("Gpu"), TEXT("FixedBounds"), TEXT("(Min=(X=-500,Y=-500,Z=-500),Max=(X=500,Y=500,Z=500),IsValid=True)"), false);
	TestTrue(TEXT("GPU system compiles"), UNiagaraMcpEditorTools::CompileSystem(System, true, false).bSucceeded);

	const TArray<FNiagaraMcpAttributeSample> Samples = UNiagaraMcpEditorTools::GetParticleAttributes(System, { 1.0f, 0.3f }, { TEXT("Age"), TEXT("Position"), TEXT("Color") }, TEXT("Gpu"), 4);
	if (!TestEqual(TEXT("Two samples"), Samples.Num(), 2))
	{
		return false;
	}
	const FNiagaraMcpEmitterAttributes* Late = FindEmitter(Samples[0], TEXT("Gpu"));
	const FNiagaraMcpEmitterAttributes* Early = FindEmitter(Samples[1], TEXT("Gpu"));
	if (!TestTrue(TEXT("Emitter reported at both times"), Late && Early))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("GPU: %d particles at 0.3 s, %d at 1.0 s; note: %s"), Early->Particles, Late->Particles, *Late->Note));
	TestEqual(TEXT("Sim target"), Late->SimTarget, FString(TEXT("GPUComputeSim")));
	TestTrue(TEXT("Read back through the sim cache"), Late->Note.Contains(TEXT("sim cache")));
	TestTrue(TEXT("Particles exist"), Early->Particles > 0 && Late->Particles > Early->Particles);
	const FNiagaraMcpAttributeStats* AgeEarly = FindStats(Early, TEXT("Age"));
	const FNiagaraMcpAttributeStats* AgeLate = FindStats(Late, TEXT("Age"));
	if (TestTrue(TEXT("Age read back at both times"), AgeEarly && AgeLate))
	{
		AddInfo(FString::Printf(TEXT("Age 0.3 s: mean %s max %s (n %d) | 1.0 s: mean %s max %s (n %d)"), *AgeEarly->Mean, *AgeEarly->Max, AgeEarly->Count, *AgeLate->Mean, *AgeLate->Max, AgeLate->Count));
		const double MaxEarly = FCString::Atod(*AgeEarly->Max);
		const double MaxLate = FCString::Atod(*AgeLate->Max);
		TestTrue(TEXT("Age is a real number within the run time (0.3 s)"), MaxEarly > 0.1 && MaxEarly <= 0.31);
		TestTrue(TEXT("Age grows over time (1.0 s)"), MaxLate > 0.8 && MaxLate <= 1.01 && FCString::Atod(*AgeLate->Mean) > FCString::Atod(*AgeEarly->Mean));
		TestEqual(TEXT("Sample values"), AgeLate->Samples.Num(), 4);
		TestEqual(TEXT("Count matches particles"), AgeLate->Count, Late->Particles);
	}
	const FNiagaraMcpAttributeStats* Position = FindStats(Late, TEXT("Position"));
	if (TestNotNull(TEXT("Position read back"), Position))
	{
		const TArray<double> Mean = Numbers(Position->Mean);
		TestTrue(TEXT("Position has 3 finite components"), Mean.Num() == 3 && FMath::IsFinite(Mean[0]) && FMath::IsFinite(Mean[2]) && FMath::Abs(Mean[0]) < 10000.0);
		const TArray<double> PMin = Numbers(Position->Min);
		const TArray<double> PMax = Numbers(Position->Max);
		TestTrue(TEXT("Positions spread over the sphere (real GPU values)"), PMin.Num() == 3 && PMax.Num() == 3 && PMax[0] - PMin[0] > 1.0 && PMax[2] - PMin[2] > 1.0);
		AddInfo(FString::Printf(TEXT("Position min %s max %s"), *Position->Min, *Position->Max));
	}
	return !HasAnyErrors();
}

// ------------------------------------------------------------------------------------------------ Lightweight emitters

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpAnalysisLightweightTest, "NiagaraMcp.Analysis.Lightweight", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpAnalysisLightweightTest::RunTest(const FString& Parameters)
{
	using namespace AnalysisTests;
	UNiagaraSystem* System = MakeSystem(RunFolder(TEXT("NS_Lightweight")), NAME_None);
	if (!TestNotNull(TEXT("System"), System))
	{
		return false;
	}
	const FNiagaraMcpEmitterInfo Added = UNiagaraMcpEditorTools::AddLightweightEmitter(System, TEXT("Light"), TEXT("MinimalLightweight"), false);
	TestEqual(TEXT("Lightweight emitter added"), Added.Name, FName(TEXT("Light")));
	TestEqual(TEXT("Reported as lightweight"), Added.SimTarget, FString(TEXT("Lightweight")));
	TestTrue(TEXT("Has a renderer"), Added.Renderers.Num() > 0);
	const FNiagaraEmitterHandle* Handle = System->GetEmitterHandles().FindByPredicate([](const FNiagaraEmitterHandle& H) { return H.GetName() == TEXT("Light"); });
	TestTrue(TEXT("Handle is stateless"), Handle && Handle->GetEmitterMode() == ENiagaraEmitterMode::Stateless);

	const FNiagaraMcpStatelessEmitterInfo Info = UNiagaraMcpEditorTools::GetStatelessEmitterInfo(System, TEXT("Light"));
	TArray<FString> Names;
	for (const FNiagaraMcpStatelessModuleInfo& Module : Info.Modules)
	{
		Names.Add(FString::Printf(TEXT("%s%s"), *Module.Name, Module.bEnabled ? TEXT("") : TEXT("(off)")));
	}
	AddInfo(FString::Printf(TEXT("Lightweight modules: %s; template %s; %d emitter properties"), *FString::Join(Names, TEXT(", ")), *Info.Template, Info.EmitterProperties.Num()));
	const FNiagaraMcpStatelessModuleInfo* Init = Info.Modules.FindByPredicate([](const FNiagaraMcpStatelessModuleInfo& M) { return M.Name == TEXT("InitializeParticle"); });
	if (!TestNotNull(TEXT("InitializeParticle module listed"), Init))
	{
		return false;
	}
	TestTrue(TEXT("Module properties listed"), Init->Properties.ContainsByPredicate([](const FNiagaraMcpEditableProperty& P) { return P.Name == TEXT("ColorDistribution"); }));
	TestTrue(TEXT("Emitter properties listed"), Info.EmitterProperties.ContainsByPredicate([](const FNiagaraMcpEditableProperty& P) { return P.Name == TEXT("FixedBounds"); }));

	{
		const TArray<FNiagaraMcpAttributeSample> Probe = UNiagaraMcpEditorTools::GetParticleAttributes(System, { 0.5f }, { TEXT("Color"), TEXT("Age") }, TEXT("Light"), 0);
		const FNiagaraMcpEmitterAttributes* E = Probe.Num() == 1 ? FindEmitter(Probe[0], TEXT("Light")) : nullptr;
		AddInfo(FString::Printf(TEXT("Lightweight probe: %s, %d particles, state %s, note: %s"), E ? *E->SimTarget : TEXT("-"), E ? E->Particles : -1, E ? *E->State : TEXT("-"), E ? *E->Note : TEXT("-")));
	}
	const FString Before = MeanColor(System, TEXT("Light"), 0.5f);
	const FNiagaraMcpPropertyResult Min = UNiagaraMcpEditorTools::SetStatelessModuleProperty(System, TEXT("Light"), TEXT("Initialize Particle"), TEXT("ColorDistribution.Values"), TEXT("((R=0,G=1,B=0,A=1))"), false);
	AddInfo(FString::Printf(TEXT("ColorDistribution.Values -> %s"), *Min.Value));
	TestTrue(TEXT("SetStatelessModuleProperty read back"), Min.Value.Contains(TEXT("G=1.0")));
	const FString After = MeanColor(System, TEXT("Light"), 0.5f);
	AddInfo(FString::Printf(TEXT("Lightweight mean color before %s, after %s"), *Before, *After));
	const TArray<double> Color = Numbers(After);
	TestTrue(TEXT("Particles turned green (read back from the lightweight emitter)"), Color.Num() >= 3 && Color[1] > 0.9 && Color[0] < 0.1 && Color[2] < 0.1);

	const FNiagaraMcpPropertyResult Bounds = UNiagaraMcpEditorTools::SetStatelessModuleProperty(System, TEXT("Light"), TEXT("Emitter"), TEXT("FixedBounds"), TEXT("(Min=(X=-321,Y=-321,Z=-321),Max=(X=321,Y=321,Z=321),IsValid=True)"), false);
	TestTrue(TEXT("Emitter property set"), Bounds.Value.Contains(TEXT("321")));

	AddExpectedMessagePlain(TEXT("Unknown lightweight module"), ELogVerbosity::Warning);
	TestTrue(TEXT("Unknown module rejected"), UNiagaraMcpEditorTools::SetStatelessModuleProperty(System, TEXT("Light"), TEXT("InitParticle"), TEXT("ColorDistribution.Min"), TEXT("1"), false).Property.IsEmpty());
	AddExpectedMessagePlain(TEXT("standard emitter"), ELogVerbosity::Warning);
	UNiagaraMcpEditorTools::AddEmitter(System, TEXT("Std"), TEXT("Minimal"), false);
	TestTrue(TEXT("Standard emitter rejected"), UNiagaraMcpEditorTools::GetStatelessEmitterInfo(System, TEXT("Std")).Modules.Num() == 0);
	return !HasAnyErrors();
}

// ------------------------------------------------------------------------------------------------ DiagnoseVisibility

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpAnalysisDiagnoseTest, "NiagaraMcp.Analysis.DiagnoseVisibility", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpAnalysisDiagnoseTest::RunTest(const FString& Parameters)
{
	using namespace AnalysisTests;
	auto Describe = [this](const TCHAR* Label, const FNiagaraMcpVisibilityReport& Report)
	{
		TArray<FString> Lines;
		for (const FNiagaraMcpVisibilityCheck& Check : Report.Checks)
		{
			if (Check.Status != TEXT("Skipped"))
			{
				Lines.Add(FString::Printf(TEXT("%s/%s=%s"), *Check.Emitter.ToString(), *Check.Check, *Check.Status));
			}
		}
		AddInfo(FString::Printf(TEXT("%s: first fail [%s] %s | fix: %s | %s"), Label, *Report.FirstFailCheck, *Report.FirstFail,
			*(Report.Checks.FindByPredicate([](const FNiagaraMcpVisibilityCheck& C) { return C.Status == TEXT("Fail"); }) ? Report.Checks.FindByPredicate([](const FNiagaraMcpVisibilityCheck& C) { return C.Status == TEXT("Fail"); })->Fix : FString()),
			*FString::Join(Lines, TEXT(" "))));
	};

	// Healthy emitter: nothing fails.
	{
		UNiagaraSystem* System = MakeSystem(RunFolder(TEXT("NS_VisOk")), TEXT("Fx"));
		const FNiagaraMcpVisibilityReport Report = UNiagaraMcpEditorTools::DiagnoseVisibility(System, NAME_None, 0.5f);
		Describe(TEXT("Healthy"), Report);
		TestTrue(TEXT("Healthy emitter is visible"), Report.bVisible && Report.FirstFail.IsEmpty());
		TestTrue(TEXT("Particles counted"), Report.Checks.ContainsByPredicate([](const FNiagaraMcpVisibilityCheck& C) { return C.Check == TEXT("Particles") && C.Status == TEXT("Ok"); }));
		TestTrue(TEXT("Material checked"), Report.Checks.ContainsByPredicate([](const FNiagaraMcpVisibilityCheck& C) { return C.Check == TEXT("MaterialUsage") && C.Status == TEXT("Ok"); }));
	}
	// No spawn module.
	{
		UNiagaraSystem* System = MakeSystem(RunFolder(TEXT("NS_VisNoSpawn")), TEXT("Fx"));
		UNiagaraMcpEditorTools::RemoveModule(System, TEXT("Fx"), TEXT("Spawn Rate"), TEXT("Any"), false);
		const FNiagaraMcpVisibilityReport Report = UNiagaraMcpEditorTools::DiagnoseVisibility(System, TEXT("Fx"), 0.5f);
		Describe(TEXT("No spawn"), Report);
		TestEqual(TEXT("No spawn: first fail is Spawn"), Report.FirstFailCheck, FString(TEXT("Spawn")));
		TestEqual(TEXT("No spawn: emitter named"), Report.FirstFailEmitter, FName(TEXT("Fx")));
		TestFalse(TEXT("No spawn: not visible"), Report.bVisible);
		const FNiagaraMcpVisibilityCheck* Fail = Report.Checks.FindByPredicate([](const FNiagaraMcpVisibilityCheck& C) { return C.Status == TEXT("Fail"); });
		TestTrue(TEXT("No spawn: fix is an AddModule call"), Fail && Fail->Fix.Contains(TEXT("AddModule")) && Fail->Fix.Contains(TEXT("SpawnRate")));
		TestTrue(TEXT("Later checks skipped"), Report.Checks.ContainsByPredicate([](const FNiagaraMcpVisibilityCheck& C) { return C.Status == TEXT("Skipped"); }));
	}
	// Renderer disabled.
	{
		UNiagaraSystem* System = MakeSystem(RunFolder(TEXT("NS_VisNoRenderer")), TEXT("Fx"));
		UNiagaraMcpEditorTools::SetRendererProperty(System, TEXT("Fx"), 0, TEXT("bIsEnabled"), TEXT("false"), false);
		const FNiagaraMcpVisibilityReport Report = UNiagaraMcpEditorTools::DiagnoseVisibility(System, NAME_None, 0.5f);
		Describe(TEXT("Renderer disabled"), Report);
		TestEqual(TEXT("Renderer disabled: first fail is Renderers"), Report.FirstFailCheck, FString(TEXT("Renderers")));
	}
	// Material without the Niagara sprite usage flag (and automatic usage off, as in a cooked build).
	{
		const FString MaterialPath = RunFolder(TEXT("M_NoNiagaraUsage"));
		UMaterialMcpEditorTools::CreateMaterial(MaterialPath, false);
		UObject* Material = LoadObject<UObject>(nullptr, *(MaterialPath + TEXT(".M_NoNiagaraUsage")));
		UMaterialMcpEditorTools::SetMaterialProperty(Material, TEXT("bAutomaticallySetUsageInEditor"), TEXT("false"), false);
		UNiagaraSystem* System = MakeSystem(RunFolder(TEXT("NS_VisMaterial")), TEXT("Fx"));
		UNiagaraMcpEditorTools::SetRendererProperty(System, TEXT("Fx"), 0, TEXT("Material"), Material ? Material->GetPathName() : MaterialPath, false);
		AddExpectedMessagePlain(TEXT("missing usage flag"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
		const FNiagaraMcpVisibilityReport Report = UNiagaraMcpEditorTools::DiagnoseVisibility(System, NAME_None, 0.5f);
		Describe(TEXT("Material usage"), Report);
		TestEqual(TEXT("Material: first fail is MaterialUsage"), Report.FirstFailCheck, FString(TEXT("MaterialUsage")));
		TestTrue(TEXT("Material: names the flag"), Report.FirstFail.Contains(TEXT("bUsedWithNiagaraSprites")));
	}
	// Alpha 0.
	{
		UNiagaraSystem* System = MakeSystem(RunFolder(TEXT("NS_VisAlpha")), TEXT("Fx"));
		UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Fx"), TEXT("Initialize Particle"), TEXT("Color Mode"), TEXT("Direct Set"), TEXT("Any"), false);
		UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Fx"), TEXT("Initialize Particle"), TEXT("Color"), TEXT("1, 0.5, 0.1, 0"), TEXT("Any"), false);
		const FNiagaraMcpVisibilityReport Report = UNiagaraMcpEditorTools::DiagnoseVisibility(System, NAME_None, 0.5f);
		Describe(TEXT("Alpha 0"), Report);
		TestEqual(TEXT("Alpha 0: first fail is Alpha"), Report.FirstFailCheck, FString(TEXT("Alpha")));
	}
	// Lifetime 0: particles die the frame they spawn.
	{
		UNiagaraSystem* System = MakeSystem(RunFolder(TEXT("NS_VisLifetime")), TEXT("Fx"));
		UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Fx"), TEXT("Initialize Particle"), TEXT("Lifetime"), TEXT("0"), TEXT("Any"), false);
		const FNiagaraMcpVisibilityReport Report = UNiagaraMcpEditorTools::DiagnoseVisibility(System, NAME_None, 0.5f);
		Describe(TEXT("Lifetime 0"), Report);
		TestEqual(TEXT("Lifetime 0: first fail is Lifetime"), Report.FirstFailCheck, FString(TEXT("Lifetime")));
	}
	return !HasAnyErrors();
}

// ------------------------------------------------------------------------------------------------ AnalyzeCost / GetCompiledHlsl

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpAnalysisCostTest, "NiagaraMcp.Analysis.Cost", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpAnalysisCostTest::RunTest(const FString& Parameters)
{
	using namespace AnalysisTests;
	UNiagaraSystem* System = MakeSystem(RunFolder(TEXT("NS_Cost")), TEXT("Cpu"));
	if (!TestNotNull(TEXT("System"), System))
	{
		return false;
	}
	UNiagaraMcpEditorTools::AddEmitter(System, TEXT("Gpu"), TEXT("Minimal"), false);
	UNiagaraMcpEditorTools::SetEmitterProperty(System, TEXT("Gpu"), TEXT("SimTarget"), TEXT("GPUComputeSim"), false);
	UNiagaraMcpEditorTools::AddModule(System, TEXT("Gpu"), TEXT("EmitterUpdate"), TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate"), -1, false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Gpu"), TEXT("Spawn Rate"), TEXT("SpawnRate"), TEXT("50"), TEXT("Any"), false);
	UNiagaraMcpEditorTools::SetEmitterProperty(System, TEXT("Gpu"), TEXT("CalculateBoundsMode"), TEXT("Fixed"), false);
	TestTrue(TEXT("Compiles"), UNiagaraMcpEditorTools::CompileSystem(System, true, false).bSucceeded);

	const FNiagaraMcpCostReport Report = UNiagaraMcpEditorTools::AnalyzeCost(System, { 0.5f, 1.0f }, NAME_None);
	if (!TestEqual(TEXT("Two emitters"), Report.Emitters.Num(), 2))
	{
		return false;
	}
	for (const FNiagaraMcpEmitterCost& Cost : Report.Emitters)
	{
		TArray<FString> Scripts;
		for (const FNiagaraMcpScriptCost& Script : Cost.Scripts)
		{
			Scripts.Add(FString::Printf(TEXT("%s/%s %s ops=%d regs=%d bytes=%d hlsl=%d"), *Script.Script, *Script.Target, *Script.CompileStatus, Script.OpCount, Script.TempRegisters, Script.ByteCodeBytes, Script.HlslLength));
		}
		TArray<FString> Stages;
		for (const FNiagaraMcpGpuStageTiming& Stage : Cost.GpuStages)
		{
			Stages.Add(FString::Printf(TEXT("%s %.1fus x%d"), *Stage.Stage, Stage.AverageMicroseconds, Stage.Dispatches));
		}
		AddInfo(FString::Printf(TEXT("%s (%s): counts %d/%d, ops %d | %s | GPU: %s"), *Cost.Emitter.ToString(), *Cost.SimTarget, Cost.ParticleCounts[0], Cost.ParticleCounts[1], Cost.ParticleOpCount,
			*FString::Join(Scripts, TEXT("; ")), *FString::Join(Stages, TEXT("; "))));
	}
	for (const FNiagaraMcpSimStepCost& Step : Report.Steps)
	{
		AddInfo(FString::Printf(TEXT("Step to %.2f s: %.3f ms (%.3f ms/tick)"), Step.Time, Step.Milliseconds, Step.MillisecondsPerTick));
	}
	AddInfo(FString::Printf(TEXT("Ranking: %s | Notes: %s"), *FString::Join(Report.Ranking, TEXT(" / ")), *FString::Join(Report.Notes, TEXT(" / "))));
	const FNiagaraMcpEmitterCost& Cpu = Report.Emitters[0];
	const FNiagaraMcpEmitterCost& Gpu = Report.Emitters[1];
	TestEqual(TEXT("CPU emitter target"), Cpu.SimTarget, FString(TEXT("CPUSim")));
	const FNiagaraMcpScriptCost* Update = Cpu.Scripts.FindByPredicate([](const FNiagaraMcpScriptCost& S) { return S.Script == TEXT("ParticleUpdate"); });
	TestTrue(TEXT("CPU update script has op count, registers and byte code"), Update && Update->OpCount > 0 && Update->TempRegisters > 0 && Update->ByteCodeBytes > 0 && Update->CompileStatus.StartsWith(TEXT("UpToDate")));
	TestTrue(TEXT("Particle op count summed"), Cpu.ParticleOpCount >= (Update ? Update->OpCount : 1));
	TestTrue(TEXT("CPU particle counts grow"), Cpu.ParticleCounts[1] > Cpu.ParticleCounts[0] && Cpu.ParticleCounts[0] > 0);
	TestTrue(TEXT("GPU particle counts read back"), Gpu.ParticleCounts[1] > Gpu.ParticleCounts[0] && Gpu.ParticleCounts[0] > 0);
	TestTrue(TEXT("GPU emitter has the compute script with HLSL"), Gpu.Scripts.ContainsByPredicate([](const FNiagaraMcpScriptCost& S) { return S.Target == TEXT("GPU") && S.HlslLength > 0; }));
	TestTrue(TEXT("GPU timings or a note"), Gpu.GpuStages.Num() > 0 || Report.Notes.ContainsByPredicate([](const FString& N) { return N.Contains(TEXT("GPU")); }));
	TestTrue(TEXT("Steps timed"), Report.Steps.Num() == 2 && Report.Steps[0].Time < Report.Steps[1].Time && Report.Steps[1].Milliseconds > 0.0);
	TestTrue(TEXT("Ranking lists both"), Report.Ranking.Num() == 2 && Report.Ranking[0].StartsWith(TEXT("Cpu")));
	TestTrue(TEXT("System scripts described"), Report.SystemScripts.Num() == 2 && Report.SystemScripts[0].ByteCodeBytes > 0);

	// Compiled HLSL, paged.
	const FNiagaraMcpHlslResult First = UNiagaraMcpEditorTools::GetCompiledHlsl(System, TEXT("Cpu"), TEXT("ParticleUpdate"), TEXT("CPU"), 0, 1000);
	AddInfo(FString::Printf(TEXT("CPU HLSL %s: %d chars"), *First.Script, First.TotalLength));
	TestTrue(TEXT("CPU HLSL window"), First.TotalLength > 1000 && First.Length == 1000 && First.bTruncated && First.Hlsl.Len() == 1000);
	const FNiagaraMcpHlslResult Last = UNiagaraMcpEditorTools::GetCompiledHlsl(System, TEXT("Cpu"), TEXT("ParticleUpdate"), TEXT("CPU"), First.TotalLength - 100, 1000);
	TestTrue(TEXT("Last page"), Last.Offset == First.TotalLength - 100 && Last.Length == 100 && !Last.bTruncated);
	const FNiagaraMcpHlslResult GpuHlsl = UNiagaraMcpEditorTools::GetCompiledHlsl(System, TEXT("Gpu"), TEXT("ParticleGPUCompute"), TEXT("GPU"), 0, 200000);
	AddInfo(FString::Printf(TEXT("GPU HLSL %s: %d chars"), *GpuHlsl.Script, GpuHlsl.TotalLength));
	TestTrue(TEXT("GPU compute HLSL"), GpuHlsl.TotalLength > 1000 && GpuHlsl.Target == TEXT("GPU") && GpuHlsl.Script == TEXT("ParticleGPUComputeScript"));
	const FNiagaraMcpHlslResult SystemHlsl = UNiagaraMcpEditorTools::GetCompiledHlsl(System, NAME_None, TEXT("SystemUpdate"), TEXT("CPU"), 0, 100);
	TestTrue(TEXT("System script HLSL"), SystemHlsl.TotalLength > 100);
	AddExpectedMessagePlain(TEXT("simulates on the CPU"), ELogVerbosity::Warning);
	TestTrue(TEXT("GPU target on a CPU emitter is rejected"), UNiagaraMcpEditorTools::GetCompiledHlsl(System, TEXT("Cpu"), TEXT("ParticleUpdate"), TEXT("GPU"), 0, 100).TotalLength == 0);
	return !HasAnyErrors();
}

// ------------------------------------------------------------------------------------------------ ApplySystemSpec

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpAnalysisApplySpecTest, "NiagaraMcp.Analysis.ApplySystemSpec", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpAnalysisApplySpecTest::RunTest(const FString& Parameters)
{
	using namespace AnalysisTests;
	UNiagaraSystem* A = MakeSystem(RunFolder(TEXT("NS_SpecA")), TEXT("Sparks"));
	if (!TestNotNull(TEXT("System A"), A))
	{
		return false;
	}
	// A: modules added / removed / overridden, a dynamic input with curve keys, emitter + renderer + system properties,
	// bindings, a second disabled emitter.
	UNiagaraMcpEditorTools::RemoveModule(A, TEXT("Sparks"), TEXT("Spawn Rate"), TEXT("Any"), false);
	const FNiagaraMcpModuleInfo Burst = UNiagaraMcpEditorTools::AddModule(A, TEXT("Sparks"), TEXT("EmitterUpdate"), TEXT("/Niagara/Modules/Emitter/SpawnBurst_Instantaneous.SpawnBurst_Instantaneous"), -1, false);
	UNiagaraMcpEditorTools::SetModuleInput(A, TEXT("Sparks"), Burst.ModuleId, TEXT("Spawn Count"), TEXT("25"), TEXT("Any"), false);
	UNiagaraMcpEditorTools::AddModule(A, TEXT("Sparks"), TEXT("ParticleSpawn"), TEXT("/Niagara/Modules/Spawn/Location/SphereLocation.SphereLocation"), -1, false);
	UNiagaraMcpEditorTools::SetModuleInput(A, TEXT("Sparks"), TEXT("Initialize Particle"), TEXT("Color Mode"), TEXT("Direct Set"), TEXT("Any"), false);
	UNiagaraMcpEditorTools::SetModuleInput(A, TEXT("Sparks"), TEXT("Initialize Particle"), TEXT("Color"), TEXT("1, 0.3, 0.1, 1"), TEXT("Any"), false);
	UNiagaraMcpEditorTools::AddModule(A, TEXT("Sparks"), TEXT("ParticleUpdate"), TEXT("/Niagara/Modules/Update/Color/Color.Color"), -1, false);
	UNiagaraMcpEditorTools::SetModuleInput(A, TEXT("Sparks"), TEXT("Color"), TEXT("Color"), TEXT("Dynamic:/Niagara/DynamicInputs/ValueFromCurve/ColorFromCurve.ColorFromCurve"), TEXT("ParticleUpdate"), false);
	UNiagaraMcpEditorTools::SetEmitterProperty(A, TEXT("Sparks"), TEXT("bLocalSpace"), TEXT("true"), false);
	UNiagaraMcpEditorTools::AddRenderer(A, TEXT("Sparks"), TEXT("Ribbon"), TEXT("None"), false);
	UNiagaraMcpEditorTools::SetRendererProperty(A, TEXT("Sparks"), 0, TEXT("SortMode"), TEXT("ViewDistance"), false);
	UNiagaraMcpEditorTools::SetUserParameter(A, TEXT("MySize"), TEXT("vec2"), TEXT("20, 20"), false);
	UNiagaraMcpEditorTools::SetRendererBinding(A, TEXT("Sparks"), 0, TEXT("SpriteSizeBinding"), TEXT("User.MySize"), false);
	UNiagaraMcpEditorTools::SetSystemProperty(A, TEXT("WarmupTime"), TEXT("1.0"), false);
	UNiagaraMcpEditorTools::AddEmitter(A, TEXT("Smoke"), TEXT("Minimal"), false);
	UNiagaraMcpEditorTools::SetEmitterEnabled(A, TEXT("Smoke"), false, false);
	UNiagaraMcpEditorTools::AddModule(A, TEXT("Smoke"), TEXT("EmitterUpdate"), TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate"), -1, false);
	UNiagaraMcpEditorTools::SetModuleInput(A, TEXT("Smoke"), TEXT("Spawn Rate"), TEXT("SpawnRate"), TEXT("7"), TEXT("Any"), false);
	TestTrue(TEXT("A compiles"), UNiagaraMcpEditorTools::CompileSystem(A, true, true).bSucceeded);

	const FNiagaraMcpSystemSpec Spec = UNiagaraMcpEditorTools::ExportSystemSpec(A);
	AddInfo(FString::Printf(TEXT("Spec A: %s"), *Spec.Spec));

	const FNiagaraMcpAssetResult CreatedB = UNiagaraMcpEditorTools::CreateSystem(RunFolder(TEXT("NS_SpecB")), false);
	UNiagaraSystem* B = LoadObject<UNiagaraSystem>(nullptr, *CreatedB.Path);
	if (!TestNotNull(TEXT("System B"), B))
	{
		return false;
	}
	// Dry run: lists operations, changes nothing.
	const FNiagaraMcpApplySpecResult Dry = UNiagaraMcpEditorTools::ApplySystemSpec(B, Spec.Spec, TEXT("Merge"), true, false);
	TestTrue(TEXT("Dry run lists operations"), Dry.Applied > 3 && Dry.Operations.ContainsByPredicate([](const FNiagaraMcpSpecOperation& O) { return O.Action == TEXT("AddEmitter") && O.Status == TEXT("WouldApply"); }));
	TestEqual(TEXT("Dry run changes nothing"), B->GetEmitterHandles().Num(), 0);

	auto LogResult = [this](const TCHAR* Label, const FNiagaraMcpApplySpecResult& Result)
	{
		TArray<FString> Problems;
		for (const FNiagaraMcpSpecOperation& Op : Result.Operations)
		{
			if (Op.Status != TEXT("Applied"))
			{
				Problems.Add(FString::Printf(TEXT("%s %s %s: %s"), *Op.Status, *Op.Action, *Op.Path, *Op.Reason));
			}
		}
		AddInfo(FString::Printf(TEXT("%s: applied %d, unchanged %d, skipped %d, failed %d. %s"), Label, Result.Applied, Result.Unchanged, Result.Skipped, Result.Failed, *FString::Join(Problems, TEXT(" | "))));
	};
	auto LogDiff = [this](const TCHAR* Label, const FNiagaraMcpSpecDiff& Diff)
	{
		TArray<FString> Lines;
		for (const FNiagaraMcpSpecDifference& D : Diff.Differences)
		{
			Lines.Add(FString::Printf(TEXT("%s: A=%s B=%s"), *D.Path, *D.A.Left(120), *D.B.Left(120)));
		}
		AddInfo(FString::Printf(TEXT("%s: %d differences %s"), Label, Diff.Differences.Num(), *FString::Join(Lines, TEXT(" | "))));
	};

	const FNiagaraMcpApplySpecResult Applied = UNiagaraMcpEditorTools::ApplySystemSpec(B, Spec.Spec, TEXT("Merge"), false, true);
	LogResult(TEXT("Merge into empty B"), Applied);
	TestEqual(TEXT("No failed operations"), Applied.Failed, 0);
	TestTrue(TEXT("B saved"), Applied.bSaved);
	const FNiagaraMcpSpecDiff Diff = UNiagaraMcpEditorTools::DiffSystems(A, B);
	LogDiff(TEXT("Diff A/B"), Diff);
	TestTrue(TEXT("ExportSystemSpec(A) -> ApplySystemSpec(B) -> DiffSystems: identical"), Diff.bIdentical);
	TestTrue(TEXT("B compiles"), UNiagaraMcpEditorTools::CompileSystem(B, true, false).bSucceeded);

	// Applying again changes nothing.
	const FNiagaraMcpApplySpecResult Again = UNiagaraMcpEditorTools::ApplySystemSpec(B, Spec.Spec, TEXT("Merge"), true, false);
	LogResult(TEXT("Second dry run"), Again);
	TestEqual(TEXT("Nothing left to apply"), Again.Applied, 0);

	// Replace removes what the spec does not have.
	UNiagaraMcpEditorTools::AddEmitter(B, TEXT("Extra"), TEXT("Minimal"), false);
	UNiagaraMcpEditorTools::AddModule(B, TEXT("Sparks"), TEXT("ParticleUpdate"), TEXT("/Niagara/Modules/Update/Forces/Drag.Drag"), -1, false);
	UNiagaraMcpEditorTools::SetModuleInput(B, TEXT("Sparks"), TEXT("Initialize Particle"), TEXT("Mass Mode"), TEXT("Random"), TEXT("Any"), false);
	const FNiagaraMcpApplySpecResult Merge = UNiagaraMcpEditorTools::ApplySystemSpec(B, Spec.Spec, TEXT("Merge"), false, false);
	TestEqual(TEXT("Merge keeps extra emitters"), B->GetEmitterHandles().Num(), 3);
	TestFalse(TEXT("Merge leaves differences"), UNiagaraMcpEditorTools::DiffSystems(A, B).bIdentical);
	const FNiagaraMcpApplySpecResult Replace = UNiagaraMcpEditorTools::ApplySystemSpec(B, Spec.Spec, TEXT("Replace"), false, false);
	LogResult(TEXT("Replace"), Replace);
	TestEqual(TEXT("Replace: no failures"), Replace.Failed, 0);
	const FNiagaraMcpSpecDiff ReplaceDiff = UNiagaraMcpEditorTools::DiffSystems(A, B);
	LogDiff(TEXT("Diff after Replace"), ReplaceDiff);
	TestTrue(TEXT("Replace restores the spec exactly"), ReplaceDiff.bIdentical);
	TestEqual(TEXT("Replace removed the extra emitter"), B->GetEmitterHandles().Num(), 2);

	// A failing nested call is recorded (and reported once with the whole result); everything else is still applied.
	FString BadSpec = Spec.Spec.Replace(TEXT("\"Spawn Count\":\"25\""), TEXT("\"Spawn Count\":\"lots\""));
	TestTrue(TEXT("Spec edited"), BadSpec != Spec.Spec);
	UNiagaraSystem* C = MakeSystem(RunFolder(TEXT("NS_SpecC")), NAME_None);
	AddExpectedMessagePlain(TEXT("lots"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	const FNiagaraMcpApplySpecResult Partial = UNiagaraMcpEditorTools::ApplySystemSpec(C, BadSpec, TEXT("Merge"), false, false);
	LogResult(TEXT("Bad value"), Partial);
	const FNiagaraMcpSpecOperation* FailedOp = Partial.Operations.FindByPredicate([](const FNiagaraMcpSpecOperation& O) { return O.Status == TEXT("Failed"); });
	TestTrue(TEXT("Bad value: one failed operation with a reason"), Partial.Failed == 1 && FailedOp && FailedOp->Path.Contains(TEXT("Spawn Count")) && !FailedOp->Reason.IsEmpty());
	TestTrue(TEXT("Bad value: the rest applied"), Partial.Applied > 10 && C->GetEmitterHandles().Num() == 2);

	AddExpectedMessagePlain(TEXT("Unknown Mode"), ELogVerbosity::Warning);
	TestEqual(TEXT("Bad mode rejected"), UNiagaraMcpEditorTools::ApplySystemSpec(B, Spec.Spec, TEXT("Overwrite"), true, false).Applied, 0);
	AddExpectedMessagePlain(TEXT("not a JSON object"), ELogVerbosity::Warning);
	UNiagaraMcpEditorTools::ApplySystemSpec(B, TEXT("{nope"), TEXT("Merge"), true, false);
	return !HasAnyErrors();
}

// ------------------------------------------------------------------------------------------------ SaveEmitterAsAsset

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpAnalysisSaveEmitterTest, "NiagaraMcp.Analysis.SaveEmitterAsAsset", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpAnalysisSaveEmitterTest::RunTest(const FString& Parameters)
{
	using namespace AnalysisTests;
	UNiagaraSystem* A = MakeSystem(RunFolder(TEXT("NS_SaveA")), TEXT("Sparks"));
	if (!TestNotNull(TEXT("System"), A))
	{
		return false;
	}
	const FNiagaraMcpModuleInfo Burst = UNiagaraMcpEditorTools::AddModule(A, TEXT("Sparks"), TEXT("EmitterUpdate"), TEXT("/Niagara/Modules/Emitter/SpawnBurst_Instantaneous.SpawnBurst_Instantaneous"), -1, false);
	UNiagaraMcpEditorTools::SetModuleInput(A, TEXT("Sparks"), Burst.ModuleId, TEXT("Spawn Count"), TEXT("33"), TEXT("Any"), false);
	UNiagaraMcpEditorTools::AddModule(A, TEXT("Sparks"), TEXT("ParticleSpawn"), TEXT("/Niagara/Modules/Spawn/Location/SphereLocation.SphereLocation"), -1, false);

	const FString AssetPath = RunFolder(TEXT("NE_Sparks"));
	const FNiagaraMcpAssetResult Saved = UNiagaraMcpEditorTools::SaveEmitterAsAsset(A, TEXT("Sparks"), AssetPath, true);
	TestEqual(TEXT("Emitter asset path"), Saved.Path, AssetPath + TEXT(".NE_Sparks"));
	TestTrue(TEXT("Emitter asset saved"), Saved.bSaved);
	UNiagaraEmitter* Asset = LoadObject<UNiagaraEmitter>(nullptr, *Saved.Path);
	if (!TestNotNull(TEXT("Emitter asset loads"), Asset))
	{
		return false;
	}
	TestTrue(TEXT("Standalone asset"), Asset->HasAnyFlags(RF_Public | RF_Standalone) && Asset->GetOutermost() != A->GetOutermost());

	UNiagaraSystem* B = MakeSystem(RunFolder(TEXT("NS_SaveB")), NAME_None);
	UNiagaraMcpEditorTools::AddEmitter(B, TEXT("Copy"), Saved.Path, false);
	auto Modules = [](UNiagaraSystem* System, FName Emitter)
	{
		TArray<FString> Out;
		for (const FNiagaraMcpModuleInfo& Module : UNiagaraMcpEditorTools::GetStack(System, Emitter, true).Modules)
		{
			const FNiagaraMcpInputInfo* Count = Module.Inputs.FindByPredicate([](const FNiagaraMcpInputInfo& I) { return I.Name == TEXT("SpawnCount"); });
			Out.Add(FString::Printf(TEXT("%s/%s%s"), *Module.Stack, *Module.DisplayName, Count ? *(TEXT("=") + Count->Value) : TEXT("")));
		}
		return Out;
	};
	const TArray<FString> Original = Modules(A, TEXT("Sparks"));
	const TArray<FString> Copy = Modules(B, TEXT("Copy"));
	AddInfo(FString::Printf(TEXT("Original: %s"), *FString::Join(Original, TEXT(", "))));
	AddInfo(FString::Printf(TEXT("From asset: %s"), *FString::Join(Copy, TEXT(", "))));
	TestTrue(TEXT("AddEmitter(TemplateEmitter = new asset) has the same modules and values"), Original.Num() > 4 && Original == Copy);
	TestTrue(TEXT("Copy compiles"), UNiagaraMcpEditorTools::CompileSystem(B, true, false).bSucceeded);

	AddExpectedMessagePlain(TEXT("already exists"), ELogVerbosity::Warning);
	TestTrue(TEXT("Existing path refused"), UNiagaraMcpEditorTools::SaveEmitterAsAsset(A, TEXT("Sparks"), AssetPath, false).Path.IsEmpty());
	return !HasAnyErrors();
}

// ------------------------------------------------------------------------------------------------ GetUserParameterUsage

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpAnalysisUserParameterUsageTest, "NiagaraMcp.Analysis.UserParameterUsage", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpAnalysisUserParameterUsageTest::RunTest(const FString& Parameters)
{
	using namespace AnalysisTests;
	UNiagaraSystem* System = MakeSystem(RunFolder(TEXT("NS_Usage")), TEXT("Fx"));
	if (!TestNotNull(TEXT("System"), System))
	{
		return false;
	}
	UNiagaraMcpEditorTools::SetUserParameter(System, TEXT("Tint"), TEXT("color"), TEXT("1, 0.5, 0, 1"), false);
	UNiagaraMcpEditorTools::SetUserParameter(System, TEXT("MySize"), TEXT("vec2"), TEXT("12, 12"), false);
	UNiagaraMcpEditorTools::SetUserParameter(System, TEXT("Rate"), TEXT("float"), TEXT("40"), false);
	UNiagaraMcpEditorTools::SetUserParameter(System, TEXT("Unused"), TEXT("float"), TEXT("1"), false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Fx"), TEXT("Initialize Particle"), TEXT("Color Mode"), TEXT("Direct Set"), TEXT("Any"), false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Fx"), TEXT("Initialize Particle"), TEXT("Color"), TEXT("@User.Tint"), TEXT("Any"), false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Fx"), TEXT("Spawn Rate"), TEXT("SpawnRate"), TEXT("=User.Rate * 2"), TEXT("Any"), false);
	UNiagaraMcpEditorTools::SetRendererBinding(System, TEXT("Fx"), 0, TEXT("SpriteSizeBinding"), TEXT("User.MySize"), false);
	// A scratch pad module that reads User.Unused2 through a Map Get node.
	UNiagaraMcpEditorTools::SetUserParameter(System, TEXT("ScratchRead"), TEXT("float"), TEXT("3"), false);
	const FNiagaraMcpLocalModuleInfo Local = UNiagaraMcpEditorTools::CreateLocalModule(System, TEXT("Fx"), TEXT("ParticleUpdate"), 0, TEXT("ReadUser"), false);
	if (UNiagaraScript* LocalScript = LoadObject<UNiagaraScript>(nullptr, *Local.ScriptPath))
	{
		const FNiagaraMcpGraphInfo Graph = UNiagaraMcpEditorTools::GetScriptGraph(LocalScript);
		const FNiagaraMcpGraphNode* Existing = Graph.Nodes.FindByPredicate([](const FNiagaraMcpGraphNode& N) { return N.Class == TEXT("NiagaraNodeParameterMapGet"); });
		const FString GetId = Existing ? Existing->NodeId : UNiagaraMcpEditorTools::AddGraphNode(LocalScript, TEXT("MapGet"), -300, 200).NodeId;
		UNiagaraMcpEditorTools::AddParameterPin(LocalScript, GetId, TEXT("User.ScratchRead"), TEXT("float"));
		UNiagaraMcpEditorTools::ApplyScriptChanges(LocalScript, false);
	}

	const TArray<FNiagaraMcpParameterUsage> All = UNiagaraMcpEditorTools::GetUserParameterUsage(System, NAME_None);
	for (const FNiagaraMcpParameterUsage& Usage : All)
	{
		TArray<FString> Uses;
		for (const FNiagaraMcpParameterUse& Use : Usage.Uses)
		{
			Uses.Add(FString::Printf(TEXT("%s %s/%s/%s/%s"), *Use.Kind, *Use.Emitter.ToString(), *Use.Where, *Use.Module, *Use.Target));
		}
		AddInfo(FString::Printf(TEXT("%s (%s): %s"), *Usage.Parameter, *Usage.Type, Usage.bUnused ? TEXT("unused") : *FString::Join(Uses, TEXT("; "))));
	}
	auto Find = [&All](const TCHAR* Name) { return All.FindByPredicate([Name](const FNiagaraMcpParameterUsage& U) { return U.Parameter == Name; }); };
	const FNiagaraMcpParameterUsage* Tint = Find(TEXT("User.Tint"));
	const FNiagaraMcpParameterUsage* Size = Find(TEXT("User.MySize"));
	const FNiagaraMcpParameterUsage* Rate = Find(TEXT("User.Rate"));
	const FNiagaraMcpParameterUsage* Unused = Find(TEXT("User.Unused"));
	TestEqual(TEXT("Every user parameter listed"), All.Num(), 5);
	const FNiagaraMcpParameterUsage* Scratch = Find(TEXT("User.ScratchRead"));
	TestTrue(TEXT("Map Get read in a scratch pad module found"), Scratch && Scratch->Uses.ContainsByPredicate([](const FNiagaraMcpParameterUse& U) { return U.Kind == TEXT("MapGet") && U.Where == TEXT("Scratch pad") && U.Module.Contains(TEXT("ReadUser")); }));
	TestTrue(TEXT("Linked module input found"), Tint && Tint->Uses.ContainsByPredicate([](const FNiagaraMcpParameterUse& U)
	{
		return U.Kind == TEXT("ModuleInput") && U.Emitter == TEXT("Fx") && U.Module == TEXT("Initialize Particle") && U.Target == TEXT("Color") && U.Where == TEXT("ParticleSpawn");
	}));
	TestTrue(TEXT("Renderer binding found"), Size && Size->Uses.ContainsByPredicate([](const FNiagaraMcpParameterUse& U) { return U.Kind == TEXT("RendererBinding") && U.Target == TEXT("SpriteSizeBinding"); }));
	TestTrue(TEXT("Expression use found"), Rate && Rate->Uses.ContainsByPredicate([](const FNiagaraMcpParameterUse& U) { return U.Kind == TEXT("ModuleInput") && U.Target.Contains(TEXT("expression")); }));
	TestTrue(TEXT("Unused parameter flagged"), Unused && Unused->bUnused && Tint && !Tint->bUnused);

	const TArray<FNiagaraMcpParameterUsage> One = UNiagaraMcpEditorTools::GetUserParameterUsage(System, TEXT("Tint"));
	TestTrue(TEXT("Single parameter"), One.Num() == 1 && One[0].Parameter == TEXT("User.Tint"));
	AddExpectedMessagePlain(TEXT("Tnt"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	TestEqual(TEXT("Unknown parameter rejected"), UNiagaraMcpEditorTools::GetUserParameterUsage(System, TEXT("Tnt")).Num(), 0);
	return !HasAnyErrors();
}

// ------------------------------------------------------------------------------------------------ Data channels

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpAnalysisDataChannelTest, "NiagaraMcp.Analysis.DataChannel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpAnalysisDataChannelTest::RunTest(const FString& Parameters)
{
	using namespace AnalysisTests;
	TArray<FNiagaraMcpVariableDecl> Variables;
	Variables.Add({ TEXT("Position"), TEXT("position") });
	Variables.Add({ TEXT("Energy"), TEXT("float") });
	Variables.Add({ TEXT("Tint"), TEXT("color") });
	const FString Path = RunFolder(TEXT("NDC_Impacts"));
	const FNiagaraMcpDataChannelInfo Created = UNiagaraMcpEditorTools::CreateNiagaraDataChannel(Path, Variables, TEXT("Islands"), true);
	TArray<FString> Lines;
	for (const FNiagaraMcpVariableDecl& Variable : Created.Variables)
	{
		Lines.Add(FString::Printf(TEXT("%s: %s"), *Variable.Name.ToString(), *Variable.Type));
	}
	AddInfo(FString::Printf(TEXT("%s (%s): %s; %d settings"), *Created.Path, *Created.Type, *FString::Join(Lines, TEXT(", ")), Created.Properties.Num()));
	TestEqual(TEXT("Path"), Created.Path, Path + TEXT(".NDC_Impacts"));
	TestEqual(TEXT("Type"), Created.Type, FString(TEXT("Islands")));
	TestTrue(TEXT("Saved"), Created.bSaved);
	TestTrue(TEXT("Three variables with types"), Created.Variables.Num() == 3 && Created.Variables[1].Name == TEXT("Energy") && Created.Variables[2].Type == TEXT("LinearColor"));

	UNiagaraDataChannelAsset* Asset = LoadObject<UNiagaraDataChannelAsset>(nullptr, *Created.Path);
	const FNiagaraMcpDataChannelInfo Read = UNiagaraMcpEditorTools::GetNiagaraDataChannel(Asset);
	TestTrue(TEXT("GetNiagaraDataChannel reads it back"), Read.Variables.Num() == 3 && Read.Type == TEXT("Islands") && Read.Properties.Num() > 0);

	const FNiagaraMcpDataChannelInfo Global = UNiagaraMcpEditorTools::CreateNiagaraDataChannel(RunFolder(TEXT("NDC_Global")), Variables, TEXT("Global"), false);
	TestEqual(TEXT("Global channel"), Global.Type, FString(TEXT("Global")));

	// Hook-up through a user data interface parameter (no dedicated module scripts ship with the engine).
	UNiagaraSystem* System = MakeSystem(RunFolder(TEXT("NS_Ndc")), TEXT("Fx"));
	UNiagaraMcpEditorTools::SetUserParameter(System, TEXT("ImpactWriter"), TEXT("NiagaraDataInterfaceDataChannelWrite"), TEXT(""), false);
	const FNiagaraMcpPropertyResult Bound = UNiagaraMcpEditorTools::SetUserDataInterfaceProperty(System, TEXT("ImpactWriter"), TEXT("Channel"), Created.Path, false);
	AddInfo(FString::Printf(TEXT("Writer channel: %s"), *Bound.Value));
	TestTrue(TEXT("Data channel write interface points at the channel"), Bound.Value.Contains(TEXT("NDC_Impacts")));

	AddExpectedMessagePlain(TEXT("Unknown data channel Type"), ELogVerbosity::Warning);
	TestTrue(TEXT("Bad type rejected"), UNiagaraMcpEditorTools::CreateNiagaraDataChannel(RunFolder(TEXT("NDC_Bad")), Variables, TEXT("Lanes"), false).Path.IsEmpty());
	TArray<FNiagaraMcpVariableDecl> Bad = Variables;
	Bad.Add({ TEXT("Curve"), TEXT("NiagaraDataInterfaceCurve") });
	AddExpectedMessagePlain(TEXT("cannot be used in a data channel"), ELogVerbosity::Warning);
	TestTrue(TEXT("Data interface variable rejected"), UNiagaraMcpEditorTools::CreateNiagaraDataChannel(RunFolder(TEXT("NDC_Bad2")), Bad, TEXT("Islands"), false).Path.IsEmpty());
	TestTrue(TEXT("Nothing created for the rejected channel"), LoadObject<UNiagaraDataChannelAsset>(nullptr, *(RunFolder(TEXT("NDC_Bad2")) + TEXT(".NDC_Bad2")), nullptr, LOAD_NoWarn | LOAD_Quiet) == nullptr);
	return !HasAnyErrors();
}

// ------------------------------------------------------------------------------------------------ RecolorEffect

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpAnalysisRecolorTest, "NiagaraMcp.Analysis.RecolorEffect", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpAnalysisRecolorTest::RunTest(const FString& Parameters)
{
	using namespace AnalysisTests;
	UNiagaraSystem* System = MakeSystem(RunFolder(TEXT("NS_Recolor")), TEXT("Fire"));
	if (!TestNotNull(TEXT("System"), System))
	{
		return false;
	}
	// Orange fire: a local color, a color curve on a second emitter, and a user color parameter.
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Fire"), TEXT("Initialize Particle"), TEXT("Color Mode"), TEXT("Direct Set"), TEXT("Any"), false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Fire"), TEXT("Initialize Particle"), TEXT("Color"), TEXT("1, 0.4, 0.05, 1"), TEXT("Any"), false);
	UNiagaraMcpEditorTools::AddEmitter(System, TEXT("Embers"), TEXT("Minimal"), false);
	UNiagaraMcpEditorTools::AddModule(System, TEXT("Embers"), TEXT("ParticleUpdate"), TEXT("/Niagara/Modules/Update/Color/Color.Color"), -1, false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Embers"), TEXT("Color"), TEXT("Color"), TEXT("Dynamic:/Niagara/DynamicInputs/ValueFromCurve/ColorFromCurve.ColorFromCurve"), TEXT("ParticleUpdate"), false);
	TArray<FNiagaraMcpCurveKey> Keys;
	Keys.Add({ 0.0f, TEXT("1, 0.5, 0, 1") });
	Keys.Add({ 1.0f, TEXT("1, 0.1, 0, 0") });
	UNiagaraMcpEditorTools::SetCurveKeys(System, TEXT("Embers"), TEXT("Color"), TEXT("Color/ColorCurve"), Keys, TEXT("Linear"), TEXT("ParticleUpdate"), false);
	UNiagaraMcpEditorTools::SetUserParameter(System, TEXT("Glow"), TEXT("color"), TEXT("1, 0.4, 0.05, 1"), false);

	const FString Before = MeanColor(System, TEXT("Fire"), 0.5f);
	const FNiagaraMcpRecolorResult Result = UNiagaraMcpEditorTools::RecolorEffect(System, TEXT("HueShift"), 120.0f, TEXT("None"), NAME_None, false);
	for (const FNiagaraMcpColorChange& Change : Result.Changes)
	{
		AddInfo(FString::Printf(TEXT("%s %s %s: %s -> %s%s"), *Change.Kind, *Change.Emitter.ToString(), *Change.Target, *Change.Before, *Change.After, Change.bChanged ? TEXT("") : TEXT(" (reported)")));
	}
	TestTrue(TEXT("Local color, curve and user parameter recolored"), Result.Changed >= 3
		&& Result.Changes.ContainsByPredicate([](const FNiagaraMcpColorChange& C) { return C.Kind == TEXT("ModuleInput") && C.bChanged; })
		&& Result.Changes.ContainsByPredicate([](const FNiagaraMcpColorChange& C) { return C.Kind == TEXT("CurveKeys") && C.bChanged; })
		&& Result.Changes.ContainsByPredicate([](const FNiagaraMcpColorChange& C) { return C.Kind == TEXT("UserParameter") && C.bChanged; }));

	const FString After = MeanColor(System, TEXT("Fire"), 0.5f);
	const float HueBefore = Hue(Before);
	const float HueAfter = Hue(After);
	AddInfo(FString::Printf(TEXT("Fire mean color %s (hue %.1f) -> %s (hue %.1f)"), *Before, HueBefore, *After, HueAfter));
	TestTrue(TEXT("Orange before"), HueBefore > 10.0f && HueBefore < 40.0f);
	TestTrue(TEXT("Hue rotated by 120 degrees (simulated particle colors)"), HueDistance(HueAfter, HueBefore + 120.0f) < 5.0f);
	const TArray<double> AfterRgb = Numbers(After);
	const TArray<double> BeforeRgb = Numbers(Before);
	TestTrue(TEXT("Brightness and alpha kept"), AfterRgb.Num() == 4 && BeforeRgb.Num() == 4 && FMath::IsNearlyEqual(FMath::Max3(AfterRgb[0], AfterRgb[1], AfterRgb[2]), FMath::Max3(BeforeRgb[0], BeforeRgb[1], BeforeRgb[2]), 0.02) && FMath::IsNearlyEqual(AfterRgb[3], BeforeRgb[3], 0.01));

	const FNiagaraMcpCurveInfo Curve = UNiagaraMcpEditorTools::GetCurveKeys(System, TEXT("Embers"), TEXT("Color"), TEXT("Color/ColorCurve"), TEXT("ParticleUpdate"));
	if (TestEqual(TEXT("Curve keeps its keys"), Curve.Keys.Num(), 2))
	{
		AddInfo(FString::Printf(TEXT("Curve keys: %s | %s (%s)"), *Curve.Keys[0].Value, *Curve.Keys[1].Value, *Curve.Interpolation));
		TestTrue(TEXT("Curve key hue rotated"), HueDistance(Hue(Curve.Keys[0].Value), Hue(TEXT("1, 0.5, 0")) + 120.0f) < 2.0f);
		TestTrue(TEXT("Curve key alpha kept"), FMath::IsNearlyEqual(Numbers(Curve.Keys[1].Value).Last(), 0.0, 0.001));
	}

	// Tint only.
	const FNiagaraMcpRecolorResult Tinted = UNiagaraMcpEditorTools::RecolorEffect(System, TEXT("Tint"), 0.0f, TEXT("0.5, 0.5, 0.5"), TEXT("Fire"), false);
	TestTrue(TEXT("Tint on one emitter"), Tinted.Changed == 1 && Tinted.Changes[0].Emitter == TEXT("Fire"));
	AddExpectedMessagePlain(TEXT("Nothing to do"), ELogVerbosity::Warning);
	UNiagaraMcpEditorTools::RecolorEffect(System, TEXT("HueShift"), 0.0f, TEXT("None"), NAME_None, false);
	return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
