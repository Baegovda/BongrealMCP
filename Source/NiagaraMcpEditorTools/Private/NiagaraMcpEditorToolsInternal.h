#pragma once

#include "CoreMinimal.h"
#include "NiagaraMcpEditorTools.h"

class UNiagaraStackFunctionInput;
class UNiagaraStackModuleItem;
class UNiagaraStackViewModel;
class UNiagaraSystem;

/** Read-back helpers shared by the toolset's source files. */
namespace NiagaraMcpTools
{
	bool CheckSystemArg(const UNiagaraSystem* System);

	/** Compile status and messages of the system's current scripts, without requesting a compile. */
	FNiagaraMcpCompileResult ReadCompileResult(UNiagaraSystem& System);

	/** Recompiles and refreshes every Niagara system touched by the given objects (after Undo/Redo). */
	void RefreshSystemsAfterTransaction(const TArray<UObject*>& Objects);

	FNiagaraMcpInputInfo DescribeInput(UNiagaraStackFunctionInput& Input);
	FNiagaraMcpModuleInfo DescribeModule(UNiagaraStackModuleItem& Item, int32 IndexInStack, bool bIncludeInputs);
	FNiagaraMcpStackInfo DescribeStack(UNiagaraStackViewModel& StackViewModel, const FString& Owner, bool bIncludeInputs);
}
