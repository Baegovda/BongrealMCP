// v0.5 analysis tools (part 2): ApplySystemSpec, SaveEmitterAsAsset, lightweight emitters, data channels, RecolorEffect.

#include "NiagaraMcpEditorTools.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraCommon.h"
#include "NiagaraDataChannel.h"
#include "NiagaraDataChannelAsset.h"
#include "NiagaraDataChannelVariable.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterBase.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraModule.h"
#include "JsonObjectConverter.h"
#include "UObject/UObjectHash.h"
#include "NiagaraMcpAnalysisUtils.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraMcpEditorToolsInternal.h"
#include "NiagaraMcpExtUtils.h"
#include "NiagaraMcpStackUtils.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

#define LOCTEXT_NAMESPACE "NiagaraMcpEditorTools"

using namespace NiagaraMcp;

// ------------------------------------------------------------------------------------------------ ApplySystemSpec

namespace NiagaraMcpApplySpec
{
	using namespace NiagaraMcpAnalysis;

	FString JsonText(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return FString();
		}
		switch (Value->Type)
		{
		case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Number: return FString::SanitizeFloat(Value->AsNumber(), 0);
		case EJson::String: return Value->AsString();
		default: return FString();
		}
	}

	TSharedPtr<FJsonObject> Child(const TSharedPtr<FJsonObject>& Parent, const FString& Key)
	{
		const TSharedPtr<FJsonObject>* Out = nullptr;
		return Parent.IsValid() && Parent->TryGetObjectField(Key, Out) && Out ? *Out : nullptr;
	}

	FString Field(const TSharedPtr<FJsonObject>& Parent, const FString& Key, const FString& Default = FString())
	{
		return Parent.IsValid() && Parent->HasField(Key) ? JsonText(Parent->TryGetField(Key)) : Default;
	}

	bool IsTruncated(const FString& Value)
	{
		return Value.Len() > 400 && Value.EndsWith(TEXT("..."));
	}

	/** Same filter as ExportSystemSpec's non-default property listing. */
	bool IsSpecProperty(const FProperty* Property)
	{
		return Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_Deprecated) && !Property->GetName().EndsWith(TEXT("_DEPRECATED"));
	}

	bool IsBindingProperty(const FProperty* Property)
	{
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		if (StructProperty && (StructProperty->Struct == FNiagaraVariableAttributeBinding::StaticStruct() || StructProperty->Struct == FNiagaraRendererMaterialParameters::StaticStruct()))
		{
			return true;
		}
		const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
		const FStructProperty* Inner = ArrayProperty ? CastField<FStructProperty>(ArrayProperty->Inner) : nullptr;
		return Inner && (Inner->Struct == FNiagaraMaterialAttributeBinding::StaticStruct() || Inner->Struct == FNiagaraVariableAttributeBinding::StaticStruct());
	}

	struct FContext
	{
		UNiagaraSystem* System = nullptr;
		bool bDryRun = false;
		bool bReplace = false;
		FString SourcePackage;
		FNiagaraMcpApplySpecResult* Result = nullptr;
		FErrorCapture* Capture = nullptr;

		/** Runs one nested tool call (or records it in a dry run). False when it failed. */
		bool Run(const TCHAR* Action, const FString& Path, const FString& Value, TFunctionRef<void()> Call)
		{
			FNiagaraMcpSpecOperation& Op = Result->Operations.AddDefaulted_GetRef();
			Op.Action = Action;
			Op.Path = Path;
			Op.Value = Value.Left(300);
			if (bDryRun)
			{
				Op.Status = TEXT("WouldApply");
				++Result->Applied;
				return true;
			}
			const int32 Start = Capture->Num();
			Call();
			if (Capture->Num() > Start)
			{
				Op.Status = TEXT("Failed");
				Op.Reason = Capture->TakeSince(Start).Left(600);
				++Result->Failed;
				return false;
			}
			Op.Status = TEXT("Applied");
			++Result->Applied;
			return true;
		}

		void Skip(const TCHAR* Action, const FString& Path, const FString& Reason)
		{
			FNiagaraMcpSpecOperation& Op = Result->Operations.AddDefaulted_GetRef();
			Op.Action = Action;
			Op.Path = Path;
			Op.Status = TEXT("Skipped");
			Op.Reason = Reason;
			++Result->Skipped;
		}

		void Same()
		{
			++Result->Unchanged;
		}

		/** True for references into the system the spec was exported from (scratch pads, local modules). */
		bool PointsIntoSource(const FString& Value) const
		{
			return !SourcePackage.IsEmpty() && Value.Contains(SourcePackage + TEXT(".")) && Value.Contains(TEXT(":"));
		}
	};

	/**
	 * Applies the "properties" object of a spec to a reflected container. Current values are read the way the spec was
	 * written (PropertyValueText, 400 characters); in Replace mode non-default properties the spec omits are reset.
	 */
	void SyncProperties(FContext& Ctx, const FString& PathPrefix, const TSharedPtr<FJsonObject>& Spec, const UStruct* Struct, const void* Container, const void* Defaults, bool bFull,
		TFunctionRef<bool(const FProperty*)> SkipProperty, const TCHAR* Action, TFunctionRef<void(const FString& Name, const FString& Value)> Set)
	{
		TSet<FString> Seen;
		if (Spec.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Spec->Values)
			{
				const FString Value = JsonText(Pair.Value);
				const FString Path = PathPrefix + Pair.Key;
				Seen.Add(Pair.Key);
				const FProperty* Property = Container ? FindFProperty<FProperty>(Struct, FName(*Pair.Key)) : nullptr;
				if (Container && Property == nullptr)
				{
					Ctx.Skip(Action, Path, FString::Printf(TEXT("No property '%s' on %s."), *Pair.Key, *Struct->GetName()));
					continue;
				}
				if (Property && NiagaraMcpExt::PropertyValueText(Property, Container, 400) == Value)
				{
					Ctx.Same();
					continue;
				}
				if (IsTruncated(Value))
				{
					Ctx.Skip(Action, Path, TEXT("The value was shortened in the spec (over 400 characters); set it with the property tool directly."));
					continue;
				}
				if (Ctx.PointsIntoSource(Value))
				{
					Ctx.Skip(Action, Path, TEXT("The value references an object inside the source system (scratch pad / local object)."));
					continue;
				}
				Ctx.Run(Action, Path, Value, [&]() { Set(Pair.Key, Value); });
			}
		}
		if (!bFull || Container == nullptr || Defaults == nullptr)
		{
			return;
		}
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Property = *It;
			if (!IsSpecProperty(Property) || SkipProperty(Property) || Seen.Contains(Property->GetName()) || NiagaraMcpExt::IsPropertyDefault(Property, Container, Defaults))
			{
				continue;
			}
			const FString Default = NiagaraMcpExt::PropertyValueText(Property, Defaults, 0);
			Ctx.Run(Action, PathPrefix + Property->GetName(), Default, [&]() { Set(Property->GetName(), Default); });
		}
	}

	// ---------------------------------------------------------------- Modules

	struct FCurrentModule
	{
		FString Key;
		FString ModuleId;
		int32 Index = 0;
		FString Script;
		bool bEnabled = true;
		TArray<TPair<FString, FString>> Inputs; // overridden inputs, SetModuleInput syntax
	};

	FString StackKeyFor(const UNiagaraStackModuleItem& Item, const FVersionedNiagaraEmitterData* EmitterData)
	{
		const UNiagaraNodeOutput* OutputNode = Item.GetOutputNode();
		const ENiagaraScriptUsage Usage = Stack::GetModuleUsage(Item);
		if (EmitterData && OutputNode && Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
		{
			for (const UNiagaraSimulationStageBase* Stage : EmitterData->GetSimulationStages())
			{
				if (Stage && Stage->Script && Stage->Script->GetUsageId() == OutputNode->GetUsageId())
				{
					return TEXT("SimulationStage:") + Stage->SimulationStageName.ToString();
				}
			}
		}
		if (EmitterData && OutputNode && Usage == ENiagaraScriptUsage::ParticleEventScript)
		{
			const TArray<FNiagaraEventScriptProperties>& Handlers = EmitterData->GetEventHandlers();
			for (int32 Index = 0; Index < Handlers.Num(); ++Index)
			{
				if (Handlers[Index].Script && Handlers[Index].Script->GetUsageId() == OutputNode->GetUsageId())
				{
					return FString::Printf(TEXT("EventHandler:%d"), Index);
				}
			}
		}
		return ShortUsage(Usage);
	}

	/** Modules per stack key, keyed like ExportSystemSpec ("Display Name", "Display Name #2"). */
	TMap<FString, TArray<FCurrentModule>> CollectModules(UNiagaraSystem& System, FName Emitter)
	{
		TMap<FString, TArray<FCurrentModule>> Out;
		TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(System);
		if (!ViewModel.IsValid())
		{
			return Out;
		}
		UNiagaraStackViewModel* StackViewModel = Stack::IsSystemName(Emitter) ? ViewModel->GetSystemStackViewModel() : Stack::GetStackViewModel(*ViewModel, Emitter);
		if (StackViewModel == nullptr)
		{
			return Out;
		}
		const FNiagaraEmitterHandle* Handle = Stack::IsSystemName(Emitter) ? nullptr : System.GetEmitterHandles().FindByPredicate([Emitter](const FNiagaraEmitterHandle& H) { return H.GetName() == Emitter; });
		const FVersionedNiagaraEmitterData* EmitterData = Handle ? Handle->GetEmitterData() : nullptr;
		TArray<UNiagaraStackModuleItem*> Items;
		Stack::CollectModuleItems(*StackViewModel, Items);
		for (UNiagaraStackModuleItem* Item : Items)
		{
			TArray<FCurrentModule>& Modules = Out.FindOrAdd(StackKeyFor(*Item, EmitterData));
			FCurrentModule& Module = Modules.AddDefaulted_GetRef();
			Module.Index = Modules.Num() - 1;
			const FString Display = Item->GetDisplayName().ToString();
			Module.Key = Display;
			for (int32 Suffix = 2; Modules.ContainsByPredicate([&Module](const FCurrentModule& M) { return &M != &Module && M.Key == Module.Key; }); ++Suffix)
			{
				Module.Key = FString::Printf(TEXT("%s #%d"), *Display, Suffix);
			}
			Module.ModuleId = Stack::GetModuleId(*Item);
			Module.Script = Item->GetModuleNode().FunctionScript ? Item->GetModuleNode().FunctionScript->GetPathName() : FString();
			Module.bEnabled = Item->GetIsEnabled();
			TArray<UNiagaraStackFunctionInput*> Inputs;
			CollectInputsRecursive(*Item, Inputs, 0);
			for (UNiagaraStackFunctionInput* Input : Inputs)
			{
				if (Input->CanReset())
				{
					Module.Inputs.Emplace(InputPath(*Input, *Item), Stack::InputValueToString(*Input));
				}
			}
		}
		return Out;
	}

	int32 Depth(const FString& InputPath)
	{
		int32 Count = 0;
		for (const TCHAR C : InputPath)
		{
			Count += C == TEXT('/') ? 1 : 0;
		}
		return Count;
	}

	void SyncStack(FContext& Ctx, FName Emitter, const FString& StackKey, const TSharedPtr<FJsonObject>& SpecStack, bool bFull, const FString& PathPrefix)
	{
		UNiagaraSystem& System = *Ctx.System;
		const FString EmitterText = Stack::IsSystemName(Emitter) ? FString(TEXT("System")) : Emitter.ToString();
		FString StageKind;
		int32 StageIndex = INDEX_NONE;
		if (StackKey.StartsWith(TEXT("SimulationStage:")) || StackKey.StartsWith(TEXT("EventHandler:")))
		{
			const FNiagaraEmitterHandle* Handle = System.GetEmitterHandles().FindByPredicate([Emitter](const FNiagaraEmitterHandle& H) { return H.GetName() == Emitter; });
			const FVersionedNiagaraEmitterData* EmitterData = Handle ? Handle->GetEmitterData() : nullptr;
			if (StackKey.StartsWith(TEXT("SimulationStage:")))
			{
				StageKind = TEXT("SimulationStage");
				const FString StageName = StackKey.Mid(16);
				if (EmitterData)
				{
					const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
					for (int32 Index = 0; Index < Stages.Num(); ++Index)
					{
						if (Stages[Index] && Stages[Index]->SimulationStageName.ToString() == StageName)
						{
							StageIndex = Index;
						}
					}
				}
			}
			else
			{
				StageKind = TEXT("EventHandler");
				StageIndex = FCString::Atoi(*StackKey.Mid(13));
				if (EmitterData == nullptr || !EmitterData->GetEventHandlers().IsValidIndex(StageIndex))
				{
					StageIndex = INDEX_NONE;
				}
			}
			if (StageIndex == INDEX_NONE && !Ctx.bDryRun)
			{
				Ctx.Skip(TEXT("AddModule"), PathPrefix, FString::Printf(TEXT("Stage '%s' does not exist on the target emitter."), *StackKey));
				return;
			}
		}

		struct FSpecModule
		{
			FString Key;
			TSharedPtr<FJsonObject> Object;
			int32 Index = 0;
		};
		TArray<FSpecModule> SpecModules;
		if (SpecStack.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : SpecStack->Values)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				if (Pair.Value.IsValid() && Pair.Value->TryGetObject(Object) && Object)
				{
					FSpecModule& Module = SpecModules.AddDefaulted_GetRef();
					Module.Key = Pair.Key;
					Module.Object = *Object;
					Module.Index = FCString::Atoi(*Field(*Object, TEXT("index"), TEXT("0")));
				}
			}
		}
		SpecModules.StableSort([](const FSpecModule& A, const FSpecModule& B) { return A.Index < B.Index; });

		auto Current = [&]() { return CollectModules(System, Emitter).FindRef(StackKey); };
		TArray<FCurrentModule> Modules = Current();

		// Replace: remove modules the spec does not list (or lists with another script).
		if (bFull)
		{
			for (const FCurrentModule& Module : Modules)
			{
				const FSpecModule* Spec = SpecModules.FindByPredicate([&Module](const FSpecModule& S) { return S.Key == Module.Key; });
				if (Spec == nullptr || Field(Spec->Object, TEXT("script")) != Module.Script)
				{
					Ctx.Run(TEXT("RemoveModule"), PathPrefix + Module.Key, Module.Script, [&]() { UNiagaraMcpEditorTools::RemoveModule(&System, Emitter, Module.ModuleId, TEXT("Any"), false); });
				}
			}
			if (!Ctx.bDryRun)
			{
				Modules = Current();
			}
		}

		for (int32 Position = 0; Position < SpecModules.Num(); ++Position)
		{
			const FSpecModule& Spec = SpecModules[Position];
			const FString Path = PathPrefix + Spec.Key;
			const FString Script = Field(Spec.Object, TEXT("script"));
			const FCurrentModule* Found = Modules.FindByPredicate([&Spec](const FCurrentModule& M) { return M.Key == Spec.Key; });
			if (Found && Found->Script != Script)
			{
				Ctx.Skip(TEXT("AddModule"), Path, FString::Printf(TEXT("A different module with this name exists (%s); Replace mode swaps it."), *Found->Script));
				continue;
			}
			FString ModuleId = Found ? Found->ModuleId : FString();
			if (Found == nullptr)
			{
				if (Script.IsEmpty())
				{
					Ctx.Skip(TEXT("AddModule"), Path, TEXT("The module has no script asset (Set Parameters / assignment module); recreate it by hand."));
					continue;
				}
				if (Ctx.PointsIntoSource(Script))
				{
					Ctx.Skip(TEXT("AddModule"), Path, TEXT("Scratch pad / local module of the source system: copy it with CreateLocalModule and add it by hand."));
					continue;
				}
				const int32 Target = Spec.Index;
				const bool bOk = Ctx.Run(TEXT("AddModule"), Path, Script, [&]()
				{
					const FNiagaraMcpModuleInfo Added = StageKind.IsEmpty()
						? UNiagaraMcpEditorTools::AddModule(&System, Emitter, StackKey, Script, Target, false)
						: UNiagaraMcpEditorTools::AddModuleToStage(&System, Emitter, StageKind, StageIndex, Script, Target, false);
					ModuleId = Added.ModuleId;
				});
				if (Ctx.bDryRun)
				{
					// Inputs of a module that does not exist yet would all be new.
					if (const TSharedPtr<FJsonObject> Inputs = Child(Spec.Object, TEXT("inputs")))
					{
						for (const TPair<FString, TSharedPtr<FJsonValue>>& Input : Inputs->Values)
						{
							Ctx.Run(TEXT("SetModuleInput"), Path + TEXT(".inputs.") + Input.Key, JsonText(Input.Value), []() {});
						}
					}
					continue;
				}
				if (!bOk || ModuleId.IsEmpty())
				{
					continue;
				}
				Modules = Current();
				Found = Modules.FindByPredicate([&ModuleId](const FCurrentModule& M) { return M.ModuleId == ModuleId; });
				if (Found == nullptr)
				{
					continue;
				}
			}
			else if (Found->Index != Spec.Index)
			{
				const int32 Target = Spec.Index;
				const FString Id = Found->ModuleId;
				Ctx.Run(TEXT("MoveModule"), Path + TEXT(".index"), FString::FromInt(Target), [&]() { UNiagaraMcpEditorTools::MoveModule(&System, Emitter, Id, TEXT("Any"), TEXT("Any"), Target, false); });
				if (!Ctx.bDryRun)
				{
					Modules = Current();
					Found = Modules.FindByPredicate([&ModuleId](const FCurrentModule& M) { return M.ModuleId == ModuleId; });
					if (Found == nullptr)
					{
						continue;
					}
				}
			}
			else
			{
				Ctx.Same();
			}

			const bool bEnabled = Field(Spec.Object, TEXT("enabled"), TEXT("true")) == TEXT("true");
			if (bEnabled != Found->bEnabled)
			{
				Ctx.Run(TEXT("SetModuleEnabled"), Path + TEXT(".enabled"), bEnabled ? TEXT("true") : TEXT("false"), [&]() { UNiagaraMcpEditorTools::SetModuleEnabled(&System, Emitter, ModuleId, bEnabled, TEXT("Any"), false); });
			}

			// Inputs: resets first (Replace), parents before their dynamic input children.
			TArray<TPair<FString, FString>> SpecInputs;
			if (const TSharedPtr<FJsonObject> Inputs = Child(Spec.Object, TEXT("inputs")))
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Input : Inputs->Values)
				{
					SpecInputs.Emplace(Input.Key, JsonText(Input.Value));
				}
			}
			SpecInputs.StableSort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B) { return Depth(A.Key) < Depth(B.Key); });
			const TArray<TPair<FString, FString>> CurrentInputs = Found->Inputs;
			if (bFull)
			{
				TArray<FString> Reset;
				for (const TPair<FString, FString>& Input : CurrentInputs)
				{
					const bool bInSpec = SpecInputs.ContainsByPredicate([&Input](const TPair<FString, FString>& S) { return S.Key == Input.Key; });
					const bool bParentReset = Reset.ContainsByPredicate([&Input](const FString& Parent) { return Input.Key.StartsWith(Parent + TEXT("/")); });
					if (!bInSpec && !bParentReset)
					{
						Reset.Add(Input.Key);
						Ctx.Run(TEXT("ResetModuleInput"), Path + TEXT(".inputs.") + Input.Key, TEXT("<default>"), [&]() { UNiagaraMcpEditorTools::ResetModuleInput(&System, Emitter, ModuleId, Input.Key, TEXT("Any"), false); });
					}
				}
			}
			for (const TPair<FString, FString>& Input : SpecInputs)
			{
				const TPair<FString, FString>* Existing = CurrentInputs.FindByPredicate([&Input](const TPair<FString, FString>& C) { return C.Key == Input.Key; });
				if (Existing && Existing->Value == Input.Value)
				{
					Ctx.Same();
					continue;
				}
				if (Ctx.PointsIntoSource(Input.Value))
				{
					Ctx.Skip(TEXT("SetModuleInput"), Path + TEXT(".inputs.") + Input.Key, TEXT("The value references a scratch pad / local script of the source system."));
					continue;
				}
				Ctx.Run(TEXT("SetModuleInput"), Path + TEXT(".inputs.") + Input.Key, Input.Value, [&]() { UNiagaraMcpEditorTools::SetModuleInput(&System, Emitter, ModuleId, Input.Key, Input.Value, TEXT("Any"), false); });
			}
		}
	}

	void SyncStacks(FContext& Ctx, FName Emitter, const TSharedPtr<FJsonObject>& SpecStacks, bool bFull, const FString& PathPrefix)
	{
		TArray<FString> Keys;
		if (SpecStacks.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : SpecStacks->Values)
			{
				Keys.Add(Pair.Key);
			}
		}
		if (bFull && !Ctx.bDryRun)
		{
			TArray<FString> CurrentKeys;
			CollectModules(*Ctx.System, Emitter).GetKeys(CurrentKeys);
			for (const FString& Key : CurrentKeys)
			{
				Keys.AddUnique(Key);
			}
		}
		for (const FString& Key : Keys)
		{
			SyncStack(Ctx, Emitter, Key, Child(SpecStacks, Key), bFull, PathPrefix + Key + TEXT("."));
		}
	}

	// ---------------------------------------------------------------- Renderers

	FString RendererTypeArgument(const FString& ClassName)
	{
		UClass* Class = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
		return Class ? Class->GetPathName() : ClassName;
	}

	void SyncRenderers(FContext& Ctx, FName Emitter, const TSharedPtr<FJsonObject>& SpecRenderers, bool bFull, const FString& PathPrefix)
	{
		UNiagaraSystem& System = *Ctx.System;
		auto Renderers = [&]() -> TArray<UNiagaraRendererProperties*>
		{
			const FNiagaraEmitterHandle* Handle = System.GetEmitterHandles().FindByPredicate([Emitter](const FNiagaraEmitterHandle& H) { return H.GetName() == Emitter; });
			return Handle ? GetRenderers(*Handle) : TArray<UNiagaraRendererProperties*>();
		};
		struct FSpecRenderer
		{
			int32 Index = 0;
			FString Class;
			FString Key;
			TSharedPtr<FJsonObject> Object;
		};
		TArray<FSpecRenderer> Specs;
		if (SpecRenderers.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : SpecRenderers->Values)
			{
				FString IndexText, Class;
				const TSharedPtr<FJsonObject>* Object = nullptr;
				if (Pair.Key.Split(TEXT(":"), &IndexText, &Class) && Pair.Value.IsValid() && Pair.Value->TryGetObject(Object) && Object)
				{
					Specs.Add({ FCString::Atoi(*IndexText), Class, Pair.Key, *Object });
				}
			}
		}
		Specs.StableSort([](const FSpecRenderer& A, const FSpecRenderer& B) { return A.Index < B.Index; });

		TArray<UNiagaraRendererProperties*> Current = Renderers();
		int32 Expected = Current.Num();
		for (int32 Position = 0; Position < Specs.Num(); ++Position)
		{
			const FSpecRenderer& Spec = Specs[Position];
			const FString Path = PathPrefix + Spec.Key;
			const int32 Index = Position;
			UNiagaraRendererProperties* Renderer = Current.IsValidIndex(Index) && Current[Index] && Current[Index]->GetClass()->GetName() == Spec.Class ? Current[Index] : nullptr;
			if (Renderer == nullptr)
			{
				const FString Type = RendererTypeArgument(Spec.Class);
				const int32 NewIndex = Expected;
				const bool bOk = Ctx.Run(TEXT("AddRenderer"), Path, Spec.Class, [&]() { UNiagaraMcpEditorTools::AddRenderer(&System, Emitter, Type, TEXT("None"), false); });
				++Expected;
				if (Ctx.bDryRun || !bOk)
				{
					continue;
				}
				if (NewIndex != Index)
				{
					Ctx.Run(TEXT("MoveRenderer"), Path, FString::FromInt(Index), [&]() { UNiagaraMcpEditorTools::MoveRenderer(&System, Emitter, NewIndex, Index, false); });
				}
				Current = Renderers();
				Renderer = Current.IsValidIndex(Index) ? Current[Index] : nullptr;
				if (Renderer == nullptr || Renderer->GetClass()->GetName() != Spec.Class)
				{
					continue;
				}
			}
			else
			{
				Ctx.Same();
			}

			const bool bEnabled = Field(Spec.Object, TEXT("enabled"), TEXT("true")) == TEXT("true");
			if (bEnabled != Renderer->GetIsEnabled())
			{
				Ctx.Run(TEXT("SetRendererProperty"), Path + TEXT(".enabled"), bEnabled ? TEXT("true") : TEXT("false"), [&]() { UNiagaraMcpEditorTools::SetRendererProperty(&System, Emitter, Index, TEXT("bIsEnabled"), bEnabled ? TEXT("true") : TEXT("false"), false); });
			}
			const UObject* Defaults = Renderer->GetClass()->GetDefaultObject();
			SyncProperties(Ctx, Path + TEXT(".properties."), Child(Spec.Object, TEXT("properties")), Renderer->GetClass(), Renderer, Defaults, bFull,
				[](const FProperty* Property) { return IsBindingProperty(Property) || Property->GetFName() == TEXT("bIsEnabled"); }, TEXT("SetRendererProperty"),
				[&](const FString& Name, const FString& Value) { UNiagaraMcpEditorTools::SetRendererProperty(&System, Emitter, Index, Name, Value, false); });

			// Attribute bindings.
			const TSharedPtr<FJsonObject> Bindings = Child(Spec.Object, TEXT("bindings"));
			for (TFieldIterator<FStructProperty> It(Renderer->GetClass()); It; ++It)
			{
				if (It->Struct != FNiagaraVariableAttributeBinding::StaticStruct() || It->HasAnyPropertyFlags(CPF_Deprecated))
				{
					continue;
				}
				const FString BindingName = It->GetName();
				const FString CurrentValue = It->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(Renderer)->GetParamMapBindableVariable().GetName().ToString();
				const FString DefaultValue = It->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(Defaults)->GetParamMapBindableVariable().GetName().ToString();
				FString Wanted = Bindings.IsValid() && Bindings->HasField(BindingName) ? Field(Bindings, BindingName) : FString();
				if (Wanted.IsEmpty())
				{
					if (!bFull || CurrentValue == DefaultValue)
					{
						continue;
					}
					Wanted = DefaultValue;
				}
				if (Wanted == CurrentValue)
				{
					Ctx.Same();
					continue;
				}
				Ctx.Run(TEXT("SetRendererBinding"), Path + TEXT(".bindings.") + BindingName, Wanted, [&]() { UNiagaraMcpEditorTools::SetRendererBinding(&System, Emitter, Index, BindingName, Wanted, false); });
			}
			if (const TSharedPtr<FJsonObject> MaterialBindings = Child(Spec.Object, TEXT("materialBindings")))
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : MaterialBindings->Values)
				{
					bool bSame = false;
					for (TFieldIterator<FStructProperty> It(Renderer->GetClass()); It; ++It)
					{
						if (It->Struct == FNiagaraRendererMaterialParameters::StaticStruct())
						{
							for (const FNiagaraMaterialAttributeBinding& Binding : It->ContainerPtrToValuePtr<FNiagaraRendererMaterialParameters>(Renderer)->AttributeBindings)
							{
								bSame |= Binding.MaterialParameterName.ToString() == Pair.Key && Binding.NiagaraVariable.GetName().ToString() == JsonText(Pair.Value);
							}
						}
					}
					if (bSame)
					{
						Ctx.Same();
					}
					else
					{
						Ctx.Skip(TEXT("SetMaterialBinding"), Path + TEXT(".materialBindings.") + Pair.Key,
							TEXT("Material parameter bindings are not applied by ApplySystemSpec (the spec does not carry their types); set them in the renderer's MaterialParameters by hand."));
					}
				}
			}
		}
		if (bFull)
		{
			Current = Ctx.bDryRun ? Current : Renderers();
			for (int32 Index = Current.Num() - 1; Index >= Specs.Num(); --Index)
			{
				Ctx.Run(TEXT("RemoveRenderer"), FString::Printf(TEXT("%s%d"), *PathPrefix, Index), Current[Index] ? Current[Index]->GetClass()->GetName() : FString(), [&]() { UNiagaraMcpEditorTools::RemoveRenderer(&System, Emitter, Index, false); });
			}
		}
	}

	// ---------------------------------------------------------------- Stages and event handlers

	void SyncStages(FContext& Ctx, FName Emitter, const TSharedPtr<FJsonObject>& SpecStages, bool bFull, const FString& PathPrefix)
	{
		UNiagaraSystem& System = *Ctx.System;
		auto Stages = [&]() -> TArray<UNiagaraSimulationStageBase*>
		{
			const FNiagaraEmitterHandle* Handle = System.GetEmitterHandles().FindByPredicate([Emitter](const FNiagaraEmitterHandle& H) { return H.GetName() == Emitter; });
			const FVersionedNiagaraEmitterData* EmitterData = Handle ? Handle->GetEmitterData() : nullptr;
			return EmitterData ? EmitterData->GetSimulationStages() : TArray<UNiagaraSimulationStageBase*>();
		};
		struct FSpecStage
		{
			FString Name;
			int32 Index = 0;
			TSharedPtr<FJsonObject> Object;
		};
		TArray<FSpecStage> Specs;
		if (SpecStages.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : SpecStages->Values)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				if (Pair.Value.IsValid() && Pair.Value->TryGetObject(Object) && Object)
				{
					Specs.Add({ Pair.Key, FCString::Atoi(*Field(*Object, TEXT("index"), TEXT("0"))), *Object });
				}
			}
		}
		Specs.StableSort([](const FSpecStage& A, const FSpecStage& B) { return A.Index < B.Index; });
		auto FindIndex = [&](const FString& Name)
		{
			const TArray<UNiagaraSimulationStageBase*> Current = Stages();
			return Current.IndexOfByPredicate([&Name](const UNiagaraSimulationStageBase* S) { return S && S->SimulationStageName.ToString() == Name; });
		};
		if (bFull)
		{
			const TArray<UNiagaraSimulationStageBase*> Current = Stages();
			for (int32 Index = Current.Num() - 1; Index >= 0; --Index)
			{
				const FString Name = Current[Index] ? Current[Index]->SimulationStageName.ToString() : FString();
				if (!Specs.ContainsByPredicate([&Name](const FSpecStage& S) { return S.Name == Name; }))
				{
					Ctx.Run(TEXT("RemoveSimulationStage"), PathPrefix + Name, Name, [&]() { UNiagaraMcpEditorTools::RemoveSimulationStage(&System, Emitter, Index, false); });
				}
			}
		}
		for (const FSpecStage& Spec : Specs)
		{
			const FString Path = PathPrefix + Spec.Name;
			const FString Class = Field(Spec.Object, TEXT("class"));
			int32 Index = FindIndex(Spec.Name);
			if (Index == INDEX_NONE)
			{
				if (Class != TEXT("NiagaraSimulationStageGeneric"))
				{
					Ctx.Skip(TEXT("AddSimulationStage"), Path, FString::Printf(TEXT("Stage class %s cannot be created by AddSimulationStage."), *Class));
					continue;
				}
				const int32 Target = Spec.Index;
				if (!Ctx.Run(TEXT("AddSimulationStage"), Path, Spec.Name, [&]() { UNiagaraMcpEditorTools::AddSimulationStage(&System, Emitter, FName(*Spec.Name), TEXT("Particles"), NAME_None, TEXT("None"), Target, false); }) || Ctx.bDryRun)
				{
					continue;
				}
				Index = FindIndex(Spec.Name);
				if (Index == INDEX_NONE)
				{
					continue;
				}
			}
			else
			{
				Ctx.Same();
			}
			UNiagaraSimulationStageBase* Stage = Stages()[Index];
			SyncProperties(Ctx, Path + TEXT(".properties."), Child(Spec.Object, TEXT("properties")), Stage->GetClass(), Stage, Stage->GetClass()->GetDefaultObject(), bFull,
				[](const FProperty* Property) { return Property->GetFName() == TEXT("SimulationStageName"); }, TEXT("SetSimulationStageProperty"),
				[&](const FString& Name, const FString& Value) { UNiagaraMcpEditorTools::SetSimulationStageProperty(&System, Emitter, Index, Name, Value, false); });
		}
	}

	void SyncEventHandlers(FContext& Ctx, FName Emitter, const TSharedPtr<FJsonObject>& SpecHandlers, bool bFull, const FString& PathPrefix)
	{
		UNiagaraSystem& System = *Ctx.System;
		auto Handlers = [&]() -> TArray<FNiagaraEventScriptProperties>
		{
			const FNiagaraEmitterHandle* Handle = System.GetEmitterHandles().FindByPredicate([Emitter](const FNiagaraEmitterHandle& H) { return H.GetName() == Emitter; });
			const FVersionedNiagaraEmitterData* EmitterData = Handle ? Handle->GetEmitterData() : nullptr;
			return EmitterData ? EmitterData->GetEventHandlers() : TArray<FNiagaraEventScriptProperties>();
		};
		TArray<TPair<int32, TSharedPtr<FJsonObject>>> Specs;
		if (SpecHandlers.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : SpecHandlers->Values)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				if (Pair.Value.IsValid() && Pair.Value->TryGetObject(Object) && Object)
				{
					Specs.Emplace(FCString::Atoi(*Pair.Key), *Object);
				}
			}
		}
		Specs.StableSort([](const TPair<int32, TSharedPtr<FJsonObject>>& A, const TPair<int32, TSharedPtr<FJsonObject>>& B) { return A.Key < B.Key; });
		const UEnum* ModeEnum = StaticEnum<EScriptExecutionMode>();
		for (int32 Position = 0; Position < Specs.Num(); ++Position)
		{
			const TSharedPtr<FJsonObject>& Spec = Specs[Position].Value;
			const FString Path = PathPrefix + FString::FromInt(Position);
			const FString SourceEmitter = Field(Spec, TEXT("sourceEmitter"));
			const FString SourceEvent = Field(Spec, TEXT("sourceEvent"));
			const FString Mode = Field(Spec, TEXT("executionMode"), TEXT("EveryParticle"));
			const int32 SpawnNumber = FCString::Atoi(*Field(Spec, TEXT("spawnNumber"), TEXT("0")));
			const int32 MaxEvents = FCString::Atoi(*Field(Spec, TEXT("maxEventsPerFrame"), TEXT("0")));
			const TArray<FNiagaraEventScriptProperties> Current = Handlers();
			if (!Current.IsValidIndex(Position))
			{
				Ctx.Run(TEXT("AddEventHandler"), Path, FString::Printf(TEXT("%s.%s"), *SourceEmitter, *SourceEvent), [&]()
				{
					UNiagaraMcpEditorTools::AddEventHandler(&System, Emitter, FName(*SourceEmitter), FName(*SourceEvent), Mode, SpawnNumber, MaxEvents, false);
				});
				continue;
			}
			const FNiagaraEventScriptProperties& Handler = Current[Position];
			const FString CurrentMode = ModeEnum->GetNameStringByValue(static_cast<int64>(Handler.ExecutionMode));
			auto SetIfDifferent = [&](const TCHAR* Property, const FString& Wanted, const FString& Have)
			{
				if (Wanted == Have)
				{
					Ctx.Same();
					return;
				}
				Ctx.Run(TEXT("SetEventHandlerProperty"), Path + TEXT(".") + Property, Wanted, [&]() { UNiagaraMcpEditorTools::SetEventHandlerProperty(&System, Emitter, Position, Property, Wanted, false); });
			};
			if (Handler.SourceEventName.ToString() != SourceEvent)
			{
				Ctx.Skip(TEXT("SetEventHandlerProperty"), Path + TEXT(".sourceEvent"), TEXT("The handler at this index listens to another event; remove it (Replace mode) and add the spec's handler."));
				continue;
			}
			SetIfDifferent(TEXT("ExecutionMode"), Mode, CurrentMode);
			SetIfDifferent(TEXT("SpawnNumber"), FString::FromInt(SpawnNumber), FString::FromInt(static_cast<int32>(Handler.SpawnNumber)));
			SetIfDifferent(TEXT("MaxEventsPerFrame"), FString::FromInt(MaxEvents), FString::FromInt(static_cast<int32>(Handler.MaxEventsPerFrame)));
		}
		if (bFull)
		{
			const TArray<FNiagaraEventScriptProperties> Current = Handlers();
			for (int32 Index = Current.Num() - 1; Index >= Specs.Num(); --Index)
			{
				Ctx.Run(TEXT("RemoveEventHandler"), PathPrefix + FString::FromInt(Index), FString(), [&]() { UNiagaraMcpEditorTools::RemoveEventHandler(&System, Emitter, Index, false); });
			}
		}
	}
}

