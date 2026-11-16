#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "NiagaraMcpCommon.h"
#include "NiagaraMcpEditorTools.h"
#include "NiagaraSystem.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/Base64.h"
#include "Modules/ModuleManager.h"

/**
 * Batch execution, Get/Set round trips and the integration fixes of 0.4.0:
 * ExecuteBatch with result references / dry run / rollback, GetStack values fed back into SetModuleInput,
 * side-effect-free ValidateSystem(bCompile=false), Undo refreshing the compiled system, RemoveEmitter through a
 * view model, did-you-mean suggestions and curve interpolation read-back.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpCoreTest, "NiagaraMcp.Core.BatchAndRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace NiagaraMcpCoreTests
{
	FNiagaraMcpBatchOp Op(const TCHAR* Id, const TCHAR* Tool, const FString& Arguments)
	{
		FNiagaraMcpBatchOp Result;
		Result.Id = Id;
		Result.Tool = Tool;
		Result.Arguments = Arguments;
		return Result;
	}

	int32 CountAt(const TArray<FNiagaraMcpParticleSample>& Samples, int32 Index)
	{
		return Samples.IsValidIndex(Index) ? Samples[Index].TotalParticles : -1;
	}
}

bool FNiagaraMcpCoreTest::RunTest(const FString& Parameters)
{
	using namespace NiagaraMcpCoreTests;
	const FString Folder = FString::Printf(TEXT("/Game/NiagaraMcpTests/Core/Run_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	const FString SystemPath = Folder + TEXT("/NS_Batch");
	const FString BurstPath = TEXT("/Niagara/Modules/Emitter/SpawnBurst_Instantaneous.SpawnBurst_Instantaneous");

	// ------------------------------------------------------------------ ExecuteBatch with references
	TArray<FNiagaraMcpBatchOp> Ops;
	Ops.Add(Op(TEXT("sys"), TEXT("CreateSystem"), FString::Printf(TEXT("{\"AssetPath\": \"%s\"}"), *SystemPath)));
	Ops.Add(Op(TEXT("em"), TEXT("AddEmitter"), TEXT("{\"System\": \"$sys.path\", \"EmitterName\": \"Sparks\"}")));
	Ops.Add(Op(TEXT("burst"), TEXT("AddModule"), FString::Printf(TEXT("{\"system\": \"$sys.path\", \"emitter_name\": \"Sparks\", \"StackName\": \"EmitterUpdate\", \"ModuleScript\": \"%s\"}"), *BurstPath)));
	Ops.Add(Op(TEXT(""), TEXT("SetModuleInput"), TEXT("{\"System\": \"$sys.path\", \"EmitterName\": \"Sparks\", \"Module\": \"$burst.moduleId\", \"Input\": \"Spawn Count\", \"Value\": \"10\"}")));
	Ops.Add(Op(TEXT(""), TEXT("SetModuleInput"), TEXT("{\"System\": \"$sys.path\", \"EmitterName\": \"Sparks\", \"Module\": \"Initialize Particle\", \"Input\": \"Lifetime\", \"Value\": \"2\"}")));
	const FNiagaraMcpBatchResult Batch = UNiagaraMcpEditorTools::ExecuteBatch(Ops, true, true, false, true);
	for (const FNiagaraMcpBatchStep& Step : Batch.Steps)
	{
		if (!Step.bOk)
		{
			AddError(FString::Printf(TEXT("Batch step %d (%s) failed: %s"), Step.Index, *Step.Tool, *Step.Error));
		}
	}
	if (!TestTrue(TEXT("Batch succeeded"), Batch.bOk && Batch.Succeeded == Ops.Num()))
	{
		return false;
	}
	TestTrue(TEXT("Batch saved the new system"), Batch.SavedPackages.ContainsByPredicate([&SystemPath](const FString& Package) { return SystemPath.StartsWith(Package); }));
	TestTrue(TEXT("Batch reports its undo count"), Batch.UndoCount >= Ops.Num() - 1);

	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *(SystemPath + TEXT(".NS_Batch")));
	if (!TestNotNull(TEXT("Batch-created system"), System))
	{
		return false;
	}
	TArray<float> Times = { 0.2f };
	TestEqual(TEXT("Batch-set burst count simulates"), CountAt(UNiagaraMcpEditorTools::GetParticleCounts(System, Times), 0), 10);

	// Dry run: argument names are checked with suggestions, nothing runs.
	TArray<FNiagaraMcpBatchOp> Bad;
	Bad.Add(Op(TEXT(""), TEXT("SetModuleInput"), FString::Printf(TEXT("{\"System\": \"%s\", \"Emiter\": \"Sparks\"}"), *SystemPath)));
	Bad.Add(Op(TEXT(""), TEXT("SetModuleInputt"), TEXT("{}")));
	const FNiagaraMcpBatchResult Dry = UNiagaraMcpEditorTools::ExecuteBatch(Bad, false, false, true, false);
	TestFalse(TEXT("Dry run flags errors"), Dry.bOk);
	TestTrue(TEXT("Unknown argument gets a suggestion"), Dry.Steps.Num() == 2 && Dry.Steps[0].Error.Contains(TEXT("emitterName")));
	TestTrue(TEXT("Unknown tool gets a suggestion"), Dry.Steps.Num() == 2 && Dry.Steps[1].Error.Contains(TEXT("SetModuleInput")));

	// Rollback: the second step fails (unknown input), so the first is undone.
	TArray<FNiagaraMcpBatchOp> Rollback;
	Rollback.Add(Op(TEXT(""), TEXT("SetModuleInput"), FString::Printf(TEXT("{\"System\": \"%s\", \"EmitterName\": \"Sparks\", \"Module\": \"Spawn Burst Instantaneous\", \"Input\": \"Spawn Count\", \"Value\": \"40\"}"), *SystemPath)));
	Rollback.Add(Op(TEXT(""), TEXT("SetModuleInput"), FString::Printf(TEXT("{\"System\": \"%s\", \"EmitterName\": \"Sparks\", \"Module\": \"Spawn Burst Instantaneous\", \"Input\": \"No Such Input\", \"Value\": \"1\"}"), *SystemPath)));
	const FNiagaraMcpBatchResult Rolled = UNiagaraMcpEditorTools::ExecuteBatch(Rollback, true, true, false, true);
	TestTrue(TEXT("Failed batch rolled back"), !Rolled.bOk && Rolled.bRolledBack);
	TestEqual(TEXT("Rolled-back value restored"), CountAt(UNiagaraMcpEditorTools::GetParticleCounts(System, Times), 0), 10);

	// Rollback also deletes never-saved assets the batch created, so a corrected re-run can reuse the path.
	const FString RollbackMaterial = Folder + TEXT("/M_Rollback");
	TArray<FNiagaraMcpBatchOp> Create;
	Create.Add(Op(TEXT("m"), TEXT("CreateMaterial"), FString::Printf(TEXT("{\"AssetPath\": \"%s\"}"), *RollbackMaterial)));
	Create.Add(Op(TEXT(""), TEXT("AddExpression"), FString::Printf(TEXT("{\"Material\": \"%s\", \"ExpressionClass\": \"NoSuchExpression\"}"), *RollbackMaterial)));
	const FNiagaraMcpBatchResult Discarded = UNiagaraMcpEditorTools::ExecuteBatch(Create, true, true, false, true);
	TestTrue(TEXT("Created asset discarded on rollback"), Discarded.bRolledBack && Discarded.DiscardedAssets.Num() == 1);
	TArray<FNiagaraMcpBatchOp> Retry;
	Retry.Add(Op(TEXT(""), TEXT("CreateMaterial"), FString::Printf(TEXT("{\"AssetPath\": \"%s\"}"), *RollbackMaterial)));
	TestTrue(TEXT("Path is free again after rollback"), UNiagaraMcpEditorTools::ExecuteBatch(Retry, true, true, false, false).bOk);

	// ------------------------------------------------------------------ Undo refreshes the compiled system
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Sparks"), TEXT("Spawn Burst Instantaneous"), TEXT("Spawn Count"), TEXT("30"), TEXT(""), false);
	TestEqual(TEXT("Count after edit"), CountAt(UNiagaraMcpEditorTools::GetParticleCounts(System, Times), 0), 30);
	UNiagaraMcpEditorTools::Undo(1);
	TestEqual(TEXT("Count after Undo"), CountAt(UNiagaraMcpEditorTools::GetParticleCounts(System, Times), 0), 10);

	// ------------------------------------------------------------------ GetStack values round-trip into SetModuleInput
	UNiagaraMcpEditorTools::SetUserParameter(System, TEXT("Tint"), TEXT("color"), TEXT("1, 0.5, 0, 1"), false);
	const FNiagaraMcpSystemSummary WithUser = UNiagaraMcpEditorTools::GetSystemSummary(System);
	TestTrue(TEXT("Summary reports User.-prefixed names"), WithUser.UserParameters.ContainsByPredicate([](const FNiagaraMcpParameterInfo& Info) { return Info.Name == TEXT("User.Tint"); }));
	AddExpectedError(TEXT("does not exist"), EAutomationExpectedErrorFlags::Contains, 0);
	TestTrue(TEXT("Link to a missing user parameter is rejected"), UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Sparks"), TEXT("Initialize Particle"), TEXT("Color"), TEXT("@User.Missing"), TEXT(""), false).Name.IsEmpty());
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Sparks"), TEXT("Initialize Particle"), TEXT("Color"), TEXT("@User.Tint"), TEXT(""), false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Sparks"), TEXT("Initialize Particle"), TEXT("Sprite Size"), TEXT("=float2(4, 4)"), TEXT(""), false);
	const FNiagaraMcpStackInfo Stack = UNiagaraMcpEditorTools::GetStack(System, TEXT("Sparks"), true);
	int32 RoundTrips = 0;
	for (const FNiagaraMcpModuleInfo& Module : Stack.Modules)
	{
		for (const FNiagaraMcpInputInfo& Input : Module.Inputs)
		{
			if (Input.Mode != TEXT("Local") && Input.Mode != TEXT("Linked") && Input.Mode != TEXT("Expression"))
			{
				continue;
			}
			const FNiagaraMcpInputInfo Back = UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Sparks"), Module.ModuleId, Input.Name, Input.Value, TEXT(""), false);
			TestEqual(FString::Printf(TEXT("Round trip %s.%s mode"), *Module.DisplayName, *Input.Name), Back.Mode, Input.Mode);
			TestEqual(FString::Printf(TEXT("Round trip %s.%s value"), *Module.DisplayName, *Input.Name), Back.Value, Input.Value);
			++RoundTrips;
		}
	}
	TestTrue(TEXT("Round-tripped inputs"), RoundTrips > 5);
	const FNiagaraMcpInputInfo* Linked = nullptr;
	const FNiagaraMcpInputInfo* Expression = nullptr;
	for (const FNiagaraMcpModuleInfo& Module : Stack.Modules)
	{
		for (const FNiagaraMcpInputInfo& Input : Module.Inputs)
		{
			Linked = Input.Mode == TEXT("Linked") ? &Input : Linked;
			Expression = Input.Mode == TEXT("Expression") ? &Input : Expression;
		}
	}
	TestTrue(TEXT("Linked value uses @ syntax"), Linked && Linked->Value.StartsWith(TEXT("@")));
	TestTrue(TEXT("Expression value uses = syntax"), Expression && Expression->Value.StartsWith(TEXT("=")));

	// A rejected value leaves the system untouched (validated before Modify).
	UNiagaraMcpEditorTools::CompileSystem(System, true, true);
	TestFalse(TEXT("Saved system is clean"), System->GetOutermost()->IsDirty());
	AddExpectedError(TEXT("Lifetime"), EAutomationExpectedErrorFlags::Contains, 0);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Sparks"), TEXT("Initialize Particle"), TEXT("Lifetime"), TEXT("not a number"), TEXT(""), false);
	TestFalse(TEXT("Invalid value does not dirty the system"), System->GetOutermost()->IsDirty());

	// ValidateSystem without compiling has no side effects.
	const FNiagaraMcpValidationResult Validation = UNiagaraMcpEditorTools::ValidateSystem(System, false);
	TestTrue(TEXT("Validation ok"), Validation.bOk);
	TestFalse(TEXT("ValidateSystem(bCompile=false) does not dirty the system"), System->GetOutermost()->IsDirty());

	// ------------------------------------------------------------------ RemoveEmitter through a view model
	UNiagaraMcpEditorTools::AddEmitter(System, TEXT("Extra"), TEXT(""), false);
	const FNiagaraMcpSystemSummary AfterRemove = UNiagaraMcpEditorTools::RemoveEmitter(System, TEXT("Extra"), false);
	TestEqual(TEXT("One emitter left"), AfterRemove.Emitters.Num(), 1);
	const FNiagaraMcpCompileResult Compiled = UNiagaraMcpEditorTools::CompileSystem(System, true, false);
	TestTrue(TEXT("Compiles after RemoveEmitter"), Compiled.bSucceeded);
	TestEqual(TEXT("Still simulates after RemoveEmitter"), CountAt(UNiagaraMcpEditorTools::GetParticleCounts(System, Times), 0), 10);

	// ------------------------------------------------------------------ Suggestions and curve interpolation
	TestTrue(TEXT("DidYouMean finds a typo"), NiagaraMcp::DidYouMean(TEXT("Spawn Burst Instantanous"), { TEXT("Spawn Rate"), TEXT("Spawn Burst Instantaneous"), TEXT("Initialize Particle") }).Contains(TEXT("Spawn Burst Instantaneous")));
	TestTrue(TEXT("DidYouMean stays quiet for unrelated names"), NiagaraMcp::DidYouMean(TEXT("Zzz"), { TEXT("Spawn Rate") }).IsEmpty());

	UNiagaraMcpEditorTools::AddModule(System, TEXT("Sparks"), TEXT("ParticleUpdate"), TEXT("/Niagara/Modules/Update/Color/Color.Color"), -1, false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Sparks"), TEXT("Color"), TEXT("Color"), TEXT("Dynamic:/Niagara/DynamicInputs/ValueFromCurve/ColorFromCurve.ColorFromCurve"), TEXT("ParticleUpdate"), false);
	TArray<FNiagaraMcpCurveKey> Keys;
	Keys.Add({ 0.0f, TEXT("1, 1, 1, 1") });
	Keys.Add({ 1.0f, TEXT("1, 0, 0, 0") });
	UNiagaraMcpEditorTools::SetCurveKeys(System, TEXT("Sparks"), TEXT("Color"), TEXT("Color/ColorCurve"), Keys, TEXT("Constant"), TEXT("ParticleUpdate"), false);
	const FNiagaraMcpCurveInfo CurveBack = UNiagaraMcpEditorTools::GetCurveKeys(System, TEXT("Sparks"), TEXT("Color"), TEXT("Color/ColorCurve"), TEXT("ParticleUpdate"));
	TestEqual(TEXT("Curve interpolation read back"), CurveBack.Interpolation, FString(TEXT("Constant")));

	const FNiagaraMcpStackInfo UpdateStack = UNiagaraMcpEditorTools::GetStack(System, TEXT("Sparks"), true);
	const FNiagaraMcpModuleInfo* ColorModule = UpdateStack.Modules.FindByPredicate([](const FNiagaraMcpModuleInfo& Module) { return Module.DisplayName == TEXT("Color"); });
	const FNiagaraMcpInputInfo* ColorInput = ColorModule ? ColorModule->Inputs.FindByPredicate([](const FNiagaraMcpInputInfo& Input) { return Input.Name.EndsWith(TEXT("Color")) && Input.Mode == TEXT("Dynamic"); }) : nullptr;
	TestTrue(TEXT("Dynamic value is a script path usable with Dynamic:"), ColorInput && ColorInput->Value.StartsWith(TEXT("Dynamic:/Niagara/")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpCoreSimSyncTest, "NiagaraMcp.Core.EditsReachSimulation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpCoreSimSyncTest::RunTest(const FString& Parameters)
{
	using namespace NiagaraMcpCoreTests;
	const FString Folder = FString::Printf(TEXT("/Game/NiagaraMcpTests/Core/Sim_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	const FString BurstPath = TEXT("/Niagara/Modules/Emitter/SpawnBurst_Instantaneous.SpawnBurst_Instantaneous");
	TArray<float> Times = { 0.2f };

	// 1) Curve edits reach the simulation without a graph change.
	{
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *UNiagaraMcpEditorTools::CreateSystem(Folder + TEXT("/NS_Curve"), false).Path);
		if (!TestNotNull(TEXT("Curve system"), System))
		{
			return false;
		}
		UNiagaraMcpEditorTools::AddEmitter(System, TEXT("A"), TEXT("Minimal"), false);
		UNiagaraMcpEditorTools::AddModule(System, TEXT("A"), TEXT("EmitterUpdate"), BurstPath, -1, false);
		UNiagaraMcpEditorTools::AddModule(System, TEXT("A"), TEXT("ParticleUpdate"), TEXT("/Niagara/Modules/Update/Color/Color.Color"), -1, false);
		UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("A"), TEXT("Color"), TEXT("Color"), TEXT("Dynamic:/Niagara/DynamicInputs/ValueFromCurve/ColorFromCurve.ColorFromCurve"), TEXT("ParticleUpdate"), false);
		UNiagaraMcpEditorTools::CompileSystem(System, true, false);
		TArray<FNiagaraMcpCurveKey> RedKeys;
		RedKeys.Add({ 0.0f, TEXT("1, 0, 0, 1") });
		RedKeys.Add({ 1.0f, TEXT("1, 0, 0, 1") });
		UNiagaraMcpEditorTools::SetCurveKeys(System, TEXT("A"), TEXT("Color"), TEXT("Color/ColorCurve"), RedKeys, TEXT("Linear"), TEXT("ParticleUpdate"), false);
		const TArray<FNiagaraMcpAttributeSample> Red = UNiagaraMcpEditorTools::GetParticleAttributes(System, Times, { TEXT("Color") }, TEXT("A"), 0);
		const FNiagaraMcpAttributeStats* RedColor = Red.Num() && Red[0].Emitters.Num() && Red[0].Emitters[0].Attributes.Num() ? &Red[0].Emitters[0].Attributes[0] : nullptr;
		TestTrue(FString::Printf(TEXT("Edited curve reaches the simulation (max %s)"), RedColor ? *RedColor->Max : TEXT("-")), RedColor && RedColor->Max.StartsWith(TEXT("1, 0, 0")));
	}

	// 1b) Fireball order: the curve is edited right after the edit that requested a compile (no compile in between),
	//     then the module is toggled off/on (a recompile with an unchanged graph). Both must keep the edited values.
	{
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *UNiagaraMcpEditorTools::CreateSystem(Folder + TEXT("/NS_CurveInFlight"), false).Path);
		if (!TestNotNull(TEXT("In-flight system"), System))
		{
			return false;
		}
		UNiagaraMcpEditorTools::AddEmitter(System, TEXT("A"), TEXT("Minimal"), false);
		UNiagaraMcpEditorTools::AddModule(System, TEXT("A"), TEXT("EmitterUpdate"), BurstPath, -1, false);
		UNiagaraMcpEditorTools::AddModule(System, TEXT("A"), TEXT("ParticleUpdate"), TEXT("/Niagara/Modules/Update/Color/Color.Color"), -1, false);
		UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("A"), TEXT("Color"), TEXT("Color"), TEXT("Dynamic:/Niagara/DynamicInputs/ValueFromCurve/ColorFromCurve.ColorFromCurve"), TEXT("ParticleUpdate"), false);
		TArray<FNiagaraMcpCurveKey> GreenKeys;
		GreenKeys.Add({ 0.0f, TEXT("0, 1, 0, 1") });
		GreenKeys.Add({ 1.0f, TEXT("0, 1, 0, 1") });
		UNiagaraMcpEditorTools::SetCurveKeys(System, TEXT("A"), TEXT("Color"), TEXT("Color/ColorCurve"), GreenKeys, TEXT("Linear"), TEXT("ParticleUpdate"), false);
		UNiagaraMcpEditorTools::CompileSystem(System, true, false);
		auto MaxColor = [&]() -> FString
		{
			const TArray<FNiagaraMcpAttributeSample> S = UNiagaraMcpEditorTools::GetParticleAttributes(System, Times, { TEXT("Color") }, TEXT("A"), 0);
			return S.Num() && S[0].Emitters.Num() && S[0].Emitters[0].Attributes.Num() ? S[0].Emitters[0].Attributes[0].Max : FString(TEXT("-"));
		};
		const FString AfterCompile = MaxColor();
		TestTrue(FString::Printf(TEXT("Curve edited during an in-flight compile reaches the simulation (max %s)"), *AfterCompile), AfterCompile.StartsWith(TEXT("0, 1, 0")));
		TArray<FNiagaraMcpCurveKey> BlueKeys;
		BlueKeys.Add({ 0.0f, TEXT("0, 0, 1, 1") });
		BlueKeys.Add({ 1.0f, TEXT("0, 0, 1, 1") });
		UNiagaraMcpEditorTools::SetCurveKeys(System, TEXT("A"), TEXT("Color"), TEXT("Color/ColorCurve"), BlueKeys, TEXT("Linear"), TEXT("ParticleUpdate"), false);
		const FString AfterToggle = MaxColor();
		TestTrue(FString::Printf(TEXT("A second curve edit also reaches the simulation (max %s)"), *AfterToggle), AfterToggle.StartsWith(TEXT("0, 0, 1")));
	}

	// 2) After RemoveEmitter, edits to the remaining emitter still simulate.
	{
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *UNiagaraMcpEditorTools::CreateSystem(Folder + TEXT("/NS_Remove"), false).Path);
		if (!TestNotNull(TEXT("Remove system"), System))
		{
			return false;
		}
		for (const TCHAR* Name : { TEXT("Keep"), TEXT("Drop") })
		{
			UNiagaraMcpEditorTools::AddEmitter(System, Name, TEXT("Minimal"), false);
			UNiagaraMcpEditorTools::AddModule(System, Name, TEXT("EmitterUpdate"), BurstPath, -1, false);
		}
		TestEqual(TEXT("Both emitters simulate"), CountAt(UNiagaraMcpEditorTools::GetParticleCounts(System, Times), 0), 2);
		UNiagaraMcpEditorTools::RemoveEmitter(System, TEXT("Drop"), false);
		TestEqual(TEXT("Remaining emitter simulates"), CountAt(UNiagaraMcpEditorTools::GetParticleCounts(System, Times), 0), 1);
		UNiagaraMcpEditorTools::AddModule(System, TEXT("Keep"), TEXT("ParticleUpdate"), TEXT("/Niagara/Modules/Update/Color/Color.Color"), -1, false);
		TestEqual(TEXT("Still simulates after editing the remaining emitter"), CountAt(UNiagaraMcpEditorTools::GetParticleCounts(System, Times), 0), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpCorePreviewTest, "NiagaraMcp.Core.PreviewRunAndBackground",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace NiagaraMcpCoreTests
{
	/** Pixel (X, Y) of a preview image (decoded PNG). */
	bool PixelAt(const FToolsetImage& Image, int32 X, int32 Y, FColor& Out)
	{
		TArray<uint8> Bytes;
		if (Image.Data.IsEmpty() || !FBase64::Decode(Image.Data, Bytes))
		{
			return false;
		}
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
		TArray<uint8> Raw;
		if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()) || !Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw))
		{
			return false;
		}
		const int64 Index = (static_cast<int64>(Y) * Wrapper->GetWidth() + X) * 4;
		if (X >= Wrapper->GetWidth() || Y >= Wrapper->GetHeight() || Index + 3 >= Raw.Num())
		{
			return false;
		}
		Out = FColor(Raw[Index + 2], Raw[Index + 1], Raw[Index], Raw[Index + 3]);
		return true;
	}

	/** Top-left pixel of a preview image (decoded PNG). */
	bool CornerPixel(const FToolsetImage& Image, FColor& Out)
	{
		TArray<uint8> Bytes;
		if (Image.Data.IsEmpty() || !FBase64::Decode(Image.Data, Bytes))
		{
			return false;
		}
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
		TArray<uint8> Raw;
		if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()) || !Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw) || Raw.Num() < 4)
		{
			return false;
		}
		Out = FColor(Raw[2], Raw[1], Raw[0], Raw[3]);
		return true;
	}
}

