// v0.4 extensions: system / emitter / renderer properties and bindings, module order, emitter enable and duplicate,
// simulation stage and event handler editing, user parameter management.

#include "NiagaraMcpEditorTools.h"

#include "NiagaraCommon.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraExternalSystemEditorUtilities.h"
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
#include "NiagaraUserRedirectionParameterStore.h"
#include "ScopedTransaction.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackEventScriptItemGroup.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackScriptItemGroup.h"
#include "ViewModels/Stack/NiagaraStackSimulationStageGroup.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

#define LOCTEXT_NAMESPACE "NiagaraMcpEditorTools"

using namespace NiagaraMcp;

namespace NiagaraMcpExtA
{
	/** Top-level property of a (possibly dotted) path, for PreEditChange. */
	FProperty* TopLevelProperty(const UStruct* Struct, const FString& PropertyPath)
	{
		FString First = PropertyPath;
		PropertyPath.Split(TEXT("."), &First, nullptr);
		return NiagaraMcpExt::FindPropertyLoose(Struct, First.TrimStartAndEnd());
	}

	/** Validates, then applies a text value to an object property inside one transaction already opened by the caller. */
	FProperty* ValidateAndSet(UObject& Object, const FString& PropertyName, const FString& Value)
	{
		FString Error;
		if (!NiagaraMcpExt::DryRunSetProperty(Object.GetClass(), &Object, &Object, PropertyName, Value, Error))
		{
			ReportError(Error);
			return nullptr;
		}
		return TopLevelProperty(Object.GetClass(), PropertyName);
	}

	bool IsBindingStruct(const FProperty* Property)
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

	/** Attribute binding properties (FNiagaraVariableAttributeBinding members) of a renderer class. */
	TArray<FStructProperty*> BindingProperties(const UClass* Class)
	{
		TArray<FStructProperty*> Result;
		for (TFieldIterator<FStructProperty> It(Class); It; ++It)
		{
			if (It->Struct == FNiagaraVariableAttributeBinding::StaticStruct() && !It->HasAnyPropertyFlags(CPF_Deprecated))
			{
				Result.Add(*It);
			}
		}
		return Result;
	}

