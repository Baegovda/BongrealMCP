// Simulation stages, event handlers and local (Scratch Pad) modules.
// The stage-menu and local-module approach follows Joey0854/UnrealMCP-ExtraToolsets (NovaToolsets, MIT license):
// stages are added by running the emitter's own "+ Stage" menu action so the editor wires the stage graph itself.

#include "NiagaraMcpEditorTools.h"

#include "Internationalization/Text.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEditorSettings.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraExternalSystemEditorUtilities.h"
#include "NiagaraGraph.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraMcpLog.h"
#include "NiagaraMcpEditorToolsInternal.h"
#include "NiagaraMcpStackUtils.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "ScopedTransaction.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/INiagaraStackItemGroupAddUtilities.h"
#include "ViewModels/Stack/NiagaraStackEmitterPropertiesGroup.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

#define LOCTEXT_NAMESPACE "NiagaraMcpEditorTools"

using namespace NiagaraMcp;

namespace NiagaraMcpStages
{
	bool TextMatches(const FText& A, const FText& B)
	{
		if (A.EqualTo(B))
		{
			return true;
		}
		const FString* SourceA = FTextInspector::GetSourceString(A);
		const FString* SourceB = FTextInspector::GetSourceString(B);
		return SourceA && SourceB && SourceA->Equals(*SourceB);
	}

	bool TextHasSource(const FText& Text, const TCHAR* Source)
	{
		const FString* SourceString = FTextInspector::GetSourceString(Text);
		return (SourceString && SourceString->Equals(Source)) || Text.ToString().Equals(Source);
	}

	FName EmitterNameFromId(UNiagaraSystem& System, const FGuid& Id)
	{
		for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
		{
			if (Id.IsValid() && Handle.GetId() == Id)
			{
				return Handle.GetName();
			}
		}
		return NAME_None;
	}

	FNiagaraMcpEmitterStages DescribeStages(UNiagaraSystem& System, FNiagaraEmitterHandle& Handle)
	{
		FNiagaraMcpEmitterStages Info;
		Info.Emitter = Handle.GetName();
		const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		if (EmitterData == nullptr)
		{
			return Info;
		}
		Info.SimTarget = StaticEnum<ENiagaraSimTarget>()->GetNameStringByValue(static_cast<int64>(EmitterData->SimTarget));

		const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
		for (int32 Index = 0; Index < Stages.Num(); ++Index)
		{
			const UNiagaraSimulationStageBase* Stage = Stages[Index];
			if (Stage == nullptr)
			{
				continue;
			}
			FNiagaraMcpStageInfo& StageInfo = Info.SimulationStages.AddDefaulted_GetRef();
			StageInfo.StageIndex = Index;
			StageInfo.Name = Stage->SimulationStageName.ToString();
			StageInfo.Class = Stage->GetClass()->GetName();
			StageInfo.bEnabled = Stage->bEnabled != 0;
			if (const UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage))
			{
				StageInfo.IterationSource = StaticEnum<ENiagaraIterationSource>()->GetNameStringByValue(static_cast<int64>(Generic->IterationSource));
				StageInfo.DataInterface = Generic->DataInterface.BoundVariable.GetName().ToString();
			}
		}

