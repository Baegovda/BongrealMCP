// v0.5 analysis tools: visibility diagnosis, cost analysis, compiled HLSL, user parameter usage.

#include "NiagaraMcpEditorTools.h"

#include "Async/TaskGraphInterfaces.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/World.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraComponent.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEffectType.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterBase.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraGPUProfilerInterface.h"
#include "NiagaraGpuComputeDispatchInterface.h"
#include "NiagaraGraph.h"
#include "NiagaraMcpAnalysisUtils.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraMcpEditorToolsInternal.h"
#include "NiagaraMcpExtUtils.h"
#include "NiagaraMcpPreview.h"
#include "NiagaraMcpStackUtils.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraPlatformSet.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "PreviewScene.h"
#include "RHI.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

#define LOCTEXT_NAMESPACE "NiagaraMcpEditorTools"

using namespace NiagaraMcp;

// ------------------------------------------------------------------------------------------------ Shared helpers

namespace NiagaraMcpAnalysis
{
	FErrorCapture::FErrorCapture()
	{
		GLog->AddOutputDevice(this);
	}

	FErrorCapture::~FErrorCapture()
	{
		GLog->RemoveOutputDevice(this);
	}

	void FErrorCapture::Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category)
	{
		// ReportError logs a Warning; the automation framework re-logs expected warnings as Verbose, so accept both.
		const ELogVerbosity::Type Level = static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
		if (Category == LogNiagaraMcp.GetCategoryName() && (Level <= ELogVerbosity::Warning || Level == ELogVerbosity::Verbose))
		{
			FScopeLock ScopeLock(&Lock);
			Messages.Add(Message);
		}
	}

	int32 FErrorCapture::Num() const
	{
		FScopeLock ScopeLock(&Lock);
		return Messages.Num();
	}

	FString FErrorCapture::TakeSince(int32 Start) const
	{
		FScopeLock ScopeLock(&Lock);
		TArray<FString> Parts;
		for (int32 Index = Start; Index < Messages.Num(); ++Index)
		{
			Parts.Add(Messages[Index]);
		}
		return FString::Join(Parts, TEXT(" | "));
	}

	TArray<double> ParseNumbers(const FString& Text)
	{
		TArray<double> Out;
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","));
		for (const FString& Part : Parts)
		{
			const FString Trimmed = Part.TrimStartAndEnd();
			if (!Trimmed.IsEmpty())
			{
				Out.Add(FCString::Atod(*Trimmed));
			}
		}
		return Out;
	}

	FString ShortUsage(ENiagaraScriptUsage Usage)
	{
		FString Text = UsageToString(Usage);
		Text.RemoveFromEnd(TEXT("Script"));
		return Text;
	}

	FString CompileStatusText(ENiagaraScriptCompileStatus Status)
	{
		FString Text = StaticEnum<ENiagaraScriptCompileStatus>()->GetNameStringByValue(static_cast<int64>(Status));
		Text.RemoveFromStart(TEXT("NCS_"));
		return Text;
	}

	bool IsLightweight(const FNiagaraEmitterHandle& Handle)
	{
		return Handle.GetEmitterMode() == ENiagaraEmitterMode::Stateless;
	}

	TArray<UNiagaraRendererProperties*> GetRenderers(const FNiagaraEmitterHandle& Handle)
	{
		TArray<UNiagaraRendererProperties*> Out;
		if (FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData())
		{
			Out = EmitterData->GetRenderers();
			return Out;
		}
		// Lightweight emitters (UNiagaraStatelessEmitter lives in an Internal header): read RendererProperties by reflection.
		UObject* Emitter = Handle.GetEmitterBase();
		const FArrayProperty* Array = Emitter ? FindFProperty<FArrayProperty>(Emitter->GetClass(), TEXT("RendererProperties")) : nullptr;
		const FObjectPropertyBase* Inner = Array ? CastField<FObjectPropertyBase>(Array->Inner) : nullptr;
		if (Inner)
		{
			FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Emitter));
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				Out.Add(Cast<UNiagaraRendererProperties>(Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index))));
			}
		}
		return Out;
	}

	TArray<UObject*> GetStatelessModules(const FNiagaraEmitterHandle& Handle)
	{
		TArray<UObject*> Out;
		UObject* Emitter = IsLightweight(Handle) ? Handle.GetEmitterBase() : nullptr;
		const FArrayProperty* Array = Emitter ? FindFProperty<FArrayProperty>(Emitter->GetClass(), TEXT("Modules")) : nullptr;
		const FObjectPropertyBase* Inner = Array ? CastField<FObjectPropertyBase>(Array->Inner) : nullptr;
		if (Inner)
		{
			FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Emitter));
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				if (UObject* Module = Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index)))
				{
					Out.Add(Module);
				}
			}
		}
		return Out;
	}

	void CollectInputsRecursive(UNiagaraStackEntry& Entry, TArray<UNiagaraStackFunctionInput*>& Out, int32 Depth)
	{
		if (Depth > 5)
		{
			return;
		}
		TArray<UNiagaraStackFunctionInput*> Inputs;
		if (UNiagaraStackModuleItem* Module = Cast<UNiagaraStackModuleItem>(&Entry))
		{
			Stack::GetModuleInputs(*Module, Inputs);
		}
		else
		{
			TArray<UNiagaraStackEntry*> Pending;
			Entry.GetUnfilteredChildren(Pending);
			while (Pending.Num() > 0)
			{
				UNiagaraStackEntry* Child = Pending.Pop(EAllowShrinking::No);
				if (UNiagaraStackFunctionInput* Input = Cast<UNiagaraStackFunctionInput>(Child))
				{
					Inputs.Add(Input);
				}
				else if (Child)
				{
					Child->GetUnfilteredChildren(Pending);
				}
			}
		}
		for (UNiagaraStackFunctionInput* Input : Inputs)
		{
			if (Input == nullptr)
			{
				continue;
			}
			Out.Add(Input);
			if (Input->GetValueMode() == UNiagaraStackFunctionInput::EValueMode::Dynamic)
			{
				CollectInputsRecursive(*Input, Out, Depth + 1);
			}
		}
	}

	FString InputPath(UNiagaraStackFunctionInput& Input, UNiagaraStackModuleItem& Module)
	{
		// "Parent/Child" for inputs of dynamic inputs (SetModuleInput syntax).
		TArray<FString> Parts;
		Parts.Add(Input.GetInputParameterHandle().GetName().ToString());
		for (UNiagaraStackEntry* Outer = Cast<UNiagaraStackEntry>(Input.GetOuter()); Outer && Outer != &Module; Outer = Cast<UNiagaraStackEntry>(Outer->GetOuter()))
		{
			// Only dynamic inputs nest (static switch children are addressed by their own name, as in ExportSystemSpec).
			UNiagaraStackFunctionInput* Parent = Cast<UNiagaraStackFunctionInput>(Outer);
			if (Parent && Parent->GetValueMode() == UNiagaraStackFunctionInput::EValueMode::Dynamic)
			{
				Parts.Insert(Parent->GetInputParameterHandle().GetName().ToString(), 0);
			}
		}
		return FString::Join(Parts, TEXT("/"));
	}
}

namespace NiagaraMcpAnalysisPrivate
{
	using namespace NiagaraMcpAnalysis;

	FPreviewScene::ConstructionValues PreviewValues()
	{
		return FPreviewScene::ConstructionValues()
			.SetEditor(true)
			.SetTransactional(false)
			.SetCreatePhysicsScene(false)
			.AllowAudioPlayback(false)
			.SetCreateDefaultLighting(false);
	}

	FString Quote(const FString& Text)
	{
		return TEXT("\"") + Text + TEXT("\"");
	}

	// ---------------------------------------------------------------- DiagnoseVisibility

	struct FChecklist
	{
		FNiagaraMcpVisibilityReport& Report;
		FName Emitter;
		bool bStopped = false;

		/** Adds a check; after the first Fail of this emitter every further check becomes Skipped. Returns false once stopped. */
		bool Add(const TCHAR* Check, const TCHAR* Status, const FString& Evidence, const FString& Fix = FString())
		{
			FNiagaraMcpVisibilityCheck& Entry = Report.Checks.AddDefaulted_GetRef();
			Entry.Emitter = Emitter;
			Entry.Check = Check;
			if (bStopped)
			{
				Entry.Status = TEXT("Skipped");
				Entry.Evidence = TEXT("not checked: an earlier check failed");
				return false;
			}
			Entry.Status = Status;
			Entry.Evidence = Evidence;
			Entry.Fix = FCString::Strcmp(Status, TEXT("Ok")) == 0 ? FString() : Fix;
			if (FCString::Strcmp(Status, TEXT("Fail")) == 0)
			{
				bStopped = true;
				if (Report.FirstFail.IsEmpty())
				{
					Report.FirstFail = FString::Printf(TEXT("%s: %s - %s"), Emitter.IsNone() ? TEXT("System") : *Emitter.ToString(), Check, *Evidence);
					Report.FirstFailCheck = Check;
					Report.FirstFailEmitter = Emitter;
				}
			}
			return !bStopped;
		}
	};

	/** Material state captured BEFORE simulating: in the editor a renderer silently sets a missing usage flag on first use. */
	struct FMaterialState
	{
		int32 RendererIndex = INDEX_NONE;
		FString RendererClass;
		FString Path;
		bool bMissing = false;
		bool bHasUsage = true;
		bool bAutoUsage = true;
		FString UsageName;
		FString UsageProperty;
		FString Domain;
		FString BlendMode;
		bool bAdditive = false;
		TArray<FString> CompileErrors;
	};