FNiagaraMcpApplySpecResult UNiagaraMcpEditorTools::ApplySystemSpec(UNiagaraSystem* System, const FString& SpecJson, const FString& Mode, bool bDryRun, bool bSave)
{
	using namespace NiagaraMcpApplySpec;
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	const FString ModeText = IsUnset(Mode) ? FString(TEXT("Merge")) : Mode.TrimStartAndEnd();
	const bool bReplace = ModeText.Equals(TEXT("Replace"), ESearchCase::IgnoreCase);
	if (!bReplace && !ModeText.Equals(TEXT("Merge"), ESearchCase::IgnoreCase))
	{
		ReportError(FString::Printf(TEXT("Unknown Mode '%s'. Use Merge or Replace."), *Mode));
		return {};
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SpecJson), Root) || !Root.IsValid())
	{
		ReportError(TEXT("SpecJson is not a JSON object. Pass the 'spec' string returned by ExportSystemSpec."));
		return {};
	}
	const TSharedPtr<FJsonObject> SpecEmitters = Child(Root, TEXT("emitters"));
	if (!SpecEmitters.IsValid() && !Root->HasField(TEXT("system")) && !Root->HasField(TEXT("userParameters")))
	{
		ReportError(TEXT("SpecJson has none of 'system', 'userParameters', 'emitters' (the ExportSystemSpec format)."));
		return {};
	}

	FNiagaraMcpApplySpecResult Result;
	Result.System = System->GetPathName();
	Result.Mode = bReplace ? TEXT("Replace") : TEXT("Merge");
	Result.bDryRun = bDryRun;

	FErrorCapture Capture;
	FContext Ctx;
	Ctx.System = System;
	Ctx.bDryRun = bDryRun;
	Ctx.bReplace = bReplace;
	Ctx.Result = &Result;
	Ctx.Capture = &Capture;
	{
		FString SourcePath = Field(Child(Root, TEXT("system")), TEXT("path"));
		SourcePath.Split(TEXT("."), &Ctx.SourcePackage, nullptr);
	}

	// System properties.
	SyncProperties(Ctx, TEXT("system.properties."), Child(Child(Root, TEXT("system")), TEXT("properties")), System->GetClass(), System, System->GetClass()->GetDefaultObject(), bReplace,
		[](const FProperty*) { return false; }, TEXT("SetSystemProperty"),
		[&](const FString& Name, const FString& Value) { UNiagaraMcpEditorTools::SetSystemProperty(System, Name, Value, false); });

	// User parameters ("User.X": "Type = Value").
	{
		const TArray<FNiagaraMcpParameterInfo> Current = NiagaraMcpExt::DescribeUserParameters(*System);
		const TSharedPtr<FJsonObject> Spec = Child(Root, TEXT("userParameters"));
		TSet<FString> Seen;
		if (Spec.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Spec->Values)
			{
				const FString Text = JsonText(Pair.Value);
				FString Type, Value;
				if (!Text.Split(TEXT(" = "), &Type, &Value))
				{
					Type = Text;
				}
				Seen.Add(Pair.Key);
				const FString Path = TEXT("userParameters.") + Pair.Key;
				const FNiagaraMcpParameterInfo* Existing = Current.FindByPredicate([&Pair](const FNiagaraMcpParameterInfo& P) { return P.Name == Pair.Key; });
				if (Existing && Existing->Type == Type && Existing->Value == Value)
				{
					Ctx.Same();
					continue;
				}
				FNiagaraTypeDefinition TypeDef;
				const bool bObjectValue = ResolveNiagaraType(Type, TypeDef) && (TypeDef.IsDataInterface() || TypeDef.IsUObject());
				Ctx.Run(TEXT("SetUserParameter"), Path, Text, [&]() { UNiagaraMcpEditorTools::SetUserParameter(System, FName(*Pair.Key), Type, bObjectValue ? FString() : Value, false); });
			}
		}
		if (bReplace)
		{
			for (const FNiagaraMcpParameterInfo& Parameter : Current)
			{
				if (!Seen.Contains(Parameter.Name))
				{
					Ctx.Run(TEXT("RemoveUserParameter"), TEXT("userParameters.") + Parameter.Name, Parameter.Type, [&]() { UNiagaraMcpEditorTools::RemoveUserParameter(System, FName(*Parameter.Name), false); });
				}
			}
		}
	}

	// Emitters: create the missing ones first (event handlers may reference them), then sync each.
	TSet<FString> NewEmitters;
	TArray<FString> EmitterNames;
	if (SpecEmitters.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : SpecEmitters->Values)
		{
			EmitterNames.Add(Pair.Key);
		}
	}
	auto HasEmitter = [System](const FString& Name) { return System->GetEmitterHandles().ContainsByPredicate([&Name](const FNiagaraEmitterHandle& H) { return H.GetName().ToString() == Name; }); };
	for (const FString& Name : EmitterNames)
	{
		const TSharedPtr<FJsonObject> Spec = Child(SpecEmitters, Name);
		if (!Spec.IsValid())
		{
			Ctx.Skip(TEXT("AddEmitter"), TEXT("emitters.") + Name, TEXT("The spec entry is not a JSON object."));
			continue;
		}
		if (HasEmitter(Name))
		{
			continue;
		}
		if (!Spec->HasField(TEXT("simTarget")))
		{
			Ctx.Skip(TEXT("AddEmitter"), TEXT("emitters.") + Name, TEXT("Lightweight emitter: the spec carries no module data. Add it with AddLightweightEmitter and edit it with SetStatelessModuleProperty."));
			continue;
		}
		if (Ctx.Run(TEXT("AddEmitter"), TEXT("emitters.") + Name, TEXT("Minimal"), [&]() { UNiagaraMcpEditorTools::AddEmitter(System, FName(*Name), TEXT("Minimal"), false); }))
		{
			NewEmitters.Add(Name);
		}
	}
	if (bReplace)
	{
		TArray<FName> Extra;
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			if (!EmitterNames.Contains(Handle.GetName().ToString()))
			{
				Extra.Add(Handle.GetName());
			}
		}
		for (const FName Name : Extra)
		{
			Ctx.Run(TEXT("RemoveEmitter"), TEXT("emitters.") + Name.ToString(), Name.ToString(), [&]() { UNiagaraMcpEditorTools::RemoveEmitter(System, Name, false); });
		}
	}

	for (const FString& Name : EmitterNames)
	{
		const TSharedPtr<FJsonObject> Spec = Child(SpecEmitters, Name);
		const FString Prefix = TEXT("emitters.") + Name + TEXT(".");
		const FName Emitter(*Name);
		const bool bFull = bReplace || NewEmitters.Contains(Name);
		const FNiagaraEmitterHandle* Handle = System->GetEmitterHandles().FindByPredicate([&Name](const FNiagaraEmitterHandle& H) { return H.GetName().ToString() == Name; });
		if (Handle == nullptr)
		{
			if (bDryRun && NewEmitters.Contains(Name))
			{
				Ctx.Run(TEXT("SyncEmitter"), Prefix + TEXT("*"), TEXT("stacks, renderers, properties of the new emitter"), []() {});
			}
			continue;
		}
		if (IsLightweight(*Handle))
		{
			Ctx.Skip(TEXT("SyncEmitter"), Prefix + TEXT("*"), TEXT("Lightweight emitter: only the enabled state is applied."));
		}

		const bool bEnabled = Field(Spec, TEXT("enabled"), TEXT("true")) == TEXT("true");
		if (bEnabled != Handle->GetIsEnabled())
		{
			Ctx.Run(TEXT("SetEmitterEnabled"), Prefix + TEXT("enabled"), bEnabled ? TEXT("true") : TEXT("false"), [&]() { UNiagaraMcpEditorTools::SetEmitterEnabled(System, Emitter, bEnabled, false); });
		}
		else
		{
			Ctx.Same();
		}
		if (IsLightweight(*Handle))
		{
			continue;
		}

		const FString SimTarget = Field(Spec, TEXT("simTarget"));
		if (FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData())
		{
			const FString CurrentTarget = StaticEnum<ENiagaraSimTarget>()->GetNameStringByValue(static_cast<int64>(EmitterData->SimTarget));
			if (!SimTarget.IsEmpty() && SimTarget != CurrentTarget)
			{
				Ctx.Run(TEXT("SetEmitterProperty"), Prefix + TEXT("simTarget"), SimTarget, [&]() { UNiagaraMcpEditorTools::SetEmitterProperty(System, Emitter, TEXT("SimTarget"), SimTarget, false); });
			}
		}
		if (FVersionedNiagaraEmitterData* EmitterData = System->GetEmitterHandles().FindByPredicate([&Name](const FNiagaraEmitterHandle& H) { return H.GetName().ToString() == Name; })->GetEmitterData())
		{
			FStructOnScope Defaults(FVersionedNiagaraEmitterData::StaticStruct());
			SyncProperties(Ctx, Prefix + TEXT("properties."), Child(Spec, TEXT("properties")), FVersionedNiagaraEmitterData::StaticStruct(), EmitterData, Defaults.GetStructMemory(), bFull,
				[](const FProperty* Property) { return Property->GetFName() == TEXT("SimTarget"); }, TEXT("SetEmitterProperty"),
				[&](const FString& PropertyName, const FString& Value) { UNiagaraMcpEditorTools::SetEmitterProperty(System, Emitter, PropertyName, Value, false); });
		}
		SyncStages(Ctx, Emitter, Child(Spec, TEXT("simulationStages")), bFull, Prefix + TEXT("simulationStages."));
		SyncEventHandlers(Ctx, Emitter, Child(Spec, TEXT("eventHandlers")), bFull, Prefix + TEXT("eventHandlers."));
		SyncStacks(Ctx, Emitter, Child(Spec, TEXT("stacks")), bFull, Prefix + TEXT("stacks."));
		SyncRenderers(Ctx, Emitter, Child(Spec, TEXT("renderers")), bFull, Prefix + TEXT("renderers."));
	}

	// System stacks.
	if (Root->HasField(TEXT("systemStacks")) || bReplace)
	{
		SyncStacks(Ctx, NAME_None, Child(Root, TEXT("systemStacks")), bReplace, TEXT("systemStacks."));
	}

	if (!bDryRun)
	{
		Stack::FinishSystemEdit(*System, false);
		if (bSave)
		{
			Result.bSaved = SaveAssetSilently(System);
		}
	}
	if (Result.Failed > 0)
	{
		// A nested failure also reaches this tool's error handler; keep the whole result in the error text.
		FString Json;
		FJsonObjectConverter::UStructToJsonObjectString(Result, Json, 0, 0, 0, nullptr, false);
		ReportError(FString::Printf(TEXT("ApplySystemSpec: %d operation(s) failed, %d applied, %d skipped. Result: %s"), Result.Failed, Result.Applied, Result.Skipped, *Json.Left(20000)));
	}
	return Result;
}

