#pragma once

#include "CoreMinimal.h"
#include "NiagaraMcpEditorTools.h"
#include "NiagaraTypes.h"

struct FNiagaraParameterStore;
class UNiagaraRendererProperties;
class UNiagaraSystem;
struct FNiagaraEmitterHandle;

/**
 * Helpers for the v0.4 extension tools (NiagaraMcpEditorTools_Ext*.cpp). Kept separate from the shared helpers so the
 * extension files merge without touching existing sources; candidates for deduplication with NiagaraMcpCommon.
 */
namespace NiagaraMcpExt
{
	/**
	 * Optional string arguments need a non-empty default to be optional in the MCP schema; these values mean "unset":
	 * "", "*", "Any", "None", "Auto", "Default" (case-insensitive). Same behavior as NiagaraMcp::IsUnset in the main tree.
	 */
	bool IsUnsetArg(const FString& S);

	/** Empty string when S is unset (see IsUnsetArg), otherwise S trimmed. */
	FString ArgOrEmpty(const FString& S);

	/** Closest names to Query (Levenshtein, case-insensitive), up to 3, as "'A', 'B'". Empty when Candidates is empty. */
	FString SuggestNames(const FString& Query, const TArray<FString>& Candidates);

	/** "Unknown <What> '<Query>'. Did you mean 'A', 'B'? Valid: ..." */
	FString UnknownNameMessage(const TCHAR* What, const FString& Query, const TArray<FString>& Candidates);

	/** true / false / 1 / 0 / yes / no / on / off. */
	bool ParseBoolText(const FString& Text, bool& bOut);

	/** Lower-cased words of Filter; every word must occur in Haystack (case-insensitive). */
	bool MatchesWords(const FString& Haystack, const TArray<FString>& Words);
	TArray<FString> SplitFilter(const FString& Filter);

	/** Same loose lookup as SetPropertyFromText: exact name, loose name, display name, or "b"+Name for bools. */
	FProperty* FindPropertyLoose(const UStruct* Struct, const FString& Name);

	/** Names of the editable (CPF_Edit, not deprecated) properties of Struct. */
	TArray<FString> EditablePropertyNames(const UStruct* Struct);

	/** Entries of an enum (without _MAX and hidden entries). */
	TArray<FString> EnumOptions(const UEnum* Enum);

	/** Value of a property as text: enum entry names, object paths, UE struct text; truncated to MaxLen characters. */
	FString PropertyValueText(const FProperty* Property, const void* Container, int32 MaxLen = 300);

	/**
	 * Lists the editable properties of Struct (instance at Container). Skip (optional) excludes properties,
	 * Filter keeps those whose name, category or type contain every filter word.
	 */
	void DescribeEditableProperties(const UStruct* Struct, const void* Container, const FString& Filter, TArray<FNiagaraMcpEditableProperty>& OutProperties,
		TFunctionRef<bool(const FProperty*)> Skip);

	/**
	 * Validates a SetPropertyFromText call without touching the target: the top-level property is copied into a scratch
	 * value and the edit is applied to the copy. Reports unknown names with suggestions. Returns false with OutError.
	 */
	bool DryRunSetProperty(UStruct* Struct, const void* Container, UObject* OwnerObject, const FString& PropertyPath, const FString& Value, FString& OutError);

	/** FNiagaraParameterBinding(WithValue) members, e.g. a simulation stage's NumIterations or EnabledBinding. */
	bool IsParameterBindingProperty(const FProperty* Property);

	/** Constant value ("4") or bound parameter ("@User.Iterations") of a parameter binding. */
	FString ParameterBindingText(const FStructProperty& Property, const void* Container);

	/**
	 * Sets a parameter binding from text: "@User.X" / "@System.X" / "@Emitter.X" binds a parameter (Emitter. is resolved
	 * with EmitterUniqueName); any other text is parsed as a constant of the binding's type. Applies to the binding at
	 * Container (pass a scratch copy to validate). Returns false with OutError.
	 */
	bool SetParameterBindingFromText(const FStructProperty& Property, void* Container, const FString& Value, const FString& EmitterUniqueName, FString& OutError);

	/** True when the property value equals the one in DefaultContainer. */
	bool IsPropertyDefault(const FProperty* Property, const void* Container, const void* DefaultContainer);

	FNiagaraEmitterHandle* FindHandleById(UNiagaraSystem& System, const FGuid& Id);
	FNiagaraMcpEmitterInfo DescribeEmitterInfo(const FNiagaraEmitterHandle& Handle);
	FNiagaraMcpEmitterStages DescribeEmitterStages(UNiagaraSystem& System, FNiagaraEmitterHandle& Handle);

	/** Renderer by index; raises an error listing the renderer count. */
	UNiagaraRendererProperties* FindRendererByIndex(FNiagaraEmitterHandle& Handle, int32 RendererIndex);

	/** Value of a parameter in a store as text (data interface class, object path or struct value). */
	FString ParameterValueText(const FNiagaraParameterStore& Store, const FNiagaraVariable& Variable);

	/** "User." + Name unless already prefixed. */
	FName UserParameterName(FName ParameterName);

	/** Finds a user parameter by name (with or without User.). Raises an error with suggestions when bReport is true. */
	bool FindUserParameter(UNiagaraSystem& System, FName ParameterName, FNiagaraVariable& OutVariable, bool bReport = true);

	/** Every user parameter of the system as tool info. */
	TArray<FNiagaraMcpParameterInfo> DescribeUserParameters(UNiagaraSystem& System);
}