	bool RendererUsage(const UNiagaraRendererProperties& Renderer, EMaterialUsage& OutUsage, FString& OutProperty)
	{
		if (Renderer.IsA<UNiagaraSpriteRendererProperties>())
		{
			OutUsage = MATUSAGE_NiagaraSprites;
			OutProperty = TEXT("bUsedWithNiagaraSprites");
			return true;
		}
		if (Renderer.IsA<UNiagaraRibbonRendererProperties>())
		{
			OutUsage = MATUSAGE_NiagaraRibbons;
			OutProperty = TEXT("bUsedWithNiagaraRibbons");
			return true;
		}
		if (Renderer.IsA<UNiagaraMeshRendererProperties>())
		{
			OutUsage = MATUSAGE_NiagaraMeshParticles;
			OutProperty = TEXT("bUsedWithNiagaraMeshParticles");
			return true;
		}
		return false;
	}

	void CaptureMaterials(const TArray<UNiagaraRendererProperties*>& Renderers, TArray<FMaterialState>& Out)
	{
		for (int32 Index = 0; Index < Renderers.Num(); ++Index)
		{
			const UNiagaraRendererProperties* Renderer = Renderers[Index];
			EMaterialUsage Usage;
			FString UsageProperty;
			if (Renderer == nullptr || !Renderer->GetIsEnabled() || !RendererUsage(*Renderer, Usage, UsageProperty))
			{
				continue;
			}
			TArray<UMaterialInterface*> Materials;
			Renderer->GetUsedMaterials(nullptr, Materials);
			if (Materials.Num() == 0)
			{
				Materials.Add(nullptr);
			}
			for (UMaterialInterface* Material : Materials)
			{
				FMaterialState& State = Out.AddDefaulted_GetRef();
				State.RendererIndex = Index;
				State.RendererClass = Renderer->GetClass()->GetName();
				State.UsageName = UMaterialInterface::GetUsageName(Usage);
				State.UsageProperty = UsageProperty;
				const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
				if (Material == nullptr || Base == nullptr)
				{
					State.bMissing = true;
					continue;
				}
				State.Path = Material->GetPathName();
				State.bHasUsage = Base->GetUsageByFlag(Usage);
				State.bAutoUsage = Base->bAutomaticallySetUsageInEditor;
				State.Domain = StaticEnum<EMaterialDomain>()->GetNameStringByValue(static_cast<int64>(Base->MaterialDomain.GetValue()));
				const EBlendMode Blend = Material->GetBlendMode();
				State.BlendMode = StaticEnum<EBlendMode>()->GetNameStringByValue(static_cast<int64>(Blend));
				State.bAdditive = Blend == BLEND_Additive;
				if (const FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
				{
					State.CompileErrors = Resource->GetCompileErrors();
				}
			}
		}
	}

	const FNiagaraMcpAttributeStats* FindStats(const FNiagaraMcpEmitterAttributes* Emitter, const TCHAR* Attribute)
	{
		return Emitter ? Emitter->Attributes.FindByPredicate([Attribute](const FNiagaraMcpAttributeStats& S) { return S.Attribute.Equals(Attribute, ESearchCase::IgnoreCase); }) : nullptr;
	}

	/** Lifetime set on Initialize Particle when it is a plain local value: -1 when not determinable. */
	float StaticLifetime(UNiagaraStackModuleItem& InitModule, FString& OutInput)
	{
		TArray<UNiagaraStackFunctionInput*> Inputs;
		Stack::GetModuleInputs(InitModule, Inputs);
		FString Mode;
		TMap<FString, UNiagaraStackFunctionInput*> ByName;
		for (UNiagaraStackFunctionInput* Input : Inputs)
		{
			const FString Name = Input->GetInputParameterHandle().GetName().ToString();
			ByName.Add(Name.Replace(TEXT(" "), TEXT("")).ToLower(), Input);
		}
		if (UNiagaraStackFunctionInput** ModeInput = ByName.Find(TEXT("lifetimemode")))
		{
			Mode = Stack::InputValueToString(**ModeInput);
		}
		const TCHAR* Key = Mode.Contains(TEXT("Random")) ? TEXT("lifetimemax") : TEXT("lifetime");
		UNiagaraStackFunctionInput** Input = ByName.Find(Key);
		if (Input == nullptr || (*Input)->GetValueMode() != UNiagaraStackFunctionInput::EValueMode::Local || Mode.Contains(TEXT("Unset")))
		{
			return -1.0f;
		}
		OutInput = (*Input)->GetInputParameterHandle().GetName().ToString();
		return FCString::Atof(*Stack::InputValueToString(**Input));
	}

	FString BoxText(const FBox& Box)
	{
		return FString::Printf(TEXT("(Min=(X=%.0f,Y=%.0f,Z=%.0f),Max=(X=%.0f,Y=%.0f,Z=%.0f),IsValid=True)"), Box.Min.X, Box.Min.Y, Box.Min.Z, Box.Max.X, Box.Max.Y, Box.Max.Z);
	}

	void DiagnoseEmitter(UNiagaraSystem& System, const FNiagaraEmitterHandle& Handle, FNiagaraSystemViewModel* ViewModel, const TArray<FMaterialState>& Materials,
		const FNiagaraMcpEmitterAttributes* Sim, float Time, FNiagaraMcpVisibilityReport& Report)
	{
		FChecklist List{ Report, Handle.GetName() };
		const FString Name = Handle.GetName().ToString();
		const FString Sys = TEXT("System");
		const bool bLightweight = IsLightweight(Handle);
		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();

		// 1. Enabled
		if (!Handle.GetIsEnabled())
		{
			List.Add(TEXT("EmitterEnabled"), TEXT("Fail"), TEXT("The emitter is disabled in the system."), FString::Printf(TEXT("SetEmitterEnabled(System, %s, true)"), *Quote(Name)));
		}
		else
		{
			List.Add(TEXT("EmitterEnabled"), TEXT("Ok"), TEXT("enabled"));
		}

		// 2. Scalability at the current effects quality
		{
			const int32 QualityLevel = FNiagaraPlatformSet::GetQualityLevel();
			const FString QualityText = FNiagaraPlatformSet::GetQualityLevelText(QualityLevel).ToString();
			bool bAllowed = true;
			if (EmitterData)
			{
				bAllowed = EmitterData->IsAllowedByScalability();
			}
			else if (UObject* Emitter = Handle.GetEmitterBase())
			{
				if (const FStructProperty* Platforms = FindFProperty<FStructProperty>(Emitter->GetClass(), TEXT("Platforms")))
				{
					bAllowed = Platforms->ContainerPtrToValuePtr<FNiagaraPlatformSet>(Emitter)->IsActive();
				}
			}
			if (!bAllowed)
			{
				List.Add(TEXT("Scalability"), TEXT("Fail"), FString::Printf(TEXT("The emitter's Platforms exclude the current effects quality %s (%d) / device profile, so it never runs here."), *QualityText, QualityLevel),
					bLightweight ? FString::Printf(TEXT("SetStatelessModuleProperty(System, %s, \"Emitter\", \"Platforms.QualityLevelMask\", \"-1\")"), *Quote(Name))
					: FString::Printf(TEXT("SetEmitterProperty(System, %s, \"Platforms.QualityLevelMask\", \"-1\")  (-1 = every quality level)"), *Quote(Name)));
			}
			else
			{
				List.Add(TEXT("Scalability"), TEXT("Ok"), FString::Printf(TEXT("allowed at effects quality %s"), *QualityText));
			}
		}

		// 3. Spawn source
		UNiagaraStackModuleItem* InitModule = nullptr;
		if (bLightweight)
		{
			UObject* Emitter = Handle.GetEmitterBase();
			const FArrayProperty* SpawnInfos = Emitter ? FindFProperty<FArrayProperty>(Emitter->GetClass(), TEXT("SpawnInfos")) : nullptr;
			const int32 Count = SpawnInfos ? FScriptArrayHelper(SpawnInfos, SpawnInfos->ContainerPtrToValuePtr<void>(Emitter)).Num() : 0;
			List.Add(TEXT("Spawn"), Count > 0 ? TEXT("Ok") : TEXT("Fail"), FString::Printf(TEXT("%d spawn info(s) (rate / burst)"), Count),
				TEXT("Add a spawn rate or burst to the lightweight emitter's SpawnInfos (GetStatelessEmitterInfo lists them)."));
		}
		else
		{
			TArray<FString> Spawners;
			TArray<FString> DisabledSpawners;
			UNiagaraStackViewModel* StackViewModel = ViewModel ? Stack::GetStackViewModel(*ViewModel, Handle.GetName()) : nullptr;
			if (StackViewModel)
			{
				TArray<UNiagaraStackModuleItem*> Items;
				Stack::CollectModuleItems(*StackViewModel, Items);
				for (UNiagaraStackModuleItem* Item : Items)
				{
					const ENiagaraScriptUsage Usage = Stack::GetModuleUsage(*Item);
					const UNiagaraScript* Script = Item->GetModuleNode().FunctionScript;
					const FString ScriptName = Script ? Script->GetName() : Item->GetModuleNode().GetFunctionName();
					if (Usage == ENiagaraScriptUsage::ParticleSpawnScript && ScriptName.Equals(TEXT("InitializeParticle"), ESearchCase::IgnoreCase) && Item->GetIsEnabled())
					{
						InitModule = Item;
					}
					const bool bEmitterStack = Usage == ENiagaraScriptUsage::EmitterSpawnScript || Usage == ENiagaraScriptUsage::EmitterUpdateScript;
					if (bEmitterStack && ScriptName.Contains(TEXT("Spawn")))
					{
						(Item->GetIsEnabled() ? Spawners : DisabledSpawners).Add(Item->GetDisplayName().ToString());
					}
				}
			}
			int32 SpawningHandlers = 0;
			if (EmitterData)
			{
				for (const FNiagaraEventScriptProperties& Handler : EmitterData->GetEventHandlers())
				{
					SpawningHandlers += Handler.ExecutionMode == EScriptExecutionMode::SpawnedParticles ? 1 : 0;
				}
			}
			if (Spawners.Num() > 0 || SpawningHandlers > 0)
			{
				List.Add(TEXT("Spawn"), TEXT("Ok"), FString::Printf(TEXT("spawn modules: %s%s"), *FString::Join(Spawners, TEXT(", ")),
					SpawningHandlers > 0 ? *FString::Printf(TEXT("; %d spawning event handler(s)"), SpawningHandlers) : TEXT("")));
			}
			else if (DisabledSpawners.Num() > 0)
			{
				List.Add(TEXT("Spawn"), TEXT("Fail"), FString::Printf(TEXT("Spawn modules are disabled: %s"), *FString::Join(DisabledSpawners, TEXT(", "))),
					FString::Printf(TEXT("SetModuleEnabled(System, %s, %s, true)"), *Quote(Name), *Quote(DisabledSpawners[0])));
			}
			else
			{
				List.Add(TEXT("Spawn"), TEXT("Fail"), TEXT("No spawn module in EmitterSpawn / EmitterUpdate (Spawn Rate, Spawn Burst Instantaneous, Spawn Per Unit) and no spawning event handler: no particle is ever created."),
					FString::Printf(TEXT("AddModule(System, %s, \"EmitterUpdate\", \"/Niagara/Modules/Emitter/SpawnRate.SpawnRate\") then SetModuleInput(System, %s, \"Spawn Rate\", \"SpawnRate\", \"50\")"), *Quote(Name), *Quote(Name)));
			}
		}

		// 4. Lifetime (static value of Initialize Particle; measured below when particles exist)
		if (InitModule)
		{
			FString InputName;
			const float Lifetime = StaticLifetime(*InitModule, InputName);
			if (Lifetime >= 0.0f && Lifetime <= KINDA_SMALL_NUMBER)
			{
				List.Add(TEXT("Lifetime"), TEXT("Fail"), FString::Printf(TEXT("Initialize Particle %s = %g s: particles die the frame they spawn."), *InputName, Lifetime),
					FString::Printf(TEXT("SetModuleInput(System, %s, \"Initialize Particle\", %s, \"1.5\")"), *Quote(Name), *Quote(InputName)));
			}
			else if (Lifetime > 0.0f && Lifetime < 0.05f)
			{
				List.Add(TEXT("Lifetime"), TEXT("Warning"), FString::Printf(TEXT("Initialize Particle %s = %g s (a few frames)."), *InputName, Lifetime),
					FString::Printf(TEXT("SetModuleInput(System, %s, \"Initialize Particle\", %s, \"1.5\")"), *Quote(Name), *Quote(InputName)));
			}
		}

		// 5. Particles at Time
		const int32 Particles = Sim ? Sim->Particles : 0;
		if (Particles <= 0)
		{
			FString Fix = FString::Printf(TEXT("Check the spawn module inputs (e.g. SetModuleInput(System, %s, \"Spawn Rate\", \"SpawnRate\", \"50\")), Emitter State (loop duration / inactive response) and the spawn time; GetParticleCounts(System, [0.1, 0.5, 1, 2]) shows when particles exist."), *Quote(Name));
			List.Add(TEXT("Particles"), TEXT("Fail"), FString::Printf(TEXT("0 particles at %.2f s (state %s)."), Time, Sim ? *Sim->State : TEXT("not simulated")), Fix);
		}
		else
		{
			List.Add(TEXT("Particles"), TEXT("Ok"), FString::Printf(TEXT("%d particles at %.2f s"), Particles, Time));
		}
		if (const FNiagaraMcpAttributeStats* Lifetime = FindStats(Sim, TEXT("Lifetime")))
		{
			const TArray<double> Max = ParseNumbers(Lifetime->Max);
			if (Max.Num() > 0 && Max[0] <= KINDA_SMALL_NUMBER)
			{
				List.Add(TEXT("Lifetime"), TEXT("Fail"), FString::Printf(TEXT("Particles.Lifetime max %s"), *Lifetime->Max),
					FString::Printf(TEXT("SetModuleInput(System, %s, \"Initialize Particle\", \"Lifetime\", \"1.5\")"), *Quote(Name)));
			}
		}

		// 6. Renderers
		const TArray<UNiagaraRendererProperties*> Renderers = GetRenderers(Handle);
		TArray<int32> Enabled;
		for (int32 Index = 0; Index < Renderers.Num(); ++Index)
		{
			if (Renderers[Index] && Renderers[Index]->GetIsEnabled())
			{
				Enabled.Add(Index);
			}
		}
		if (Renderers.Num() == 0)
		{
			List.Add(TEXT("Renderers"), TEXT("Fail"), TEXT("The emitter has no renderer: particles simulate but nothing draws them."),
				FString::Printf(TEXT("AddRenderer(System, %s, \"Sprite\")"), *Quote(Name)));
		}
		else if (Enabled.Num() == 0)
		{
			List.Add(TEXT("Renderers"), TEXT("Fail"), FString::Printf(TEXT("All %d renderer(s) are disabled."), Renderers.Num()),
				FString::Printf(TEXT("SetRendererProperty(System, %s, 0, \"bIsEnabled\", \"true\")"), *Quote(Name)));
		}
		else
		{
			TArray<FString> Classes;
			for (int32 Index : Enabled)
			{
				Classes.Add(FString::Printf(TEXT("%d:%s"), Index, *Renderers[Index]->GetClass()->GetName().Replace(TEXT("Niagara"), TEXT("")).Replace(TEXT("RendererProperties"), TEXT(""))));
			}
			List.Add(TEXT("Renderers"), TEXT("Ok"), FString::Printf(TEXT("enabled: %s"), *FString::Join(Classes, TEXT(", "))));
		}

		// 7. Renderer feedback + sim target support
		if (EmitterData && Enabled.Num() > 0)
		{
			TArray<FString> Errors;
			TArray<FString> Warnings;
			for (int32 Index : Enabled)
			{
				TArray<FNiagaraRendererFeedback> ErrorFeedback, WarningFeedback, InfoFeedback;
				Renderers[Index]->GetRendererFeedback(Handle.GetInstance(), ErrorFeedback, WarningFeedback, InfoFeedback);
				auto Text = [](const FNiagaraRendererFeedback& Feedback)
				{
					const FString Description = Feedback.GetDescriptionText().ToString();
					return Description.IsEmpty() ? Feedback.GetSummaryText().ToString() : Description;
				};
				for (const FNiagaraRendererFeedback& Feedback : ErrorFeedback)
				{
					Errors.Add(FString::Printf(TEXT("renderer %d: %s"), Index, *Text(Feedback)));
				}
				for (const FNiagaraRendererFeedback& Feedback : WarningFeedback)
				{
					Warnings.Add(FString::Printf(TEXT("renderer %d: %s"), Index, *Text(Feedback)));
				}
			}
			if (Errors.Num() > 0)
			{
				List.Add(TEXT("RendererFeedback"), TEXT("Fail"), FString::Join(Errors, TEXT(" | ")), TEXT("GetRendererProperties(System, Emitter, RendererIndex) to inspect; fix the reported setting."));
			}
			else if (Warnings.Num() > 0)
			{
				List.Add(TEXT("RendererFeedback"), TEXT("Warning"), FString::Join(Warnings, TEXT(" | ")), TEXT("See the material checks below; GetRendererProperties(System, Emitter, RendererIndex) to inspect."));
			}
			else
			{
				List.Add(TEXT("RendererFeedback"), TEXT("Ok"), TEXT("no renderer errors or warnings"));
			}

			TArray<FString> Unsupported;
			for (int32 Index : Enabled)
			{
				if (!Renderers[Index]->IsSimTargetSupported(EmitterData->SimTarget))
				{
					Unsupported.Add(FString::Printf(TEXT("%d:%s"), Index, *Renderers[Index]->GetClass()->GetName()));
				}
			}
			const FString Target = StaticEnum<ENiagaraSimTarget>()->GetNameStringByValue(static_cast<int64>(EmitterData->SimTarget));
			if (Unsupported.Num() > 0)
			{
				List.Add(TEXT("SimTarget"), TEXT("Fail"), FString::Printf(TEXT("Renderer(s) %s do not support %s."), *FString::Join(Unsupported, TEXT(", ")), *Target),
					FString::Printf(TEXT("SetEmitterProperty(System, %s, \"SimTarget\", \"%s\")"), *Quote(Name), EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim ? TEXT("CPUSim") : TEXT("GPUComputeSim")));
			}
			else
			{
				List.Add(TEXT("SimTarget"), TEXT("Ok"), FString::Printf(TEXT("%s supported by every enabled renderer"), *Target));
			}
		}

		// 8. Materials (state captured before the simulation)
		bool bAdditive = false;
		for (const FMaterialState& State : Materials)
		{
			const FString Owner = FString::Printf(TEXT("renderer %d (%s)"), State.RendererIndex, *State.RendererClass);
			if (State.bMissing)
			{
				List.Add(TEXT("Material"), TEXT("Fail"), FString::Printf(TEXT("%s has no material."), *Owner),
					FString::Printf(TEXT("SetRendererProperty(System, %s, %d, \"Material\", \"/Niagara/DefaultAssets/DefaultSpriteMaterial.DefaultSpriteMaterial\")"), *Quote(Name), State.RendererIndex));
				continue;
			}
			if (State.CompileErrors.Num() > 0)
			{
				List.Add(TEXT("Material"), TEXT("Fail"), FString::Printf(TEXT("%s material %s has compile errors: %s"), *Owner, *State.Path, *FString::Join(State.CompileErrors, TEXT(" | ")).Left(400)),
					FString::Printf(TEXT("CompileMaterial(%s) and fix the errors, or SetRendererProperty(System, %s, %d, \"Material\", <another material>)"), *Quote(State.Path), *Quote(Name), State.RendererIndex));
				continue;
			}
			if (State.Domain != TEXT("MD_Surface"))
			{
				List.Add(TEXT("Material"), TEXT("Fail"), FString::Printf(TEXT("%s material %s has domain %s; particle renderers need Surface."), *Owner, *State.Path, *State.Domain),
					FString::Printf(TEXT("SetMaterialProperty(%s, \"MaterialDomain\", \"MD_Surface\")"), *Quote(State.Path)));
				continue;
			}
			List.Add(TEXT("Material"), TEXT("Ok"), FString::Printf(TEXT("%s: %s (%s)"), *Owner, *State.Path, *State.BlendMode));
			if (!State.bHasUsage)
			{
				const FString Fix = FString::Printf(TEXT("SetMaterialProperty(%s, %s, \"true\")"), *Quote(State.Path), *Quote(State.UsageProperty));
				if (State.bAutoUsage)
				{
					List.Add(TEXT("MaterialUsage"), TEXT("Warning"), FString::Printf(TEXT("%s material %s lacks the %s usage flag; the editor sets it on first use (and dirties the material), cooked builds fall back to the default material."), *Owner, *State.Path, *State.UsageName), Fix);
				}
				else
				{
					List.Add(TEXT("MaterialUsage"), TEXT("Fail"), FString::Printf(TEXT("%s material %s lacks the %s usage flag (%s = false, automatic usage off): the renderer uses the default material / draws nothing correct."), *Owner, *State.Path, *State.UsageName, *State.UsageProperty), Fix);
				}
			}
			else
			{
				List.Add(TEXT("MaterialUsage"), TEXT("Ok"), FString::Printf(TEXT("%s set"), *State.UsageProperty));
			}
			bAdditive |= State.bAdditive;
		}

		// 9. Attribute values
		if (Particles > 0)
		{
			if (const FNiagaraMcpAttributeStats* Color = FindStats(Sim, TEXT("Color")))
			{
				const TArray<double> Max = ParseNumbers(Color->Max);
				const TArray<double> Mean = ParseNumbers(Color->Mean);
				if (Max.Num() >= 4 && Mean.Num() >= 4)
				{
					const FString ColorFix = InitModule ? FString::Printf(TEXT("SetModuleInput(System, %s, \"Initialize Particle\", \"Color\", \"1, 1, 1, 1\") (and check Scale Color / Color modules that write Particles.Color)"), *Quote(Name))
						: TEXT("Check the modules that write Particles.Color (GetStack shows them).");
					if (Max[3] <= 0.001)
					{
						List.Add(TEXT("Alpha"), TEXT("Fail"), FString::Printf(TEXT("Color.a is 0 for every particle (mean %s, max %s)."), *Color->Mean, *Color->Max), ColorFix);
					}
					else if (Max[3] < 0.05)
					{
						List.Add(TEXT("Alpha"), TEXT("Warning"), FString::Printf(TEXT("Color.a at most %.3f: nearly transparent."), Max[3]), ColorFix);
					}
					else if (bAdditive && FMath::Max3(Max[0], Max[1], Max[2]) <= 0.001)
					{
						List.Add(TEXT("Alpha"), TEXT("Fail"), FString::Printf(TEXT("Color RGB is black (max %s) on an additive material: adds nothing."), *Color->Max), ColorFix);
					}
					else
					{
						List.Add(TEXT("Alpha"), TEXT("Ok"), FString::Printf(TEXT("Color mean %s"), *Color->Mean));
					}
				}
			}
			bool bSizeChecked = false;
			for (int32 Index : Enabled)
			{
				const UNiagaraRendererProperties* Renderer = Renderers[Index];
				const TCHAR* Attribute = Renderer->IsA<UNiagaraSpriteRendererProperties>() ? TEXT("SpriteSize") : Renderer->IsA<UNiagaraMeshRendererProperties>() ? TEXT("Scale") : Renderer->IsA<UNiagaraRibbonRendererProperties>() ? TEXT("RibbonWidth") : nullptr;
				const FNiagaraMcpAttributeStats* Size = Attribute ? FindStats(Sim, Attribute) : nullptr;
				if (Size == nullptr || bSizeChecked)
				{
					continue;
				}
				bSizeChecked = true;
				const TArray<double> Max = ParseNumbers(Size->Max);
				double Largest = 0.0;
				for (double V : Max)
				{
					Largest = FMath::Max(Largest, FMath::Abs(V));
				}
				const FString Input = FCString::Strcmp(Attribute, TEXT("SpriteSize")) == 0 ? TEXT("Uniform Sprite Size") : FCString::Strcmp(Attribute, TEXT("Scale")) == 0 ? TEXT("Uniform Mesh Scale") : TEXT("Ribbon Width");
				if (Largest <= 0.01)
				{
					List.Add(TEXT("Size"), TEXT("Fail"), FString::Printf(TEXT("Particles.%s max %s: particles have no size."), Attribute, *Size->Max),
						FString::Printf(TEXT("SetModuleInput(System, %s, \"Initialize Particle\", %s, \"%s\") (and check Scale ... Size modules)"), *Quote(Name), *Quote(Input), FCString::Strcmp(Attribute, TEXT("Scale")) == 0 ? TEXT("1") : TEXT("10")));
				}
				else
				{
					List.Add(TEXT("Size"), TEXT("Ok"), FString::Printf(TEXT("%s max %s"), Attribute, *Size->Max));
				}
			}

			// 10. Bounds (GPU emitters need fixed bounds; particles outside them are culled with the bounds)
			const FNiagaraMcpAttributeStats* Position = FindStats(Sim, TEXT("Position"));
			if (Position && EmitterData && EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim)
			{
				const TArray<double> Min = ParseNumbers(Position->Min);
				const TArray<double> Max = ParseNumbers(Position->Max);
				FBox Fixed(ForceInit);
				FString Source;
				if (EmitterData->CalculateBoundsMode == ENiagaraEmitterCalculateBoundMode::Fixed)
				{
					Fixed = EmitterData->FixedBounds;
					Source = TEXT("emitter FixedBounds");
				}
				else if (GetPropertyText(System.GetClass(), &System, TEXT("bFixedBounds")) == TEXT("true"))
				{
					Fixed = System.GetFixedBounds();
					Source = TEXT("system FixedBounds");
				}
				if (Min.Num() >= 3 && Max.Num() >= 3)
				{
					const FBox Extent(FVector(Min[0], Min[1], Min[2]), FVector(Max[0], Max[1], Max[2]));
					const FString Suggested = BoxText(Extent.ExpandBy(FMath::Max(10.0, Extent.GetSize().GetMax() * 0.1)));
					if (!Fixed.IsValid || Fixed.GetVolume() <= 0.0)
					{
						List.Add(TEXT("Bounds"), TEXT("Warning"), FString::Printf(TEXT("GPU emitter without fixed bounds (CalculateBoundsMode %s): bounds come from a CPU estimate and the effect may be culled. Particle extent %s."),
							*StaticEnum<ENiagaraEmitterCalculateBoundMode>()->GetNameStringByValue(static_cast<int64>(EmitterData->CalculateBoundsMode)), *BoxText(Extent)),
							FString::Printf(TEXT("SetEmitterProperty(System, %s, \"CalculateBoundsMode\", \"Fixed\") then SetEmitterProperty(System, %s, \"FixedBounds\", \"%s\")"), *Quote(Name), *Quote(Name), *Suggested));
					}
					else if (!Fixed.IsInside(Extent.Min) || !Fixed.IsInside(Extent.Max))
					{
						const bool bNoOverlap = !Fixed.Intersect(Extent);
						List.Add(TEXT("Bounds"), bNoOverlap ? TEXT("Fail") : TEXT("Warning"), FString::Printf(TEXT("Particles span %s but the %s are %s%s."), *BoxText(Extent), *Source, *BoxText(Fixed),
							bNoOverlap ? TEXT(" (no overlap: culled whenever the bounds are off screen)") : TEXT(" (partly outside)")),
							FString::Printf(TEXT("SetEmitterProperty(System, %s, \"FixedBounds\", \"%s\")"), *Quote(Name), *Suggested));
					}
					else
					{
						List.Add(TEXT("Bounds"), TEXT("Ok"), FString::Printf(TEXT("particles %s inside the %s"), *BoxText(Extent), *Source));
					}
				}
			}

			// 11. Space
			if (Position && EmitterData)
			{
				const TArray<double> Mean = ParseNumbers(Position->Mean);
				const double Distance = Mean.Num() >= 3 ? FVector(Mean[0], Mean[1], Mean[2]).Size() : 0.0;
				if (Distance > 100000.0)
				{
					List.Add(TEXT("Space"), TEXT("Warning"), FString::Printf(TEXT("%s space; particles average %.0f units from the component (far from the camera framing the system)."), EmitterData->bLocalSpace ? TEXT("Local") : TEXT("World"), Distance),
						TEXT("Check location modules / velocity (GetParticleAttributes Position over time)."));
				}
				else
				{
					List.Add(TEXT("Space"), TEXT("Ok"), FString::Printf(TEXT("%s space, mean position %s"), EmitterData->bLocalSpace ? TEXT("local") : TEXT("world"), *Position->Mean));
				}
			}
		}
	}
}

// ------------------------------------------------------------------------------------------------ DiagnoseVisibility

FNiagaraMcpVisibilityReport UNiagaraMcpEditorTools::DiagnoseVisibility(UNiagaraSystem* System, FName EmitterName, float Time)
{
	using namespace NiagaraMcpAnalysisPrivate;
	FNiagaraMcpVisibilityReport Report;
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return Report;
	}
	const bool bAll = Stack::IsSystemName(EmitterName);
	if (!bAll && Stack::FindEmitter(*System, EmitterName) == nullptr)
	{
		return Report;
	}
	Report.System = System->GetPathName();
	Report.Time = FMath::Clamp(Time, 0.0f, 60.0f);