	void CollectMaterialBindings(UNiagaraRendererProperties& Renderer, TArray<FNiagaraMcpMaterialBindingInfo>& Out)
	{
		auto AddBinding = [&Out](const FNiagaraMaterialAttributeBinding& Binding)
		{
			FNiagaraMcpMaterialBindingInfo& Info = Out.AddDefaulted_GetRef();
			Info.MaterialParameter = Binding.MaterialParameterName.ToString();
			Info.Variable = Binding.NiagaraVariable.GetName().ToString();
			if (!Binding.NiagaraChildVariable.GetName().IsNone())
			{
				Info.Variable += TEXT(" / ") + Binding.NiagaraChildVariable.GetName().ToString();
			}
		};
		for (TFieldIterator<FProperty> It(Renderer.GetClass()); It; ++It)
		{
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(*It); StructProperty && StructProperty->Struct == FNiagaraRendererMaterialParameters::StaticStruct())
			{
				const FNiagaraRendererMaterialParameters* Parameters = StructProperty->ContainerPtrToValuePtr<FNiagaraRendererMaterialParameters>(&Renderer);
				for (const FNiagaraMaterialAttributeBinding& Binding : Parameters->AttributeBindings)
				{
					AddBinding(Binding);
				}
			}
			else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(*It))
			{
				const FStructProperty* Inner = CastField<FStructProperty>(ArrayProperty->Inner);
				if (Inner && Inner->Struct == FNiagaraMaterialAttributeBinding::StaticStruct())
				{
					FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(&Renderer));
					for (int32 Index = 0; Index < Helper.Num(); ++Index)
					{
						AddBinding(*reinterpret_cast<const FNiagaraMaterialAttributeBinding*>(Helper.GetRawPtr(Index)));
					}
				}
			}
		}
	}

	/** Variables a renderer of this emitter could bind: user parameters plus everything the last compile of the emitter and system scripts used. */
	TMap<FName, FNiagaraTypeDefinition> CollectBindableVariables(UNiagaraSystem& System, FNiagaraEmitterHandle& Handle)
	{
		TMap<FName, FNiagaraTypeDefinition> Result;
		auto AddVariable = [&Result](const FNiagaraVariableBase& Variable, bool bParticleAttribute)
		{
			FString Name = Variable.GetName().ToString();
			if (bParticleAttribute && !Name.Contains(TEXT(".")))
			{
				Name = TEXT("Particles.") + Name;
			}
			if (!Name.IsEmpty() && Variable.GetType().IsValid() && !Variable.GetType().IsDataInterface())
			{
				Result.Add(FName(*Name), Variable.GetType());
			}
		};
		TArray<FNiagaraVariable> UserParameters;
		System.GetExposedParameters().GetUserParameters(UserParameters);
		for (const FNiagaraVariable& Parameter : UserParameters)
		{
			const FString Name = Parameter.GetName().ToString();
			AddVariable(FNiagaraVariableBase(Parameter.GetType(), FName(*(Name.StartsWith(TEXT("User.")) ? Name : TEXT("User.") + Name))), false);
		}

		TArray<UNiagaraScript*> Scripts;
		if (FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData())
		{
			EmitterData->GetScripts(Scripts, false);
			if (UNiagaraScript* GpuScript = EmitterData->GetGPUComputeScript())
			{
				Scripts.AddUnique(GpuScript);
			}
		}
		Scripts.AddUnique(System.GetSystemSpawnScript());
		Scripts.AddUnique(System.GetSystemUpdateScript());
		for (const UNiagaraScript* Script : Scripts)
		{
			if (Script == nullptr)
			{
				continue;
			}
			const FNiagaraVMExecutableData& ExeData = Script->GetVMExecutableData();
			const bool bParticleScript = Script->IsParticleScript() || Script->IsGPUScript();
			if (bParticleScript)
			{
				for (const FNiagaraVariableBase& Attribute : ExeData.Attributes)
				{
					AddVariable(Attribute, true);
				}
			}
			for (const FNiagaraVariable& Parameter : ExeData.Parameters.Parameters)
			{
				AddVariable(Parameter, false);
			}
		}
		return Result;
	}

	/** Script item group of a stack (standard stacks: usage id is the zero guid). */
	UNiagaraStackScriptItemGroup* FindScriptGroup(UNiagaraStackEntry* Entry, ENiagaraScriptUsage Usage, const FGuid& UsageId, int32 Depth)
	{
		if (Entry == nullptr || Depth > 4)
		{
			return nullptr;
		}
		if (UNiagaraStackScriptItemGroup* Group = Cast<UNiagaraStackScriptItemGroup>(Entry))
		{
			if (Group->GetScriptUsage() == Usage && Group->GetScriptUsageId() == UsageId)
			{
				return Group;
			}
		}
		TArray<UNiagaraStackEntry*> Children;
		Entry->GetUnfilteredChildren(Children);
		for (UNiagaraStackEntry* Child : Children)
		{
			if (Child && !Child->IsA<UNiagaraStackModuleItem>())
			{
				if (UNiagaraStackScriptItemGroup* Found = FindScriptGroup(Child, Usage, UsageId, Depth + 1))
				{
					return Found;
				}
			}
		}
		return nullptr;
	}

	template<typename TGroup>
	void CollectGroups(UNiagaraStackEntry* Entry, TArray<TGroup*>& Out, int32 Depth)
	{
		if (Entry == nullptr || Depth > 4)
		{
			return;
		}
		if (TGroup* Group = Cast<TGroup>(Entry))
		{
			Out.Add(Group);
			return;
		}
		TArray<UNiagaraStackEntry*> Children;
		Entry->GetUnfilteredChildren(Children);
		for (UNiagaraStackEntry* Child : Children)
		{
			if (Child && !Child->IsA<UNiagaraStackModuleItem>())
			{
				CollectGroups(Child, Out, Depth + 1);
			}
		}
	}

	FGuid UsageIdOf(const UNiagaraStackModuleItem& Item)
	{
		const UNiagaraNodeOutput* OutputNode = Item.GetOutputNode();
		return OutputNode ? OutputNode->GetUsageId() : FGuid();
	}

	FNiagaraMcpModuleInfo DescribeModuleByGuid(UNiagaraSystem& System, FName EmitterName, const FGuid& NodeGuid)
	{
		TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(System);
		UNiagaraStackViewModel* StackViewModel = ViewModel.IsValid() ? Stack::GetStackViewModel(*ViewModel, EmitterName) : nullptr;
		if (StackViewModel == nullptr)
		{
			return {};
		}
		TArray<UNiagaraStackModuleItem*> Items;
		Stack::CollectModuleItems(*StackViewModel, Items);
		TMap<TPair<ENiagaraScriptUsage, FGuid>, int32> Counters;
		for (UNiagaraStackModuleItem* Item : Items)
		{
			int32& Counter = Counters.FindOrAdd(TPair<ENiagaraScriptUsage, FGuid>(Stack::GetModuleUsage(*Item), UsageIdOf(*Item)));
			if (Item->GetModuleNode().NodeGuid == NodeGuid)
			{
				return NiagaraMcpTools::DescribeModule(*Item, Counter, true);
			}
			++Counter;
		}
		ReportError(TEXT("The module could not be found after the edit."));
		return {};
	}

	/**
	 * Moves or copies a module through the stack's drag-and-drop path (UNiagaraStackScriptItemGroup::DropEntriesOnTarget,
	 * which runs FNiagaraStackGraphUtilities::MoveModule): overrides, rapid iteration values and dynamic inputs travel
	 * with the module.
	 */
	FNiagaraMcpModuleInfo MoveOrCopyModule(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& InStackName, const FString& InTargetStackName, int32 TargetIndex, bool bSave, bool bCopy)
	{
		const FString StackName = NiagaraMcpExt::ArgOrEmpty(InStackName);
		const FString TargetStackName = NiagaraMcpExt::ArgOrEmpty(InTargetStackName);
		if (!NiagaraMcpTools::CheckSystemArg(System))
		{
			return {};
		}
		TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(*System);
		UNiagaraStackViewModel* StackViewModel = ViewModel.IsValid() ? Stack::GetStackViewModel(*ViewModel, EmitterName) : nullptr;
		UNiagaraStackModuleItem* Item = StackViewModel ? Stack::FindModuleItem(*StackViewModel, Module, StackName) : nullptr;
		if (Item == nullptr)
		{
			return {};
		}
		const ENiagaraScriptUsage SourceUsage = Stack::GetModuleUsage(*Item);
		const FGuid SourceUsageId = UsageIdOf(*Item);
		ENiagaraScriptUsage TargetUsage = SourceUsage;
		FGuid TargetUsageId = SourceUsageId;
		if (!TargetStackName.TrimStartAndEnd().IsEmpty())
		{
			if (!ParseUsage(TargetStackName, TargetUsage))
			{
				return {};
			}
			if (!IsStandardStack(TargetUsage) || IsSystemStack(TargetUsage) != Stack::IsSystemName(EmitterName))
			{
				ReportError(Stack::IsSystemName(EmitterName)
					? TEXT("TargetStackName must be SystemSpawn or SystemUpdate for system modules.")
					: TEXT("TargetStackName must be EmitterSpawn, EmitterUpdate, ParticleSpawn or ParticleUpdate (modules move within one emitter)."));
				return {};
			}
			if (TargetUsage != SourceUsage)
			{
				TargetUsageId = FGuid();
			}
		}
		if (!bCopy && !Item->CanMoveAndDelete())
		{
			ReportError(FString::Printf(TEXT("Module '%s' is inherited from the parent emitter and cannot be moved. Use DuplicateModule to copy it."), *Item->GetDisplayName().ToString()));
			return {};
		}
		if (const FVersionedNiagaraScriptData* ScriptData = Item->GetModuleNode().GetScriptData())
		{
			if (TargetUsage != SourceUsage && !ScriptData->GetSupportedUsageContexts().ContainsByPredicate([TargetUsage](ENiagaraScriptUsage Usage) { return UNiagaraScript::IsEquivalentUsage(TargetUsage, Usage); }))
			{
				ReportError(FString::Printf(TEXT("Module '%s' is not allowed in %s stacks."), *Item->GetDisplayName().ToString(), *UsageToString(TargetUsage)));
				return {};
			}
		}

		UNiagaraStackScriptItemGroup* TargetGroup = FindScriptGroup(StackViewModel->GetRootEntry(), TargetUsage, TargetUsageId, 0);
		if (TargetGroup == nullptr)
		{
			ReportError(FString::Printf(TEXT("Stack %s was not found in '%s'."), *UsageToString(TargetUsage), *EmitterName.ToString()));
			return {};
		}

		TArray<UNiagaraStackModuleItem*> AllItems;
		Stack::CollectModuleItems(*StackViewModel, AllItems);
		TArray<UNiagaraStackModuleItem*> TargetItems;
		TSet<FGuid> NodeGuidsBefore;
		for (UNiagaraStackModuleItem* Candidate : AllItems)
		{
			NodeGuidsBefore.Add(Candidate->GetModuleNode().NodeGuid);
			if (Stack::GetModuleUsage(*Candidate) == TargetUsage && UsageIdOf(*Candidate) == TargetUsageId)
			{
				TargetItems.Add(Candidate);
			}
		}
		const int32 SourcePos = TargetItems.IndexOfByKey(Item);
		const bool bSameStack = SourcePos != INDEX_NONE;
		const int32 MaxFinal = (bSameStack && !bCopy) ? TargetItems.Num() - 1 : TargetItems.Num();
		if (TargetIndex < -1 || TargetIndex > MaxFinal)
		{
			ReportError(FString::Printf(TEXT("TargetIndex %d is out of range; use -1 (end) or 0-%d."), TargetIndex, MaxFinal));
			return {};
		}
		const int32 FinalIndex = TargetIndex < 0 ? MaxFinal : TargetIndex;
		const FGuid ModuleGuid = Item->GetModuleNode().NodeGuid;
		if (!bCopy && bSameStack && SourcePos == FinalIndex)
		{
			return NiagaraMcpTools::DescribeModule(*Item, SourcePos, true);
		}

		// Insert position in the list before the move (the drop target semantics).
		const int32 InsertIndex = (!bCopy && bSameStack && FinalIndex > SourcePos) ? FinalIndex + 1 : FinalIndex;
		UNiagaraStackEntry* TargetEntry = TargetGroup;
		EItemDropZone DropZone = EItemDropZone::BelowItem;
		if (InsertIndex < TargetItems.Num())
		{
			TargetEntry = TargetItems[InsertIndex];
			DropZone = EItemDropZone::AboveItem;
		}
		else if (TargetItems.Num() > 0)
		{
			TargetEntry = TargetItems.Last();
			DropZone = EItemDropZone::BelowItem;
		}

		TArray<UNiagaraStackEntry*> Dragged;
		Dragged.Add(Item);
		const UNiagaraStackEntry::FDropRequest Request(MakeShared<FNiagaraStackEntryDragDropOp>(Dragged), DropZone,
			bCopy ? UNiagaraStackEntry::EDragOptions::Copy : UNiagaraStackEntry::EDragOptions::None, UNiagaraStackEntry::EDropOptions::Overview);
		const TOptional<UNiagaraStackEntry::FDropRequestResponse> CanDrop = TargetEntry->CanDrop(Request);
		if (!CanDrop.IsSet() || !CanDrop->DropZone.IsSet())
		{
			ReportError(FString::Printf(TEXT("The stack refused to %s '%s' there%s"), bCopy ? TEXT("copy") : TEXT("move"), *Item->GetDisplayName().ToString(),
				CanDrop.IsSet() && !CanDrop->DropMessage.IsEmpty() ? *(TEXT(": ") + CanDrop->DropMessage.ToString()) : TEXT(".")));
			return {};
		}

		{
			FScopedTransaction Transaction(bCopy ? LOCTEXT("DuplicateModule", "Niagara MCP: Duplicate Module") : LOCTEXT("MoveModule", "Niagara MCP: Move Module"));
			System->Modify();
			TargetEntry->Drop(Request);
		}
		Stack::FinishSystemEdit(*System, bSave);

		FGuid ResultGuid = ModuleGuid;
		if (bCopy)
		{
			ResultGuid.Invalidate();
			TSharedPtr<FNiagaraSystemViewModel> AfterViewModel = Stack::AcquireViewModel(*System);
			UNiagaraStackViewModel* AfterStack = AfterViewModel.IsValid() ? Stack::GetStackViewModel(*AfterViewModel, EmitterName) : nullptr;
			TArray<UNiagaraStackModuleItem*> AfterItems;
			if (AfterStack)
			{
				Stack::CollectModuleItems(*AfterStack, AfterItems);
			}
			for (UNiagaraStackModuleItem* Candidate : AfterItems)
			{
				if (!NodeGuidsBefore.Contains(Candidate->GetModuleNode().NodeGuid))
				{
					ResultGuid = Candidate->GetModuleNode().NodeGuid;
					break;
				}
			}
			if (!ResultGuid.IsValid())
			{
				ReportError(TEXT("The duplicate was not created."));
				return {};
			}
		}
		return DescribeModuleByGuid(*System, EmitterName, ResultGuid);
	}

	FVersionedNiagaraEmitterData* ResolveEmitterData(UNiagaraSystem* System, FName EmitterName, FNiagaraEmitterHandle*& OutHandle)
	{
		OutHandle = nullptr;
		if (!NiagaraMcpTools::CheckSystemArg(System))
		{
			return nullptr;
		}
		OutHandle = Stack::FindEmitter(*System, EmitterName);
		return OutHandle ? OutHandle->GetEmitterData() : nullptr;
	}

	UNiagaraSimulationStageBase* ResolveStage(FNiagaraEmitterHandle& Handle, FVersionedNiagaraEmitterData& EmitterData, int32 StageIndex)
	{
		const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData.GetSimulationStages();
		if (!Stages.IsValidIndex(StageIndex) || Stages[StageIndex] == nullptr)
		{
			TArray<FString> Names;
			for (int32 Index = 0; Index < Stages.Num(); ++Index)
			{
				Names.Add(FString::Printf(TEXT("%d=%s"), Index, Stages[Index] ? *Stages[Index]->SimulationStageName.ToString() : TEXT("null")));
			}
			ReportError(FString::Printf(TEXT("StageIndex %d is not a simulation stage of emitter '%s'. Stages: %s"), StageIndex, *Handle.GetName().ToString(),
				Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("(none)")));
			return nullptr;
		}
		return Stages[StageIndex];
	}

	bool CheckHandlerIndex(FNiagaraEmitterHandle& Handle, FVersionedNiagaraEmitterData& EmitterData, int32 HandlerIndex)
	{
		const TArray<FNiagaraEventScriptProperties>& Handlers = EmitterData.GetEventHandlers();
		if (!Handlers.IsValidIndex(HandlerIndex) || Handlers[HandlerIndex].Script == nullptr)
		{
			ReportError(FString::Printf(TEXT("HandlerIndex %d is not an event handler of emitter '%s' (it has %d)."), HandlerIndex, *Handle.GetName().ToString(), Handlers.Num()));
			return false;
		}
		return true;
	}
}