// ------------------------------------------------------------------------------------------------ SaveEmitterAsAsset

FNiagaraMcpAssetResult UNiagaraMcpEditorTools::SaveEmitterAsAsset(UNiagaraSystem* System, FName EmitterName, const FString& NewAssetPath, bool bSave)
{
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	FNiagaraEmitterHandle* Handle = Stack::FindEmitter(*System, EmitterName);
	if (Handle == nullptr)
	{
		return {};
	}
	UNiagaraEmitter* Source = Handle->GetInstance().Emitter;
	if (Source == nullptr || NiagaraMcpAnalysis::IsLightweight(*Handle))
	{
		ReportError(FString::Printf(TEXT("Emitter '%s' is a lightweight emitter; only standard emitters can be saved as emitter assets."), *EmitterName.ToString()));
		return {};
	}
	// Modules from the system's own scratch pad would keep pointing into the system package.
	TArray<UObject*> Nodes;
	GetObjectsWithOuter(Source, Nodes, EGetObjectsFlags::IncludeNestedObjects);
	for (UObject* Object : Nodes)
	{
		if (const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Object))
		{
			if (Call->FunctionScript && Call->FunctionScript->IsIn(System) && !Call->FunctionScript->IsIn(Source))
			{
				ReportError(FString::Printf(TEXT("Emitter '%s' uses the system scratch pad module '%s', which cannot leave the system. Move it into the emitter or replace it with a module asset first."),
					*EmitterName.ToString(), *Call->FunctionScript->GetName()));
				return {};
			}
		}
	}
	FString AssetName;
	UPackage* Package = CreateNewAssetPackage(NewAssetPath, AssetName);
	if (Package == nullptr)
	{
		return {};
	}
	// Not transacted: undoing an asset creation leaves a broken asset behind.
	UNiagaraEmitter* Asset = Cast<UNiagaraEmitter>(StaticDuplicateObject(Source, Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional, UNiagaraEmitter::StaticClass()));
	if (Asset == nullptr)
	{
		ReportError(TEXT("Duplicating the emitter failed."));
		return {};
	}
	// The flag argument of StaticDuplicateObject only keeps flags the source has; a system's emitter has neither Public nor Standalone.
	Asset->ClearFlags(RF_Transient);
	Asset->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	for (const FNiagaraAssetVersion& Version : Asset->GetAllAvailableVersions())
	{
		if (FVersionedNiagaraEmitterData* Data = Asset->GetEmitterData(Version.VersionGuid))
		{
			Data->RemoveParent();
		}
	}
	Asset->SetUniqueEmitterName(AssetName);
	Asset->bIsInheritable = true;
	Asset->TemplateAssetDescription = FText();
	Asset->Category = FText();
	Package->SetAssetAccessSpecifier(EAssetAccessSpecifier::Public);
	FAssetRegistryModule::AssetCreated(Asset);
	Asset->PostEditChange();
	Package->MarkPackageDirty();

	FNiagaraMcpAssetResult Result;
	Result.Path = Asset->GetPathName();
	Result.Class = Asset->GetClass()->GetName();
	if (bSave)
	{
		Result.bSaved = SaveAssetSilently(Asset);
	}
	return Result;
}