	FChecklist SystemList{ Report, NAME_None };
	System->RequestCompile(false);
	FString WaitError;
	Preview::WaitForSystemCompile(*System, Preview::DefaultMaxWaitSeconds, WaitError);
	const FNiagaraMcpCompileResult Compile = NiagaraMcpTools::ReadCompileResult(*System);
	if (!Compile.bSucceeded || !System->IsValid())
	{
		TArray<FString> Errors;
		for (const FNiagaraMcpCompileMessage& Message : Compile.Messages)
		{
			if (Message.Severity == TEXT("Error"))
			{
				Errors.Add(FString::Printf(TEXT("%s: %s"), *Message.Script, *Message.Message));
			}
		}
		SystemList.Add(TEXT("Compile"), TEXT("Fail"), Errors.Num() > 0 ? FString::Join(Errors, TEXT(" | ")).Left(800) : FString(TEXT("The system is not valid (scripts not compiled).")),
			TEXT("Fix the module named in the error (GetStack / SetModuleInput / RemoveModule), then CompileSystem(System). ValidateSystem lists stack issues with fixes."));
		Report.bVisible = false;
		return Report;
	}
	SystemList.Add(TEXT("Compile"), TEXT("Ok"), TEXT("compiled without errors"));

	if (!System->IsAllowedByScalability())
	{
		SystemList.Add(TEXT("Scalability"), TEXT("Fail"), FString::Printf(TEXT("The system's Platforms exclude the current effects quality %s."), *FNiagaraPlatformSet::GetQualityLevelText(FNiagaraPlatformSet::GetQualityLevel()).ToString()),
			TEXT("SetSystemProperty(System, \"Platforms.QualityLevelMask\", \"-1\")"));
		return Report;
	}
	if (const UNiagaraEffectType* EffectType = System->GetEffectType())
	{
		const FNiagaraSystemScalabilitySettings& Settings = EffectType->GetActiveSystemScalabilitySettings();
		TArray<FString> Culling;
		if (Settings.bCullByDistance)
		{
			Culling.Add(FString::Printf(TEXT("distance > %.0f"), Settings.MaxDistance));
		}
		if (Settings.bCullMaxInstanceCount)
		{
			Culling.Add(FString::Printf(TEXT("more than %d instances of the effect type"), Settings.MaxInstances));
		}
		if (Settings.bCullPerSystemMaxInstanceCount)
		{
			Culling.Add(FString::Printf(TEXT("more than %d instances of this system"), Settings.MaxSystemInstances));
		}
		if (Settings.VisibilityCulling.bCullWhenNotRendered)
		{
			Culling.Add(FString::Printf(TEXT("not rendered for %.1f s"), Settings.VisibilityCulling.MaxTimeWithoutRender));
		}
		if (Settings.VisibilityCulling.bCullByViewFrustum)
		{
			Culling.Add(FString::Printf(TEXT("outside the view frustum for %.1f s"), Settings.VisibilityCulling.MaxTimeOutsideViewFrustum));
		}
		const bool bNeverRuns = (Settings.bCullMaxInstanceCount && Settings.MaxInstances <= 0) || (Settings.bCullPerSystemMaxInstanceCount && Settings.MaxSystemInstances <= 0)
			|| (Settings.bCullByDistance && Settings.MaxDistance <= 0.0f);
		const FString Evidence = FString::Printf(TEXT("effect type %s culls when: %s"), *EffectType->GetPathName(), Culling.Num() ? *FString::Join(Culling, TEXT("; ")) : TEXT("never"));
		if (bNeverRuns)
		{
			SystemList.Add(TEXT("EffectType"), TEXT("Fail"), Evidence + TEXT(" -> every instance is culled."), TEXT("SetSystemProperty(System, \"EffectType\", \"None\") or raise the limits in the effect type asset."));
			return Report;
		}
		SystemList.Add(TEXT("EffectType"), Culling.Num() > 0 ? TEXT("Warning") : TEXT("Ok"), Evidence,
			Culling.Num() > 0 ? TEXT("In a level the effect can be culled by these rules; test closer / with fewer instances, or SetSystemProperty(System, \"EffectType\", \"None\").") : FString());
	}