// ------------------------------------------------------------------------------------------------ System / emitter / renderer properties

FNiagaraMcpEditablePropertyList UNiagaraMcpEditorTools::GetSystemProperties(UNiagaraSystem* System, const FString& Filter)
{
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	FNiagaraMcpEditablePropertyList List;
	List.Owner = System->GetPathName();
	List.Class = System->GetClass()->GetName();
	NiagaraMcpExt::DescribeEditableProperties(System->GetClass(), System, Filter, List.Properties, [](const FProperty*) { return false; });
	return List;
}

FNiagaraMcpPropertyResult UNiagaraMcpEditorTools::SetSystemProperty(UNiagaraSystem* System, const FString& PropertyName, const FString& Value, bool bSave)
{
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	FProperty* TopLevel = NiagaraMcpExtA::ValidateAndSet(*System, PropertyName, Value);
	if (TopLevel == nullptr)
	{
		return {};
	}

	{
		FScopedTransaction Transaction(LOCTEXT("SetSystemProperty", "Niagara MCP: Set System Property"));
		System->Modify();
		System->PreEditChange(TopLevel);
		FString PropertyError;
		FProperty* Changed = SetPropertyFromText(System->GetClass(), System, System, PropertyName, Value, PropertyError);
		if (Changed == nullptr)
		{
			ReportError(PropertyError);
			return {};
		}
		FPropertyChangedEvent ChangedEvent(Changed, EPropertyChangeType::ValueSet);
		System->PostEditChangeProperty(ChangedEvent);
	}
	Stack::FinishSystemEdit(*System, false);

	FNiagaraMcpPropertyResult Result;
	Result.Property = PropertyName;
	Result.Value = GetPropertyText(System->GetClass(), System, PropertyName);
	Result.bSaved = bSave && SaveAssetSilently(System);
	return Result;
}

FNiagaraMcpEditablePropertyList UNiagaraMcpEditorTools::GetEmitterProperties(UNiagaraSystem* System, FName EmitterName, const FString& Filter)
{
	FNiagaraEmitterHandle* Handle = nullptr;
	FVersionedNiagaraEmitterData* EmitterData = NiagaraMcpExtA::ResolveEmitterData(System, EmitterName, Handle);
	if (EmitterData == nullptr)
	{
		return {};
	}
	FNiagaraMcpEditablePropertyList List;
	List.Owner = FString::Printf(TEXT("Emitter %s"), *Handle->GetName().ToString());
	List.Class = FVersionedNiagaraEmitterData::StaticStruct()->GetName();
	NiagaraMcpExt::DescribeEditableProperties(FVersionedNiagaraEmitterData::StaticStruct(), EmitterData, Filter, List.Properties, [](const FProperty*) { return false; });
	return List;
}