// ------------------------------------------------------------------------------------------------ Lightweight emitters

namespace NiagaraMcpLightweight
{
	using namespace NiagaraMcpAnalysis;

	FNiagaraEmitterHandle* FindLightweight(UNiagaraSystem& System, FName EmitterName)
	{
		FNiagaraEmitterHandle* Handle = Stack::FindEmitter(System, EmitterName);
		if (Handle && !IsLightweight(*Handle))
		{
			ReportError(FString::Printf(TEXT("Emitter '%s' is a standard emitter; use the stack tools (GetStack, SetModuleInput). Lightweight emitters are added with AddLightweightEmitter."), *EmitterName.ToString()));
			return nullptr;
		}
		return Handle;
	}

	FString ModuleName(const UObject& Module)
	{
		FString Name = Module.GetClass()->GetName();
		Name.RemoveFromStart(TEXT("NiagaraStatelessModule_"));
		return Name;
	}

	const FProperty* EnabledProperty(const UObject& Module)
	{
		return FindFProperty<FProperty>(Module.GetClass(), TEXT("bModuleEnabled"));
	}

	FNiagaraMcpStatelessEmitterInfo Describe(const FNiagaraEmitterHandle& Handle)
	{
		FNiagaraMcpStatelessEmitterInfo Info;
		Info.Emitter = Handle.GetName();
		UObject* Emitter = Handle.GetEmitterBase();
		if (Emitter == nullptr)
		{
			return Info;
		}
		Info.Template = GetPropertyText(Emitter->GetClass(), Emitter, TEXT("EmitterTemplate"));
		for (UObject* Module : GetStatelessModules(Handle))
		{
			FNiagaraMcpStatelessModuleInfo& ModuleInfo = Info.Modules.AddDefaulted_GetRef();
			ModuleInfo.Name = ModuleName(*Module);
			ModuleInfo.DisplayName = Module->GetClass()->GetDisplayNameText().ToString();
			ModuleInfo.Class = Module->GetClass()->GetName();
			const FProperty* Enabled = EnabledProperty(*Module);
			ModuleInfo.bEnabled = Enabled ? GetPropertyText(Module->GetClass(), Module, TEXT("bModuleEnabled")) == TEXT("true") : true;
			ModuleInfo.bCanDisable = Enabled && Module->CanEditChange(Enabled);
			NiagaraMcpExt::DescribeEditableProperties(Module->GetClass(), Module, TEXT("Any"), ModuleInfo.Properties, [](const FProperty* Property) { return Property->GetFName() == TEXT("bDebugDrawEnabled"); });
		}
		NiagaraMcpExt::DescribeEditableProperties(Emitter->GetClass(), Emitter, TEXT("Any"), Info.EmitterProperties,
			[](const FProperty* Property) { return Property->GetFName() == TEXT("Modules") || Property->GetFName() == TEXT("RendererProperties"); });
		for (const UNiagaraRendererProperties* Renderer : GetRenderers(Handle))
		{
			Info.Renderers.Add(Renderer ? Renderer->GetClass()->GetName() : FString(TEXT("None")));
		}
		return Info;
	}
}