	// Gather everything that the simulation could change first (materials: the editor auto-sets missing usage flags on use).
	TArray<const FNiagaraEmitterHandle*> Handles;
	TMap<FGuid, TArray<FMaterialState>> MaterialStates;
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		if (bAll || Handle.GetName().ToString().Equals(EmitterName.ToString(), ESearchCase::IgnoreCase))
		{
			Handles.Add(&Handle);
			CaptureMaterials(GetRenderers(Handle), MaterialStates.FindOrAdd(Handle.GetId()));
		}
	}

	// One off-screen run to Time (GPU / lightweight emitters are read back through a sim cache).
	TArray<FNiagaraMcpAttributeSample> Samples;
	{
		FErrorCapture Capture;
		Samples = UNiagaraMcpEditorTools::GetParticleAttributes(System, { Report.Time }, { TEXT("Color"), TEXT("SpriteSize"), TEXT("Scale"), TEXT("RibbonWidth"), TEXT("Position"), TEXT("Lifetime") },
			bAll ? NAME_None : EmitterName, 0);
	}

	TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(*System);
	for (const FNiagaraEmitterHandle* Handle : Handles)
	{
		const FNiagaraMcpEmitterAttributes* Sim = nullptr;
		if (Samples.Num() > 0)
		{
			Sim = Samples[0].Emitters.FindByPredicate([Handle](const FNiagaraMcpEmitterAttributes& E) { return E.Emitter == Handle->GetName(); });
		}
		DiagnoseEmitter(*System, *Handle, ViewModel.Get(), MaterialStates.FindRef(Handle->GetId()), Sim, Report.Time, Report);
	}
	Report.bVisible = Report.FirstFail.IsEmpty() && Handles.Num() > 0;
	return Report;
}

