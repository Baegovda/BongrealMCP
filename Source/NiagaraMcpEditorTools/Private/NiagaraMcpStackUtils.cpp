#include "NiagaraMcpStackUtils.h"

#include "Editor.h"
#include "NiagaraEditorModule.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

namespace NiagaraMcp::Stack
{
	EEditorLookup FindEditorViewModel(UNiagaraSystem& System, TSharedPtr<FNiagaraSystemViewModel>& OutViewModel)
	{
		OutViewModel.Reset();
		if (!IsAssetEditorOpen(&System))
		{
			return EEditorLookup::NotOpen;
		}

		// Every system view model registers itself for its system and the lookup returns the oldest one.
		// Headless models only live for a single call, so while the editor is open the oldest one is the
		// editor's. Only editor models own a preview component, which confirms it.
		TSharedPtr<FNiagaraSystemViewModel> ViewModel = FNiagaraEditorModule::Get().GetExistingViewModelForSystem(&System);
		if (!ViewModel.IsValid() || ViewModel->GetPreviewComponent() == nullptr)
		{
			return EEditorLookup::Unreachable;
		}
		OutViewModel = ViewModel;
		return EEditorLookup::Found;
	}

	static TSharedRef<FNiagaraSystemViewModel> MakeHeadlessViewModel(UNiagaraSystem& System)
	{
		TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeShared<FNiagaraSystemViewModel>();
		FNiagaraSystemViewModelOptions Options;
		Options.bCanModifyEmittersFromTimeline = false;
		Options.bCompileForEdit = false;
		Options.bCanSimulate = false;
		Options.bCanAutoCompile = false;
		// Not data-only: data-only stacks drop module issues and treat inherited modules as deletable (see
		// UNiagaraStackModuleItem::RefreshIssues and UNiagaraStackEntry::HasBaseEmitter), so results would differ from the editor.
		Options.bIsForDataProcessingOnly = false;
		// Initialize subscribes to the Niagara message manager with this key; an unset key asserts.
		Options.MessageLogGuid = System.GetAssetGuid();
		ViewModel->Initialize(System, Options);
		return ViewModel;
	}

	TSharedPtr<FNiagaraSystemViewModel> AcquireViewModel(UNiagaraSystem& System)
	{
		TSharedPtr<FNiagaraSystemViewModel> EditorViewModel;
		switch (FindEditorViewModel(System, EditorViewModel))
		{
		case EEditorLookup::Found:
			return EditorViewModel;
		case EEditorLookup::NotOpen:
			return MakeHeadlessViewModel(System);
		default:
			ReportError(FString::Printf(TEXT("The Niagara editor for '%s' is open but its view model could not be found. Close and reopen that editor, then retry."), *System.GetPathName()));
			return nullptr;
		}
	}

	bool IsSystemName(FName EmitterName)
	{
		return EmitterName.IsNone() || EmitterName.ToString().Equals(TEXT("System"), ESearchCase::IgnoreCase);
	}