		const TArray<FNiagaraEventScriptProperties>& Handlers = EmitterData->GetEventHandlers();
		for (int32 Index = 0; Index < Handlers.Num(); ++Index)
		{
			const FNiagaraEventScriptProperties& Props = Handlers[Index];
			FNiagaraMcpStageInfo& HandlerInfo = Info.EventHandlers.AddDefaulted_GetRef();
			HandlerInfo.StageIndex = Index;
			HandlerInfo.Name = TEXT("Event Handler");
			HandlerInfo.Class = TEXT("NiagaraEventHandler");
			const FName Source = EmitterNameFromId(System, Props.SourceEmitterID);
			HandlerInfo.SourceEmitter = Source.IsNone() ? Handle.GetName().ToString() : Source.ToString();
			HandlerInfo.SourceEvent = Props.SourceEventName.ToString();
			HandlerInfo.ExecutionMode = StaticEnum<EScriptExecutionMode>()->GetNameStringByValue(static_cast<int64>(Props.ExecutionMode));
			HandlerInfo.SpawnNumber = static_cast<int32>(Props.SpawnNumber);
			HandlerInfo.MaxEventsPerFrame = static_cast<int32>(Props.MaxEventsPerFrame);
		}
		return Info;
	}

	UNiagaraStackEmitterPropertiesGroup* FindPropertiesGroup(UNiagaraStackEntry* Entry, int32 Depth)
	{
		if (Entry == nullptr || Depth > 3)
		{
			return nullptr;
		}
		if (UNiagaraStackEmitterPropertiesGroup* Group = Cast<UNiagaraStackEmitterPropertiesGroup>(Entry))
		{
			return Group;
		}
		TArray<UNiagaraStackEntry*> Children;
		Entry->GetUnfilteredChildren(Children);
		for (UNiagaraStackEntry* Child : Children)
		{
			if (UNiagaraStackEmitterPropertiesGroup* Found = FindPropertiesGroup(Child, Depth + 1))
			{
				return Found;
			}
		}
		return nullptr;
	}

	/** Finds an entry of the emitter's "+ Stage" menu; running it uses the editor's own add path. */
	TSharedPtr<INiagaraStackItemGroupAddAction> FindStageAddAction(FNiagaraSystemViewModel& ViewModel, FName EmitterName,
		TFunctionRef<bool(const INiagaraStackItemGroupAddAction&)> Predicate, INiagaraStackItemGroupAddUtilities*& OutUtilities)
	{
		OutUtilities = nullptr;
		UNiagaraStackViewModel* StackViewModel = Stack::GetStackViewModel(ViewModel, EmitterName);
		UNiagaraStackEntry* Root = StackViewModel ? StackViewModel->GetRootEntry() : nullptr;
		if (Root == nullptr)
		{
			return nullptr;
		}
		UNiagaraStackEmitterPropertiesGroup* Group = FindPropertiesGroup(Root, 0);
		if (Group == nullptr)
		{
			Root->RefreshChildren();
			Group = FindPropertiesGroup(Root, 0);
		}
		OutUtilities = Group ? Group->GetAddUtilities() : nullptr;
		if (OutUtilities == nullptr)
		{
			ReportError(FString::Printf(TEXT("Emitter '%s' has no stage add menu. Turn off Summary View for this emitter; stateless emitters do not support stages."), *EmitterName.ToString()));
			return nullptr;
		}

		TArray<TSharedRef<INiagaraStackItemGroupAddAction>> Actions;
		FNiagaraStackItemGroupAddOptions Options;
		Options.bIncludeNonLibrary = true;
		OutUtilities->GenerateAddActions(Actions, Options);
		TArray<FString> Available;
		for (const TSharedRef<INiagaraStackItemGroupAddAction>& Action : Actions)
		{
			if (Predicate(*Action))
			{
				return Action;
			}
			Available.Add(Action->GetDisplayName().ToString());
		}
		ReportError(FString::Printf(TEXT("No matching entry in the stage menu of emitter '%s'. Available: %s"), *EmitterName.ToString(), *FString::Join(Available, TEXT(", "))));
		return nullptr;
	}

	UNiagaraNodeOutput* FindStageOutputNode(FNiagaraEmitterHandle& Handle, ENiagaraScriptUsage TargetStack, int32 StageIndex)
	{
		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		if (EmitterData == nullptr)
		{
			return nullptr;
		}
		FGuid UsageId;
		if (TargetStack == ENiagaraScriptUsage::ParticleSimulationStageScript)
		{
			const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
			if (!Stages.IsValidIndex(StageIndex) || Stages[StageIndex] == nullptr || Stages[StageIndex]->Script == nullptr)
			{
				ReportError(FString::Printf(TEXT("StageIndex %d is not a simulation stage of emitter '%s' (it has %d)."), StageIndex, *Handle.GetName().ToString(), Stages.Num()));
				return nullptr;
			}
			UsageId = Stages[StageIndex]->Script->GetUsageId();
		}
		else
		{
			const TArray<FNiagaraEventScriptProperties>& Handlers = EmitterData->GetEventHandlers();
			if (!Handlers.IsValidIndex(StageIndex) || Handlers[StageIndex].Script == nullptr)
			{
				ReportError(FString::Printf(TEXT("StageIndex %d is not an event handler of emitter '%s' (it has %d)."), StageIndex, *Handle.GetName().ToString(), Handlers.Num()));
				return nullptr;
			}
			UsageId = Handlers[StageIndex].Script->GetUsageId();
		}

		UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
		UNiagaraGraph* Graph = Source ? Source->NodeGraph.Get() : nullptr;
		TArray<UNiagaraNodeOutput*> OutputNodes;
		if (Graph)
		{
			Graph->GetNodesOfClass(OutputNodes);
		}
		for (UNiagaraNodeOutput* OutputNode : OutputNodes)
		{
			if (OutputNode->GetUsage() == TargetStack && OutputNode->GetUsageId() == UsageId)
			{
				return OutputNode;
			}
		}
		ReportError(TEXT("The stage has no output node in the emitter graph. Open the system once in the Niagara editor to repair it."));
		return nullptr;
	}

	bool CheckModuleAllowed(const UNiagaraScript* Module, ENiagaraScriptUsage TargetStack)
	{
		if (Module == nullptr || Module->GetUsage() != ENiagaraScriptUsage::Module)
		{
			ReportError(TEXT("ModuleScript must be a Niagara module script."));
			return false;
		}
		const FVersionedNiagaraScriptData* ScriptData = Module->GetLatestScriptData();
		if (ScriptData && (ScriptData->ModuleUsageBitmask & (1 << static_cast<int32>(TargetStack))) == 0)
		{
			ReportError(FString::Printf(TEXT("Module '%s' is not allowed in %s stacks (see SetModuleScriptInfo)."), *Module->GetName(), *UsageToString(TargetStack)));
			return false;
		}
		return true;
	}

	UNiagaraNodeFunctionCall* PlaceInStage(UNiagaraSystem& System, FNiagaraEmitterHandle& Handle, ENiagaraScriptUsage TargetStack, int32 StageIndex, UNiagaraScript* Module, int32 TargetIndex)
	{
		UNiagaraNodeOutput* OutputNode = FindStageOutputNode(Handle, TargetStack, StageIndex);
		if (OutputNode == nullptr || !CheckModuleAllowed(Module, TargetStack))
		{
			return nullptr;
		}
		System.Modify();
		return FNiagaraStackGraphUtilities::AddScriptModuleToStack(Module, *OutputNode, TargetIndex < 0 ? INDEX_NONE : TargetIndex, FString(), Module->GetExposedVersion().VersionGuid);
	}

	/** Reads a placed module back through a fresh stack (by its node guid). */
	FNiagaraMcpModuleInfo DescribePlaced(UNiagaraSystem& System, FName EmitterName, const UNiagaraNodeFunctionCall* Node)
	{
		FNiagaraMcpModuleInfo Info;
		if (Node == nullptr)
		{
			return Info;
		}
		TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(System);
		UNiagaraStackViewModel* StackViewModel = ViewModel.IsValid() ? Stack::GetStackViewModel(*ViewModel, EmitterName) : nullptr;
		if (StackViewModel)
		{
			TArray<UNiagaraStackModuleItem*> Items;
			Stack::CollectModuleItems(*StackViewModel, Items);
			for (UNiagaraStackModuleItem* Item : Items)
			{
				if (&Item->GetModuleNode() == Node)
				{
					return NiagaraMcpTools::DescribeModule(*Item, INDEX_NONE, true);
				}
			}
		}
		Info.ModuleId = Node->NodeGuid.ToString(EGuidFormats::Digits);
		Info.FunctionName = Node->GetFunctionName();
		return Info;
	}

	FNiagaraMcpLocalModuleInfo DescribeLocalModule(UNiagaraScript* Script)
	{
		FNiagaraMcpLocalModuleInfo Info;
		if (Script == nullptr)
		{
			return Info;
		}
		Info.ScriptPath = Script->GetPathName();
		Info.Name = Script->GetName();
		if (const FVersionedNiagaraScriptData* ScriptData = Script->GetLatestScriptData())
		{
			for (ENiagaraScriptUsage Usage : UNiagaraScript::GetSupportedUsageContextsForBitmask(ScriptData->ModuleUsageBitmask))
			{
				Info.AllowedStacks.Add(UsageToString(Usage));
			}
		}
		return Info;
	}
}

