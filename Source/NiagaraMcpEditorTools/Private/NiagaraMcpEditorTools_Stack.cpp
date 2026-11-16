#include "NiagaraMcpEditorTools.h"

#include "Curves/RichCurve.h"
#include "NiagaraDataInterface.h"
#include "NiagaraDataInterfaceCurveBase.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraMcpEditorToolsInternal.h"
#include "NiagaraMcpStackUtils.h"
#include "NiagaraMessages.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "ScopedTransaction.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

#define LOCTEXT_NAMESPACE "NiagaraMcpEditorTools"

using namespace NiagaraMcp;

namespace NiagaraMcpTools
{
	static FString VersionToString(const UNiagaraScript* Script, const FGuid& VersionGuid)
	{
		if (Script == nullptr)
		{
			return FString();
		}
		const FVersionedNiagaraScriptData* Data = VersionGuid.IsValid() ? Script->GetScriptData(VersionGuid) : Script->GetLatestScriptData();
		if (Data == nullptr)
		{
			return FString();
		}
		return FString::Printf(TEXT("%d.%d"), Data->Version.MajorVersion, Data->Version.MinorVersion);
	}

	FNiagaraMcpInputInfo DescribeInput(UNiagaraStackFunctionInput& Input)
	{
		FNiagaraMcpInputInfo Info;
		Info.Name = Input.GetInputParameterHandle().GetName().ToString();
		Info.DisplayName = Input.GetDisplayName().ToString();
		Info.Type = Input.GetInputType().GetName();
		Info.Mode = Stack::ValueModeToString(Input);
		Info.Value = Stack::InputValueToString(Input);
		Info.bStatic = Input.IsStaticParameter();
		Info.bCanReset = Input.CanReset();
		return Info;
	}

	FNiagaraMcpModuleInfo DescribeModule(UNiagaraStackModuleItem& Item, int32 IndexInStack, bool bIncludeInputs)
	{
		FNiagaraMcpModuleInfo Info;
		UNiagaraNodeFunctionCall& Node = Item.GetModuleNode();
		Info.ModuleId = Stack::GetModuleId(Item);
		Info.DisplayName = Item.GetDisplayName().ToString();
		Info.FunctionName = Node.GetFunctionName();
		Info.Stack = UsageToString(Stack::GetModuleUsage(Item));
		Info.Index = IndexInStack;
		Info.bEnabled = Item.GetIsEnabled();
		if (UNiagaraScript* Script = Node.FunctionScript)
		{
			Info.ScriptPath = Script->GetPathName();
			Info.ScriptVersion = VersionToString(Script, Node.SelectedScriptVersion);
			Info.LatestScriptVersion = VersionToString(Script, Script->GetExposedVersion().VersionGuid);
		}

		Info.bHasNote = Item.HasStackNoteData();
		if (Info.bHasNote)
		{
			const FNiagaraStackNoteData Note = Item.GetStackNoteData();
			Info.Note.Header = Note.MessageHeader.ToString();
			Info.Note.Message = Note.Message.ToString();
			Info.Note.bInline = Note.bInlineNote;
		}

		if (bIncludeInputs)
		{
			TArray<UNiagaraStackFunctionInput*> Inputs;
			Stack::GetModuleInputs(Item, Inputs);
			for (UNiagaraStackFunctionInput* Input : Inputs)
			{
				if (Input)
				{
					Info.Inputs.Add(DescribeInput(*Input));
				}
			}
		}
		return Info;
	}

	static int32 IndexInOwnStack(UNiagaraStackViewModel& StackViewModel, const UNiagaraStackModuleItem& Item)
	{
		TArray<UNiagaraStackModuleItem*> Items;
		Stack::CollectModuleItems(StackViewModel, Items);
		const ENiagaraScriptUsage Usage = Stack::GetModuleUsage(Item);
		int32 Index = 0;
		for (const UNiagaraStackModuleItem* Candidate : Items)
		{
			if (Candidate == &Item)
			{
				return Index;
			}
			if (Stack::GetModuleUsage(*Candidate) == Usage)
			{
				++Index;
			}
		}
		return INDEX_NONE;
	}

	FNiagaraMcpStackInfo DescribeStack(UNiagaraStackViewModel& StackViewModel, const FString& Owner, bool bIncludeInputs)
	{
		FNiagaraMcpStackInfo Info;
		Info.Owner = Owner;
		TArray<UNiagaraStackModuleItem*> Items;
		Stack::CollectModuleItems(StackViewModel, Items);
		TMap<ENiagaraScriptUsage, int32> Counters;
		for (UNiagaraStackModuleItem* Item : Items)
		{
			int32& Counter = Counters.FindOrAdd(Stack::GetModuleUsage(*Item));
			Info.Modules.Add(DescribeModule(*Item, Counter++, bIncludeInputs));
		}
		return Info;
	}