FNiagaraMcpEmitterInfo UNiagaraMcpEditorTools::AddLightweightEmitter(UNiagaraSystem* System, FName EmitterName, const FString& Template, bool bSave)
{
	if (!NiagaraMcpTools::CheckSystemArg(System) || !EnsureNoOpenEditor(System))
	{
		return {};
	}
	FString TemplatePath = IsUnset(Template) ? FString(TEXT("MinimalLightweight")) : Template.TrimStartAndEnd();
	for (const TCHAR* Known : { TEXT("MinimalLightweight"), TEXT("FountainLightweight"), TEXT("DirectionalBurstLightweight") })
	{
		if (TemplatePath.Equals(Known, ESearchCase::IgnoreCase))
		{
			TemplatePath = FString::Printf(TEXT("/Niagara/DefaultAssets/Templates/Systems/%s.%s"), Known, Known);
		}
	}
	UNiagaraSystem* TemplateSystem = LoadAssetByPath<UNiagaraSystem>(TemplatePath, TEXT("Template"));
	if (TemplateSystem == nullptr)
	{
		return {};
	}
	const FNiagaraEmitterHandle* Source = TemplateSystem->GetEmitterHandles().FindByPredicate([](const FNiagaraEmitterHandle& H) { return NiagaraMcpAnalysis::IsLightweight(H) && H.GetStatelessEmitter() != nullptr; });
	if (Source == nullptr)
	{
		ReportError(FString::Printf(TEXT("'%s' has no lightweight emitter. Use MinimalLightweight, FountainLightweight or DirectionalBurstLightweight."), *TemplatePath));
		return {};
	}
	FString BaseName = EmitterName.IsNone() ? Source->GetName().ToString() : EmitterName.ToString();
	FString Name = BaseName;
	auto Taken = [System](const FString& Candidate)
	{
		return System->GetEmitterHandles().ContainsByPredicate([&Candidate](const FNiagaraEmitterHandle& H) { return H.GetName().ToString().Equals(Candidate, ESearchCase::IgnoreCase); })
			|| StaticFindObjectFast(nullptr, System, FName(*Candidate)) != nullptr;
	};
	for (int32 Suffix = 1; Taken(Name); ++Suffix)
	{
		Name = FString::Printf(TEXT("%s%03d"), *BaseName, Suffix);
	}
	FGuid NewId;
	{
		FScopedTransaction Transaction(LOCTEXT("AddLightweightEmitter", "Niagara MCP: Add Lightweight Emitter"));
		System->Modify();
		NewId = System->DuplicateEmitterHandle(*Source, FName(*Name)).GetId();
	}
	System->RequestCompile(false);
	Stack::FinishSystemEdit(*System, bSave);
	const FNiagaraEmitterHandle* Added = NiagaraMcpExt::FindHandleById(*System, NewId);
	if (Added == nullptr)
	{
		ReportError(TEXT("Adding the lightweight emitter failed."));
		return {};
	}
	FNiagaraMcpEmitterInfo Info = NiagaraMcpExt::DescribeEmitterInfo(*Added);
	Info.SimTarget = TEXT("Lightweight");
	const TArray<UNiagaraRendererProperties*> Renderers = NiagaraMcpAnalysis::GetRenderers(*Added);
	for (int32 Index = 0; Index < Renderers.Num(); ++Index)
	{
		FNiagaraMcpRendererInfo& Renderer = Info.Renderers.AddDefaulted_GetRef();
		Renderer.Index = Index;
		if (Renderers[Index])
		{
			Renderer.Class = Renderers[Index]->GetClass()->GetName();
			Renderer.bEnabled = Renderers[Index]->GetIsEnabled();
			Renderer.Material = GetPropertyText(Renderers[Index]->GetClass(), Renderers[Index], TEXT("Material"));
		}
	}
	return Info;
}