using namespace NiagaraMcpStages;

FNiagaraMcpEmitterStages UNiagaraMcpEditorTools::GetEmitterStages(UNiagaraSystem* System, FName EmitterName)
{
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	FNiagaraEmitterHandle* Handle = Stack::FindEmitter(*System, EmitterName);
	return Handle ? DescribeStages(*System, *Handle) : FNiagaraMcpEmitterStages();
}

FNiagaraMcpEmitterStages UNiagaraMcpEditorTools::AddSimulationStage(UNiagaraSystem* System, FName EmitterName, FName StageName, const FString& IterationSource, FName DataInterfaceName, const FString& DataInterfaceClass, int32 TargetIndex, bool bSave)
{
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	FNiagaraEmitterHandle* Handle = Stack::FindEmitter(*System, EmitterName);
	FVersionedNiagaraEmitterData* EmitterData = Handle ? Handle->GetEmitterData() : nullptr;
	if (EmitterData == nullptr)
	{
		return {};
	}

	int64 SourceValue = static_cast<int64>(ENiagaraIterationSource::Particles);
	if (!IterationSource.TrimStartAndEnd().IsEmpty() && !ParseEnumText(StaticEnum<ENiagaraIterationSource>(), IterationSource, SourceValue))
	{
		ReportError(FString::Printf(TEXT("Unknown IterationSource '%s'. Use Particles or DataInterface."), *IterationSource));
		return {};
	}
	const ENiagaraIterationSource Source = static_cast<ENiagaraIterationSource>(SourceValue);
	UClass* InterfaceClass = nullptr;
	if (Source == ENiagaraIterationSource::DataInterface)
	{
		FNiagaraTypeDefinition InterfaceType;
		if (DataInterfaceName.IsNone() || !ResolveNiagaraType(DataInterfaceClass, InterfaceType) || !InterfaceType.IsDataInterface())
		{
			ReportError(TEXT("DataInterface iteration needs DataInterfaceName (e.g. Emitter.Grid) and a data interface DataInterfaceClass (e.g. NiagaraDataInterfaceGrid2DCollection)."));
			return {};
		}
		InterfaceClass = InterfaceType.GetClass();
	}

	const TArray<UNiagaraSimulationStageBase*> StagesBefore = EmitterData->GetSimulationStages();
	if (TargetIndex < -1 || TargetIndex > StagesBefore.Num())
	{
		ReportError(FString::Printf(TEXT("TargetIndex %d is out of range; use -1 or 0-%d."), TargetIndex, StagesBefore.Num()));
		return {};
	}

	// Acquire the view model before opening the transaction: a headless view model builds a sequencer whose
	// undoable objects would otherwise be recorded in this tool's undo entry.
	TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(*System);
	if (!ViewModel.IsValid())
	{
		return {};
	}
	FScopedTransaction Transaction(LOCTEXT("AddSimulationStage", "Niagara MCP: Add Simulation Stage"));
	{
		const FText StageDisplayName = UNiagaraSimulationStageGeneric::StaticClass()->GetDisplayNameText();
		INiagaraStackItemGroupAddUtilities* AddUtilities = nullptr;
		TSharedPtr<INiagaraStackItemGroupAddAction> AddAction = FindStageAddAction(*ViewModel, EmitterName,
			[&StageDisplayName](const INiagaraStackItemGroupAddAction& Action) { return TextMatches(Action.GetDisplayName(), StageDisplayName); }, AddUtilities);
		if (!AddAction.IsValid())
		{
			Transaction.Cancel();
			return {};
		}
		AddUtilities->ExecuteAddAction(AddAction.ToSharedRef(), TargetIndex < 0 ? INDEX_NONE : TargetIndex);
	}

	EmitterData = Handle->GetEmitterData();
	UNiagaraSimulationStageBase* NewStage = nullptr;
	for (UNiagaraSimulationStageBase* Stage : EmitterData->GetSimulationStages())
	{
		if (!StagesBefore.Contains(Stage))
		{
			NewStage = Stage;
			break;
		}
	}
	if (NewStage == nullptr)
	{
		ReportError(TEXT("The stage menu action ran but no new simulation stage appeared."));
		return {};
	}

	NewStage->Modify();
	if (!StageName.IsNone())
	{
		NewStage->SimulationStageName = StageName;
	}
	FProperty* ChangedProperty = FindFProperty<FProperty>(UNiagaraSimulationStageBase::StaticClass(), GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageBase, SimulationStageName));
	if (UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(NewStage))
	{
		Generic->IterationSource = Source;
		if (Source == ENiagaraIterationSource::DataInterface)
		{
			Generic->DataInterface.BoundVariable = FNiagaraVariable(FNiagaraTypeDefinition(InterfaceClass), DataInterfaceName);
		}
		ChangedProperty = FindFProperty<FProperty>(UNiagaraSimulationStageGeneric::StaticClass(), GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageGeneric, DataInterface));
	}
	FPropertyChangedEvent ChangedEvent(ChangedProperty, EPropertyChangeType::ValueSet);
	NewStage->PostEditChangeProperty(ChangedEvent);

	Stack::FinishSystemEdit(*System, bSave);
	return DescribeStages(*System, *Handle);
}

