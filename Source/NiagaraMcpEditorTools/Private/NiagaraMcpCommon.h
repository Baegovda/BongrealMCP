#pragma once

#include "CoreMinimal.h"
#include "NiagaraCommon.h"
#include "NiagaraTypes.h"
#include "UObject/Package.h"

class UNiagaraSystem;

DECLARE_LOG_CATEGORY_EXTERN(LogNiagaraMcp, Log, All);

/**
 * Helpers shared by every toolset in this plugin. Nothing here pops a modal dialog: a modal on the game
 * thread stalls every MCP request queued behind it, so saves, creates and errors all take silent paths.
 */
namespace NiagaraMcp
{
	/** Reports a tool error. The Toolset Registry turns script errors into MCP tool errors. */
	void ReportError(const FString& Message);

	/** Saves the asset's package without checkout / overwrite prompts. */
	bool SaveAssetSilently(UObject* Asset);

	/**
	 * Validates a path for a NEW asset ("/Game/Folder/Name" or "/Game/Folder/Name.Name") and refuses
	 * existing packages, so creation never hits an overwrite prompt.
	 */
	bool SplitNewAssetPath(const FString& AssetPath, FString& OutPackageName, FString& OutAssetName);

	/** Creates an empty package for a new asset. Null (and an error) when the path is invalid or taken. */
	UPackage* CreateNewAssetPackage(const FString& AssetPath, FString& OutAssetName);

	/** Loads an object by path ("/Game/X/Y", "/Game/X/Y.Y" or "/Game/X/Y.Y:Sub"). Raises an error on failure. */
	UObject* LoadObjectByPath(const FString& Path, UClass* ExpectedClass, const TCHAR* What);

	template<typename T>
	T* LoadAssetByPath(const FString& Path, const TCHAR* What)
	{
		return Cast<T>(LoadObjectByPath(Path, T::StaticClass(), What));
	}

	/** True when an asset editor is open for Asset. */
	bool IsAssetEditorOpen(UObject* Asset);

	/** Raises an error and returns false when an asset editor is open for Asset. */
	bool EnsureNoOpenEditor(UObject* Asset);

	/**
	 * Parses a stack / script usage. Accepts the enum names (ParticleSpawnScript) and short forms
	 * (ParticleSpawn, particle_spawn, "Particle Spawn"). An empty string is rejected.
	 */
	bool ParseUsage(const FString& Text, ENiagaraScriptUsage& OutUsage);
	FString UsageToString(ENiagaraScriptUsage Usage);

	/** True for the six stacks that a system / emitter always has. */
	bool IsStandardStack(ENiagaraScriptUsage Usage);
	bool IsSystemStack(ENiagaraScriptUsage Usage);

	/**
	 * Resolves a Niagara type: float, int, bool, vec2, vec3, vec4, color, position, quat, matrix, map,
	 * a Niagara type name, an enum path or a data interface class (path or short name).
	 */
	bool ResolveNiagaraType(const FString& TypeName, FNiagaraTypeDefinition& OutType);

	/**
	 * Writes a value given as text into struct memory. Accepted forms:
	 *   - enum entry names for enum types (e.g. "Self", "ENiagaraExecutionStateManagement::Self") or numbers
	 *   - true / false for bools
	 *   - comma or space separated numbers filled into the numeric fields in order ("1, 2, 3"; "0.5")
	 *     A single number fills every numeric field.
	 *   - UE text format "(X=1,Y=2,Z=3)"
	 */
	bool ImportStructValue(const UScriptStruct* Struct, void* Memory, const FString& Text, const UEnum* Enum, FString& OutError);

	/** Enum entry as text: display name for Blueprint enums ("Direct Set"), entry name for native enums. */
	FString EnumValueToString(const UEnum* Enum, int64 Value);

	/** Parses an enum entry by name, display name (loosely) or number. */
	bool ParseEnumText(const UEnum* Enum, const FString& Text, int64& OutValue);

	/** Inverse of ImportStructValue for read-back. */
	FString ExportStructValue(const UScriptStruct* Struct, const void* Memory, const UEnum* Enum);

	/**
	 * Sets a reflected property (dotted paths reach into nested structs) from text. Object properties take
	 * an asset path and are loaded. Returns the top-level property that changed, null on error.
	 */
	FProperty* SetPropertyFromText(UStruct* Struct, void* Container, UObject* OwnerObject, const FString& PropertyPath, const FString& Value, FString& OutError);

	/** Reads a reflected property (dotted path) as text. Empty when not found. */
	FString GetPropertyText(const UStruct* Struct, const void* Container, const FString& PropertyPath);

	/** Comma separated list of editable property names, for error messages. */
	FString ListEditableProperties(const UStruct* Struct);

	/** Case-insensitive name compare that also ignores spaces and underscores ("Spawn Burst Instantaneous" == "SpawnBurst_Instantaneous"). */
	bool LooseNameEquals(const FString& A, const FString& B);

	/**
	 * " Did you mean 'X' or 'Y'?" for the candidates closest to Query (edit distance on loosely normalized
	 * names, substring matches first), or an empty string when nothing is close. Append it to lookup errors.
	 */
	/**
	 * True for an optional text argument left at "no value": "", "*", "Any", "None", "Auto" or "Default"
	 * (case-insensitive). Optional FString tool parameters need a non-empty C++ default to be optional in the
	 * MCP schema (empty defaults are dropped by the schema generator), so they default to one of these words.
	 */
	bool IsUnset(const FString& Value);

	/** true/false, 1/0, yes/no, on/off (any case). False (and bOut untouched) for anything else. */
	bool ParseBool(const FString& Text, bool& bOut);

	FString DidYouMean(const FString& Query, const TArray<FString>& Candidates, int32 MaxSuggestions = 3);
}