// ------------------------------------------------------------------------------------------------ AnalyzeCost

namespace NiagaraMcpAnalysisPrivate
{
	FNiagaraMcpScriptCost DescribeScriptCost(const UNiagaraScript& Script, bool bGpu)
	{
		FNiagaraMcpScriptCost Cost;
		Cost.Script = ShortUsage(Script.GetUsage());
		Cost.Target = bGpu ? TEXT("GPU") : TEXT("CPU");
		const FNiagaraVMExecutableData& Data = Script.GetVMExecutableData();
		Cost.CompileStatus = CompileStatusText(Data.LastCompileStatus);
		if (!bGpu)
		{
			Cost.OpCount = static_cast<int32>(Data.LastOpCount);
			Cost.TempRegisters = Data.NumTempRegisters;
			Cost.ByteCodeBytes = Data.ByteCode.GetLength();
			Cost.HlslLength = Data.LastHlslTranslation.Len();
		}
		else
		{
			Cost.HlslLength = Data.LastHlslTranslationGPU.Len();
		}
		for (const FNiagaraCompileEvent& Event : Data.LastCompileEvents)
		{
			Cost.CompileErrors += Event.Severity == FNiagaraCompileEventSeverity::Error ? 1 : 0;
			Cost.CompileWarnings += Event.Severity == FNiagaraCompileEventSeverity::Warning ? 1 : 0;
		}
		return Cost;
	}
}

FNiagaraMcpCostReport UNiagaraMcpEditorTools::AnalyzeCost(UNiagaraSystem* System, const TArray<float>& InTimes, FName EmitterName)
{
	using namespace NiagaraMcpAnalysisPrivate;
	FNiagaraMcpCostReport Report;
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return Report;
	}
	const bool bAll = Stack::IsSystemName(EmitterName);
	if (!bAll && Stack::FindEmitter(*System, EmitterName) == nullptr)
	{
		return Report;
	}
	TArray<float> Times = InTimes;
	if (Times.Num() == 0)
	{
		Times = { 0.5f, 1.0f, 2.0f };
	}
	if (Times.Num() > 32)
	{
		ReportError(TEXT("At most 32 times per call."));
		return {};
	}
	Report.System = System->GetPathName();
	Report.Times = Times;

	System->RequestCompile(false);
	FString WaitError;
	Preview::WaitForSystemCompile(*System, Preview::DefaultMaxWaitSeconds, WaitError);
	if (!System->IsValid())
	{
		ReportError(FString::Printf(TEXT("'%s' is not valid (compile errors?). Run CompileSystem or DiagnoseVisibility for details."), *System->GetPathName()));
		return {};
	}
	if (System->HasAnyGPUEmitters() && !Preview::WaitForShaderJobs(Preview::DefaultMaxWaitSeconds, WaitError))
	{
		ReportError(WaitError);
		return {};
	}

	// Static script costs.
	for (UNiagaraScript* Script : { System->GetSystemSpawnScript(), System->GetSystemUpdateScript() })
	{
		if (Script)
		{
			Report.SystemScripts.Add(DescribeScriptCost(*Script, false));
		}
	}
	bool bZeroOps = false;
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		if (!bAll && !Handle.GetName().ToString().Equals(EmitterName.ToString(), ESearchCase::IgnoreCase))
		{
			continue;
		}
		FNiagaraMcpEmitterCost& Cost = Report.Emitters.AddDefaulted_GetRef();
		Cost.Emitter = Handle.GetName();
		Cost.bEnabled = Handle.GetIsEnabled();
		Cost.ParticleCounts.Init(0, Times.Num());
		if (FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData())
		{
			const bool bGpu = EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim;
			Cost.SimTarget = StaticEnum<ENiagaraSimTarget>()->GetNameStringByValue(static_cast<int64>(EmitterData->SimTarget));
			TArray<UNiagaraScript*> Scripts;
			EmitterData->GetScripts(Scripts, true, false);
			for (UNiagaraScript* Script : Scripts)
			{
				if (Script == nullptr)
				{
					continue;
				}
				const bool bGpuScript = Script->GetUsage() == ENiagaraScriptUsage::ParticleGPUComputeScript;
				if (bGpu != bGpuScript && (bGpuScript || Script->IsParticleScript()))
				{
					continue; // CPU particle scripts of a GPU emitter (and vice versa) are not executed.
				}
				FNiagaraMcpScriptCost ScriptCost = DescribeScriptCost(*Script, bGpuScript);
				if (!bGpuScript && Script->IsParticleScript())
				{
					Cost.ParticleOpCount += ScriptCost.OpCount;
					bZeroOps |= ScriptCost.OpCount == 0 && ScriptCost.ByteCodeBytes > 0;
				}
				Cost.Scripts.Add(MoveTemp(ScriptCost));
			}
		}
		else
		{
			Cost.SimTarget = TEXT("Lightweight");
		}
	}
	if (bZeroOps)
	{
		Report.Notes.Add(TEXT("Some OpCount values are 0: the VM op count is only known after a compile in this editor session (scripts loaded from the derived data cache have none). CompileSystem refreshes them; ByteCodeBytes is always available."));
	}

	// One continuous run through the times, timing each step.
	FPreviewScene Scene(PreviewValues());
	TStrongObjectPtr<UNiagaraComponent> Component(NewObject<UNiagaraComponent>(GetTransientPackage()));
	Component->SetAutoActivate(false);
	Component->SetAsset(System);
	Component->SetForceSolo(true);
	Scene.AddComponent(Component.Get(), FTransform::Identity);

	bool bAnyGpu = false;
	for (const FNiagaraMcpEmitterCost& Cost : Report.Emitters)
	{
		bAnyGpu |= Cost.SimTarget == TEXT("GPUComputeSim");
	}