FNiagaraMcpRendererDetails UNiagaraMcpEditorTools::GetRendererProperties(UNiagaraSystem* System, FName EmitterName, int32 RendererIndex, const FString& Filter)
{
	FNiagaraEmitterHandle* Handle = nullptr;
	if (NiagaraMcpExtA::ResolveEmitterData(System, EmitterName, Handle) == nullptr)
	{
		return {};
	}
	UNiagaraRendererProperties* Renderer = NiagaraMcpExt::FindRendererByIndex(*Handle, RendererIndex);
	if (Renderer == nullptr)
	{
		return {};
	}

	FNiagaraMcpRendererDetails Details;
	Details.Index = RendererIndex;
	Details.Class = Renderer->GetClass()->GetName();
	Details.bEnabled = Renderer->GetIsEnabled();
	NiagaraMcpExt::DescribeEditableProperties(Renderer->GetClass(), Renderer, Filter, Details.Properties, [](const FProperty* Property) { return NiagaraMcpExtA::IsBindingStruct(Property); });

	const TArray<FString> Words = NiagaraMcpExt::SplitFilter(Filter);
	const UObject* Defaults = Renderer->GetClass()->GetDefaultObject();
	for (FStructProperty* BindingProperty : NiagaraMcpExtA::BindingProperties(Renderer->GetClass()))
	{
		const FNiagaraVariableAttributeBinding* Binding = BindingProperty->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(Renderer);
		const FNiagaraVariableAttributeBinding* Default = BindingProperty->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(Defaults);
		FNiagaraMcpBindingInfo Info;
		Info.Name = BindingProperty->GetName();
		Info.Type = Binding->GetType().GetName();
		Info.Variable = Binding->GetParamMapBindableVariable().GetName().ToString();
		Info.DefaultVariable = Default ? Default->GetParamMapBindableVariable().GetName().ToString() : FString();
		if (NiagaraMcpExt::MatchesWords(Info.Name + TEXT(" ") + Info.Variable + TEXT(" ") + Info.Type, Words))
		{
			Details.Bindings.Add(MoveTemp(Info));
		}
	}
	NiagaraMcpExtA::CollectMaterialBindings(*Renderer, Details.MaterialBindings);
	return Details;
}

FNiagaraMcpPropertyResult UNiagaraMcpEditorTools::SetRendererBinding(UNiagaraSystem* System, FName EmitterName, int32 RendererIndex, const FString& BindingName, const FString& Variable, bool bSave)
{
	FNiagaraEmitterHandle* Handle = nullptr;
	if (NiagaraMcpExtA::ResolveEmitterData(System, EmitterName, Handle) == nullptr)
	{
		return {};
	}
	UNiagaraRendererProperties* Renderer = NiagaraMcpExt::FindRendererByIndex(*Handle, RendererIndex);
	if (Renderer == nullptr)
	{
		return {};
	}

	// Binding property: exact or loose name, with or without the "Binding" suffix.
	const TArray<FStructProperty*> Bindings = NiagaraMcpExtA::BindingProperties(Renderer->GetClass());
	FStructProperty* BindingProperty = nullptr;
	TArray<FString> BindingNames;
	for (FStructProperty* Candidate : Bindings)
	{
		BindingNames.Add(Candidate->GetName());
		if (LooseNameEquals(Candidate->GetName(), BindingName) || LooseNameEquals(Candidate->GetName(), BindingName + TEXT("Binding")))
		{
			BindingProperty = Candidate;
		}
	}
	if (BindingProperty == nullptr)
	{
		ReportError(NiagaraMcpExt::UnknownNameMessage(TEXT("binding"), BindingName, BindingNames));
		return {};
	}
	FNiagaraVariableAttributeBinding* Binding = BindingProperty->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(Renderer);
	const FNiagaraTypeDefinition BindingType = Binding->GetType();

	FString VariableText = Variable.TrimStartAndEnd();
	VariableText.RemoveFromStart(TEXT("@"));
	if (VariableText.IsEmpty() || !VariableText.Contains(TEXT(".")))
	{
		ReportError(FString::Printf(TEXT("Variable must be a full name with namespace, e.g. Particles.Color or User.Tint (got '%s')."), *Variable));
		return {};
	}

	// Particle attributes come from the compiled scripts; make sure pending edits are compiled.
	System->WaitForCompilationComplete(false, false);
	const TMap<FName, FNiagaraTypeDefinition> Known = NiagaraMcpExtA::CollectBindableVariables(*System, *Handle);
	const FNiagaraTypeDefinition* FoundType = nullptr;
	FName VariableName(*VariableText);
	for (const TPair<FName, FNiagaraTypeDefinition>& Pair : Known)
	{
		if (Pair.Key.ToString().Equals(VariableText, ESearchCase::IgnoreCase))
		{
			FoundType = &Pair.Value;
			VariableName = Pair.Key;
			break;
		}
	}
	auto IsCompatible = [&](const FName Name, const FNiagaraTypeDefinition& Type)
	{
		const bool bTypeOk = Type == BindingType || Type.IsSameBaseDefinition(BindingType)
			|| (Type == FNiagaraTypeDefinition::GetPositionDef() && BindingType == FNiagaraTypeDefinition::GetVec3Def())
			|| (Type == FNiagaraTypeDefinition::GetVec3Def() && BindingType == FNiagaraTypeDefinition::GetPositionDef());
		return bTypeOk && Renderer->IsSupportedVariableForBinding(FNiagaraVariableBase(Type, Name), BindingProperty->GetFName());
	};
	TArray<FString> Compatible;
	for (const TPair<FName, FNiagaraTypeDefinition>& Pair : Known)
	{
		if (IsCompatible(Pair.Key, Pair.Value))
		{
			Compatible.Add(Pair.Key.ToString());
		}
	}
	Compatible.Sort();
	if (FoundType == nullptr)
	{
		ReportError(FString::Printf(TEXT("Variable '%s' is not known to emitter '%s' (particle attributes come from the last compile; add the module that writes it first). %s variables usable for %s: %s"),
			*VariableText, *Handle->GetName().ToString(), *BindingType.GetName(), *BindingProperty->GetName(),
			Compatible.Num() ? *FString::Join(Compatible, TEXT(", ")) : TEXT("(none)")));
		return {};
	}
	if (!IsCompatible(VariableName, *FoundType))
	{
		ReportError(FString::Printf(TEXT("'%s' is a %s but %s needs a %s (or the renderer does not accept this namespace here). Compatible: %s"),
			*VariableText, *FoundType->GetName(), *BindingProperty->GetName(), *BindingType.GetName(),
			Compatible.Num() ? *FString::Join(Compatible, TEXT(", ")) : TEXT("(none)")));
		return {};
	}

	const FVersionedNiagaraEmitter VersionedEmitter = Handle->GetInstance();
	{
		FScopedTransaction Transaction(LOCTEXT("SetRendererBinding", "Niagara MCP: Set Renderer Binding"));
		Renderer->Modify();
		Renderer->PreEditChange(BindingProperty);
		// Same call as the binding dropdown (FNiagaraVariableAttributeBindingCustomization::ChangeSource).
		Binding->SetValue(VariableName, FVersionedNiagaraEmitterBase(VersionedEmitter.Emitter, VersionedEmitter.Version), Renderer->GetCurrentSourceMode());
		FPropertyChangedEvent ChangedEvent(BindingProperty, EPropertyChangeType::ValueSet);
		Renderer->PostEditChangeProperty(ChangedEvent);
	}
	Stack::FinishSystemEdit(*System, false);

	FNiagaraMcpPropertyResult Result;
	Result.Property = BindingProperty->GetName();
	Result.Value = Binding->GetParamMapBindableVariable().GetName().ToString();
	Result.bSaved = bSave && SaveAssetSilently(System);
	return Result;
}