FNiagaraMcpStatelessEmitterInfo UNiagaraMcpEditorTools::GetStatelessEmitterInfo(UNiagaraSystem* System, FName EmitterName)
{
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	const FNiagaraEmitterHandle* Handle = NiagaraMcpLightweight::FindLightweight(*System, EmitterName);
	return Handle ? NiagaraMcpLightweight::Describe(*Handle) : FNiagaraMcpStatelessEmitterInfo();
}

FNiagaraMcpPropertyResult UNiagaraMcpEditorTools::SetStatelessModuleProperty(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& PropertyName, const FString& Value, bool bSave)
{
	using namespace NiagaraMcpLightweight;
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	FNiagaraEmitterHandle* Handle = FindLightweight(*System, EmitterName);
	UObject* Emitter = Handle ? Handle->GetEmitterBase() : nullptr;
	if (Emitter == nullptr)
	{
		return {};
	}
	UObject* Target = nullptr;
	const FString ModuleText = Module.TrimStartAndEnd();
	if (ModuleText.Equals(TEXT("Emitter"), ESearchCase::IgnoreCase))
	{
		Target = Emitter;
	}
	else
	{
		TArray<FString> Names;
		for (UObject* Candidate : NiagaraMcpAnalysis::GetStatelessModules(*Handle))
		{
			const FString Name = ModuleName(*Candidate);
			Names.Add(Name);
			if (LooseNameEquals(Name, ModuleText) || LooseNameEquals(Candidate->GetClass()->GetName(), ModuleText) || LooseNameEquals(Candidate->GetClass()->GetDisplayNameText().ToString(), ModuleText))
			{
				Target = Candidate;
			}
		}
		if (Target == nullptr)
		{
			ReportError(FString::Printf(TEXT("Unknown lightweight module '%s'.%s Modules: %s, or Emitter for the emitter itself."), *Module, *DidYouMean(ModuleText, Names), *FString::Join(Names, TEXT(", "))));
			return {};
		}
	}
	if (PropertyName.TrimStartAndEnd().Equals(TEXT("bModuleEnabled"), ESearchCase::IgnoreCase) && Target != Emitter)
	{
		const FProperty* Enabled = EnabledProperty(*Target);
		if (Enabled && !Target->CanEditChange(Enabled))
		{
			ReportError(FString::Printf(TEXT("Module '%s' cannot be disabled."), *ModuleName(*Target)));
			return {};
		}
	}
	// Validate on a scratch copy first: nothing changes on a bad name or value.
	FString Error;
	if (!NiagaraMcpExt::DryRunSetProperty(Target->GetClass(), Target, Target, PropertyName, Value, Error))
	{
		ReportError(Error);
		return {};
	}
	FProperty* Changed = nullptr;
	{
		FScopedTransaction Transaction(LOCTEXT("SetStatelessModuleProperty", "Niagara MCP: Set Lightweight Module Property"));
		System->Modify();
		Emitter->Modify();
		Target->Modify();
		Changed = SetPropertyFromText(Target->GetClass(), Target, Target, PropertyName, Value, Error);
		if (Changed == nullptr)
		{
			Transaction.Cancel();
			ReportError(Error);
			return {};
		}
		FPropertyChangedEvent TargetEvent(Changed, EPropertyChangeType::ValueSet);
		Target->PostEditChangeProperty(TargetEvent);
		if (Target != Emitter)
		{
			// The emitter rebuilds its runtime data from the modules and resets running instances.
			FPropertyChangedEvent EmitterEvent(FindFProperty<FProperty>(Emitter->GetClass(), TEXT("Modules")), EPropertyChangeType::ValueSet);
			Emitter->PostEditChangeProperty(EmitterEvent);
		}
	}
	{
		FNiagaraSystemUpdateContext UpdateContext(System, true);
		System->UpdateScalability();
	}
	Stack::FinishSystemEdit(*System, bSave);
	FNiagaraMcpPropertyResult Result;
	Result.Property = PropertyName;
	Result.Value = GetPropertyText(Target->GetClass(), Target, PropertyName);
	Result.bSaved = bSave;
	return Result;
}