#if WITH_NIAGARA_GPU_PROFILER
	struct FGpuAccumulator
	{
		TMap<FName, TMap<FName, TPair<int32, uint64>>> PerEmitter; // emitter -> stage -> (dispatches, microseconds)
		int32 Frames = 0;
	};
	TSharedRef<FGpuAccumulator> Gpu = MakeShared<FGpuAccumulator>();
	TUniquePtr<FNiagaraGpuProfilerListener> Listener;
	if (bAnyGpu)
	{
		Listener = MakeUnique<FNiagaraGpuProfilerListener>();
		TWeakObjectPtr<UNiagaraComponent> WeakComponent(Component.Get());
		Listener->SetHandler([Gpu, WeakComponent, System](const FNiagaraGpuFrameResultsPtr& Results)
		{
			if (!Results.IsValid())
			{
				return;
			}
			++Gpu->Frames;
			for (const FNiagaraGpuFrameResults::FDispatchResults& Dispatch : Results->DispatchResults)
			{
				if (Dispatch.OwnerComponent.Get() != WeakComponent.Get())
				{
					continue;
				}
				FName Emitter = NAME_None;
				if (const UNiagaraEmitter* Owner = Dispatch.OwnerEmitter.Emitter.Get())
				{
					for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
					{
						if (Handle.GetInstance().Emitter == Owner)
						{
							Emitter = Handle.GetName();
						}
					}
				}
				TPair<int32, uint64>& Entry = Gpu->PerEmitter.FindOrAdd(Emitter).FindOrAdd(Dispatch.StageName);
				Entry.Key += 1;
				Entry.Value += Dispatch.DurationMicroseconds;
			}
		});
		Listener->SetEnabled(true);
	}
#endif

	double StepStart = FPlatformTime::Seconds();
	float Previous = 0.0f;
	TArray<FNiagaraMcpSimStepCost> Steps;
	Steps.SetNum(Times.Num());
	Preview::SimulateInOrder(*Component, Times, 1.0f / 60.0f, [&](int32 Index, float Seconds)
	{
		const double Now = FPlatformTime::Seconds();
		FNiagaraMcpSimStepCost& Step = Steps[Index];
		Step.Time = Seconds;
		Step.SimulatedSeconds = FMath::Max(0.0f, Seconds - Previous);
		Step.Milliseconds = (Now - StepStart) * 1000.0;
		const double Ticks = FMath::Max(1.0, FMath::CeilToDouble(Step.SimulatedSeconds * 60.0 - KINDA_SMALL_NUMBER));
		Step.MillisecondsPerTick = Step.Milliseconds / Ticks;
		Previous = Seconds;

		FNiagaraSystemInstanceControllerPtr Controller = Component->GetSystemInstanceController();
		if (FNiagaraSystemInstance* Instance = Controller.IsValid() ? Controller->GetSystemInstance_Unsafe() : nullptr)
		{
			for (const FNiagaraEmitterInstanceRef& Emitter : Instance->GetEmitters())
			{
				const FName Name = Emitter->GetEmitterHandle().GetName();
				if (FNiagaraMcpEmitterCost* Cost = Report.Emitters.FindByPredicate([Name](const FNiagaraMcpEmitterCost& C) { return C.Emitter == Name; }))
				{
					Cost->ParticleCounts[Index] = Emitter->GetNumParticles();
					Cost->PeakParticles = FMath::Max(Cost->PeakParticles, Cost->ParticleCounts[Index]);
				}
			}
		}
		StepStart = FPlatformTime::Seconds();
	});
	// Steps in time order (the run visits them sorted).
	Steps.StableSort([](const FNiagaraMcpSimStepCost& A, const FNiagaraMcpSimStepCost& B) { return A.Time < B.Time; });
	Report.Steps = MoveTemp(Steps);

#if WITH_NIAGARA_GPU_PROFILER
	if (Listener.IsValid())
	{
		// GPU timings arrive asynchronously (render queries read back a frame or more later, then posted to the game
		// thread). Run a few extra flushed ticks and pump the game thread task queue to collect them.
		UWorld* World = Component->GetWorld();
		FNiagaraGpuComputeDispatchInterface* Dispatch = World ? FNiagaraGpuComputeDispatchInterface::Get(World) : nullptr;
		for (int32 Pump = 0; Pump < 8 && Gpu->PerEmitter.Num() == 0; ++Pump)
		{
			Component->AdvanceSimulationByTime(1.0f / 60.0f, 1.0f / 60.0f);
			if (World)
			{
				World->SendAllEndOfFrameUpdates();
			}
			if (Dispatch)
			{
				Dispatch->FlushPendingTicks_GameThread();
			}
			FlushRenderingCommands();
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		}
		Listener->SetEnabled(false);
		for (FNiagaraMcpEmitterCost& Cost : Report.Emitters)
		{
			if (const TMap<FName, TPair<int32, uint64>>* Stages = Gpu->PerEmitter.Find(Cost.Emitter))
			{
				for (const TPair<FName, TPair<int32, uint64>>& Stage : *Stages)
				{
					FNiagaraMcpGpuStageTiming& Timing = Cost.GpuStages.AddDefaulted_GetRef();
					Timing.Stage = Stage.Key.ToString();
					Timing.Dispatches = Stage.Value.Key;
					Timing.AverageMicroseconds = Stage.Value.Key > 0 ? static_cast<double>(Stage.Value.Value) / Stage.Value.Key : 0.0;
				}
			}
		}
		if (Gpu->PerEmitter.Num() == 0)
		{
			Report.Notes.Add(TEXT("GPU stage timings: the Niagara GPU profiler delivered no results for this off-screen run (results are read back across rendered frames). Use stat GPU / Niagara Insights in a running viewport for GPU timings."));
		}
	}
#else
	if (bAnyGpu)
	{
		Report.Notes.Add(TEXT("GPU stage timings are not available in this build (WITH_NIAGARA_GPU_PROFILER is off)."));
	}
#endif
	Component->DeactivateImmediate();

	// GPU / lightweight particle counts are not known on the CPU during the timed run: read them back (second run, sim cache).
	const bool bAnyGpuOrLightweight = Report.Emitters.ContainsByPredicate([](const FNiagaraMcpEmitterCost& C) { return C.SimTarget != TEXT("CPUSim"); });
	if (bAnyGpuOrLightweight)
	{
		TArray<FNiagaraMcpAttributeSample> Samples;
		{
			FErrorCapture Capture;
			Samples = UNiagaraMcpEditorTools::GetParticleAttributes(System, Times, { TEXT("Age") }, bAll ? NAME_None : EmitterName, 0);
		}
		for (int32 Index = 0; Index < Samples.Num() && Index < Times.Num(); ++Index)
		{
			for (const FNiagaraMcpEmitterAttributes& Emitter : Samples[Index].Emitters)
			{
				FNiagaraMcpEmitterCost* Cost = Report.Emitters.FindByPredicate([&Emitter](const FNiagaraMcpEmitterCost& C) { return C.Emitter == Emitter.Emitter; });
				if (Cost && Cost->SimTarget != TEXT("CPUSim"))
				{
					Cost->ParticleCounts[Index] = Emitter.Particles;
					Cost->PeakParticles = FMath::Max(Cost->PeakParticles, Emitter.Particles);
				}
			}
		}
		Report.Notes.Add(TEXT("GPU / lightweight particle counts were read back from the GPU in a second run (sim cache); the step timings cover the first run only."));
	}

	// Ranking by estimated CPU cost.
	TArray<const FNiagaraMcpEmitterCost*> Sorted;
	for (const FNiagaraMcpEmitterCost& Cost : Report.Emitters)
	{
		Sorted.Add(&Cost);
	}
	auto Score = [](const FNiagaraMcpEmitterCost& Cost) { return Cost.SimTarget == TEXT("CPUSim") ? static_cast<double>(Cost.ParticleOpCount) * FMath::Max(1, Cost.PeakParticles) : 0.0; };
	Sorted.StableSort([&Score](const FNiagaraMcpEmitterCost& A, const FNiagaraMcpEmitterCost& B) { return Score(A) > Score(B); });
	for (const FNiagaraMcpEmitterCost* Cost : Sorted)
	{
		if (Cost->SimTarget == TEXT("CPUSim"))
		{
			Report.Ranking.Add(FString::Printf(TEXT("%s: CPU, %d ops/particle x %d peak particles = %.0f"), *Cost->Emitter.ToString(), Cost->ParticleOpCount, Cost->PeakParticles, Score(*Cost)));
		}
		else
		{
			Report.Ranking.Add(FString::Printf(TEXT("%s: %s, %d peak particles (GPU / lightweight cost is not in the CPU estimate)"), *Cost->Emitter.ToString(), *Cost->SimTarget, Cost->PeakParticles));
		}
	}
	return Report;
}