FNiagaraMcpEmitterInfo UNiagaraMcpEditorTools::MoveRenderer(UNiagaraSystem* System, FName EmitterName, int32 RendererIndex, int32 NewIndex, bool bSave)
{
	FNiagaraEmitterHandle* Handle = nullptr;
	FVersionedNiagaraEmitterData* EmitterData = NiagaraMcpExtA::ResolveEmitterData(System, EmitterName, Handle);
	if (EmitterData == nullptr)
	{
		return {};
	}
	UNiagaraRendererProperties* Renderer = NiagaraMcpExt::FindRendererByIndex(*Handle, RendererIndex);
	if (Renderer == nullptr)
	{
		return {};
	}
	const int32 Count = EmitterData->GetRenderers().Num();
	if (NewIndex < 0 || NewIndex >= Count)
	{
		ReportError(FString::Printf(TEXT("NewIndex %d is out of range; use 0-%d."), NewIndex, Count - 1));
		return {};
	}
	if (NewIndex != RendererIndex)
	{
		const FVersionedNiagaraEmitter VersionedEmitter = Handle->GetInstance();
		FScopedTransaction Transaction(LOCTEXT("MoveRenderer", "Niagara MCP: Move Renderer"));
		VersionedEmitter.Emitter->Modify();
		VersionedEmitter.Emitter->MoveRenderer(Renderer, NewIndex, VersionedEmitter.Version);
	}
	Stack::FinishSystemEdit(*System, bSave && NewIndex != RendererIndex);
	return NiagaraMcpExt::DescribeEmitterInfo(*Handle);
}

// ------------------------------------------------------------------------------------------------ Module order

FNiagaraMcpModuleInfo UNiagaraMcpEditorTools::MoveModule(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& StackName, const FString& TargetStackName, int32 TargetIndex, bool bSave)
{
	return NiagaraMcpExtA::MoveOrCopyModule(System, EmitterName, Module, StackName, TargetStackName, TargetIndex, bSave, false);
}

FNiagaraMcpModuleInfo UNiagaraMcpEditorTools::DuplicateModule(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& StackName, const FString& TargetStackName, int32 TargetIndex, bool bSave)
{
	return NiagaraMcpExtA::MoveOrCopyModule(System, EmitterName, Module, StackName, TargetStackName, TargetIndex, bSave, true);
}

// ------------------------------------------------------------------------------------------------ Emitters

FNiagaraMcpEmitterInfo UNiagaraMcpEditorTools::SetEmitterEnabled(UNiagaraSystem* System, FName EmitterName, bool bEnabled, bool bSave)
{
	FNiagaraEmitterHandle* Handle = nullptr;
	if (NiagaraMcpExtA::ResolveEmitterData(System, EmitterName, Handle) == nullptr && Handle == nullptr)
	{
		return {};
	}
	const FGuid HandleId = Handle->GetId();
	if (Handle->GetIsEnabled() != bEnabled)
	{
		TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(*System);
		if (!ViewModel.IsValid())
		{
			return {};
		}
		TSharedPtr<FNiagaraEmitterHandleViewModel> HandleViewModel;
		for (const TSharedRef<FNiagaraEmitterHandleViewModel>& Candidate : ViewModel->GetEmitterHandleViewModels())
		{
			if (Candidate->GetId() == HandleId)
			{
				HandleViewModel = Candidate;
				break;
			}
		}
		if (!HandleViewModel.IsValid())
		{
			ReportError(FString::Printf(TEXT("Emitter '%s' has no view model."), *EmitterName.ToString()));
			return {};
		}
		{
			FScopedTransaction Transaction(LOCTEXT("SetEmitterEnabled", "Niagara MCP: Enable/Disable Emitter"));
			System->Modify();
			HandleViewModel->SetIsEnabled(bEnabled, true);
		}
		Stack::FinishSystemEdit(*System, bSave);
	}
	FNiagaraEmitterHandle* After = NiagaraMcpExt::FindHandleById(*System, HandleId);
	return After ? NiagaraMcpExt::DescribeEmitterInfo(*After) : FNiagaraMcpEmitterInfo();
}

FNiagaraMcpEmitterInfo UNiagaraMcpEditorTools::DuplicateEmitter(UNiagaraSystem* System, FName EmitterName, FName NewName, bool bSave)
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
	if (Handle->GetInstance().Emitter == nullptr)
	{
		ReportError(FString::Printf(TEXT("Emitter '%s' is a stateless emitter; duplicate it in the Niagara editor."), *EmitterName.ToString()));
		return {};
	}
	TSet<FName> Names;
	for (const FNiagaraEmitterHandle& Existing : System->GetEmitterHandles())
	{
		Names.Add(Existing.GetName());
	}
	const FName UniqueName = FNiagaraUtilities::GetUniqueName(NewName.IsNone() ? Handle->GetName() : NewName, Names);
	Names.Add(UniqueName);
	const FName TempName = FNiagaraUtilities::GetUniqueName(FName(*(UniqueName.ToString() + TEXT("_McpRebuild"))), Names);

	FGuid NewHandleId;
	{
		FScopedTransaction Transaction(LOCTEXT("DuplicateEmitter", "Niagara MCP: Duplicate Emitter"));
		System->Modify();

		// Same steps as FNiagaraSystemViewModel::DuplicateEmitters: the engine's handle duplicate, then the system graph's
		// emitter nodes and the overview graph are rebuilt. The rebuild (FNiagaraStackGraphUtilities::RebuildEmitterNodes)
		// is not exported, so it is run through the exported remove path on a second, temporary duplicate.
		const FNiagaraEmitterHandle Source = *Handle;
		TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
		Handles.Reserve(Handles.Num() + 2); // keep view models' handle pointers valid
		NewHandleId = System->DuplicateEmitterHandle(Source, UniqueName).GetId();
		const FNiagaraEmitterHandle TempHandle = System->DuplicateEmitterHandle(Source, TempName);
		UNiagaraEmitter* TempEmitter = TempHandle.GetInstance().Emitter;

		FNiagaraExt_StackItemReference TempRef(System, TempHandle.GetName());
		FNiagaraExternalEditContext Context(TempRef);
		UNiagaraExternalEditUtilities::RemoveEmitter(TempRef, Context);
		if (Context.HasErrors() || NiagaraMcpExt::FindHandleById(*System, TempHandle.GetId()) != nullptr)
		{
			// Should not happen; remove the temporary handle directly so the system stays consistent.
			System->RemoveEmitterHandlesById({ TempHandle.GetId() });
			for (const FText& ContextError : Context.Errors)
			{
				UE_LOG(LogNiagaraMcp, Warning, TEXT("DuplicateEmitter rebuild: %s"), *ContextError.ToString());
			}
		}
		if (TempEmitter)
		{
			TempEmitter->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		}
	}
	Stack::FinishSystemEdit(*System, bSave);
	FNiagaraEmitterHandle* NewHandle = NiagaraMcpExt::FindHandleById(*System, NewHandleId);
	if (NewHandle == nullptr)
	{
		ReportError(TEXT("Duplicating the emitter failed."));
		return {};
	}
	return NiagaraMcpExt::DescribeEmitterInfo(*NewHandle);
}

