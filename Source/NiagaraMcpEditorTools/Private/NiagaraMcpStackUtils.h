#pragma once

#include "CoreMinimal.h"
#include "NiagaraCommon.h"

class FNiagaraSystemViewModel;
class UNiagaraStackFunctionInput;
class UNiagaraStackModuleItem;
class UNiagaraStackViewModel;
class UNiagaraSystem;
struct FNiagaraEmitterHandle;

/**
 * Stack access goes through the same view models the Niagara editor uses, so every edit takes the exact
 * code path of the stack UI (rapid iteration parameters, static switches, script versions, notes, undo).
 * When the system is open in the Niagara editor its own view model is used and the editor updates live;
 * otherwise a short-lived headless view model is built for the call.
 */
namespace NiagaraMcp::Stack
{
	enum class EEditorLookup
	{
		NotOpen,
		Found,
		Unreachable,
	};

	EEditorLookup FindEditorViewModel(UNiagaraSystem& System, TSharedPtr<FNiagaraSystemViewModel>& OutViewModel);

	/** The open editor's view model, or a headless one. Null (with an error) when neither can be used. */
	TSharedPtr<FNiagaraSystemViewModel> AcquireViewModel(UNiagaraSystem& System);

	/** Finds an emitter handle by name. Raises an error listing the emitters when not found. */
	FNiagaraEmitterHandle* FindEmitter(UNiagaraSystem& System, FName EmitterName);

	/** True when EmitterName addresses the system stacks ("", "None" or "System"). */
	bool IsSystemName(FName EmitterName);

	/** Stack of an emitter, or the system stack when IsSystemName(EmitterName). */
	UNiagaraStackViewModel* GetStackViewModel(FNiagaraSystemViewModel& ViewModel, FName EmitterName);

	/** All module items of a stack in display order. */
	void CollectModuleItems(UNiagaraStackViewModel& StackViewModel, TArray<UNiagaraStackModuleItem*>& OutItems);

	/** The stack (script usage) a module item lives in. */
	ENiagaraScriptUsage GetModuleUsage(const UNiagaraStackModuleItem& Item);

	/** Stable id of a module item: the function call node guid. */
	FString GetModuleId(const UNiagaraStackModuleItem& Item);

	/**
	 * Finds a module by display name ("Spawn Burst Instantaneous"), function name ("SpawnBurst_Instantaneous"),
	 * or module id. StackFilter (optional) restricts the search to one stack, e.g. "ParticleSpawn".
	 * Raises an error listing the candidates when nothing or more than one module matches.
	 */
	UNiagaraStackModuleItem* FindModuleItem(UNiagaraStackViewModel& StackViewModel, const FString& ModuleName, const FString& StackFilter);

	/** Top-level inputs of a module (refreshes the module's children when they were not built yet). */
	void GetModuleInputs(UNiagaraStackModuleItem& Item, TArray<UNiagaraStackFunctionInput*>& OutInputs);

	/** Finds an input by name ("SpawnCount", "Module.SpawnCount" or the display name). Raises an error listing inputs. */
	UNiagaraStackFunctionInput* FindInput(UNiagaraStackModuleItem& Item, const FString& InputName);

	/** Mode, value and link of an input as text. */
	FString ValueModeToString(UNiagaraStackFunctionInput& Input);
	FString InputValueToString(UNiagaraStackFunctionInput& Input);

	/** Notifies the editor, requests a recompile and optionally saves. */
	void FinishSystemEdit(UNiagaraSystem& System, bool bSave);

	/**
	 * After editing a data interface's properties (curve keys, texture, array values...): copies the edit into the
	 * compiled runtime instance, as the Niagara editor does (FNiagaraSystemViewModel::NotifyDataObjectChanged).
	 * DI property edits don't change the graph, so no recompile happens and the simulation would keep the old values.
	 */
	void SyncCompiledDataInterface(UNiagaraSystem& System, UNiagaraDataInterface& DataInterface);
}