// ------------------------------------------------------------------------------------------------ GetCompiledHlsl

FNiagaraMcpHlslResult UNiagaraMcpEditorTools::GetCompiledHlsl(UNiagaraSystem* System, FName EmitterName, const FString& Script, const FString& Target, int32 Offset, int32 MaxLength)
{
	using namespace NiagaraMcpAnalysisPrivate;
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	const FString TargetText = IsUnset(Target) ? FString(TEXT("CPU")) : Target.TrimStartAndEnd();
	const bool bGpu = TargetText.Equals(TEXT("GPU"), ESearchCase::IgnoreCase);
	if (!bGpu && !TargetText.Equals(TEXT("CPU"), ESearchCase::IgnoreCase))
	{
		ReportError(FString::Printf(TEXT("Unknown Target '%s'. Use CPU or GPU."), *Target));
		return {};
	}
	const FString ScriptText = IsUnset(Script) ? FString(TEXT("ParticleUpdate")) : Script.TrimStartAndEnd();
	ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
	const bool bGpuCompute = ScriptText.StartsWith(TEXT("ParticleGPUCompute"), ESearchCase::IgnoreCase) || ScriptText.Equals(TEXT("GPU"), ESearchCase::IgnoreCase);
	if (bGpuCompute)
	{
		Usage = ENiagaraScriptUsage::ParticleGPUComputeScript;
	}
	else if (!ParseUsage(ScriptText, Usage))
	{
		ReportError(FString::Printf(TEXT("Unknown Script '%s'. Use ParticleSpawn, ParticleUpdate, EmitterSpawn, EmitterUpdate, SystemSpawn, SystemUpdate or ParticleGPUCompute."), *Script));
		return {};
	}

	FNiagaraMcpHlslResult Result;
	Result.Emitter = Stack::IsSystemName(EmitterName) ? NAME_None : EmitterName;
	Result.Target = bGpu || bGpuCompute ? TEXT("GPU") : TEXT("CPU");

	auto ResolveScript = [&]() -> UNiagaraScript*
	{
		if (Stack::IsSystemName(EmitterName) || Usage == ENiagaraScriptUsage::SystemSpawnScript || Usage == ENiagaraScriptUsage::SystemUpdateScript)
		{
			// Emitter spawn / update stacks are compiled into the system scripts.
			if (Usage == ENiagaraScriptUsage::SystemSpawnScript || Usage == ENiagaraScriptUsage::EmitterSpawnScript)
			{
				return System->GetSystemSpawnScript();
			}
			if (Usage == ENiagaraScriptUsage::SystemUpdateScript || Usage == ENiagaraScriptUsage::EmitterUpdateScript)
			{
				return System->GetSystemUpdateScript();
			}
			ReportError(TEXT("Particle scripts need an EmitterName."));
			return nullptr;
		}
		FNiagaraEmitterHandle* Handle = Stack::FindEmitter(*System, EmitterName);
		if (Handle == nullptr)
		{
			return nullptr;
		}
		FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
		if (EmitterData == nullptr)
		{
			ReportError(FString::Printf(TEXT("Emitter '%s' is a lightweight emitter: it has no compiled scripts."), *EmitterName.ToString()));
			return nullptr;
		}
		const bool bGpuEmitter = EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim;
		if ((bGpu || bGpuCompute) && !bGpuEmitter)
		{
			ReportError(FString::Printf(TEXT("Emitter '%s' simulates on the CPU: use Target CPU (or SetEmitterProperty SimTarget GPUComputeSim first)."), *EmitterName.ToString()));
			return nullptr;
		}
		if (bGpuEmitter && Usage != ENiagaraScriptUsage::EmitterSpawnScript && Usage != ENiagaraScriptUsage::EmitterUpdateScript)
		{
			// GPU emitters run one combined compute shader for spawn, update and simulation stages.
			Usage = ENiagaraScriptUsage::ParticleGPUComputeScript;
			Result.Target = TEXT("GPU");
			return EmitterData->GetGPUComputeScript();
		}
		if (Usage == ENiagaraScriptUsage::EmitterSpawnScript)
		{
			return System->GetSystemSpawnScript();
		}
		if (Usage == ENiagaraScriptUsage::EmitterUpdateScript)
		{
			return System->GetSystemUpdateScript();
		}
		if (Usage == ENiagaraScriptUsage::ParticleSpawnScript && EmitterData->UsesInterpolatedSpawning())
		{
			Usage = ENiagaraScriptUsage::ParticleSpawnScriptInterpolated;
		}
		return EmitterData->GetScript(Usage, FGuid());
	};

	UNiagaraScript* Compiled = ResolveScript();
	if (Compiled == nullptr)
	{
		return {};
	}
	auto ReadText = [&]() -> FString
	{
		const FNiagaraVMExecutableData& Data = Compiled->GetVMExecutableData();
		return Result.Target == TEXT("GPU") ? Data.LastHlslTranslationGPU : Data.LastHlslTranslation;
	};
	System->RequestCompile(false);
	System->WaitForCompilationComplete(false, false);
	FString Text = ReadText();
	if (Text.IsEmpty())
	{
		// CPU HLSL is transient (not stored in the derived data cache): a forced compile regenerates it. Forcing
		// touches the graph (Modify), so restore the package's clean state for this read tool.
		const bool bWasDirty = System->GetOutermost()->IsDirty();
		System->RequestCompile(true);
		System->WaitForCompilationComplete(false, false);
		if (!bWasDirty)
		{
			System->GetOutermost()->SetDirtyFlag(false);
		}
		Compiled = ResolveScript();
		Text = Compiled ? ReadText() : FString();
	}
	if (Text.IsEmpty())
	{
		ReportError(FString::Printf(TEXT("No HLSL is available for %s %s (compile status %s). Run CompileSystem and check for errors."), *ShortUsage(Usage), *Result.Target,
			Compiled ? *CompileStatusText(Compiled->GetVMExecutableData().LastCompileStatus) : TEXT("unknown")));
		return {};
	}
	Result.Script = UsageToString(Usage);
	Result.TotalLength = Text.Len();
	Result.Offset = FMath::Clamp(Offset, 0, Text.Len());
	const int32 Window = FMath::Clamp(MaxLength, 1, 200000);
	Result.Hlsl = Text.Mid(Result.Offset, Window);
	Result.Length = Result.Hlsl.Len();
	Result.bTruncated = Result.Offset + Result.Length < Result.TotalLength;
	return Result;
}

// ------------------------------------------------------------------------------------------------ GetUserParameterUsage

namespace NiagaraMcpAnalysisPrivate
{
	/** Scans reflected properties of an object / struct for user parameter references (bindings). */
	void ScanBindings(const UStruct* Struct, const void* Container, const FString& EmitterUniqueName, TFunctionRef<void(const FString& Target, FName Parameter, const TCHAR* Kind)> Found, int32 Depth = 0)
	{
		if (Depth > 3 || Struct == nullptr || Container == nullptr)
		{
			return;
		}
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient))
			{
				continue;
			}
			const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (StructProperty == nullptr)
			{
				const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
				const FStructProperty* Inner = ArrayProperty ? CastField<FStructProperty>(ArrayProperty->Inner) : nullptr;
				if (Inner && Inner->Struct == FNiagaraMaterialAttributeBinding::StaticStruct())
				{
					FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Container));
					for (int32 Index = 0; Index < Helper.Num(); ++Index)
					{
						const FNiagaraMaterialAttributeBinding* Binding = reinterpret_cast<const FNiagaraMaterialAttributeBinding*>(Helper.GetRawPtr(Index));
						Found(Binding->MaterialParameterName.ToString(), Binding->NiagaraVariable.GetName(), TEXT("MaterialBinding"));
					}
				}
				continue;
			}
			const void* Value = StructProperty->ContainerPtrToValuePtr<void>(Container);
			if (StructProperty->Struct == FNiagaraUserParameterBinding::StaticStruct())
			{
				Found(Property->GetName(), static_cast<const FNiagaraUserParameterBinding*>(Value)->Parameter.GetName(), TEXT("DataInterfaceBinding"));
			}
			else if (StructProperty->Struct == FNiagaraVariableAttributeBinding::StaticStruct())
			{
				Found(Property->GetName(), static_cast<const FNiagaraVariableAttributeBinding*>(Value)->GetParamMapBindableVariable().GetName(), TEXT("RendererBinding"));
			}
			else if (StructProperty->Struct == FNiagaraRendererMaterialParameters::StaticStruct())
			{
				for (const FNiagaraMaterialAttributeBinding& Binding : static_cast<const FNiagaraRendererMaterialParameters*>(Value)->AttributeBindings)
				{
					Found(Binding.MaterialParameterName.ToString(), Binding.NiagaraVariable.GetName(), TEXT("MaterialBinding"));
				}
			}
			else if (NiagaraMcpExt::IsParameterBindingProperty(Property))
			{
				FString Text = NiagaraMcpExt::ParameterBindingText(*StructProperty, Container);
				if (Text.StartsWith(TEXT("@")))
				{
					Found(Property->GetName(), FName(*Text.Mid(1)), TEXT("StageBinding"));
				}
			}
			else if (StructProperty->Struct != FNiagaraVariable::StaticStruct() && StructProperty->Struct != FNiagaraVariableBase::StaticStruct())
			{
				ScanBindings(StructProperty->Struct, Value, EmitterUniqueName, Found, Depth + 1);
			}
		}
	}
}