FNiagaraMcpEmitterStages UNiagaraMcpEditorTools::AddEventHandler(UNiagaraSystem* System, FName EmitterName, FName SourceEmitterName, FName SourceEventName, const FString& ExecutionMode, int32 SpawnNumber, int32 MaxEventsPerFrame, bool bSave)
{
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	FNiagaraEmitterHandle* Handle = Stack::FindEmitter(*System, EmitterName);
	FVersionedNiagaraEmitterData* EmitterData = Handle ? Handle->GetEmitterData() : nullptr;
	if (EmitterData == nullptr)
	{
		return {};
	}
	if (EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim)
	{
		ReportError(FString::Printf(TEXT("Emitter '%s' is a GPU emitter. Niagara events only work on CPU emitters."), *EmitterName.ToString()));
		return {};
	}
	int64 ModeValue = static_cast<int64>(EScriptExecutionMode::EveryParticle);
	if (!ExecutionMode.TrimStartAndEnd().IsEmpty() && !ParseEnumText(StaticEnum<EScriptExecutionMode>(), ExecutionMode, ModeValue))
	{
		ReportError(FString::Printf(TEXT("Unknown ExecutionMode '%s'. Use EveryParticle or SpawnedParticles."), *ExecutionMode));
		return {};
	}
	FGuid SourceEmitterId;
	if (!SourceEmitterName.IsNone() && SourceEmitterName != EmitterName)
	{
		FNiagaraEmitterHandle* SourceHandle = Stack::FindEmitter(*System, SourceEmitterName);
		if (SourceHandle == nullptr)
		{
			return {};
		}
		SourceEmitterId = SourceHandle->GetId();
	}

	TSet<FGuid> UsageIdsBefore;
	for (const FNiagaraEventScriptProperties& Existing : EmitterData->GetEventHandlers())
	{
		if (Existing.Script)
		{
			UsageIdsBefore.Add(Existing.Script->GetUsageId());
		}
	}

	// Acquire the view model before opening the transaction: a headless view model builds a sequencer whose
	// undoable objects would otherwise be recorded in this tool's undo entry.
	TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(*System);
	if (!ViewModel.IsValid())
	{
		return {};
	}
	FScopedTransaction Transaction(LOCTEXT("AddEventHandler", "Niagara MCP: Add Event Handler"));
	{
		INiagaraStackItemGroupAddUtilities* AddUtilities = nullptr;
		TSharedPtr<INiagaraStackItemGroupAddAction> AddAction = FindStageAddAction(*ViewModel, EmitterName,
			[](const INiagaraStackItemGroupAddAction& Action) { return TextHasSource(Action.GetDisplayName(), TEXT("Event Handler")); }, AddUtilities);
		if (!AddAction.IsValid())
		{
			Transaction.Cancel();
			return {};
		}
		AddUtilities->ExecuteAddAction(AddAction.ToSharedRef(), INDEX_NONE);
	}

	EmitterData = Handle->GetEmitterData();
	int32 NewIndex = INDEX_NONE;
	for (int32 Index = 0; Index < EmitterData->EventHandlerScriptProps.Num(); ++Index)
	{
		const UNiagaraScript* Script = EmitterData->EventHandlerScriptProps[Index].Script;
		if (Script && !UsageIdsBefore.Contains(Script->GetUsageId()))
		{
			NewIndex = Index;
			break;
		}
	}
	if (NewIndex == INDEX_NONE)
	{
		ReportError(TEXT("The event handler menu action ran but no new event handler appeared."));
		return {};
	}

	const FVersionedNiagaraEmitter VersionedEmitter = Handle->GetInstance();
	VersionedEmitter.Emitter->Modify();
	FNiagaraEventScriptProperties& Props = EmitterData->EventHandlerScriptProps[NewIndex];
	Props.ExecutionMode = static_cast<EScriptExecutionMode>(ModeValue);
	Props.SpawnNumber = static_cast<uint32>(FMath::Max(0, SpawnNumber));
	Props.MaxEventsPerFrame = static_cast<uint32>(FMath::Max(0, MaxEventsPerFrame));
	Props.SourceEmitterID = SourceEmitterId;
	Props.SourceEventName = SourceEventName;
	FPropertyChangedEvent ChangedEvent(nullptr, EPropertyChangeType::ValueSet);
	VersionedEmitter.Emitter->PostEditChangeVersionedProperty(ChangedEvent, VersionedEmitter.Version);

	Stack::FinishSystemEdit(*System, bSave);
	return DescribeStages(*System, *Handle);
}