// ------------------------------------------------------------------------------------------------ Simulation stages and event handlers

FNiagaraMcpPropertyResult UNiagaraMcpEditorTools::SetSimulationStageProperty(UNiagaraSystem* System, FName EmitterName, int32 StageIndex, const FString& PropertyName, const FString& Value, bool bSave)
{
	FNiagaraEmitterHandle* Handle = nullptr;
	FVersionedNiagaraEmitterData* EmitterData = NiagaraMcpExtA::ResolveEmitterData(System, EmitterName, Handle);
	UNiagaraSimulationStageBase* Stage = EmitterData ? NiagaraMcpExtA::ResolveStage(*Handle, *EmitterData, StageIndex) : nullptr;
	if (Stage == nullptr)
	{
		return {};
	}
	// bEnabled is not an editable UPROPERTY (the stack shows it as the stage's checkbox); handle it like the checkbox.
	const bool bEnabledProperty = LooseNameEquals(PropertyName, TEXT("bEnabled")) || LooseNameEquals(PropertyName, TEXT("Enabled"));
	bool bNewEnabled = true;
	FProperty* TopLevel = nullptr;
	if (bEnabledProperty)
	{
		if (!NiagaraMcpExt::ParseBoolText(Value, bNewEnabled))
		{
			ReportError(FString::Printf(TEXT("'%s' is not a bool. Use true or false."), *Value));
			return {};
		}
	}
	// Parameter bindings (NumIterations, EnabledBinding, ElementCountX, ...) take a constant or "@User.X", like the stage's binding widget.
	FStructProperty* BindingProperty = nullptr;
	if (!bEnabledProperty && !PropertyName.Contains(TEXT(".")))
	{
		BindingProperty = CastField<FStructProperty>(NiagaraMcpExt::FindPropertyLoose(Stage->GetClass(), PropertyName.TrimStartAndEnd()));
		if (!NiagaraMcpExt::IsParameterBindingProperty(BindingProperty))
		{
			BindingProperty = nullptr;
		}
	}
	const FString EmitterUniqueName = Handle->GetInstance().Emitter ? Handle->GetInstance().Emitter->GetUniqueEmitterName() : FString();
	if (BindingProperty)
	{
		// Validate on a scratch copy of the binding.
		FStructOnScope Scratch(BindingProperty->Struct);
		BindingProperty->Struct->CopyScriptStruct(Scratch.GetStructMemory(), BindingProperty->ContainerPtrToValuePtr<void>(Stage));
		FString Error;
		if (!NiagaraMcpExt::SetParameterBindingFromText(*BindingProperty, Scratch.GetStructMemory() - BindingProperty->GetOffset_ForInternal(), Value, EmitterUniqueName, Error))
		{
			ReportError(FString::Printf(TEXT("%s: %s"), *BindingProperty->GetName(), *Error));
			return {};
		}
	}
	else if (!bEnabledProperty)
	{
		TopLevel = NiagaraMcpExtA::ValidateAndSet(*Stage, PropertyName, Value);
		if (TopLevel == nullptr)
		{
			return {};
		}
	}
	{
		FScopedTransaction Transaction(LOCTEXT("SetSimulationStageProperty", "Niagara MCP: Set Simulation Stage Property"));
		Stage->Modify();
		if (bEnabledProperty)
		{
			// Same as the stage's enable checkbox in the stack (UNiagaraStackSimulationStagePropertiesItem::SetSimulationStageEnabled).
			Stage->SetEnabled(bNewEnabled);
		}
		else if (BindingProperty)
		{
			Stage->PreEditChange(BindingProperty);
			FString Error;
			NiagaraMcpExt::SetParameterBindingFromText(*BindingProperty, Stage, Value, EmitterUniqueName, Error);
			FPropertyChangedEvent ChangedEvent(BindingProperty, EPropertyChangeType::ValueSet);
			Stage->PostEditChangeProperty(ChangedEvent);
		}
		else
		{
			Stage->PreEditChange(TopLevel);
			FString PropertyError;
			FProperty* Changed = SetPropertyFromText(Stage->GetClass(), Stage, Stage, PropertyName, Value, PropertyError);
			if (Changed == nullptr)
			{
				ReportError(PropertyError);
				return {};
			}
			FPropertyChangedEvent ChangedEvent(Changed, EPropertyChangeType::ValueSet);
			Stage->PostEditChangeProperty(ChangedEvent);
		}
	}
	Stack::FinishSystemEdit(*System, false);

	FNiagaraMcpPropertyResult Result;
	Result.Property = PropertyName;
	Result.Value = bEnabledProperty ? FString(Stage->bEnabled ? TEXT("true") : TEXT("false"))
		: BindingProperty ? NiagaraMcpExt::ParameterBindingText(*BindingProperty, Stage) : GetPropertyText(Stage->GetClass(), Stage, PropertyName);
	Result.bSaved = bSave && SaveAssetSilently(System);
	return Result;
}

FNiagaraMcpEmitterStages UNiagaraMcpEditorTools::RemoveSimulationStage(UNiagaraSystem* System, FName EmitterName, int32 StageIndex, bool bSave)
{
	FNiagaraEmitterHandle* Handle = nullptr;
	FVersionedNiagaraEmitterData* EmitterData = NiagaraMcpExtA::ResolveEmitterData(System, EmitterName, Handle);
	UNiagaraSimulationStageBase* Stage = EmitterData ? NiagaraMcpExtA::ResolveStage(*Handle, *EmitterData, StageIndex) : nullptr;
	if (Stage == nullptr)
	{
		return {};
	}
	TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(*System);
	UNiagaraStackViewModel* StackViewModel = ViewModel.IsValid() ? Stack::GetStackViewModel(*ViewModel, EmitterName) : nullptr;
	if (StackViewModel == nullptr)
	{
		return {};
	}
	StackViewModel->GetRootEntry()->RefreshChildren();
	TArray<UNiagaraStackSimulationStageGroup*> Groups;
	NiagaraMcpExtA::CollectGroups(StackViewModel->GetRootEntry(), Groups, 0);
	UNiagaraStackSimulationStageGroup* const* Group = Groups.FindByPredicate([Stage](const UNiagaraStackSimulationStageGroup* Candidate) { return Candidate->GetSimulationStage() == Stage; });
	if (Group == nullptr)
	{
		ReportError(TEXT("The simulation stage is not shown in the emitter stack (is the emitter in Summary View?)."));
		return {};
	}
	FText CannotDelete;
	if (!(*Group)->TestCanDeleteWithMessage(CannotDelete))
	{
		ReportError(FString::Printf(TEXT("The simulation stage cannot be removed: %s"), *CannotDelete.ToString()));
		return {};
	}
	{
		FScopedTransaction Transaction(LOCTEXT("RemoveSimulationStage", "Niagara MCP: Remove Simulation Stage"));
		System->Modify();
		(*Group)->Delete();
	}
	Stack::FinishSystemEdit(*System, bSave);
	return NiagaraMcpExt::DescribeEmitterStages(*System, *Handle);
}