	/** View model + stack + module lookup shared by every module tool. */
	struct FModuleContext
	{
		TSharedPtr<FNiagaraSystemViewModel> ViewModel;
		UNiagaraStackViewModel* StackViewModel = nullptr;
		UNiagaraStackModuleItem* Item = nullptr;
		FName EmitterName;
		/** Node guid of the module; stays valid while the stack entries are rebuilt. */
		FString ModuleId;
	};

	static bool ResolveModule(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& StackName, FModuleContext& Out)
	{
		if (!CheckSystemArg(System))
		{
			return false;
		}
		Out.EmitterName = EmitterName;
		Out.ViewModel = Stack::AcquireViewModel(*System);
		Out.StackViewModel = Out.ViewModel.IsValid() ? Stack::GetStackViewModel(*Out.ViewModel, EmitterName) : nullptr;
		Out.Item = Out.StackViewModel ? Stack::FindModuleItem(*Out.StackViewModel, Module, StackName) : nullptr;
		if (Out.Item)
		{
			Out.ModuleId = Stack::GetModuleId(*Out.Item);
		}
		return Out.Item != nullptr;
	}

	static UNiagaraGraph* GetStackGraph(UNiagaraSystem& System, FName EmitterName, ENiagaraScriptUsage Usage)
	{
		UNiagaraScriptSource* Source = nullptr;
		if (IsSystemStack(Usage))
		{
			UNiagaraScript* SystemScript = System.GetSystemSpawnScript();
			Source = SystemScript ? Cast<UNiagaraScriptSource>(SystemScript->GetLatestSource()) : nullptr;
		}
		else
		{
			FNiagaraEmitterHandle* Handle = Stack::FindEmitter(System, EmitterName);
			FVersionedNiagaraEmitterData* EmitterData = Handle ? Handle->GetEmitterData() : nullptr;
			if (EmitterData == nullptr)
			{
				return nullptr;
			}
			Source = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
		}
		UNiagaraGraph* Graph = Source ? Source->NodeGraph.Get() : nullptr;
		if (Graph == nullptr)
		{
			ReportError(TEXT("The stack graph is missing."));
		}
		return Graph;
	}

	static UNiagaraNodeOutput* FindStandardOutputNode(UNiagaraGraph& Graph, ENiagaraScriptUsage Usage)
	{
		TArray<UNiagaraNodeOutput*> OutputNodes;
		Graph.GetNodesOfClass(OutputNodes);
		for (UNiagaraNodeOutput* OutputNode : OutputNodes)
		{
			if (OutputNode && OutputNode->GetUsage() == Usage)
			{
				return OutputNode;
			}
		}
		ReportError(FString::Printf(TEXT("No %s output node in the stack graph. Open the system once in the Niagara editor to repair it."), *UsageToString(Usage)));
		return nullptr;
	}

	/**
	 * Edits make the stack rebuild its entries; the old entries are finalized and must not be touched again.
	 * Look the module up again (by its node guid) in a freshly refreshed stack.
	 */
	static UNiagaraStackModuleItem* ReResolve(FModuleContext& Context)
	{
		Context.StackViewModel = Context.ViewModel.IsValid() ? Stack::GetStackViewModel(*Context.ViewModel, Context.EmitterName) : nullptr;
		Context.Item = Context.StackViewModel ? Stack::FindModuleItem(*Context.StackViewModel, Context.ModuleId, FString()) : nullptr;
		return Context.Item;
	}

	static UNiagaraStackFunctionInput* RefindInput(FModuleContext& Context, const FString& Input)
	{
		return ReResolve(Context) ? Stack::FindInput(*Context.Item, Input) : nullptr;
	}

	static FNiagaraMcpModuleInfo DescribeResolved(FModuleContext& Context, bool bIncludeInputs)
	{
		return ReResolve(Context) ? DescribeModule(*Context.Item, IndexInOwnStack(*Context.StackViewModel, *Context.Item), bIncludeInputs) : FNiagaraMcpModuleInfo();
	}