FNiagaraMcpModuleInfo UNiagaraMcpEditorTools::AddModuleToStage(UNiagaraSystem* System, FName EmitterName, const FString& StageKind, int32 StageIndex, const FString& ModuleScript, int32 TargetIndex, bool bSave)
{
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	const FString Kind = StageKind.TrimStartAndEnd();
	ENiagaraScriptUsage TargetStack;
	// Accept the same names GetStack reports (ParticleSimulationStageScript, ParticleEventScript) as well as the short ones.
	if (!ParseUsage(Kind, TargetStack))
	{
		return {};
	}
	if (TargetStack != ENiagaraScriptUsage::ParticleSimulationStageScript && TargetStack != ENiagaraScriptUsage::ParticleEventScript)
	{
		ReportError(TEXT("StageKind must be SimulationStage or EventHandler (ParticleSimulationStageScript / ParticleEventScript). Use AddModule for the standard stacks."));
		return {};
	}
	FNiagaraEmitterHandle* Handle = Stack::FindEmitter(*System, EmitterName);
	UNiagaraScript* Module = LoadAssetByPath<UNiagaraScript>(ModuleScript, TEXT("ModuleScript"));
	if (Handle == nullptr || Module == nullptr)
	{
		return {};
	}

	UNiagaraNodeFunctionCall* Node = nullptr;
	{
		FScopedTransaction Transaction(LOCTEXT("AddModuleToStage", "Niagara MCP: Add Module To Stage"));
		Node = PlaceInStage(*System, *Handle, TargetStack, StageIndex, Module, TargetIndex);
		if (Node == nullptr)
		{
			Transaction.Cancel();
			return {};
		}
	}
	Stack::FinishSystemEdit(*System, bSave);
	return DescribePlaced(*System, EmitterName, Node);
}