FNiagaraMcpEmitterStages UNiagaraMcpEditorTools::MoveSimulationStage(UNiagaraSystem* System, FName EmitterName, int32 StageIndex, int32 NewIndex, bool bSave)
{
	FNiagaraEmitterHandle* Handle = nullptr;
	FVersionedNiagaraEmitterData* EmitterData = NiagaraMcpExtA::ResolveEmitterData(System, EmitterName, Handle);
	UNiagaraSimulationStageBase* Stage = EmitterData ? NiagaraMcpExtA::ResolveStage(*Handle, *EmitterData, StageIndex) : nullptr;
	if (Stage == nullptr)
	{
		return {};
	}
	const int32 Count = EmitterData->GetSimulationStages().Num();
	if (NewIndex < 0 || NewIndex >= Count)
	{
		ReportError(FString::Printf(TEXT("NewIndex %d is out of range; use 0-%d."), NewIndex, Count - 1));
		return {};
	}
	if (NewIndex != StageIndex)
	{
		const FVersionedNiagaraEmitter VersionedEmitter = Handle->GetInstance();
		FScopedTransaction Transaction(LOCTEXT("MoveSimulationStage", "Niagara MCP: Move Simulation Stage"));
		VersionedEmitter.Emitter->Modify();
		// MoveSimulationStageToIndex takes the insert position in the list before the move (like the stack's drop).
		VersionedEmitter.Emitter->MoveSimulationStageToIndex(Stage, NewIndex > StageIndex ? NewIndex + 1 : NewIndex, VersionedEmitter.Version);
	}
	Stack::FinishSystemEdit(*System, bSave && NewIndex != StageIndex);
	return NiagaraMcpExt::DescribeEmitterStages(*System, *Handle);
}

FNiagaraMcpEmitterStages UNiagaraMcpEditorTools::RemoveEventHandler(UNiagaraSystem* System, FName EmitterName, int32 HandlerIndex, bool bSave)
{
	FNiagaraEmitterHandle* Handle = nullptr;
	FVersionedNiagaraEmitterData* EmitterData = NiagaraMcpExtA::ResolveEmitterData(System, EmitterName, Handle);
	if (EmitterData == nullptr || !NiagaraMcpExtA::CheckHandlerIndex(*Handle, *EmitterData, HandlerIndex))
	{
		return {};
	}
	const FGuid UsageId = EmitterData->GetEventHandlers()[HandlerIndex].Script->GetUsageId();
	TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(*System);
	UNiagaraStackViewModel* StackViewModel = ViewModel.IsValid() ? Stack::GetStackViewModel(*ViewModel, EmitterName) : nullptr;
	if (StackViewModel == nullptr)
	{
		return {};
	}
	StackViewModel->GetRootEntry()->RefreshChildren();
	TArray<UNiagaraStackEventScriptItemGroup*> Groups;
	NiagaraMcpExtA::CollectGroups(StackViewModel->GetRootEntry(), Groups, 0);
	UNiagaraStackEventScriptItemGroup* const* Group = Groups.FindByPredicate([&UsageId](const UNiagaraStackEventScriptItemGroup* Candidate) { return Candidate->GetScriptUsageId() == UsageId; });
	if (Group == nullptr)
	{
		ReportError(TEXT("The event handler is not shown in the emitter stack (is the emitter in Summary View?)."));
		return {};
	}
	FText CannotDelete;
	if (!(*Group)->TestCanDeleteWithMessage(CannotDelete))
	{
		ReportError(FString::Printf(TEXT("The event handler cannot be removed: %s"), *CannotDelete.ToString()));
		return {};
	}
	{
		FScopedTransaction Transaction(LOCTEXT("RemoveEventHandler", "Niagara MCP: Remove Event Handler"));
		System->Modify();
		(*Group)->Delete();
	}
	Stack::FinishSystemEdit(*System, bSave);
	return NiagaraMcpExt::DescribeEmitterStages(*System, *Handle);
}

FNiagaraMcpPropertyResult UNiagaraMcpEditorTools::SetEventHandlerProperty(UNiagaraSystem* System, FName EmitterName, int32 HandlerIndex, const FString& PropertyName, const FString& Value, bool bSave)
{
	FNiagaraEmitterHandle* Handle = nullptr;
	FVersionedNiagaraEmitterData* EmitterData = NiagaraMcpExtA::ResolveEmitterData(System, EmitterName, Handle);
	if (EmitterData == nullptr || !NiagaraMcpExtA::CheckHandlerIndex(*Handle, *EmitterData, HandlerIndex))
	{
		return {};
	}
	UScriptStruct* PropsStruct = FNiagaraEventScriptProperties::StaticStruct();
	const bool bSourceEmitter = LooseNameEquals(PropertyName, TEXT("SourceEmitter")) || LooseNameEquals(PropertyName, TEXT("SourceEmitterName"));
	FGuid SourceEmitterId;
	if (bSourceEmitter)
	{
		const FName SourceName(*Value.TrimStartAndEnd());
		if (!SourceName.IsNone() && SourceName != Handle->GetName())
		{
			FNiagaraEmitterHandle* SourceHandle = Stack::FindEmitter(*System, SourceName);
			if (SourceHandle == nullptr)
			{
				return {};
			}
			SourceEmitterId = SourceHandle->GetId();
		}
	}
	else
	{
		TArray<FString> Valid = NiagaraMcpExt::EditablePropertyNames(PropsStruct);
		Valid.Remove(TEXT("SourceEmitterID"));
		Valid.Add(TEXT("SourceEmitter"));
		if (NiagaraMcpExt::FindPropertyLoose(PropsStruct, PropertyName) == nullptr || LooseNameEquals(PropertyName, TEXT("SourceEmitterID")))
		{
			ReportError(NiagaraMcpExt::UnknownNameMessage(TEXT("event handler property"), PropertyName, Valid));
			return {};
		}
		FString Error;
		if (!NiagaraMcpExt::DryRunSetProperty(PropsStruct, &EmitterData->EventHandlerScriptProps[HandlerIndex], nullptr, PropertyName, Value, Error))
		{
			ReportError(Error);
			return {};
		}
	}

	const FVersionedNiagaraEmitter VersionedEmitter = Handle->GetInstance();
	{
		FScopedTransaction Transaction(LOCTEXT("SetEventHandlerProperty", "Niagara MCP: Set Event Handler Property"));
		VersionedEmitter.Emitter->Modify();
		FNiagaraEventScriptProperties& Props = EmitterData->EventHandlerScriptProps[HandlerIndex];
		if (bSourceEmitter)
		{
			Props.SourceEmitterID = SourceEmitterId;
		}
		else
		{
			FString PropertyError;
			if (SetPropertyFromText(PropsStruct, &Props, VersionedEmitter.Emitter, PropertyName, Value, PropertyError) == nullptr)
			{
				ReportError(PropertyError);
				return {};
			}
		}
		// Same notification as the stack's event handler details (UNiagaraStackEventWrapper::PostEditChangeProperty).
		FProperty* HandlersProperty = FindFProperty<FProperty>(FVersionedNiagaraEmitterData::StaticStruct(), TEXT("EventHandlerScriptProps"));
		FPropertyChangedEvent ChangedEvent(HandlersProperty, EPropertyChangeType::ValueSet);
		VersionedEmitter.Emitter->PostEditChangeVersionedProperty(ChangedEvent, VersionedEmitter.Version);
	}
	Stack::FinishSystemEdit(*System, false);

	FNiagaraMcpPropertyResult Result;
	Result.Property = PropertyName;
	FVersionedNiagaraEmitterData* After = Handle->GetEmitterData();
	if (After && After->GetEventHandlers().IsValidIndex(HandlerIndex))
	{
		const FNiagaraEventScriptProperties& Props = After->GetEventHandlers()[HandlerIndex];
		if (bSourceEmitter)
		{
			const FNiagaraEmitterHandle* SourceHandle = Props.SourceEmitterID.IsValid() ? NiagaraMcpExt::FindHandleById(*System, Props.SourceEmitterID) : nullptr;
			Result.Value = SourceHandle ? SourceHandle->GetName().ToString() : Handle->GetName().ToString();
		}
		else
		{
			Result.Value = GetPropertyText(PropsStruct, &Props, PropertyName);
		}
	}
	Result.bSaved = bSave && SaveAssetSilently(System);
	return Result;
}