// ------------------------------------------------------------------------------------------------ Data channels

namespace NiagaraMcpDataChannels
{
	UClass* ChannelClass(const FString& Type)
	{
		const FString Name = IsUnset(Type) ? FString(TEXT("Islands")) : Type.TrimStartAndEnd();
		for (const TCHAR* Known : { TEXT("Islands"), TEXT("Global"), TEXT("GameplayBurst") })
		{
			if (Name.Equals(Known, ESearchCase::IgnoreCase) || Name.Equals(FString(TEXT("NiagaraDataChannel_")) + Known, ESearchCase::IgnoreCase))
			{
				return FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Niagara.NiagaraDataChannel_%s"), Known));
			}
		}
		return nullptr;
	}

	FNiagaraMcpDataChannelInfo Describe(const UNiagaraDataChannelAsset& Asset)
	{
		FNiagaraMcpDataChannelInfo Info;
		Info.Path = Asset.GetPathName();
		const UNiagaraDataChannel* Channel = Asset.Get();
		if (Channel == nullptr)
		{
			return Info;
		}
		Info.Type = Channel->GetClass()->GetName();
		Info.Type.RemoveFromStart(TEXT("NiagaraDataChannel_"));
		for (const FNiagaraDataChannelVariable& Variable : Channel->GetVariables())
		{
			FNiagaraMcpVariableDecl& Decl = Info.Variables.AddDefaulted_GetRef();
			Decl.Name = Variable.GetName();
			Decl.Type = Variable.GetType().GetName();
		}
		NiagaraMcpExt::DescribeEditableProperties(Channel->GetClass(), Channel, TEXT("Any"), Info.Properties, [](const FProperty* Property) { return Property->GetFName() == TEXT("ChannelVariables"); });
		return Info;
	}
}

FNiagaraMcpDataChannelInfo UNiagaraMcpEditorTools::CreateNiagaraDataChannel(const FString& AssetPath, const TArray<FNiagaraMcpVariableDecl>& Variables, const FString& Type, bool bSave)
{
	using namespace NiagaraMcpDataChannels;
	if (!INiagaraModule::DataChannelsEnabled())
	{
		ReportError(TEXT("Niagara Data Channels are disabled (fx.Niagara.DataChannels.Enabled 0)."));
		return {};
	}
	UClass* Class = ChannelClass(Type);
	if (Class == nullptr)
	{
		ReportError(FString::Printf(TEXT("Unknown data channel Type '%s'. Use Islands, Global or GameplayBurst."), *Type));
		return {};
	}
	// Validate every variable before creating anything.
	TArray<FNiagaraDataChannelVariable> ChannelVariables;
	for (const FNiagaraMcpVariableDecl& Decl : Variables)
	{
		if (Decl.Name.IsNone())
		{
			ReportError(TEXT("Every variable needs a Name."));
			return {};
		}
		FNiagaraTypeDefinition TypeDef;
		if (!ResolveNiagaraType(Decl.Type, TypeDef))
		{
			return {};
		}
		if (!FNiagaraDataChannelVariable::IsAllowedType(TypeDef))
		{
			ReportError(FString::Printf(TEXT("Type '%s' of '%s' cannot be used in a data channel (data interfaces, objects, half and matrix types are not allowed)."), *Decl.Type, *Decl.Name.ToString()));
			return {};
		}
		if (ChannelVariables.ContainsByPredicate([&Decl](const FNiagaraDataChannelVariable& V) { return V.GetName() == Decl.Name; }))
		{
			ReportError(FString::Printf(TEXT("Variable '%s' is listed twice."), *Decl.Name.ToString()));
			return {};
		}
		FNiagaraDataChannelVariable& Variable = ChannelVariables.AddDefaulted_GetRef();
		Variable.SetName(Decl.Name);
		Variable.SetType(FNiagaraDataChannelVariable::ToDataChannelType(TypeDef));
	}
	FString AssetName;
	UPackage* Package = CreateNewAssetPackage(AssetPath, AssetName);
	if (Package == nullptr)
	{
		return {};
	}
	// Not transacted: undoing an asset creation leaves a broken asset behind.
	UNiagaraDataChannelAsset* Asset = NewObject<UNiagaraDataChannelAsset>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	UNiagaraDataChannel* Channel = NewObject<UNiagaraDataChannel>(Asset, Class, NAME_None, RF_Transactional);
	if (FObjectPropertyBase* ChannelProperty = FindFProperty<FObjectPropertyBase>(UNiagaraDataChannelAsset::StaticClass(), TEXT("DataChannel")))
	{
		ChannelProperty->SetObjectPropertyValue_InContainer(Asset, Channel);
	}
	if (FArrayProperty* VariablesProperty = FindFProperty<FArrayProperty>(UNiagaraDataChannel::StaticClass(), TEXT("ChannelVariables")))
	{
		Channel->PreEditChange(VariablesProperty);
		VariablesProperty->CopyCompleteValue(VariablesProperty->ContainerPtrToValuePtr<void>(Channel), &ChannelVariables);
		FPropertyChangedEvent Event(VariablesProperty, EPropertyChangeType::ValueSet);
		Channel->PostEditChangeProperty(Event);
	}
	FAssetRegistryModule::AssetCreated(Asset);
	Package->MarkPackageDirty();
	FNiagaraMcpDataChannelInfo Info = Describe(*Asset);
	if (Asset->Get() == nullptr)
	{
		ReportError(TEXT("The data channel object could not be attached to the asset."));
		return Info;
	}
	if (bSave)
	{
		Info.bSaved = SaveAssetSilently(Asset);
	}
	return Info;
}

FNiagaraMcpDataChannelInfo UNiagaraMcpEditorTools::GetNiagaraDataChannel(UNiagaraDataChannelAsset* DataChannel)
{
	if (DataChannel == nullptr)
	{
		ReportError(TEXT("DataChannel is null or could not be loaded. Pass an asset path such as /Game/FX/NDC_Impacts.NDC_Impacts."));
		return {};
	}
	return NiagaraMcpDataChannels::Describe(*DataChannel);
}

// ------------------------------------------------------------------------------------------------ RecolorEffect

namespace NiagaraMcpRecolor
{
	struct FColorOp
	{
		FLinearColor Tint = FLinearColor::White;
		bool bTint = false;
		float Hue = 0.0f;
		bool bHue = false;

		FLinearColor Apply(const FLinearColor& In) const
		{
			FLinearColor Out = In;
			if (bHue)
			{
				FLinearColor Hsv = In.LinearRGBToHSV();
				Hsv.R = FMath::Fmod(Hsv.R + Hue + 3600.0f, 360.0f);
				Out = Hsv.HSVToLinearRGB();
				Out.A = In.A;
			}
			if (bTint)
			{
				Out.R *= Tint.R;
				Out.G *= Tint.G;
				Out.B *= Tint.B;
			}
			return Out;
		}
	};

	FString ColorText(const FLinearColor& C)
	{
		return FString::Printf(TEXT("%g, %g, %g, %g"), C.R, C.G, C.B, C.A);
	}

	bool ParseColor(const FString& Text, FLinearColor& Out)
	{
		const TArray<double> Values = NiagaraMcpAnalysis::ParseNumbers(Text.Replace(TEXT("("), TEXT("")).Replace(TEXT(")"), TEXT("")).Replace(TEXT("R="), TEXT("")).Replace(TEXT("G="), TEXT("")).Replace(TEXT("B="), TEXT("")).Replace(TEXT("A="), TEXT("")));
		if (Values.Num() < 3)
		{
			return false;
		}
		Out = FLinearColor(Values[0], Values[1], Values[2], Values.Num() > 3 ? Values[3] : 1.0f);
		return true;
	}

	struct FPlannedEdit
	{
		FName Emitter;
		FString ModuleId;
		FString ModuleName;
		FString Input;
		bool bCurve = false;
		FString Value;
	};
}