	static UClass* ResolveDataInterfaceClass(const FString& InName)
	{
		const FString Name = InName.TrimStartAndEnd();
		UClass* Class = Name.StartsWith(TEXT("/")) ? LoadObject<UClass>(nullptr, *Name) : FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst);
		if (Class == nullptr && !Name.StartsWith(TEXT("/")))
		{
			Class = FindFirstObject<UClass>(*(FString(TEXT("NiagaraDataInterface")) + Name), EFindFirstObjectOptions::NativeFirst);
		}
		if (Class == nullptr || !Class->IsChildOf(UNiagaraDataInterface::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
		{
			ReportError(FString::Printf(TEXT("'%s' is not a data interface class, e.g. NiagaraDataInterfaceCurve or /Script/Niagara.NiagaraDataInterfaceTexture."), *InName));
			return nullptr;
		}
		return Class;
	}

	/** FRichCurve channels of a curve data interface in declaration order (RedCurve, GreenCurve, ... / XCurve, ... / Curve). */
	static TArray<FStructProperty*> CurveChannels(const UNiagaraDataInterface& DataInterface)
	{
		TArray<FStructProperty*> Channels;
		for (TFieldIterator<FStructProperty> It(DataInterface.GetClass()); It; ++It)
		{
			if (It->Struct == FRichCurve::StaticStruct() && !It->GetName().Contains(TEXT("Cooked")))
			{
				Channels.Add(*It);
			}
		}
		return Channels;
	}

	static FNiagaraMcpCurveInfo DescribeCurve(UNiagaraDataInterface& DataInterface)
	{
		FNiagaraMcpCurveInfo Info;
		Info.DataInterfaceClass = DataInterface.GetClass()->GetName();
		const TArray<FStructProperty*> Channels = CurveChannels(DataInterface);
		TArray<float> Times;
		for (FStructProperty* Channel : Channels)
		{
			Info.Channels.Add(Channel->GetName());
			const FRichCurve* Curve = Channel->ContainerPtrToValuePtr<FRichCurve>(&DataInterface);
			for (const FRichCurveKey& Key : Curve->GetConstRefOfKeys())
			{
				if (!Times.ContainsByPredicate([&Key](float Time) { return FMath::IsNearlyEqual(Time, Key.Time, 1.e-4f); }))
				{
					Times.Add(Key.Time);
				}
			}
		}
		Times.Sort();

		TSet<ERichCurveInterpMode> Modes;
		for (FStructProperty* Channel : Channels)
		{
			for (const FRichCurveKey& Key : Channel->ContainerPtrToValuePtr<FRichCurve>(&DataInterface)->GetConstRefOfKeys())
			{
				Modes.Add(Key.InterpMode);
			}
		}
		if (Modes.Num() > 1)
		{
			Info.Interpolation = TEXT("Mixed");
		}
		else
		{
			const ERichCurveInterpMode Mode = Modes.Num() ? *Modes.CreateConstIterator() : RCIM_Cubic;
			Info.Interpolation = Mode == RCIM_Linear ? TEXT("Linear") : Mode == RCIM_Constant ? TEXT("Constant") : TEXT("Cubic");
		}

		for (float Time : Times)
		{
			FNiagaraMcpCurveKey& Key = Info.Keys.AddDefaulted_GetRef();
			Key.Time = Time;
			TArray<FString> Values;
			for (FStructProperty* Channel : Channels)
			{
				Values.Add(FString::SanitizeFloat(Channel->ContainerPtrToValuePtr<FRichCurve>(&DataInterface)->Eval(Time)));
			}
			Key.Value = FString::Join(Values, TEXT(", "));
		}
		return Info;
	}

	static UNiagaraDataInterface* ResolveCurveInput(FModuleContext& Context, const FString& Input)
	{
		UNiagaraStackFunctionInput* FunctionInput = Stack::FindInput(*Context.Item, Input);
		if (FunctionInput == nullptr)
		{
			return nullptr;
		}
		UNiagaraDataInterface* DataInterface = FunctionInput->GetDataValueObject();
		if (DataInterface == nullptr || CurveChannels(*DataInterface).Num() == 0)
		{
			ReportError(FString::Printf(TEXT("Input '%s' does not hold a curve data interface (it holds %s). Set it first with SetModuleInput Value \"Data:NiagaraDataInterfaceCurve\" (or ColorCurve, VectorCurve, Vector2DCurve, Vector4Curve)."),
				*Input, DataInterface ? *DataInterface->GetClass()->GetName() : TEXT("no data interface")));
			return nullptr;
		}
		return DataInterface;
	}
}

using namespace NiagaraMcpTools;

FNiagaraMcpModuleInfo UNiagaraMcpEditorTools::AddModule(UNiagaraSystem* System, FName EmitterName, const FString& StackName, const FString& ModuleScript, int32 TargetIndex, bool bSave)
{
	if (!CheckSystemArg(System))
	{
		return {};
	}
	ENiagaraScriptUsage Usage;
	if (!ParseUsage(StackName, Usage))
	{
		return {};
	}
	if (!IsStandardStack(Usage))
	{
		ReportError(TEXT("Stack must be SystemSpawn, SystemUpdate, EmitterSpawn, EmitterUpdate, ParticleSpawn or ParticleUpdate."));
		return {};
	}
	if (IsSystemStack(Usage) != Stack::IsSystemName(EmitterName))
	{
		ReportError(IsSystemStack(Usage)
			? TEXT("System stacks belong to the system; pass EmitterName None (or System).")
			: TEXT("Emitter and particle stacks need an EmitterName."));
		return {};
	}

	UNiagaraScript* Module = LoadAssetByPath<UNiagaraScript>(ModuleScript, TEXT("ModuleScript"));
	if (Module == nullptr)
	{
		return {};
	}
	if (Module->GetUsage() != ENiagaraScriptUsage::Module)
	{
		ReportError(FString::Printf(TEXT("'%s' is a %s script, not a module."), *Module->GetPathName(), *UsageToString(Module->GetUsage())));
		return {};
	}
	const FVersionedNiagaraScriptData* ScriptData = Module->GetLatestScriptData();
	if (ScriptData && (ScriptData->ModuleUsageBitmask & (1 << static_cast<int32>(Usage))) == 0)
	{
		ReportError(FString::Printf(TEXT("Module '%s' is not allowed in %s stacks. Use SetModuleScriptInfo to allow it, or pick another stack."), *Module->GetName(), *UsageToString(Usage)));
		return {};
	}

	UNiagaraGraph* Graph = GetStackGraph(*System, EmitterName, Usage);
	UNiagaraNodeOutput* OutputNode = Graph ? FindStandardOutputNode(*Graph, Usage) : nullptr;
	if (OutputNode == nullptr)
	{
		return {};
	}

	FScopedTransaction Transaction(LOCTEXT("AddModule", "Niagara MCP: Add Module"));
	System->Modify();
	UNiagaraNodeFunctionCall* NewNode = FNiagaraStackGraphUtilities::AddScriptModuleToStack(Module, *OutputNode, TargetIndex < 0 ? INDEX_NONE : TargetIndex, FString(), Module->GetExposedVersion().VersionGuid);
	if (NewNode == nullptr)
	{
		ReportError(FString::Printf(TEXT("Failed to add '%s' to %s."), *Module->GetName(), *UsageToString(Usage)));
		Transaction.Cancel();
		return {};
	}
	Stack::FinishSystemEdit(*System, bSave);

	// Read back through a fresh stack so the new module's inputs are built.
	TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(*System);
	UNiagaraStackViewModel* StackViewModel = ViewModel.IsValid() ? Stack::GetStackViewModel(*ViewModel, EmitterName) : nullptr;
	if (StackViewModel)
	{
		TArray<UNiagaraStackModuleItem*> Items;
		Stack::CollectModuleItems(*StackViewModel, Items);
		for (UNiagaraStackModuleItem* Item : Items)
		{
			if (&Item->GetModuleNode() == NewNode)
			{
				return DescribeModule(*Item, IndexInOwnStack(*StackViewModel, *Item), true);
			}
		}
	}

	FNiagaraMcpModuleInfo Info;
	Info.ModuleId = NewNode->NodeGuid.ToString(EGuidFormats::Digits);
	Info.FunctionName = NewNode->GetFunctionName();
	Info.Stack = UsageToString(Usage);
	Info.ScriptPath = Module->GetPathName();
	return Info;
}

FNiagaraMcpStackInfo UNiagaraMcpEditorTools::RemoveModule(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& StackName, bool bSave)
{
	FModuleContext Context;
	if (!ResolveModule(System, EmitterName, Module, StackName, Context))
	{
		return {};
	}
	FText CannotDelete;
	if (!Context.Item->TestCanDeleteWithMessage(CannotDelete))
	{
		ReportError(FString::Printf(TEXT("Module '%s' cannot be removed: %s"), *Context.Item->GetDisplayName().ToString(), *CannotDelete.ToString()));
		return {};
	}

	{
		FScopedTransaction Transaction(LOCTEXT("RemoveModule", "Niagara MCP: Remove Module"));
		System->Modify();
		Context.Item->Delete();
	}
	Stack::FinishSystemEdit(*System, bSave);

	return DescribeStack(*Context.StackViewModel, Stack::IsSystemName(EmitterName) ? FString(TEXT("System")) : EmitterName.ToString(), false);
}

FNiagaraMcpModuleInfo UNiagaraMcpEditorTools::SetModuleEnabled(UNiagaraSystem* System, FName EmitterName, const FString& Module, bool bEnabled, const FString& StackName, bool bSave)
{
	FModuleContext Context;
	if (!ResolveModule(System, EmitterName, Module, StackName, Context))
	{
		return {};
	}
	{
		FScopedTransaction Transaction(LOCTEXT("SetModuleEnabled", "Niagara MCP: Enable/Disable Module"));
		System->Modify();
		FNiagaraStackGraphUtilities::SetModuleIsEnabled(Context.Item->GetModuleNode(), bEnabled);
	}
	Stack::FinishSystemEdit(*System, bSave);
	return DescribeResolved(Context, false);
}

FNiagaraMcpInputInfo UNiagaraMcpEditorTools::SetModuleInput(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& Input, const FString& Value, const FString& StackName, bool bSave)
{
	FModuleContext Context;
	if (!ResolveModule(System, EmitterName, Module, StackName, Context))
	{
		return {};
	}
	UNiagaraStackFunctionInput* FunctionInput = Stack::FindInput(*Context.Item, Input);
	if (FunctionInput == nullptr)
	{
		return {};
	}

	const FString Trimmed = Value.TrimStartAndEnd();
	const FNiagaraTypeDefinition InputType = FunctionInput->GetInputType();

	// Resolve and validate everything before touching the system: a cancelled transaction does not roll back.
	enum class EKind { Linked, Expression, Dynamic, Data, Asset, Local };
	EKind Kind = EKind::Local;
	UNiagaraScript* DynamicInput = nullptr;
	UClass* DataInterfaceClass = nullptr;
	UObject* Asset = nullptr;
	TSharedPtr<FStructOnScope> NewValue;

	if (Trimmed.StartsWith(TEXT("@")))
	{
		Kind = EKind::Linked;
		const FString LinkName = Trimmed.Mid(1).TrimStartAndEnd();
		if (LinkName.IsEmpty() || !LinkName.Contains(TEXT(".")))
		{
			ReportError(FString::Printf(TEXT("A linked value needs a namespaced parameter name, e.g. \"@Particles.NormalizedAge\", \"@Emitter.Age\" or \"@User.%s\" (got \"%s\")."),
				LinkName.IsEmpty() ? TEXT("Color") : *LinkName, *Trimmed));
			return {};
		}
		// A link to a missing user parameter would silently read zero: check User.* names against the system.
		if (LinkName.StartsWith(TEXT("User."), ESearchCase::IgnoreCase))
		{
			TArray<FNiagaraVariable> UserParameters;
			System->GetExposedParameters().GetUserParameters(UserParameters);
			TArray<FString> Names;
			bool bFound = false;
			for (const FNiagaraVariable& Parameter : UserParameters)
			{
				FString Name = Parameter.GetName().ToString();
				if (!Name.StartsWith(TEXT("User.")))
				{
					Name = TEXT("User.") + Name;
				}
				Names.Add(Name);
				bFound |= Name.Equals(LinkName, ESearchCase::IgnoreCase);
			}
			if (!bFound)
			{
				ReportError(FString::Printf(TEXT("User parameter '%s' does not exist.%s Create it with SetUserParameter first. User parameters: %s"),
					*LinkName, *DidYouMean(LinkName, Names), Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("(none)")));
				return {};
			}
		}
	}
	else if (Trimmed.StartsWith(TEXT("=")))
	{
		Kind = EKind::Expression;
	}
	else if (Trimmed.StartsWith(TEXT("Dynamic:"), ESearchCase::IgnoreCase))
	{
		Kind = EKind::Dynamic;
		DynamicInput = LoadAssetByPath<UNiagaraScript>(Trimmed.Mid(8).TrimStartAndEnd(), TEXT("Dynamic input"));
		if (DynamicInput == nullptr)
		{
			return {};
		}
		if (DynamicInput->GetUsage() != ENiagaraScriptUsage::DynamicInput)
		{
			ReportError(FString::Printf(TEXT("'%s' is not a dynamic input script."), *DynamicInput->GetPathName()));
			return {};
		}
	}
	else if (Trimmed.StartsWith(TEXT("Data:"), ESearchCase::IgnoreCase))
	{
		Kind = EKind::Data;
		DataInterfaceClass = ResolveDataInterfaceClass(Trimmed.Mid(5));
		if (DataInterfaceClass == nullptr)
		{
			return {};
		}
		// Writing back the value GetStack reported ("Data:<same class>") must not replace the data interface with a
		// default one (that would drop its curve keys / settings): it is already set, nothing to do.
		const UNiagaraDataInterface* Current = FunctionInput->GetValueMode() == UNiagaraStackFunctionInput::EValueMode::Data ? FunctionInput->GetDataValueObject() : nullptr;
		if (Current && Current->GetClass() == DataInterfaceClass)
		{
			return DescribeInput(*FunctionInput);
		}
	}
	else if (Trimmed.StartsWith(TEXT("Asset:"), ESearchCase::IgnoreCase))
	{
		Kind = EKind::Asset;
		Asset = LoadObjectByPath(Trimmed.Mid(6).TrimStartAndEnd(), nullptr, TEXT("Asset"));
		if (Asset == nullptr)
		{
			return {};
		}
	}
	else
	{
		if (InputType.IsDataInterface())
		{
			ReportError(FString::Printf(TEXT("Input '%s' is a data interface (%s). Use \"Data:<Class>\" to pick its class and SetDataInterfaceInputProperty to edit it."), *Input, *InputType.GetName()));
			return {};
		}
		const UScriptStruct* Struct = InputType.GetScriptStruct();
		if (Struct == nullptr)
		{
			ReportError(FString::Printf(TEXT("Input '%s' (%s) has no local value layout."), *Input, *InputType.GetName()));
			return {};
		}
		NewValue = MakeShared<FStructOnScope>(Struct);
		TSharedPtr<const FStructOnScope> Current = FunctionInput->GetLocalValueStruct();
		if (Current.IsValid() && Current->IsValid() && Current->GetStruct() == Struct)
		{
			Struct->CopyScriptStruct(NewValue->GetStructMemory(), Current->GetStructMemory());
		}
		FString ValueError;
		if (!ImportStructValue(Struct, NewValue->GetStructMemory(), Trimmed, InputType.GetEnum(), ValueError))
		{
			ReportError(FString::Printf(TEXT("Input '%s' (%s): %s"), *Input, *InputType.GetName(), *ValueError));
			return {};
		}
	}

	{
		FScopedTransaction Transaction(LOCTEXT("SetModuleInput", "Niagara MCP: Set Module Input"));
		System->Modify();
		switch (Kind)
		{
		case EKind::Linked:
			FunctionInput->SetLinkedParameterValue(FNiagaraVariableBase(InputType, FName(*Trimmed.Mid(1).TrimStartAndEnd())));
			break;
		case EKind::Expression:
			FunctionInput->SetCustomExpression(Trimmed.Mid(1).TrimStartAndEnd());
			break;
		case EKind::Dynamic:
			FunctionInput->SetDynamicInput(DynamicInput, FString(), DynamicInput->GetExposedVersion().VersionGuid);
			break;
		case EKind::Data:
			FunctionInput->SetDataInterfaceValue(DataInterfaceClass);
			break;
		case EKind::Asset:
			FunctionInput->SetObjectAssetValue(Asset);
			break;
		default:
			FunctionInput->SetLocalValue(NewValue.ToSharedRef());
			break;
		}
	}

	Stack::FinishSystemEdit(*System, bSave);
	UNiagaraStackFunctionInput* After = RefindInput(Context, Input);
	return After ? DescribeInput(*After) : FNiagaraMcpInputInfo();
}

FNiagaraMcpInputInfo UNiagaraMcpEditorTools::ResetModuleInput(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& Input, const FString& StackName, bool bSave)
{
	FModuleContext Context;
	if (!ResolveModule(System, EmitterName, Module, StackName, Context))
	{
		return {};
	}
	UNiagaraStackFunctionInput* FunctionInput = Stack::FindInput(*Context.Item, Input);
	if (FunctionInput == nullptr)
	{
		return {};
	}
	if (FunctionInput->CanReset())
	{
		FScopedTransaction Transaction(LOCTEXT("ResetModuleInput", "Niagara MCP: Reset Module Input"));
		System->Modify();
		FunctionInput->Reset();
		Stack::FinishSystemEdit(*System, bSave);
	}
	UNiagaraStackFunctionInput* After = RefindInput(Context, Input);
	return After ? DescribeInput(*After) : FNiagaraMcpInputInfo();
}

FNiagaraMcpPropertyResult UNiagaraMcpEditorTools::SetDataInterfaceInputProperty(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& Input, const FString& PropertyName, const FString& Value, const FString& StackName, bool bSave)
{
	FModuleContext Context;
	if (!ResolveModule(System, EmitterName, Module, StackName, Context))
	{
		return {};
	}
	UNiagaraStackFunctionInput* FunctionInput = Stack::FindInput(*Context.Item, Input);
	if (FunctionInput == nullptr)
	{
		return {};
	}
	UNiagaraDataInterface* DataInterface = FunctionInput->GetDataValueObject();
	if (DataInterface == nullptr)
	{
		ReportError(FString::Printf(TEXT("Input '%s' has no data interface value (mode %s)."), *Input, *Stack::ValueModeToString(*FunctionInput)));
		return {};
	}

	FScopedTransaction Transaction(LOCTEXT("SetDataInterfaceProperty", "Niagara MCP: Set Data Interface Property"));
	DataInterface->Modify();
	FString PropertyError;
	FProperty* Changed = SetPropertyFromText(DataInterface->GetClass(), DataInterface, DataInterface, PropertyName, Value, PropertyError);
	if (Changed == nullptr)
	{
		ReportError(PropertyError);
		Transaction.Cancel();
		return {};
	}
	FPropertyChangedEvent ChangedEvent(Changed, EPropertyChangeType::ValueSet);
	DataInterface->PostEditChangeProperty(ChangedEvent);
	Stack::SyncCompiledDataInterface(*System, *DataInterface);
	Stack::FinishSystemEdit(*System, false);

	FNiagaraMcpPropertyResult Result;
	Result.Property = PropertyName;
	Result.Value = GetPropertyText(DataInterface->GetClass(), DataInterface, PropertyName);
	Result.bSaved = bSave && SaveAssetSilently(System);
	return Result;
}

FNiagaraMcpModuleInfo UNiagaraMcpEditorTools::UpgradeModuleVersion(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& StackName, bool bSave)
{
	FModuleContext Context;
	if (!ResolveModule(System, EmitterName, Module, StackName, Context))
	{
		return {};
	}
	UNiagaraScript* Script = Context.Item->GetModuleNode().FunctionScript;
	if (Script == nullptr)
	{
		ReportError(TEXT("This module has no script asset to upgrade."));
		return {};
	}
	const FGuid Latest = Script->GetExposedVersion().VersionGuid;
	if (Context.Item->GetModuleNode().SelectedScriptVersion != Latest)
	{
		FScopedTransaction Transaction(LOCTEXT("UpgradeModuleVersion", "Niagara MCP: Upgrade Module Version"));
		System->Modify();
		Context.Item->ChangeScriptVersion(Latest);
		Stack::FinishSystemEdit(*System, bSave);
	}
	return DescribeResolved(Context, true);
}

FNiagaraMcpNoteInfo UNiagaraMcpEditorTools::SetModuleNote(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& Header, const FString& Message, bool bInline, const FString& StackName, bool bSave)
{
	FModuleContext Context;
	if (!ResolveModule(System, EmitterName, Module, StackName, Context))
	{
		return {};
	}

	{
		FScopedTransaction Transaction(LOCTEXT("SetModuleNote", "Niagara MCP: Set Module Note"));
		System->Modify();
		if (Header.IsEmpty() && Message.IsEmpty())
		{
			if (Context.Item->HasStackNoteData())
			{
				Context.Item->DeleteStackNoteData();
			}
		}
		else
		{
			FNiagaraStackNoteData Note;
			Note.MessageHeader = FText::FromString(Header);
			Note.Message = FText::FromString(Message);
			Note.bInlineNote = bInline;
			Context.Item->SetStackNoteData(Note);
		}
	}
	System->MarkPackageDirty();
	Stack::FinishSystemEdit(*System, bSave);

	FNiagaraMcpNoteInfo Info;
	if (ReResolve(Context) && Context.Item->HasStackNoteData())
	{
		const FNiagaraStackNoteData Note = Context.Item->GetStackNoteData();
		Info.Header = Note.MessageHeader.ToString();
		Info.Message = Note.Message.ToString();
		Info.bInline = Note.bInlineNote;
	}
	return Info;
}

FNiagaraMcpCurveInfo UNiagaraMcpEditorTools::GetCurveKeys(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& Input, const FString& StackName)
{
	FModuleContext Context;
	if (!ResolveModule(System, EmitterName, Module, StackName, Context))
	{
		return {};
	}
	UNiagaraDataInterface* DataInterface = ResolveCurveInput(Context, Input);
	return DataInterface ? DescribeCurve(*DataInterface) : FNiagaraMcpCurveInfo();
}

FNiagaraMcpCurveInfo UNiagaraMcpEditorTools::SetCurveKeys(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& Input, const TArray<FNiagaraMcpCurveKey>& Keys, const FString& Interpolation, const FString& StackName, bool bSave)
{
	FModuleContext Context;
	if (!ResolveModule(System, EmitterName, Module, StackName, Context))
	{
		return {};
	}
	UNiagaraDataInterface* DataInterface = ResolveCurveInput(Context, Input);
	if (DataInterface == nullptr)
	{
		return {};
	}
	if (Keys.Num() == 0)
	{
		ReportError(TEXT("Keys is empty."));
		return {};
	}

	ERichCurveInterpMode InterpMode = RCIM_Cubic;
	const FString Interp = Interpolation.TrimStartAndEnd();
	if (Interp.Equals(TEXT("Linear"), ESearchCase::IgnoreCase))
	{
		InterpMode = RCIM_Linear;
	}
	else if (Interp.Equals(TEXT("Constant"), ESearchCase::IgnoreCase))
	{
		InterpMode = RCIM_Constant;
	}
	else if (!Interp.IsEmpty() && !Interp.Equals(TEXT("Cubic"), ESearchCase::IgnoreCase))
	{
		ReportError(FString::Printf(TEXT("Unknown interpolation '%s'. Use Cubic, Linear or Constant."), *Interpolation));
		return {};
	}

	// Parse everything first so a bad key changes nothing.
	const TArray<FStructProperty*> Channels = CurveChannels(*DataInterface);
	TArray<TArray<float>> Values;
	for (const FNiagaraMcpCurveKey& Key : Keys)
	{
		TArray<FString> Parts;
		Key.Value.Replace(TEXT(","), TEXT(" ")).ParseIntoArrayWS(Parts);
		if (Parts.Num() != 1 && Parts.Num() != Channels.Num())
		{
			ReportError(FString::Printf(TEXT("Key at %g has %d values; %s needs %d (%s) or 1 for all."), Key.Time, Parts.Num(),
				*DataInterface->GetClass()->GetName(), Channels.Num(), *FString::JoinBy(Channels, TEXT(", "), [](const FStructProperty* P) { return P->GetName(); })));
			return {};
		}
		TArray<float>& Row = Values.AddDefaulted_GetRef();
		for (int32 Channel = 0; Channel < Channels.Num(); ++Channel)
		{
			const FString& Part = Parts.Num() == 1 ? Parts[0] : Parts[Channel];
			if (!Part.IsNumeric())
			{
				ReportError(FString::Printf(TEXT("'%s' is not a number."), *Part));
				return {};
			}
			Row.Add(FCString::Atof(*Part));
		}
	}

	{
		FScopedTransaction Transaction(LOCTEXT("SetCurveKeys", "Niagara MCP: Set Curve Keys"));
		System->Modify();
		DataInterface->Modify();
		for (int32 Channel = 0; Channel < Channels.Num(); ++Channel)
		{
			FRichCurve* Curve = Channels[Channel]->ContainerPtrToValuePtr<FRichCurve>(DataInterface);
			Curve->Reset();
			for (int32 KeyIndex = 0; KeyIndex < Keys.Num(); ++KeyIndex)
			{
				const FKeyHandle Handle = Curve->AddKey(Keys[KeyIndex].Time, Values[KeyIndex][Channel]);
				Curve->SetKeyInterpMode(Handle, InterpMode);
				if (InterpMode == RCIM_Cubic)
				{
					Curve->SetKeyTangentMode(Handle, RCTM_Auto);
				}
			}
			Curve->AutoSetTangents();
		}
		if (UNiagaraDataInterfaceCurveBase* CurveBase = Cast<UNiagaraDataInterfaceCurveBase>(DataInterface))
		{
			CurveBase->UpdateLUT();
		}
		FPropertyChangedEvent ChangedEvent(Channels[0], EPropertyChangeType::ValueSet);
		DataInterface->PostEditChangeProperty(ChangedEvent);
	}
	Stack::SyncCompiledDataInterface(*System, *DataInterface);
	Stack::FinishSystemEdit(*System, false);

	FNiagaraMcpCurveInfo Info = DescribeCurve(*DataInterface);
	Info.bSaved = bSave && SaveAssetSilently(System);
	return Info;
}

#undef LOCTEXT_NAMESPACE