TArray<FNiagaraMcpParameterUsage> UNiagaraMcpEditorTools::GetUserParameterUsage(UNiagaraSystem* System, FName ParameterName)
{
	using namespace NiagaraMcpAnalysisPrivate;
	TArray<FNiagaraMcpParameterUsage> Result;
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return Result;
	}
	TArray<FNiagaraMcpParameterInfo> Parameters = NiagaraMcpExt::DescribeUserParameters(*System);
	if (!ParameterName.IsNone())
	{
		FNiagaraVariable Variable;
		if (!NiagaraMcpExt::FindUserParameter(*System, ParameterName, Variable, true))
		{
			return Result;
		}
		const FString Wanted = NiagaraMcpExt::UserParameterName(ParameterName).ToString();
		Parameters.RemoveAll([&Wanted](const FNiagaraMcpParameterInfo& P) { return !P.Name.Equals(Wanted, ESearchCase::IgnoreCase); });
	}
	TMap<FString, int32> Index;
	for (const FNiagaraMcpParameterInfo& Parameter : Parameters)
	{
		FNiagaraMcpParameterUsage& Usage = Result.AddDefaulted_GetRef();
		Usage.Parameter = Parameter.Name;
		Usage.Type = Parameter.Type;
		Index.Add(Parameter.Name.ToLower(), Result.Num() - 1);
	}
	auto AddUse = [&](FName Parameter, const TCHAR* Kind, FName Emitter, const FString& Where, const FString& Module, const FString& Target)
	{
		const int32* Found = Index.Find(Parameter.ToString().ToLower());
		if (Found == nullptr)
		{
			return;
		}
		FNiagaraMcpParameterUsage& Usage = Result[*Found];
		const bool bDuplicate = Usage.Uses.ContainsByPredicate([&](const FNiagaraMcpParameterUse& U) { return U.Kind == Kind && U.Emitter == Emitter && U.Where == Where && U.Module == Module && U.Target == Target; });
		if (!bDuplicate)
		{
			FNiagaraMcpParameterUse& Use = Usage.Uses.AddDefaulted_GetRef();
			Use.Kind = Kind;
			Use.Emitter = Emitter;
			Use.Where = Where;
			Use.Module = Module;
			Use.Target = Target;
		}
	};

	// Module inputs (linked, expressions, data interface bindings) through the stack view models.
	TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(*System);
	if (ViewModel.IsValid())
	{
		auto ScanStack = [&](UNiagaraStackViewModel* StackViewModel, FName Emitter)
		{
			if (StackViewModel == nullptr)
			{
				return;
			}
			TArray<UNiagaraStackModuleItem*> Items;
			Stack::CollectModuleItems(*StackViewModel, Items);
			for (UNiagaraStackModuleItem* Item : Items)
			{
				const FString Where = ShortUsage(Stack::GetModuleUsage(*Item));
				const FString Module = Item->GetDisplayName().ToString();
				TArray<UNiagaraStackFunctionInput*> Inputs;
				CollectInputsRecursive(*Item, Inputs, 0);
				for (UNiagaraStackFunctionInput* Input : Inputs)
				{
					const FString Path = InputPath(*Input, *Item);
					switch (Input->GetValueMode())
					{
					case UNiagaraStackFunctionInput::EValueMode::Linked:
						AddUse(Input->GetLinkedParameterValue().GetName(), TEXT("ModuleInput"), Emitter, Where, Module, Path);
						break;
					case UNiagaraStackFunctionInput::EValueMode::Expression:
					{
						const FString Expression = Input->GetCustomExpressionText().ToString();
						for (const FNiagaraMcpParameterUsage& Usage : Result)
						{
							if (Expression.Contains(Usage.Parameter))
							{
								AddUse(FName(*Usage.Parameter), TEXT("ModuleInput"), Emitter, Where, Module, Path + TEXT(" (expression)"));
							}
						}
						break;
					}
					case UNiagaraStackFunctionInput::EValueMode::Data:
						if (const UNiagaraDataInterface* DataInterface = Input->GetDataValueObject())
						{
							ScanBindings(DataInterface->GetClass(), DataInterface, FString(), [&](const FString& Target, FName Parameter, const TCHAR* Kind)
							{
								AddUse(Parameter, TEXT("DataInterfaceBinding"), Emitter, Where, Module, Path + TEXT(".") + Target);
							});
						}
						break;
					default:
						break;
					}
				}
			}
		};
		ScanStack(ViewModel->GetSystemStackViewModel(), NAME_None);
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			if (!IsLightweight(Handle))
			{
				ScanStack(Stack::GetStackViewModel(*ViewModel, Handle.GetName()), Handle.GetName());
			}
		}
	}

	// Map Get reads in every graph owned by the system (system / emitter graphs and scratch pad scripts).
	TArray<UObject*> Graphs;
	GetObjectsWithOuter(System, Graphs, EGetObjectsFlags::IncludeNestedObjects);
	for (UObject* Object : Graphs)
	{
		const UNiagaraGraph* Graph = Cast<UNiagaraGraph>(Object);
		if (Graph == nullptr)
		{
			continue;
		}
		FName Emitter = NAME_None;
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			if (Handle.GetInstance().Emitter && Graph->IsIn(Handle.GetInstance().Emitter))
			{
				Emitter = Handle.GetName();
			}
		}
		const UNiagaraScript* OwnerScript = Graph->GetTypedOuter<UNiagaraScript>();
		const bool bScratch = OwnerScript && (OwnerScript->GetUsage() == ENiagaraScriptUsage::Module || OwnerScript->GetUsage() == ENiagaraScriptUsage::DynamicInput || OwnerScript->GetUsage() == ENiagaraScriptUsage::Function);
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			// In system / emitter graphs a linked module input is itself a Map Get node: report those only once (as ModuleInput).
			if (Node == nullptr || !Node->GetClass()->GetName().Contains(TEXT("ParameterMapGet")))
			{
				continue;
			}
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output && Pin->PinName.ToString().StartsWith(TEXT("User.")))
				{
					const int32* Found = Index.Find(Pin->PinName.ToString().ToLower());
					if (!bScratch && Found && Result[*Found].Uses.ContainsByPredicate([Emitter](const FNiagaraMcpParameterUse& U) { return U.Kind == TEXT("ModuleInput") && U.Emitter == Emitter; }))
					{
						continue;
					}
					AddUse(Pin->PinName, TEXT("MapGet"), Emitter, bScratch ? FString(TEXT("Scratch pad")) : FString(TEXT("Graph")), bScratch ? OwnerScript->GetName() : FString(), Pin->PinName.ToString());
				}
			}
		}
	}

	// Renderers, simulation stages and emitter properties.
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		const TArray<UNiagaraRendererProperties*> Renderers = GetRenderers(Handle);
		for (int32 RendererIndex = 0; RendererIndex < Renderers.Num(); ++RendererIndex)
		{
			if (const UNiagaraRendererProperties* Renderer = Renderers[RendererIndex])
			{
				const FString Where = FString::Printf(TEXT("Renderer %d (%s)"), RendererIndex, *Renderer->GetClass()->GetName());
				ScanBindings(Renderer->GetClass(), Renderer, FString(), [&](const FString& Target, FName Parameter, const TCHAR* Kind)
				{
					AddUse(Parameter, FCString::Strcmp(Kind, TEXT("DataInterfaceBinding")) == 0 ? TEXT("RendererBinding") : Kind, Handle.GetName(), Where, FString(), Target);
				});
			}
		}
		if (FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData())
		{
			const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
			for (int32 StageIndex = 0; StageIndex < Stages.Num(); ++StageIndex)
			{
				if (const UNiagaraSimulationStageBase* Stage = Stages[StageIndex])
				{
					const FString Where = FString::Printf(TEXT("SimulationStage %d (%s)"), StageIndex, *Stage->SimulationStageName.ToString());
					ScanBindings(Stage->GetClass(), Stage, FString(), [&](const FString& Target, FName Parameter, const TCHAR* Kind)
					{
						AddUse(Parameter, TEXT("StageBinding"), Handle.GetName(), Where, FString(), Target);
					});
				}
			}
			ScanBindings(FVersionedNiagaraEmitterData::StaticStruct(), EmitterData, FString(), [&](const FString& Target, FName Parameter, const TCHAR* Kind)
			{
				AddUse(Parameter, TEXT("EmitterProperty"), Handle.GetName(), TEXT("Emitter properties"), FString(), Target);
			});
		}
	}
	// User data interfaces that bind other user parameters.
	TArray<FNiagaraVariable> UserParameters;
	System->GetExposedParameters().GetUserParameters(UserParameters);
	for (const FNiagaraVariable& Parameter : UserParameters)
	{
		if (Parameter.GetType().IsDataInterface())
		{
			if (const UNiagaraDataInterface* DataInterface = System->GetExposedParameters().GetDataInterface(Parameter))
			{
				ScanBindings(DataInterface->GetClass(), DataInterface, FString(), [&](const FString& Target, FName Bound, const TCHAR* Kind)
				{
					AddUse(Bound, TEXT("DataInterfaceBinding"), NAME_None, TEXT("User.") + Parameter.GetName().ToString(), FString(), Target);
				});
			}
		}
	}
	for (FNiagaraMcpParameterUsage& Usage : Result)
	{
		Usage.bUnused = Usage.Uses.Num() == 0;
	}
	return Result;
}

#undef LOCTEXT_NAMESPACE
