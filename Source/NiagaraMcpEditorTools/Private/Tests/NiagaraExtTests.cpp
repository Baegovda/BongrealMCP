#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraMcpEditorTools.h"
#include "NiagaraDataInterface.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"

/**
 * End-to-end check of the v0.4 extension tools on a system built in the test (/Game/NiagaraMcpTests/NiagaraExt/...).
 *   UnrealEditor-Cmd.exe <Project>.uproject -ExecCmds="Automation RunTests NiagaraMcp.NiagaraExt;Quit" -unattended -nopause
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpExtToolsTest, "NiagaraMcp.NiagaraExt.Tools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace NiagaraExtTests
{
	const FNiagaraMcpModuleInfo* FindModule(const FNiagaraMcpStackInfo& Stack, const FString& DisplayName, const FString& StackPrefix)
	{
		return Stack.Modules.FindByPredicate([&](const FNiagaraMcpModuleInfo& M) { return M.DisplayName == DisplayName && M.Stack.StartsWith(StackPrefix); });
	}

	const FNiagaraMcpModuleInfo* FindModuleById(const FNiagaraMcpStackInfo& Stack, const FString& ModuleId)
	{
		return Stack.Modules.FindByPredicate([&](const FNiagaraMcpModuleInfo& M) { return M.ModuleId == ModuleId; });
	}

	const FNiagaraMcpInputInfo* FindInput(const FNiagaraMcpModuleInfo& Module, const FString& Name)
	{
		return Module.Inputs.FindByPredicate([&](const FNiagaraMcpInputInfo& I) { return NiagaraMcp::LooseNameEquals(I.Name, Name) || NiagaraMcp::LooseNameEquals(I.DisplayName, Name); });
	}

	const FNiagaraMcpEditableProperty* FindProperty(const TArray<FNiagaraMcpEditableProperty>& Properties, const FString& Name)
	{
		return Properties.FindByPredicate([&](const FNiagaraMcpEditableProperty& P) { return P.Name == Name; });
	}

	const FNiagaraMcpBindingInfo* FindBinding(const FNiagaraMcpRendererDetails& Details, const FString& Name)
	{
		return Details.Bindings.FindByPredicate([&](const FNiagaraMcpBindingInfo& B) { return B.Name == Name; });
	}

	const FNiagaraMcpAttributeStats* FindStats(const FNiagaraMcpAttributeSample& Sample, FName Emitter, const FString& Attribute)
	{
		for (const FNiagaraMcpEmitterAttributes& E : Sample.Emitters)
		{
			if (E.Emitter == Emitter)
			{
				return E.Attributes.FindByPredicate([&](const FNiagaraMcpAttributeStats& S) { return S.Attribute == Attribute; });
			}
		}
		return nullptr;
	}

	bool HasEmitter(UNiagaraSystem* System, FName Name)
	{
		return System->GetEmitterHandles().ContainsByPredicate([Name](const FNiagaraEmitterHandle& H) { return H.GetName() == Name; });
	}
}

bool FNiagaraMcpExtToolsTest::RunTest(const FString& Parameters)
{
	using namespace NiagaraExtTests;
	const FString Folder = FString::Printf(TEXT("/Game/NiagaraMcpTests/NiagaraExt/Run_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	const FNiagaraMcpAssetResult Created = UNiagaraMcpEditorTools::CreateSystem(Folder + TEXT("/NS_Ext"), true);
	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *Created.Path);
	if (!TestNotNull(TEXT("System"), System))
	{
		return false;
	}

	// ------------------------------------------------------------------ Base emitter
	UNiagaraMcpEditorTools::AddEmitter(System, TEXT("Sparks"), TEXT(""), false);
	const FNiagaraMcpModuleInfo Burst = UNiagaraMcpEditorTools::AddModule(System, TEXT("Sparks"), TEXT("EmitterUpdate"), TEXT("/Niagara/Modules/Emitter/SpawnBurst_Instantaneous.SpawnBurst_Instantaneous"), -1, false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Sparks"), Burst.ModuleId, TEXT("Spawn Count"), TEXT("20"), TEXT(""), false);
	UNiagaraMcpEditorTools::AddModule(System, TEXT("Sparks"), TEXT("ParticleSpawn"), TEXT("/Niagara/Modules/Spawn/Location/SphereLocation.SphereLocation"), -1, false);

	// Static switch inputs go through SetModuleInput (no separate tool needed).
	const FNiagaraMcpInputInfo LifetimeMode = UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Sparks"), TEXT("Initialize Particle"), TEXT("Lifetime Mode"), TEXT("Direct Set"), TEXT(""), false);
	TestTrue(TEXT("Lifetime Mode is a static switch"), LifetimeMode.bStatic);
	TestTrue(TEXT("Static switch set by SetModuleInput"), LifetimeMode.Value.Contains(TEXT("Direct")));
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Sparks"), TEXT("Initialize Particle"), TEXT("Lifetime"), TEXT("1.5"), TEXT(""), false);

	// ------------------------------------------------------------------ System properties
	const FNiagaraMcpEditablePropertyList SystemProps = UNiagaraMcpEditorTools::GetSystemProperties(System, TEXT("warmup"));
	TestNotNull(TEXT("GetSystemProperties lists WarmupTime"), FindProperty(SystemProps.Properties, TEXT("WarmupTime")));
	const FNiagaraMcpEditablePropertyList AllSystemProps = UNiagaraMcpEditorTools::GetSystemProperties(System, TEXT("Any"));
	TestTrue(TEXT("Filter Any lists more properties"), AllSystemProps.Properties.Num() > SystemProps.Properties.Num());
	// WarmupTime snaps to whole WarmupTickDelta steps (1/15 s); 1.0 is exact.
	const FNiagaraMcpPropertyResult Warmup = UNiagaraMcpEditorTools::SetSystemProperty(System, TEXT("WarmupTime"), TEXT("1.0"), false);
	AddInfo(FString::Printf(TEXT("WarmupTime -> %s"), *Warmup.Value));
	TestTrue(TEXT("SetSystemProperty WarmupTime"), FMath::IsNearlyEqual(FCString::Atof(*Warmup.Value), 1.0f, 0.1f));
	const FNiagaraMcpEditablePropertyList TickCount = UNiagaraMcpEditorTools::GetSystemProperties(System, TEXT("WarmupTickCount"));
	TestTrue(TEXT("WarmupTickCount follows WarmupTime"), TickCount.Properties.Num() > 0 && FCString::Atoi(*TickCount.Properties[0].Value) >= 14);
	UNiagaraMcpEditorTools::SetSystemProperty(System, TEXT("WarmupTime"), TEXT("0"), false);
	TestEqual(TEXT("SetSystemProperty bFixedBounds"), UNiagaraMcpEditorTools::SetSystemProperty(System, TEXT("bFixedBounds"), TEXT("true"), false).Value, FString(TEXT("true")));
	AddExpectedMessage(TEXT("Unknown property"), EAutomationExpectedMessageFlags::Contains, 0);
	TestTrue(TEXT("Unknown system property is rejected"), UNiagaraMcpEditorTools::SetSystemProperty(System, TEXT("WarmupTme"), TEXT("1"), false).Property.IsEmpty());

	// ------------------------------------------------------------------ Emitter properties
	const FNiagaraMcpEditablePropertyList EmitterProps = UNiagaraMcpEditorTools::GetEmitterProperties(System, TEXT("Sparks"), TEXT("Any"));
	const FNiagaraMcpEditableProperty* SimTarget = FindProperty(EmitterProps.Properties, TEXT("SimTarget"));
	TestTrue(TEXT("Emitter SimTarget listed with options"), SimTarget && SimTarget->Options.Contains(TEXT("CPUSim")) && SimTarget->Value == TEXT("CPUSim"));

	// ------------------------------------------------------------------ Renderers
	const FNiagaraMcpRendererDetails Sprite = UNiagaraMcpEditorTools::GetRendererProperties(System, TEXT("Sparks"), 0, TEXT("Any"));
	const FNiagaraMcpBindingInfo* ColorBinding = FindBinding(Sprite, TEXT("ColorBinding"));
	TestTrue(TEXT("ColorBinding -> Particles.Color"), ColorBinding && ColorBinding->Variable == TEXT("Particles.Color"));
	TestTrue(TEXT("Renderer properties listed"), Sprite.Properties.Num() > 5);
	UNiagaraMcpEditorTools::SetUserParameter(System, TEXT("MySize"), TEXT("vec2"), TEXT("20, 20"), false);
	const FNiagaraMcpPropertyResult Bound = UNiagaraMcpEditorTools::SetRendererBinding(System, TEXT("Sparks"), 0, TEXT("SpriteSizeBinding"), TEXT("User.MySize"), false);
	TestEqual(TEXT("SetRendererBinding result"), Bound.Value, FString(TEXT("User.MySize")));
	const FNiagaraMcpRendererDetails SpriteAfter = UNiagaraMcpEditorTools::GetRendererProperties(System, TEXT("Sparks"), 0, TEXT("size"));
	const FNiagaraMcpBindingInfo* SizeBinding = FindBinding(SpriteAfter, TEXT("SpriteSizeBinding"));
	AddInfo(FString::Printf(TEXT("SpriteSizeBinding: %s (default %s)"), SizeBinding ? *SizeBinding->Variable : TEXT("missing"), SizeBinding ? *SizeBinding->DefaultVariable : TEXT("")));
	TestTrue(TEXT("SpriteSizeBinding round-trips"), SizeBinding && SizeBinding->Variable == TEXT("User.MySize") && SizeBinding->DefaultVariable == TEXT("Particles.SpriteSize"));
	AddExpectedMessage(TEXT("needs a"), EAutomationExpectedMessageFlags::Contains, 0);
	TestTrue(TEXT("Type mismatch rejected"), UNiagaraMcpEditorTools::SetRendererBinding(System, TEXT("Sparks"), 0, TEXT("ColorBinding"), TEXT("User.MySize"), false).Property.IsEmpty());

	UNiagaraMcpEditorTools::AddRenderer(System, TEXT("Sparks"), TEXT("Ribbon"), TEXT(""), false);
	const FNiagaraMcpEmitterInfo Moved = UNiagaraMcpEditorTools::MoveRenderer(System, TEXT("Sparks"), 1, 0, false);
	TestTrue(TEXT("MoveRenderer put the ribbon first"), Moved.Renderers.Num() == 2 && Moved.Renderers[0].Class.Contains(TEXT("Ribbon")));
	UNiagaraMcpEditorTools::RemoveRenderer(System, TEXT("Sparks"), 0, false);

	// ------------------------------------------------------------------ Module order
	const FNiagaraMcpModuleInfo MovedInit = UNiagaraMcpEditorTools::MoveModule(System, TEXT("Sparks"), TEXT("Initialize Particle"), TEXT("ParticleSpawn"), TEXT("Any"), -1, false);
	TestEqual(TEXT("MoveModule returns the new index"), MovedInit.Index, 1);
	{
		const FNiagaraMcpStackInfo Stack = UNiagaraMcpEditorTools::GetStack(System, TEXT("Sparks"), true);
		const FNiagaraMcpModuleInfo* Init = FindModule(Stack, TEXT("Initialize Particle"), TEXT("ParticleSpawn"));
		const FNiagaraMcpModuleInfo* Sphere = FindModule(Stack, TEXT("Sphere Location"), TEXT("ParticleSpawn"));
		TestTrue(TEXT("Order changed: Sphere Location first"), Init && Sphere && Sphere->Index == 0 && Init->Index == 1);
		const FNiagaraMcpInputInfo* Lifetime = Init ? FindInput(*Init, TEXT("Lifetime")) : nullptr;
		TestTrue(TEXT("Overridden Lifetime survived the move"), Lifetime && FMath::IsNearlyEqual(FCString::Atof(*Lifetime->Value), 1.5f));
	}
	UNiagaraMcpEditorTools::MoveModule(System, TEXT("Sparks"), TEXT("Initialize Particle"), TEXT("ParticleSpawn"), TEXT("Any"), 0, false);

	const FNiagaraMcpModuleInfo ColorModule = UNiagaraMcpEditorTools::AddModule(System, TEXT("Sparks"), TEXT("ParticleUpdate"), TEXT("/Niagara/Modules/Update/Color/Color.Color"), -1, false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Sparks"), ColorModule.ModuleId, TEXT("Color"), TEXT("1, 0, 0, 1"), TEXT(""), false);
	const FNiagaraMcpModuleInfo ColorCopy = UNiagaraMcpEditorTools::DuplicateModule(System, TEXT("Sparks"), ColorModule.ModuleId, TEXT("Any"), TEXT("Any"), 0, false);
	TestTrue(TEXT("DuplicateModule created a new module"), !ColorCopy.ModuleId.IsEmpty() && ColorCopy.ModuleId != ColorModule.ModuleId);
	TestEqual(TEXT("Duplicate placed at index 0"), ColorCopy.Index, 0);
	const FNiagaraMcpInputInfo* CopiedColor = FindInput(ColorCopy, TEXT("Color"));
	AddInfo(FString::Printf(TEXT("Duplicated Color input: %s %s"), CopiedColor ? *CopiedColor->Mode : TEXT("missing"), CopiedColor ? *CopiedColor->Value : TEXT("")));
	TestTrue(TEXT("Duplicate kept the Color override"), CopiedColor && CopiedColor->Value.Replace(TEXT(".0"), TEXT("")).StartsWith(TEXT("1, 0, 0")));

	// ------------------------------------------------------------------ Emitters
	UNiagaraMcpEditorTools::AddEmitter(System, TEXT("Extra"), TEXT(""), false);
	TestFalse(TEXT("SetEmitterEnabled false"), UNiagaraMcpEditorTools::SetEmitterEnabled(System, TEXT("Extra"), false, false).bEnabled);
	TestTrue(TEXT("SetEmitterEnabled true"), UNiagaraMcpEditorTools::SetEmitterEnabled(System, TEXT("Extra"), true, false).bEnabled);

	const FNiagaraMcpEmitterInfo Copy = UNiagaraMcpEditorTools::DuplicateEmitter(System, TEXT("Sparks"), TEXT("SparksCopy"), false);
	TestEqual(TEXT("DuplicateEmitter name"), Copy.Name, FName(TEXT("SparksCopy")));
	TestEqual(TEXT("Three emitters after duplicate"), System->GetEmitterHandles().Num(), 3);
	{
		const FNiagaraMcpStackInfo Original = UNiagaraMcpEditorTools::GetStack(System, TEXT("Sparks"), false);
		const FNiagaraMcpStackInfo Duplicate = UNiagaraMcpEditorTools::GetStack(System, TEXT("SparksCopy"), false);
		TestEqual(TEXT("Duplicate has the same modules"), Duplicate.Modules.Num(), Original.Modules.Num());
	}

	// ------------------------------------------------------------------ Compile + particle readback (Age grows over time)
	const FNiagaraMcpCompileResult Compiled = UNiagaraMcpEditorTools::CompileSystem(System, true, true);
	for (const FNiagaraMcpCompileMessage& Message : Compiled.Messages)
	{
		AddInfo(FString::Printf(TEXT("Compile %s %s: %s"), *Message.Severity, *Message.Script, *Message.Message));
	}
	TestTrue(TEXT("System compiles"), Compiled.bSucceeded);
	const FNiagaraMcpPropertyResult ColorBack = UNiagaraMcpEditorTools::SetRendererBinding(System, TEXT("Sparks"), 0, TEXT("Color"), TEXT("Particles.Color"), false);
	TestEqual(TEXT("Binding to a compiled particle attribute"), ColorBack.Value, FString(TEXT("Particles.Color")));
	const TArray<FNiagaraMcpParticleSample> Counts = UNiagaraMcpEditorTools::GetParticleCounts(System, { 0.3f });
	if (Counts.Num() == 1)
	{
		const FNiagaraMcpEmitterCount* CopyCount = Counts[0].Emitters.FindByPredicate([](const FNiagaraMcpEmitterCount& C) { return C.Emitter == TEXT("SparksCopy"); });
		TestTrue(TEXT("Duplicated emitter simulates"), CopyCount && CopyCount->Particles == 20);
	}
	const TArray<FNiagaraMcpAttributeSample> Attributes = UNiagaraMcpEditorTools::GetParticleAttributes(System, { 0.2f, 0.8f }, { TEXT("Age"), TEXT("Particles.Position"), TEXT("Bogus") }, TEXT("Sparks"), 4);
	if (TestEqual(TEXT("Two attribute samples"), Attributes.Num(), 2))
	{
		const FNiagaraMcpAttributeStats* Age0 = FindStats(Attributes[0], TEXT("Sparks"), TEXT("Age"));
		const FNiagaraMcpAttributeStats* Age1 = FindStats(Attributes[1], TEXT("Sparks"), TEXT("Age"));
		if (TestTrue(TEXT("Age read at both times"), Age0 && Age1))
		{
			AddInfo(FString::Printf(TEXT("Age mean %s -> %s (count %d), samples %d"), *Age0->Mean, *Age1->Mean, Age0->Count, Age0->Samples.Num()));
			TestEqual(TEXT("20 particles read"), Age0->Count, 20);
			TestTrue(TEXT("Age increases over time"), FCString::Atof(*Age1->Mean) > FCString::Atof(*Age0->Mean) + 0.4f);
			TestEqual(TEXT("MaxSamples respected"), Age0->Samples.Num(), 4);
		}
		const FNiagaraMcpAttributeStats* Position = FindStats(Attributes[0], TEXT("Sparks"), TEXT("Position"));
		TestTrue(TEXT("Position has 3 components"), Position && Position->Mean.Contains(TEXT(",")));
		TestTrue(TEXT("Only the requested emitter"), Attributes[0].Emitters.Num() == 1 && Attributes[0].Emitters[0].Note.Contains(TEXT("Bogus")));
	}

	// ------------------------------------------------------------------ Spec export and diff against a DuplicateAsset copy
	const FNiagaraMcpSystemSpec Spec = UNiagaraMcpEditorTools::ExportSystemSpec(System);
	AddInfo(FString::Printf(TEXT("Spec (%d chars): %s"), Spec.Spec.Len(), *Spec.Spec.Left(1500)));
	TestTrue(TEXT("Spec lists emitters and modules"), Spec.Spec.Contains(TEXT("SparksCopy")) && Spec.Spec.Contains(TEXT("Initialize Particle")) && Spec.Spec.Contains(TEXT("bFixedBounds")));
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		// Asset creation is not wrapped in a transaction (undoing it would leave a broken asset).
		UNiagaraSystem* Duplicate = Cast<UNiagaraSystem>(AssetTools.DuplicateAsset(TEXT("NS_Ext_Copy"), Folder, System));
		if (TestNotNull(TEXT("DuplicateAsset"), Duplicate))
		{
			const FNiagaraMcpSpecDiff Same = UNiagaraMcpEditorTools::DiffSystems(System, Duplicate);
			for (const FNiagaraMcpSpecDifference& D : Same.Differences)
			{
				AddInfo(FString::Printf(TEXT("Unexpected diff %s: %s | %s"), *D.Path, *D.A, *D.B));
			}
			TestTrue(TEXT("Duplicate asset is identical"), Same.bIdentical);
			UNiagaraMcpEditorTools::SetModuleInput(Duplicate, TEXT("Sparks"), TEXT("Initialize Particle"), TEXT("Lifetime"), TEXT("3.0"), TEXT(""), false);
			const FNiagaraMcpSpecDiff Changed = UNiagaraMcpEditorTools::DiffSystems(System, Duplicate);
			const FNiagaraMcpSpecDifference* LifetimeDiff = Changed.Differences.FindByPredicate([](const FNiagaraMcpSpecDifference& D) { return D.Path.Contains(TEXT("Sparks.stacks.ParticleSpawn.Initialize Particle.inputs.Lifetime")); });
			TestTrue(TEXT("DiffSystems finds the changed input"), LifetimeDiff && FCString::Atof(*LifetimeDiff->A) == 1.5f && FCString::Atof(*LifetimeDiff->B) == 3.0f);
			TestEqual(TEXT("Only one difference"), Changed.Differences.Num(), 1);
			NiagaraMcp::SaveAssetSilently(Duplicate);
		}
	}

	// ------------------------------------------------------------------ Simulation stages (GPU emitter)
	UNiagaraMcpEditorTools::AddEmitter(System, TEXT("GridFx"), TEXT(""), false);
	UNiagaraMcpEditorTools::SetEmitterProperty(System, TEXT("GridFx"), TEXT("SimTarget"), TEXT("GPUComputeSim"), false);
	UNiagaraMcpEditorTools::SetEmitterProperty(System, TEXT("GridFx"), TEXT("CalculateBoundsMode"), TEXT("Fixed"), false);
	UNiagaraMcpEditorTools::AddSimulationStage(System, TEXT("GridFx"), TEXT("StageA"), TEXT("Particles"), NAME_None, TEXT(""), -1, false);
	UNiagaraMcpEditorTools::AddSimulationStage(System, TEXT("GridFx"), TEXT("StageB"), TEXT("Particles"), NAME_None, TEXT(""), -1, false);
	TestEqual(TEXT("NumIterations set"), UNiagaraMcpEditorTools::SetSimulationStageProperty(System, TEXT("GridFx"), 0, TEXT("NumIterations"), TEXT("4"), false).Value, FString(TEXT("4")));
	UNiagaraMcpEditorTools::SetUserParameter(System, TEXT("Iterations"), TEXT("int"), TEXT("2"), false);
	TestEqual(TEXT("NumIterations bound to a user parameter"), UNiagaraMcpEditorTools::SetSimulationStageProperty(System, TEXT("GridFx"), 0, TEXT("NumIterations"), TEXT("@User.Iterations"), false).Value, FString(TEXT("@User.Iterations")));
	UNiagaraMcpEditorTools::SetSimulationStageProperty(System, TEXT("GridFx"), 0, TEXT("NumIterations"), TEXT("4"), false);
	TestEqual(TEXT("Stage disabled"), UNiagaraMcpEditorTools::SetSimulationStageProperty(System, TEXT("GridFx"), 0, TEXT("bEnabled"), TEXT("false"), false).Value.ToLower(), FString(TEXT("false")));
	TestFalse(TEXT("Stage 0 reports disabled"), UNiagaraMcpEditorTools::GetEmitterStages(System, TEXT("GridFx")).SimulationStages[0].bEnabled);
	const FNiagaraMcpEmitterStages MovedStages = UNiagaraMcpEditorTools::MoveSimulationStage(System, TEXT("GridFx"), 0, 1, false);
	TestTrue(TEXT("MoveSimulationStage swapped the stages"), MovedStages.SimulationStages.Num() == 2 && MovedStages.SimulationStages[0].Name == TEXT("StageB") && MovedStages.SimulationStages[1].Name == TEXT("StageA"));
	const FNiagaraMcpEmitterStages RemovedStages = UNiagaraMcpEditorTools::RemoveSimulationStage(System, TEXT("GridFx"), 0, false);
	TestTrue(TEXT("RemoveSimulationStage left StageA"), RemovedStages.SimulationStages.Num() == 1 && RemovedStages.SimulationStages[0].Name == TEXT("StageA"));
	const TArray<FNiagaraMcpAttributeSample> GpuAttributes = UNiagaraMcpEditorTools::GetParticleAttributes(System, { 0.1f }, {}, TEXT("GridFx"), 2);
	TestTrue(TEXT("GPU emitter reports a note"), GpuAttributes.Num() == 1 && GpuAttributes[0].Emitters.Num() == 1 && GpuAttributes[0].Emitters[0].Note.Contains(TEXT("GPU")));

	// ------------------------------------------------------------------ Event handlers
	UNiagaraMcpEditorTools::AddEventHandler(System, TEXT("Extra"), TEXT("Sparks"), TEXT("DeathEvent"), TEXT("SpawnedParticles"), 3, 0, false);
	TestEqual(TEXT("SpawnNumber set"), UNiagaraMcpEditorTools::SetEventHandlerProperty(System, TEXT("Extra"), 0, TEXT("SpawnNumber"), TEXT("5"), false).Value, FString(TEXT("5")));
	TestEqual(TEXT("SourceEmitter set"), UNiagaraMcpEditorTools::SetEventHandlerProperty(System, TEXT("Extra"), 0, TEXT("SourceEmitter"), TEXT("SparksCopy"), false).Value, FString(TEXT("SparksCopy")));
	TestEqual(TEXT("Event handler reads back"), UNiagaraMcpEditorTools::GetEmitterStages(System, TEXT("Extra")).EventHandlers[0].SpawnNumber, 5);
	TestEqual(TEXT("RemoveEventHandler"), UNiagaraMcpEditorTools::RemoveEventHandler(System, TEXT("Extra"), 0, false).EventHandlers.Num(), 0);

	// ------------------------------------------------------------------ User parameters
	UNiagaraMcpEditorTools::SetUserParameter(System, TEXT("Tint"), TEXT("color"), TEXT("0, 1, 0, 1"), false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Sparks"), ColorModule.ModuleId, TEXT("Color"), TEXT("@User.Tint"), TEXT(""), false);
	const FNiagaraMcpParameterInfo Renamed = UNiagaraMcpEditorTools::RenameUserParameter(System, TEXT("Tint"), TEXT("TintRenamed"), false);
	TestEqual(TEXT("RenameUserParameter"), Renamed.Name, FString(TEXT("User.TintRenamed")));
	{
		const FNiagaraMcpStackInfo Stack = UNiagaraMcpEditorTools::GetStack(System, TEXT("Sparks"), true);
		const FNiagaraMcpModuleInfo* Color = FindModuleById(Stack, ColorModule.ModuleId);
		const FNiagaraMcpInputInfo* Input = Color ? FindInput(*Color, TEXT("Color")) : nullptr;
		TestTrue(TEXT("Linked input follows the rename"), Input && Input->Mode == TEXT("Linked") && Input->Value.Contains(TEXT("User.TintRenamed")));
	}
	const TArray<FNiagaraMcpParameterInfo> Remaining = UNiagaraMcpEditorTools::RemoveUserParameter(System, TEXT("User.TintRenamed"), false);
	TestFalse(TEXT("RemoveUserParameter"), Remaining.ContainsByPredicate([](const FNiagaraMcpParameterInfo& P) { return P.Name.Contains(TEXT("TintRenamed")); }));
	{
		const FNiagaraMcpStackInfo Stack = UNiagaraMcpEditorTools::GetStack(System, TEXT("Sparks"), true);
		const FNiagaraMcpModuleInfo* Color = FindModuleById(Stack, ColorModule.ModuleId);
		const FNiagaraMcpInputInfo* Input = Color ? FindInput(*Color, TEXT("Color")) : nullptr;
		TestTrue(TEXT("Linked input reset after removal"), Input && Input->Mode != TEXT("Linked"));
	}
	UNiagaraMcpEditorTools::SetUserParameter(System, TEXT("Noise"), TEXT("NiagaraDataInterfaceTexture"), TEXT(""), false);
	TestTrue(TEXT("Texture DI property"), UNiagaraMcpEditorTools::SetUserDataInterfaceProperty(System, TEXT("Noise"), TEXT("Texture"), TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"), false).Value.Contains(TEXT("DefaultTexture")));
	UNiagaraMcpEditorTools::SetUserParameter(System, TEXT("Values"), TEXT("NiagaraDataInterfaceArrayFloat"), TEXT(""), false);
	const FNiagaraMcpPropertyResult ArrayValues = UNiagaraMcpEditorTools::SetUserDataInterfaceProperty(System, TEXT("User.Values"), TEXT("FloatData"), TEXT("(1.0,2.5,4.0)"), false);
	AddInfo(FString::Printf(TEXT("Array DI FloatData = %s"), *ArrayValues.Value));
	TestTrue(TEXT("Array DI property"), ArrayValues.Value.Contains(TEXT("2.5")));

	// ------------------------------------------------------------------ Function specifiers
	const FNiagaraMcpGraphInfo ModuleGraph = UNiagaraMcpEditorTools::CreateModuleScript(Folder + TEXT("/NM_Reader"), TEXT("Reads particle attributes."), { TEXT("ParticleUpdate") }, true);
	if (UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, *ModuleGraph.ScriptPath))
	{
		// Data interface function nodes come from a data interface pin's context menu (not the plain add menu), so the test
		// places a Particle Attribute Reader "Get Float By Index" call directly, the way that menu action does.
		UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(ModuleScript->GetLatestSource());
		UNiagaraGraph* Graph = Source ? Source->NodeGraph.Get() : nullptr;
		UClass* ReaderClass = FindObject<UClass>(nullptr, TEXT("/Script/Niagara.NiagaraDataInterfaceParticleRead"));
		const UNiagaraDataInterface* Reader = ReaderClass ? Cast<UNiagaraDataInterface>(ReaderClass->GetDefaultObject()) : nullptr;
		TArray<FNiagaraFunctionSignature> Signatures;
		if (Reader)
		{
			Reader->GetFunctionSignatures(Signatures);
		}
		const FNiagaraFunctionSignature* GetFloat = Signatures.FindByPredicate([](const FNiagaraFunctionSignature& S) { return S.GetNameString().Equals(TEXT("Get Float By Index")); });
		if (TestNotNull(TEXT("Graph"), Graph) && TestNotNull(TEXT("Get Float By Index signature"), GetFloat))
		{
			UNiagaraNodeFunctionCall* Call = NewObject<UNiagaraNodeFunctionCall>(Graph, NAME_None, RF_Transactional);
			Call->Signature = *GetFloat;
			Graph->AddNode(Call, false, false);
			Call->CreateNewGuid();
			Call->PostPlacedNewNode();
			Call->AllocateDefaultPins();
			const FString NodeId = Call->NodeGuid.ToString(EGuidFormats::Digits);
			const FNiagaraMcpFunctionSpecifierInfo Specifiers = UNiagaraMcpEditorTools::SetFunctionSpecifier(ModuleScript, NodeId, TEXT("Attribute"), TEXT("Age"));
			AddInfo(FString::Printf(TEXT("Specifiers of %s: %s"), *Specifiers.Function, *FString::Join(Specifiers.Specifiers, TEXT(", "))));
			TestTrue(TEXT("SetFunctionSpecifier"), Specifiers.Specifiers.Contains(TEXT("Attribute=Age")));
			TestEqual(TEXT("Specifier stored on the node"), Call->FunctionSpecifiers.FindRef(TEXT("Attribute")), FName(TEXT("Age")));
			AddExpectedMessage(TEXT("Unknown function specifier"), EAutomationExpectedMessageFlags::Contains, 0);
			TestTrue(TEXT("Unknown specifier rejected"), UNiagaraMcpEditorTools::SetFunctionSpecifier(ModuleScript, NodeId, TEXT("Atribute"), TEXT("Age")).NodeId.IsEmpty());
		}
	}

	// ------------------------------------------------------------------ Dynamic inputs
	const TArray<FNiagaraMcpDynamicInputInfo> DynamicInputs = UNiagaraMcpEditorTools::FindDynamicInputs(TEXT("random range"), TEXT("float"), 10);
	TestTrue(TEXT("FindDynamicInputs finds float random range"), DynamicInputs.Num() > 0 && DynamicInputs.ContainsByPredicate([](const FNiagaraMcpDynamicInputInfo& D) { return D.Name.Contains(TEXT("Random")); }));
	TestFalse(TEXT("FindDynamicInputs respects OutputType"), DynamicInputs.ContainsByPredicate([](const FNiagaraMcpDynamicInputInfo& D) { return D.OutputType != TEXT("NiagaraFloat"); }));
	if (DynamicInputs.Num() > 0)
	{
		AddInfo(FString::Printf(TEXT("Dynamic input: %s (%s)"), *DynamicInputs[0].Path, *DynamicInputs[0].OutputType));
		const FNiagaraMcpInputInfo Applied = UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Sparks"), TEXT("Initialize Particle"), TEXT("Lifetime"), TEXT("Dynamic:") + DynamicInputs[0].Path, TEXT(""), false);
		TestEqual(TEXT("Found dynamic input usable with SetModuleInput"), Applied.Mode, FString(TEXT("Dynamic")));
	}
	TestTrue(TEXT("FindDynamicInputs Any"), UNiagaraMcpEditorTools::FindDynamicInputs(TEXT("Any"), TEXT("Any"), 5).Num() == 5);

	TestTrue(TEXT("Final compile"), UNiagaraMcpEditorTools::CompileSystem(System, true, true).bSucceeded);
	return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