FNiagaraMcpRecolorResult UNiagaraMcpEditorTools::RecolorEffect(UNiagaraSystem* System, const FString& Mode, float HueShiftDegrees, const FString& Tint, FName EmitterName, bool bSave)
{
	using namespace NiagaraMcpRecolor;
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	const FString ModeText = IsUnset(Mode) ? FString(TEXT("HueShift")) : Mode.TrimStartAndEnd();
	FColorOp Op;
	if (ModeText.Equals(TEXT("HueShift"), ESearchCase::IgnoreCase))
	{
		Op.bHue = !FMath::IsNearlyZero(FMath::Fmod(HueShiftDegrees, 360.0f));
		Op.Hue = HueShiftDegrees;
	}
	else if (!ModeText.Equals(TEXT("Tint"), ESearchCase::IgnoreCase))
	{
		ReportError(FString::Printf(TEXT("Unknown Mode '%s'. Use HueShift or Tint."), *Mode));
		return {};
	}
	if (!IsUnset(Tint))
	{
		if (!ParseColor(Tint, Op.Tint))
		{
			ReportError(FString::Printf(TEXT("Tint '%s' is not a color; use \"r, g, b\" (linear), e.g. \"1, 0.6, 0.6\"."), *Tint));
			return {};
		}
		Op.bTint = true;
	}
	if (!Op.bHue && !Op.bTint)
	{
		ReportError(TEXT("Nothing to do: pass HueShiftDegrees (Mode HueShift) and/or a Tint."));
		return {};
	}
	const bool bAll = Stack::IsSystemName(EmitterName);
	if (!bAll && Stack::FindEmitter(*System, EmitterName) == nullptr)
	{
		return {};
	}

	FNiagaraMcpRecolorResult Result;
	Result.System = System->GetPathName();

	// Plan from the stacks first (the tool calls below rebuild view models).
	TArray<FPlannedEdit> Plan;
	{
		TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(*System);
		if (!ViewModel.IsValid())
		{
			return {};
		}
		auto Scan = [&](UNiagaraStackViewModel* StackViewModel, FName Emitter)
		{
			if (StackViewModel == nullptr)
			{
				return;
			}
			TArray<UNiagaraStackModuleItem*> Items;
			Stack::CollectModuleItems(*StackViewModel, Items);
			for (UNiagaraStackModuleItem* Item : Items)
			{
				TArray<UNiagaraStackFunctionInput*> Inputs;
				NiagaraMcpAnalysis::CollectInputsRecursive(*Item, Inputs, 0);
				for (UNiagaraStackFunctionInput* Input : Inputs)
				{
					const UNiagaraStackFunctionInput::EValueMode ValueMode = Input->GetValueMode();
					const bool bColor = Input->GetInputType() == FNiagaraTypeDefinition::GetColorDef() && ValueMode == UNiagaraStackFunctionInput::EValueMode::Local;
					const UNiagaraDataInterface* DataInterface = ValueMode == UNiagaraStackFunctionInput::EValueMode::Data ? Input->GetDataValueObject() : nullptr;
					const bool bCurve = DataInterface && DataInterface->GetClass()->GetName() == TEXT("NiagaraDataInterfaceColorCurve");
					if (!bColor && !bCurve)
					{
						continue;
					}
					FPlannedEdit& Edit = Plan.AddDefaulted_GetRef();
					Edit.Emitter = Emitter;
					Edit.ModuleId = Stack::GetModuleId(*Item);
					Edit.ModuleName = Item->GetDisplayName().ToString();
					Edit.Input = NiagaraMcpAnalysis::InputPath(*Input, *Item);
					Edit.bCurve = bCurve;
					Edit.Value = bColor ? Stack::InputValueToString(*Input) : FString();
				}
			}
		};
		if (bAll)
		{
			Scan(ViewModel->GetSystemStackViewModel(), NAME_None);
		}
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			if ((bAll || Handle.GetName() == EmitterName) && !NiagaraMcpAnalysis::IsLightweight(Handle))
			{
				Scan(Stack::GetStackViewModel(*ViewModel, Handle.GetName()), Handle.GetName());
			}
		}
	}

	NiagaraMcpAnalysis::FErrorCapture Capture;
	for (const FPlannedEdit& Edit : Plan)
	{
		FNiagaraMcpColorChange Change;
		Change.Emitter = Edit.Emitter;
		if (!Edit.bCurve)
		{
			FLinearColor Before;
			if (!ParseColor(Edit.Value, Before))
			{
				continue;
			}
			const FLinearColor After = Op.Apply(Before);
			if (After.Equals(Before, 1e-4f))
			{
				continue;
			}
			Change.Kind = TEXT("ModuleInput");
			Change.Target = Edit.ModuleName + TEXT(".") + Edit.Input;
			Change.Before = ColorText(Before);
			Change.After = ColorText(After);
			const int32 Start = Capture.Num();
			UNiagaraMcpEditorTools::SetModuleInput(System, Edit.Emitter, Edit.ModuleId, Edit.Input, Change.After, TEXT("Any"), false);
			Change.bChanged = Capture.Num() == Start;
		}
		else
		{
			const FNiagaraMcpCurveInfo Curve = UNiagaraMcpEditorTools::GetCurveKeys(System, Edit.Emitter, Edit.ModuleId, Edit.Input, TEXT("Any"));
			if (Curve.Keys.Num() == 0)
			{
				continue;
			}
			TArray<FNiagaraMcpCurveKey> Keys = Curve.Keys;
			TArray<FString> Before, After;
			for (FNiagaraMcpCurveKey& Key : Keys)
			{
				FLinearColor Color;
				if (ParseColor(Key.Value, Color))
				{
					Before.Add(ColorText(Color));
					Key.Value = ColorText(Op.Apply(Color));
					After.Add(Key.Value);
				}
			}
			Change.Kind = TEXT("CurveKeys");
			Change.Target = FString::Printf(TEXT("%s.%s (%d keys)"), *Edit.ModuleName, *Edit.Input, Keys.Num());
			Change.Before = FString::Join(Before, TEXT(" | "));
			Change.After = FString::Join(After, TEXT(" | "));
			const FString Interpolation = Curve.Interpolation == TEXT("Mixed") || Curve.Interpolation.IsEmpty() ? FString(TEXT("Cubic")) : Curve.Interpolation;
			const int32 Start = Capture.Num();
			UNiagaraMcpEditorTools::SetCurveKeys(System, Edit.Emitter, Edit.ModuleId, Edit.Input, Keys, Interpolation, TEXT("Any"), false);
			Change.bChanged = Capture.Num() == Start;
		}
		Result.Changed += Change.bChanged ? 1 : 0;
		Result.Changes.Add(MoveTemp(Change));
	}

	// User color parameters (whole-effect recolor only).
	if (bAll)
	{
		for (const FNiagaraMcpParameterInfo& Parameter : NiagaraMcpExt::DescribeUserParameters(*System))
		{
			FLinearColor Before;
			if (Parameter.Type != FNiagaraTypeDefinition::GetColorDef().GetName() || !ParseColor(Parameter.Value, Before))
			{
				continue;
			}
			const FLinearColor After = Op.Apply(Before);
			if (After.Equals(Before, 1e-4f))
			{
				continue;
			}
			FNiagaraMcpColorChange& Change = Result.Changes.AddDefaulted_GetRef();
			Change.Kind = TEXT("UserParameter");
			Change.Target = Parameter.Name;
			Change.Before = ColorText(Before);
			Change.After = ColorText(After);
			const int32 Start = Capture.Num();
			UNiagaraMcpEditorTools::SetUserParameter(System, FName(*Parameter.Name), TEXT("color"), Change.After, false);
			Change.bChanged = Capture.Num() == Start;
			Result.Changed += Change.bChanged ? 1 : 0;
		}
	}

	// Material colors: reported only (materials are shared assets).
	TSet<const UMaterialInterface*> Reported;
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		if (!bAll && Handle.GetName() != EmitterName)
		{
			continue;
		}
		for (const UNiagaraRendererProperties* Renderer : NiagaraMcpAnalysis::GetRenderers(Handle))
		{
			if (Renderer == nullptr)
			{
				continue;
			}
			TArray<UMaterialInterface*> Materials;
			Renderer->GetUsedMaterials(nullptr, Materials);
			for (const UMaterialInterface* Material : Materials)
			{
				if (Material == nullptr || Reported.Contains(Material))
				{
					continue;
				}
				Reported.Add(Material);
				TArray<FMaterialParameterInfo> Infos;
				TArray<FGuid> Ids;
				Material->GetAllVectorParameterInfo(Infos, Ids);
				for (const FMaterialParameterInfo& Info : Infos)
				{
					FLinearColor Value;
					if (!Material->GetVectorParameterValue(Info, Value))
					{
						continue;
					}
					FNiagaraMcpColorChange& Change = Result.Changes.AddDefaulted_GetRef();
					Change.Kind = TEXT("MaterialParameter");
					Change.Emitter = Handle.GetName();
					Change.Target = FString::Printf(TEXT("%s %s"), *Material->GetPathName(), *Info.Name.ToString());
					Change.Before = ColorText(Value);
					Change.After = ColorText(Op.Apply(Value));
					Change.bChanged = false;
				}
			}
		}
	}

	if (Result.Changed > 0)
	{
		Stack::FinishSystemEdit(*System, false);
		if (bSave)
		{
			Result.bSaved = SaveAssetSilently(System);
		}
	}
	const int32 Failed = Result.Changes.FilterByPredicate([](const FNiagaraMcpColorChange& C) { return !C.bChanged && C.Kind != TEXT("MaterialParameter"); }).Num();
	if (Failed > 0)
	{
		// A nested failure also reaches this tool's error handler; keep the whole result in the error text.
		FString Json;
		FJsonObjectConverter::UStructToJsonObjectString(Result, Json, 0, 0, 0, nullptr, false);
		ReportError(FString::Printf(TEXT("RecolorEffect: %d color(s) could not be changed (%s). Result: %s"), Failed, *Capture.TakeSince(0).Left(600), *Json.Left(20000)));
	}
	return Result;
}

#undef LOCTEXT_NAMESPACE