FNiagaraMcpLocalModuleInfo UNiagaraMcpEditorTools::CreateLocalModule(UNiagaraSystem* System, FName EmitterName, const FString& StackName, int32 StageIndex, FName ModuleName, bool bSave)
{
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	ENiagaraScriptUsage TargetStack;
	if (!ParseUsage(StackName, TargetStack))
	{
		return {};
	}
	const bool bStageStack = TargetStack == ENiagaraScriptUsage::ParticleSimulationStageScript || TargetStack == ENiagaraScriptUsage::ParticleEventScript;
	if (!bStageStack && !IsStandardStack(TargetStack))
	{
		ReportError(FString::Printf(TEXT("Stack %s cannot hold modules."), *UsageToString(TargetStack)));
		return {};
	}
	FNiagaraEmitterHandle* Handle = nullptr;
	if (!IsSystemStack(TargetStack))
	{
		Handle = Stack::FindEmitter(*System, EmitterName);
		if (Handle == nullptr)
		{
			return {};
		}
	}
	if (bStageStack && FindStageOutputNode(*Handle, TargetStack, StageIndex) == nullptr)
	{
		return {};
	}
	UNiagaraScript* DefaultModule = Cast<UNiagaraScript>(GetDefault<UNiagaraEditorSettings>()->DefaultModuleScript.TryLoad());
	if (DefaultModule == nullptr)
	{
		ReportError(TEXT("DefaultModuleScript in the Niagara editor settings could not be loaded."));
		return {};
	}

	FScopedTransaction Transaction(LOCTEXT("CreateLocalModule", "Niagara MCP: Create Local Module"));
	// Same as the Scratch Pad's "new module": duplicate the default module into the system.
	if (!System->HasAnyFlags(RF_Transactional))
	{
		System->SetFlags(RF_Transactional);
	}
	System->Modify();
	const FName BaseName = ModuleName.IsNone() ? FName(TEXT("ScratchModule")) : ModuleName;
	const FName ObjectName = StaticFindObjectFast(nullptr, System, BaseName) ? MakeUniqueObjectName(System, UNiagaraScript::StaticClass(), BaseName) : BaseName;
	UNiagaraScript* NewScript = CastChecked<UNiagaraScript>(StaticDuplicateObject(DefaultModule, System, ObjectName));
	NewScript->ClearFlags(RF_Public | RF_Standalone);
	NewScript->SetFlags(RF_Transactional);
	System->ScratchPadScripts.Add(NewScript);
	if (FVersionedNiagaraScriptData* ScriptData = NewScript->GetLatestScriptData())
	{
		ScriptData->ModuleUsageBitmask |= (1 << static_cast<int32>(TargetStack));
	}

	UNiagaraNodeFunctionCall* Node = nullptr;
	bool bReported = false;
	if (bStageStack)
	{
		Node = PlaceInStage(*System, *Handle, TargetStack, StageIndex, NewScript, INDEX_NONE);
		bReported = Node == nullptr;
	}
	else
	{
		FNiagaraExt_StackItemReference Ref;
		Ref.System = System;
		Ref.EmitterName = IsSystemStack(TargetStack) ? NAME_None : EmitterName;
		Ref.ScriptName = FName(*UsageToString(TargetStack));
		FNiagaraExternalEditContext EditContext(Ref);
		FNiagaraExt_ModuleTopology Topology;
		UNiagaraExternalEditUtilities::AddModule(Ref, NewScript, Topology, EditContext);

		// AddModule inserts the node first and may report topology errors afterwards, so always look for the
		// node: if it exists the module is placed (errors are logged as warnings); only when it doesn't is it
		// safe to discard the new script.
		TArray<UNiagaraNodeFunctionCall*> Calls;
		if (UNiagaraScriptSource* Source = IsSystemStack(TargetStack)
			? Cast<UNiagaraScriptSource>(System->GetSystemSpawnScript()->GetLatestSource())
			: Cast<UNiagaraScriptSource>(Handle->GetEmitterData()->GraphSource))
		{
			Source->NodeGraph->GetNodesOfClass(Calls);
		}
		for (UNiagaraNodeFunctionCall* Call : Calls)
		{
			if (Call->FunctionScript == NewScript)
			{
				Node = Call;
				break;
			}
		}
		if (EditContext.HasErrors())
		{
			for (const FText& ContextError : EditContext.Errors)
			{
				if (Node)
				{
					UE_LOG(LogNiagaraMcp, Warning, TEXT("CreateLocalModule: %s"), *ContextError.ToString());
				}
				else
				{
					ReportError(ContextError.ToString());
				}
			}
			bReported = Node == nullptr;
		}
	}

	if (Node == nullptr)
	{
		System->ScratchPadScripts.Remove(NewScript);
		NewScript->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		Transaction.Cancel();
		if (!bReported)
		{
			ReportError(TEXT("Placing the local module failed."));
		}
		return {};
	}

	Stack::FinishSystemEdit(*System, bSave);
	FNiagaraMcpLocalModuleInfo Info = DescribeLocalModule(NewScript);
	Info.PlacedModule = DescribePlaced(*System, IsSystemStack(TargetStack) ? NAME_None : EmitterName, Node);
	return Info;
}

TArray<FNiagaraMcpLocalModuleInfo> UNiagaraMcpEditorTools::ListLocalModules(UNiagaraSystem* System)
{
	TArray<FNiagaraMcpLocalModuleInfo> Modules;
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return Modules;
	}
	for (const TObjectPtr<UNiagaraScript>& Script : System->ScratchPadScripts)
	{
		if (Script)
		{
			Modules.Add(DescribeLocalModule(Script));
		}
	}
	return Modules;
}

#undef LOCTEXT_NAMESPACE