// ------------------------------------------------------------------------------------------------ User parameters

TArray<FNiagaraMcpParameterInfo> UNiagaraMcpEditorTools::RemoveUserParameter(UNiagaraSystem* System, FName ParameterName, bool bSave)
{
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	FNiagaraVariable Parameter;
	if (!NiagaraMcpExt::FindUserParameter(*System, ParameterName, Parameter))
	{
		return {};
	}
	// The user parameter panel's delete: resets linked inputs and bindings, then removes the parameter. The utility
	// uses the system's existing view model; keep one alive (its own fallback view model asserts on a missing message key).
	// Acquired before the transaction so the headless view model's sequencer objects are not recorded.
	TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(*System);
	if (!ViewModel.IsValid())
	{
		return {};
	}
	{
		FScopedTransaction Transaction(LOCTEXT("RemoveUserParameter", "Niagara MCP: Remove User Parameter"));
		System->Modify();
		FNiagaraEditorUtilities::UserParameters::RemoveUserParameterFromSystem(*System, Parameter);
	}
	Stack::FinishSystemEdit(*System, bSave);
	FNiagaraVariable Remaining;
	if (NiagaraMcpExt::FindUserParameter(*System, ParameterName, Remaining, false))
	{
		ReportError(FString::Printf(TEXT("User parameter '%s' could not be removed."), *Parameter.GetName().ToString()));
		return {};
	}
	return NiagaraMcpExt::DescribeUserParameters(*System);
}

FNiagaraMcpParameterInfo UNiagaraMcpEditorTools::RenameUserParameter(UNiagaraSystem* System, FName ParameterName, FName NewName, bool bSave)
{
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	FNiagaraVariable Parameter;
	if (!NiagaraMcpExt::FindUserParameter(*System, ParameterName, Parameter))
	{
		return {};
	}
	const FString NewShort = NewName.ToString().TrimStartAndEnd();
	if (NewName.IsNone() || NewShort.IsEmpty() || NewShort.Equals(TEXT("User."), ESearchCase::IgnoreCase))
	{
		ReportError(TEXT("NewName is required, e.g. User.ImpactColor."));
		return {};
	}
	const FName NewFullName = NiagaraMcpExt::UserParameterName(NewName);
	FNiagaraVariable Existing;
	if (NiagaraMcpExt::FindUserParameter(*System, NewFullName, Existing, false))
	{
		if (Existing.GetName() == Parameter.GetName())
		{
			FNiagaraMcpParameterInfo Info;
			Info.Name = Parameter.GetName().ToString();
			Info.Type = Parameter.GetType().GetName();
			Info.Value = NiagaraMcpExt::ParameterValueText(System->GetExposedParameters(), Parameter);
			return Info;
		}
		ReportError(FString::Printf(TEXT("A user parameter named '%s' already exists."), *NewFullName.ToString()));
		return {};
	}
	// The user parameter panel's rename (FNiagaraSystemViewModel::RenameParameter over all graphs): updates linked
	// module inputs, map get nodes and renderer bindings. Keep a view model alive for the utility (see RemoveUserParameter).
	TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(*System);
	if (!ViewModel.IsValid())
	{
		return {};
	}
	{
		FScopedTransaction Transaction(LOCTEXT("RenameUserParameter", "Niagara MCP: Rename User Parameter"));
		System->Modify();
		FNiagaraEditorUtilities::UserParameters::RenameUserParameterForSystem(*System, Parameter, FNiagaraVariableBase(Parameter.GetType(), NewFullName));
	}
	Stack::FinishSystemEdit(*System, bSave);

	FNiagaraVariable Renamed;
	if (!NiagaraMcpExt::FindUserParameter(*System, NewFullName, Renamed, false))
	{
		ReportError(FString::Printf(TEXT("Renaming '%s' to '%s' failed."), *Parameter.GetName().ToString(), *NewFullName.ToString()));
		return {};
	}
	FNiagaraMcpParameterInfo Info;
	Info.Name = Renamed.GetName().ToString();
	Info.Type = Renamed.GetType().GetName();
	Info.Value = NiagaraMcpExt::ParameterValueText(System->GetExposedParameters(), Renamed);
	return Info;
}

FNiagaraMcpPropertyResult UNiagaraMcpEditorTools::SetUserDataInterfaceProperty(UNiagaraSystem* System, FName ParameterName, const FString& PropertyName, const FString& Value, bool bSave)
{
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	FNiagaraVariable Parameter;
	if (!NiagaraMcpExt::FindUserParameter(*System, ParameterName, Parameter))
	{
		return {};
	}
	UNiagaraDataInterface* DataInterface = Parameter.GetType().IsDataInterface() ? System->GetExposedParameters().GetDataInterface(Parameter) : nullptr;
	if (DataInterface == nullptr)
	{
		ReportError(FString::Printf(TEXT("User parameter '%s' is a %s, not a data interface. Use SetUserParameter for values."), *Parameter.GetName().ToString(), *Parameter.GetType().GetName()));
		return {};
	}
	FProperty* TopLevel = NiagaraMcpExtA::ValidateAndSet(*DataInterface, PropertyName, Value);
	if (TopLevel == nullptr)
	{
		return {};
	}
	{
		FScopedTransaction Transaction(LOCTEXT("SetUserDataInterfaceProperty", "Niagara MCP: Set User Data Interface Property"));
		System->Modify();
		DataInterface->Modify();
		DataInterface->PreEditChange(TopLevel);
		FString PropertyError;
		FProperty* Changed = SetPropertyFromText(DataInterface->GetClass(), DataInterface, DataInterface, PropertyName, Value, PropertyError);
		if (Changed == nullptr)
		{
			ReportError(PropertyError);
			return {};
		}
		FPropertyChangedEvent ChangedEvent(Changed, EPropertyChangeType::ValueSet);
		DataInterface->PostEditChangeProperty(ChangedEvent);
	}
	Stack::SyncCompiledDataInterface(*System, *DataInterface);
	Stack::FinishSystemEdit(*System, false);

	FNiagaraMcpPropertyResult Result;
	Result.Property = PropertyName;
	Result.Value = GetPropertyText(DataInterface->GetClass(), DataInterface, PropertyName);
	Result.bSaved = bSave && SaveAssetSilently(System);
	return Result;
}

#undef LOCTEXT_NAMESPACE