	FNiagaraEmitterHandle* FindEmitter(UNiagaraSystem& System, FName EmitterName)
	{
		TArray<FString> Names;
		for (FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
		{
			if (Handle.GetName() == EmitterName || Handle.GetName().ToString().Equals(EmitterName.ToString(), ESearchCase::IgnoreCase))
			{
				return &Handle;
			}
			Names.Add(Handle.GetName().ToString());
		}
		ReportError(FString::Printf(TEXT("[EMITTER_NOT_FOUND] Emitter '%s' not found in '%s'.%s Emitters: %s"),
			*EmitterName.ToString(), *System.GetName(), *DidYouMean(EmitterName.ToString(), Names), Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("(none)")));
		return nullptr;
	}

	UNiagaraStackViewModel* GetStackViewModel(FNiagaraSystemViewModel& ViewModel, FName EmitterName)
	{
		if (IsSystemName(EmitterName))
		{
			UNiagaraStackViewModel* SystemStack = ViewModel.GetSystemStackViewModel();
			if (SystemStack == nullptr)
			{
				ReportError(TEXT("Could not build the system stack."));
			}
			return SystemStack;
		}

		TArray<FString> Names;
		for (const TSharedRef<FNiagaraEmitterHandleViewModel>& HandleViewModel : ViewModel.GetEmitterHandleViewModels())
		{
			if (HandleViewModel->GetName().ToString().Equals(EmitterName.ToString(), ESearchCase::IgnoreCase))
			{
				UNiagaraStackViewModel* StackViewModel = HandleViewModel->GetEmitterStackViewModel();
				if (StackViewModel == nullptr)
				{
					ReportError(FString::Printf(TEXT("Could not build the stack for emitter '%s'."), *EmitterName.ToString()));
				}
				return StackViewModel;
			}
			Names.Add(HandleViewModel->GetName().ToString());
		}
		ReportError(FString::Printf(TEXT("[EMITTER_NOT_FOUND] Emitter '%s' not found.%s Emitters: %s (use None or System for the system stacks)"), *EmitterName.ToString(), *DidYouMean(EmitterName.ToString(), Names), Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("(none)")));
		return nullptr;
	}

	static void CollectModuleItemsRecursive(UNiagaraStackEntry* Entry, TArray<UNiagaraStackModuleItem*>& OutItems, int32 Depth)
	{
		if (Entry == nullptr || Depth > 8)
		{
			return;
		}
		if (UNiagaraStackModuleItem* ModuleItem = Cast<UNiagaraStackModuleItem>(Entry))
		{
			OutItems.Add(ModuleItem);
			return; // modules do not contain modules
		}
		TArray<UNiagaraStackEntry*> Children;
		Entry->GetUnfilteredChildren(Children);
		for (UNiagaraStackEntry* Child : Children)
		{
			CollectModuleItemsRecursive(Child, OutItems, Depth + 1);
		}
	}

	void CollectModuleItems(UNiagaraStackViewModel& StackViewModel, TArray<UNiagaraStackModuleItem*>& OutItems)
	{
		UNiagaraStackEntry* Root = StackViewModel.GetRootEntry();
		if (Root == nullptr)
		{
			return;
		}
		Root->RefreshChildren();
		CollectModuleItemsRecursive(Root, OutItems, 0);
	}

	ENiagaraScriptUsage GetModuleUsage(const UNiagaraStackModuleItem& Item)
	{
		const UNiagaraNodeOutput* OutputNode = Item.GetOutputNode();
		return OutputNode ? OutputNode->GetUsage() : ENiagaraScriptUsage::Module;
	}

	FString GetModuleId(const UNiagaraStackModuleItem& Item)
	{
		return Item.GetModuleNode().NodeGuid.ToString(EGuidFormats::Digits);
	}

	UNiagaraStackModuleItem* FindModuleItem(UNiagaraStackViewModel& StackViewModel, const FString& ModuleName, const FString& StackFilter)
	{
		ENiagaraScriptUsage FilterUsage = ENiagaraScriptUsage::Module;
		const bool bFilter = !IsUnset(StackFilter);
		if (bFilter && !ParseUsage(StackFilter, FilterUsage))
		{
			return nullptr;
		}

		TArray<UNiagaraStackModuleItem*> Items;
		CollectModuleItems(StackViewModel, Items);

		TArray<UNiagaraStackModuleItem*> Matches;
		TArray<FString> Available;
		TArray<FString> AvailableNames;
		for (UNiagaraStackModuleItem* Item : Items)
		{
			const ENiagaraScriptUsage Usage = GetModuleUsage(*Item);
			if (bFilter && Usage != FilterUsage)
			{
				continue;
			}
			const FString DisplayName = Item->GetDisplayName().ToString();
			const FString FunctionName = Item->GetModuleNode().GetFunctionName();
			Available.Add(FString::Printf(TEXT("'%s' [%s]"), *DisplayName, *UsageToString(Usage)));
			AvailableNames.AddUnique(DisplayName);
			if (GetModuleId(*Item).Equals(ModuleName, ESearchCase::IgnoreCase)
				|| LooseNameEquals(DisplayName, ModuleName)
				|| LooseNameEquals(FunctionName, ModuleName)
				|| Item->GetModuleNode().GetName().Equals(ModuleName, ESearchCase::IgnoreCase))
			{
				Matches.Add(Item);
			}
		}

		if (Matches.Num() == 1)
		{
			return Matches[0];
		}
		if (Matches.Num() == 0)
		{
			ReportError(FString::Printf(TEXT("[MODULE_NOT_FOUND] Module '%s' not found%s.%s Modules: %s"), *ModuleName,
				bFilter ? *FString::Printf(TEXT(" in %s"), *UsageToString(FilterUsage)) : TEXT(""), *DidYouMean(ModuleName, AvailableNames),
				Available.Num() ? *FString::Join(Available, TEXT(", ")) : TEXT("(none)")));
			return nullptr;
		}
		TArray<FString> Candidates;
		for (UNiagaraStackModuleItem* Match : Matches)
		{
			Candidates.Add(FString::Printf(TEXT("%s [%s] id=%s"), *Match->GetDisplayName().ToString(), *UsageToString(GetModuleUsage(*Match)), *GetModuleId(*Match)));
		}
		ReportError(FString::Printf(TEXT("[MODULE_AMBIGUOUS] Module '%s' matches %d modules; pass StackName or the module id. Candidates: %s"), *ModuleName, Matches.Num(), *FString::Join(Candidates, TEXT("; "))));
		return nullptr;
	}

	/**
	 * Inputs can be nested: an input whose edit condition depends on another (e.g. Lifetime under Lifetime Mode)
	 * is a child of that input in the stack. Walk the module's entries and collect every function input, without
	 * descending into dynamic inputs (their inputs belong to the dynamic input script, not to this module).
	 */
	static void CollectInputsRecursive(UNiagaraStackEntry* Entry, TArray<UNiagaraStackFunctionInput*>& OutInputs, int32 Depth)
	{
		if (Entry == nullptr || Depth > 8)
		{
			return;
		}
		TArray<UNiagaraStackEntry*> Children;
		Entry->GetUnfilteredChildren(Children);
		for (UNiagaraStackEntry* Child : Children)
		{
			if (UNiagaraStackFunctionInput* Input = Cast<UNiagaraStackFunctionInput>(Child))
			{
				OutInputs.AddUnique(Input);
				if (Input->GetValueMode() == UNiagaraStackFunctionInput::EValueMode::Dynamic)
				{
					continue;
				}
			}
			else if (Child && Child->IsA<UNiagaraStackModuleItem>())
			{
				continue;
			}
			CollectInputsRecursive(Child, OutInputs, Depth + 1);
		}
	}

	void GetModuleInputs(UNiagaraStackModuleItem& Item, TArray<UNiagaraStackFunctionInput*>& OutInputs)
	{
		OutInputs.Reset();
		CollectInputsRecursive(&Item, OutInputs, 0);
		if (OutInputs.Num() == 0)
		{
			// Fall back to the module's own list (also builds the children when they were never built).
			Item.GetParameterInputs(OutInputs);
		}
	}

	static bool InputMatches(UNiagaraStackFunctionInput& Input, const FString& WantedName)
	{
		return LooseNameEquals(Input.GetInputParameterHandle().GetName().ToString(), WantedName) || LooseNameEquals(Input.GetDisplayName().ToString(), WantedName);
	}

	/** Every function input below Entry, including the inputs of dynamic inputs. */
	static void CollectAllInputs(UNiagaraStackEntry* Entry, TArray<UNiagaraStackFunctionInput*>& OutInputs, int32 Depth)
	{
		if (Entry == nullptr || Depth > 10)
		{
			return;
		}
		TArray<UNiagaraStackEntry*> Children;
		Entry->GetUnfilteredChildren(Children);
		for (UNiagaraStackEntry* Child : Children)
		{
			if (UNiagaraStackFunctionInput* Input = Cast<UNiagaraStackFunctionInput>(Child))
			{
				OutInputs.AddUnique(Input);
			}
			CollectAllInputs(Child, OutInputs, Depth + 1);
		}
	}

	UNiagaraStackFunctionInput* FindInput(UNiagaraStackModuleItem& Item, const FString& InputName)
	{
		// "Parent/Child" reaches an input of a dynamic input, e.g. "Color/Curve" for Color set to "Color From Curve".
		TArray<FString> Path;
		InputName.ParseIntoArray(Path, TEXT("/"));
		if (Path.Num() > 1)
		{
			UNiagaraStackFunctionInput* Current = FindInput(Item, Path[0]);
			for (int32 Segment = 1; Current && Segment < Path.Num(); ++Segment)
			{
				TArray<UNiagaraStackFunctionInput*> Below;
				CollectAllInputs(Current, Below, 0);
				UNiagaraStackFunctionInput* const* Found = Below.FindByPredicate([&](UNiagaraStackFunctionInput* Input) { return InputMatches(*Input, Path[Segment].TrimStartAndEnd()); });
				if (Found == nullptr)
				{
					TArray<FString> Names;
					for (UNiagaraStackFunctionInput* Input : Below)
					{
						Names.Add(Input->GetInputParameterHandle().GetName().ToString());
					}
					ReportError(FString::Printf(TEXT("[INPUT_NOT_FOUND] '%s' has no input '%s'.%s Inputs below it: %s"), *Path[Segment - 1], *Path[Segment], *DidYouMean(Path[Segment], Names),
						Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("(none; set it to a dynamic input first)")));
					return nullptr;
				}
				Current = *Found;
			}
			return Current;
		}

		FString WantedName = InputName.TrimStartAndEnd();
		WantedName.RemoveFromStart(TEXT("Module."));

		TArray<UNiagaraStackFunctionInput*> Inputs;
		GetModuleInputs(Item, Inputs);

		TArray<FString> Names;
		for (UNiagaraStackFunctionInput* Input : Inputs)
		{
			const FString HandleName = Input->GetInputParameterHandle().GetName().ToString();
			const FString DisplayName = Input->GetDisplayName().ToString();
			if (LooseNameEquals(HandleName, WantedName) || LooseNameEquals(DisplayName, WantedName))
			{
				return Input;
			}
			Names.Add(HandleName);
		}
		ReportError(FString::Printf(TEXT("[INPUT_NOT_FOUND] Input '%s' not found on module '%s'.%s Inputs: %s"), *InputName, *Item.GetDisplayName().ToString(), *DidYouMean(WantedName, Names),
			Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("(none)")));
		return nullptr;
	}

	FString ValueModeToString(UNiagaraStackFunctionInput& Input)
	{
		switch (Input.GetValueMode())
		{
		case UNiagaraStackFunctionInput::EValueMode::Local: return TEXT("Local");
		case UNiagaraStackFunctionInput::EValueMode::Linked: return TEXT("Linked");
		case UNiagaraStackFunctionInput::EValueMode::Dynamic: return TEXT("Dynamic");
		case UNiagaraStackFunctionInput::EValueMode::Data: return TEXT("Data");
		case UNiagaraStackFunctionInput::EValueMode::ObjectAsset: return TEXT("ObjectAsset");
		case UNiagaraStackFunctionInput::EValueMode::Expression: return TEXT("Expression");
		case UNiagaraStackFunctionInput::EValueMode::DefaultFunction: return TEXT("DefaultFunction");
		case UNiagaraStackFunctionInput::EValueMode::InvalidOverride: return TEXT("InvalidOverride");
		case UNiagaraStackFunctionInput::EValueMode::UnsupportedDefault: return TEXT("UnsupportedDefault");
		default: return TEXT("None");
		}
	}

	// Emits exactly the syntax SetModuleInput accepts, so a value read with GetStack can be written back as is.
	FString InputValueToString(UNiagaraStackFunctionInput& Input)
	{
		const FNiagaraTypeDefinition& Type = Input.GetInputType();
		switch (Input.GetValueMode())
		{
		case UNiagaraStackFunctionInput::EValueMode::Local:
		{
			TSharedPtr<const FStructOnScope> Local = Input.GetLocalValueStruct();
			if (Local.IsValid() && Local->IsValid())
			{
				return ExportStructValue(Type.GetScriptStruct(), Local->GetStructMemory(), Type.GetEnum());
			}
			return FString();
		}
		case UNiagaraStackFunctionInput::EValueMode::Linked:
			return TEXT("@") + Input.GetLinkedParameterValue().GetName().ToString();
		case UNiagaraStackFunctionInput::EValueMode::Dynamic:
		{
			const UNiagaraNodeFunctionCall* DynamicNode = Input.GetDynamicInputNode();
			const UNiagaraScript* DynamicScript = DynamicNode ? DynamicNode->FunctionScript.Get() : nullptr;
			if (DynamicScript)
			{
				return TEXT("Dynamic:") + DynamicScript->GetPathName();
			}
			return DynamicNode ? FString::Printf(TEXT("Dynamic:%s"), *DynamicNode->GetFunctionName()) : FString(TEXT("Dynamic:"));
		}
		case UNiagaraStackFunctionInput::EValueMode::Expression:
			return TEXT("=") + Input.GetCustomExpressionText().ToString();
		case UNiagaraStackFunctionInput::EValueMode::Data:
		{
			const UNiagaraDataInterface* DataInterface = Input.GetDataValueObject();
			return DataInterface ? TEXT("Data:") + DataInterface->GetClass()->GetName() : FString();
		}
		case UNiagaraStackFunctionInput::EValueMode::ObjectAsset:
		{
			const UObject* Object = Input.GetObjectAssetValue();
			return Object ? TEXT("Asset:") + Object->GetPathName() : FString(TEXT("Asset:None"));
		}
		default:
			return FString();
		}
	}

	void SyncCompiledDataInterface(UNiagaraSystem& System, UNiagaraDataInterface& DataInterface)
	{
		// Same as FNiagaraSystemViewModel::UpdateCompiledDataInterfaces + OnCompiledDataInterfaceChanged.
		if (UNiagaraDataInterface* Runtime = FNiagaraEditorUtilities::GetResolvedRuntimeInstanceForEditorDataInterfaceInstance(System, DataInterface))
		{
			Runtime->Modify(false);
			DataInterface.CopyTo(Runtime);
		}
		else if (System.GetExposedParameters().FindVariableFromDataInterface(&DataInterface) != nullptr)
		{
			System.GetExposedParameters().OnInterfaceChange();
		}
		System.OnCompiledDataInterfaceChanged();

		// The copy above only reaches runtime data that already exists. A compile duplicates the graph (and its data
		// interfaces) when it is REQUESTED, so a compile already in flight - e.g. started by the SetModuleInput that
		// created this curve - finishes with the old values, and RequestCompile(false) sees nothing to do because a
		// DI property edit doesn't change the graph. Force a recompile so the scripts copy the current values.
		if (DataInterface.GetTypedOuter<UNiagaraNodeInput>() != nullptr)
		{
			System.RequestCompile(true);
		}
	}

	void FinishSystemEdit(UNiagaraSystem& System, bool bSave)
	{
		// An open Niagara editor rebuilds its view models on this notification.
		FPropertyChangedEvent ChangedEvent(nullptr, EPropertyChangeType::ValueSet);
		System.PostEditChangeProperty(ChangedEvent);
		System.RequestCompile(false);

		TSharedPtr<FNiagaraSystemViewModel> EditorViewModel;
		if (FindEditorViewModel(System, EditorViewModel) == EEditorLookup::Found)
		{
			for (const TSharedRef<FNiagaraEmitterHandleViewModel>& HandleViewModel : EditorViewModel->GetEmitterHandleViewModels())
			{
				if (UNiagaraStackViewModel* StackViewModel = HandleViewModel->GetEmitterStackViewModel())
				{
					StackViewModel->RequestRefreshDeferred();
				}
			}
			if (UNiagaraStackViewModel* SystemStack = EditorViewModel->GetSystemStackViewModel())
			{
				SystemStack->RequestRefreshDeferred();
			}
		}

		if (bSave)
		{
			SaveAssetSilently(&System);
		}
	}
}