bool FNiagaraMcpCorePreviewTest::RunTest(const FString& Parameters)
{
	using namespace NiagaraMcpCoreTests;
	const FString Folder = FString::Printf(TEXT("/Game/NiagaraMcpTests/Core/Preview_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *UNiagaraMcpEditorTools::CreateSystem(Folder + TEXT("/NS_Run"), false).Path);
	if (!TestNotNull(TEXT("System"), System))
	{
		return false;
	}
	UNiagaraMcpEditorTools::AddEmitter(System, TEXT("A"), TEXT("Minimal"), false);
	UNiagaraMcpEditorTools::AddModule(System, TEXT("A"), TEXT("EmitterUpdate"), TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate"), -1, false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("A"), TEXT("Spawn Rate"), TEXT("SpawnRate"), TEXT("50"), TEXT("Any"), false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("A"), TEXT("Initialize Particle"), TEXT("Lifetime"), TEXT("5"), TEXT("Any"), false);

	// Unsorted times: one continuous run, results in the caller's order.
	TArray<float> Times = { 0.8f, 0.2f, 0.5f };
	const TArray<FNiagaraMcpParticleSample> Counts = UNiagaraMcpEditorTools::GetParticleCounts(System, Times);
	TestTrue(TEXT("Counts come back in the caller's order"), Counts.Num() == 3 && FMath::IsNearlyEqual(Counts[0].Time, 0.8f) && FMath::IsNearlyEqual(Counts[1].Time, 0.2f));
	TestTrue(TEXT("Spawn rate accumulates over the run"), Counts.Num() == 3 && Counts[0].TotalParticles > Counts[2].TotalParticles && Counts[2].TotalParticles > Counts[1].TotalParticles);
	const TArray<FNiagaraMcpAttributeSample> Ages = UNiagaraMcpEditorTools::GetParticleAttributes(System, Times, { TEXT("Age") }, TEXT("A"), 0);
	auto MaxAge = [&](int32 Index) -> float
	{
		return Ages.IsValidIndex(Index) && Ages[Index].Emitters.Num() && Ages[Index].Emitters[0].Attributes.Num() ? FCString::Atof(*Ages[Index].Emitters[0].Attributes[0].Max) : -1.0f;
	};
	TestTrue(FString::Printf(TEXT("Oldest particle age follows each time (%.2f, %.2f, %.2f)"), MaxAge(0), MaxAge(1), MaxAge(2)),
		MaxAge(0) > 0.7f && MaxAge(1) < 0.25f && MaxAge(2) > 0.4f && MaxAge(2) < 0.55f);

	// Background option.
	FColor BlackCorner(0, 0, 0), GreyCorner(0, 0, 0);
	const FToolsetImage Black = UNiagaraMcpEditorTools::CapturePreview(System, { 0.5f }, 128, 0.0f, -15.0f, 400.0f, TEXT("Black")).Image;
	const FToolsetImage Grey = UNiagaraMcpEditorTools::CapturePreview(System, { 0.5f }, 128, 0.0f, -15.0f, 400.0f, TEXT("Grey")).Image;
	const bool bDecoded = CornerPixel(Black, BlackCorner) && CornerPixel(Grey, GreyCorner);
	TestTrue(FString::Printf(TEXT("Grey backdrop is visible and neutral (black corner %d,%d,%d / grey corner %d,%d,%d)"), BlackCorner.R, BlackCorner.G, BlackCorner.B, GreyCorner.R, GreyCorner.G, GreyCorner.B),
		bDecoded && GreyCorner.R > BlackCorner.R + 25 && FMath::Abs(GreyCorner.R - GreyCorner.B) < 20);
	// Every camera yaw gets the backdrop (it was black for yaws facing away from the light).
	const FToolsetImage TwoYaws = UNiagaraMcpEditorTools::CapturePreview(System, { 0.5f }, 128, 0.0f, -15.0f, 400.0f, TEXT("Grey"), TEXT("0, 90")).Image;
	FColor Left(0, 0, 0), Right(0, 0, 0);
	const bool bTwo = PixelAt(TwoYaws, 2, 2, Left) && PixelAt(TwoYaws, 128 + 2, 2, Right);
	TestTrue(FString::Printf(TEXT("Grey backdrop on both yaws (%d / %d)"), Left.R, Right.R), bTwo && Left.R > 25 && Right.R > 25 && FMath::Abs(Left.R - Right.R) < 40);
	AddExpectedError(TEXT("Unknown Background"), EAutomationExpectedErrorFlags::Contains, 0);
	TestTrue(TEXT("Unknown background is rejected"), UNiagaraMcpEditorTools::CapturePreview(System, { 0.5f }, 128, 0.0f, -15.0f, 0.0f, TEXT("Purple")).Image.Data.IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
