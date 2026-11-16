#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "ToolsetRegistry/ToolsetImage.h"
#include "NiagaraMcpEditorTools.generated.h"

class UNiagaraScript;
class UNiagaraSystem;

/** A renderer on an emitter. */
USTRUCT()
struct FNiagaraMcpRendererInfo
{
	GENERATED_BODY()

	/** Index in the emitter's renderer list. Use as RendererIndex. */
	UPROPERTY()
	int32 Index = INDEX_NONE;

	/** Renderer class, e.g. NiagaraRibbonRendererProperties. */
	UPROPERTY()
	FString Class;

	UPROPERTY()
	bool bEnabled = true;

	/** Material path when the renderer has a Material property. */
	UPROPERTY()
	FString Material;
};

/** One emitter of a system. */
USTRUCT()
struct FNiagaraMcpEmitterInfo
{
	GENERATED_BODY()

	/** Emitter name inside the system. Use as EmitterName. */
	UPROPERTY()
	FName Name;

	UPROPERTY()
	bool bEnabled = true;

	/** CPUSim or GPUComputeSim. */
	UPROPERTY()
	FString SimTarget;

	UPROPERTY()
	bool bLocalSpace = false;

	UPROPERTY()
	TArray<FNiagaraMcpRendererInfo> Renderers;

	/** Simulation stage names in execution order. */
	UPROPERTY()
	TArray<FString> SimulationStages;

	UPROPERTY()
	int32 EventHandlerCount = 0;
};

/** A user parameter or other named value. */
USTRUCT()
struct FNiagaraMcpParameterInfo
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Type;

	UPROPERTY()
	FString Value;
};

/** Overview of a system. */
USTRUCT()
struct FNiagaraMcpSystemSummary
{
	GENERATED_BODY()

	UPROPERTY()
	FString SystemPath;

	UPROPERTY()
	TArray<FNiagaraMcpEmitterInfo> Emitters;

	UPROPERTY()
	TArray<FNiagaraMcpParameterInfo> UserParameters;

	/** True while the system or its scripts still have a compile pending. */
	UPROPERTY()
	bool bHasOutstandingCompile = false;

	/** True when the Niagara editor is open on the system (edits then show up there live). */
	UPROPERTY()
	bool bOpenInEditor = false;
};

/** One input of a module in a stack. */
USTRUCT()
struct FNiagaraMcpInputInfo
{
	GENERATED_BODY()

	/** Input name, e.g. SpawnCount. Use as Input. */
	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString DisplayName;

	/** Niagara type name, e.g. NiagaraFloat, Vector3f, ENiagaraExecutionStateManagement. */
	UPROPERTY()
	FString Type;

	/** Local, Linked, Dynamic, Data, ObjectAsset, Expression, DefaultFunction, ... */
	UPROPERTY()
	FString Mode;

	/**
	 * Current value in SetModuleInput syntax, so it can be passed back unchanged: "1.5" / "0, 0, 1" (local),
	 * "@Particles.Age" (linked), "=Particles.Age * 2" (expression), "Dynamic:/Niagara/.../Script.Script",
	 * "Data:NiagaraDataInterfaceCurve", "Asset:/Game/.../Asset.Asset".
	 */
	UPROPERTY()
	FString Value;

	/** Static switch input (changes the compiled graph). */
	UPROPERTY()
	bool bStatic = false;

	/** False when the input already has its default value. */
	UPROPERTY()
	bool bCanReset = false;
};

/** A stack note attached to a module. */
USTRUCT()
struct FNiagaraMcpNoteInfo
{
	GENERATED_BODY()

	UPROPERTY()
	FString Header;

	UPROPERTY()
	FString Message;

	/** Shown inline in the module row instead of as an extra row. */
	UPROPERTY()
	bool bInline = false;
};

/** A module placed in a stack. */
USTRUCT()
struct FNiagaraMcpModuleInfo
{
	GENERATED_BODY()

	/** Stable id (function call node guid). Can be used as Module in every stack tool. */
	UPROPERTY()
	FString ModuleId;

	/** Title shown in the stack, e.g. "Spawn Burst Instantaneous". */
	UPROPERTY()
	FString DisplayName;

	/** Function name of the node, unique inside the stack graph. */
	UPROPERTY()
	FString FunctionName;

	/** Stack the module lives in, e.g. ParticleSpawnScript. */
	UPROPERTY()
	FString Stack;

	/** Position inside its stack. */
	UPROPERTY()
	int32 Index = INDEX_NONE;

	/** Module script asset (empty for Set Parameters / assignment modules). */
	UPROPERTY()
	FString ScriptPath;

	/** Script version in use and the latest exposed version of the script. */
	UPROPERTY()
	FString ScriptVersion;

	UPROPERTY()
	FString LatestScriptVersion;

	UPROPERTY()
	bool bEnabled = true;

	UPROPERTY()
	bool bHasNote = false;

	UPROPERTY()
	FNiagaraMcpNoteInfo Note;

	UPROPERTY()
	TArray<FNiagaraMcpInputInfo> Inputs;
};

/** All modules of an emitter (or of the system stacks). */
USTRUCT()
struct FNiagaraMcpStackInfo
{
	GENERATED_BODY()

	/** Emitter name, or System. */
	UPROPERTY()
	FString Owner;

	UPROPERTY()
	TArray<FNiagaraMcpModuleInfo> Modules;
};

/** A module script asset found in the project or engine content. */
USTRUCT()
struct FNiagaraMcpModuleScriptInfo
{
	GENERATED_BODY()

	/** Object path; pass it as ModuleScript to AddModule. */
	UPROPERTY()
	FString Path;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Description;

	UPROPERTY()
	FString Category;

	/** Stacks the module may be placed in. */
	UPROPERTY()
	TArray<FString> AllowedStacks;

	/** Exposed in the stack's add menu. */
	UPROPERTY()
	bool bLibraryExposed = false;
};

/** A compile message. */
USTRUCT()
struct FNiagaraMcpCompileMessage
{
	GENERATED_BODY()

	/** Emitter/stack the message belongs to. */
	UPROPERTY()
	FString Script;

	/** Error, Warning, Log or Display. */
	UPROPERTY()
	FString Severity;

	UPROPERTY()
	FString Message;
};

/** Result of compiling a system. */
USTRUCT()
struct FNiagaraMcpCompileResult
{
	GENERATED_BODY()

	UPROPERTY()
	FString SystemPath;

	/** No script reported an error. */
	UPROPERTY()
	bool bSucceeded = false;

	/** Compilation was not waited for, or is still running. */
	UPROPERTY()
	bool bStillCompiling = false;

	/** "<Owner>/<Stack>: <Status>" per compiled script. */
	UPROPERTY()
	TArray<FString> ScriptStatus;

	UPROPERTY()
	TArray<FNiagaraMcpCompileMessage> Messages;

	UPROPERTY()
	bool bSaved = false;
};

/** Result of creating an asset. */
USTRUCT()
struct FNiagaraMcpAssetResult
{
	GENERATED_BODY()

	/** Object path of the new asset. */
	UPROPERTY()
	FString Path;

	UPROPERTY()
	FString Class;

	UPROPERTY()
	bool bSaved = false;
};

/** Read-back of a property after a change. */
USTRUCT()
struct FNiagaraMcpPropertyResult
{
	GENERATED_BODY()

	UPROPERTY()
	FString Property;

	UPROPERTY()
	FString Value;

	UPROPERTY()
	bool bSaved = false;
};

/** A pin of a script graph node. */
USTRUCT()
struct FNiagaraMcpGraphPin
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	/** Input or Output. */
	UPROPERTY()
	FString Direction;

	/** Niagara type name; "Add" pins are listed with type Add. */
	UPROPERTY()
	FString Type;

	UPROPERTY()
	FString DefaultValue;

	/** "<NodeId>:<PinName>" of every connected pin. */
	UPROPERTY()
	TArray<FString> LinkedTo;
};

/** A node of a script graph. */
USTRUCT()
struct FNiagaraMcpGraphNode
{
	GENERATED_BODY()

	/** Node guid; use as NodeId. */
	UPROPERTY()
	FString NodeId;

	/** Node class, e.g. NiagaraNodeParameterMapGet. */
	UPROPERTY()
	FString Class;

	UPROPERTY()
	FString Title;

	UPROPERTY()
	int32 PosX = 0;

	UPROPERTY()
	int32 PosY = 0;

	UPROPERTY()
	TArray<FNiagaraMcpGraphPin> Pins;

	/** HLSL body of Custom Hlsl nodes. */
	UPROPERTY()
	FString CustomHlsl;
};

/** A script graph. */
USTRUCT()
struct FNiagaraMcpGraphInfo
{
	GENERATED_BODY()

	UPROPERTY()
	FString ScriptPath;

	UPROPERTY()
	FString Usage;

	UPROPERTY()
	FString Description;

	/** Stacks a module script may be placed in. */
	UPROPERTY()
	TArray<FString> AllowedStacks;

	UPROPERTY()
	TArray<FNiagaraMcpGraphNode> Nodes;

	/** Nodes matching the filter (all pages). */
	UPROPERTY()
	int32 TotalNodes = 0;

	/** Offset of the next page, -1 when this is the last. */
	UPROPERTY()
	int32 NextOffset = -1;
};

/** Input or output declaration of a Custom Hlsl node. */
USTRUCT()
struct FNiagaraMcpHlslPin
{
	GENERATED_BODY()

	/** Name used in the HLSL code. */
	UPROPERTY()
	FName Name;

	/** float, int, bool, vec2, vec3, vec4, color, position, quat, matrix, map, a Niagara type name or a data interface class. */
	UPROPERTY()
	FString Type;
};

/** Result of pushing script graph edits to everything that uses the script. */
USTRUCT()
struct FNiagaraMcpApplyInfo
{
	GENERATED_BODY()

	UPROPERTY()
	FString ScriptPath;

	/** Module nodes (in system stacks) refreshed from the edited graph. */
	UPROPERTY()
	int32 RefreshedModuleNodes = 0;

	/** Systems asked to recompile, plus emitter assets and scripts that call this script (refreshed and saved). Use CompileSystem to read the systems' errors. */
	UPROPERTY()
	TArray<FString> RecompiledSystems;

	UPROPERTY()
	bool bSaved = false;
};

/** One problem found by ValidateSystem. */
USTRUCT()
struct FNiagaraMcpIssue
{
	GENERATED_BODY()

	/** Error, Warning or Info. */
	UPROPERTY()
	FString Severity;

	/** Where it was found: "Compile", "Stack" or "Check", plus the emitter/stack entry. */
	UPROPERTY()
	FString Source;

	/** Emitter the issue belongs to (None for the system); pass it to ApplyStackIssueFix. */
	UPROPERTY()
	FName EmitterName;

	UPROPERTY()
	FString Message;

	/** Stack issue id; pass it to ApplyStackIssueFix. Empty for compile messages and plugin checks. */
	UPROPERTY()
	FString IssueId;

	/** Automatic fixes the stack offers, in order (FixIndex 0, 1, ...). */
	UPROPERTY()
	TArray<FString> Fixes;
};

/** Result of ValidateSystem. */
USTRUCT()
struct FNiagaraMcpValidationResult
{
	GENERATED_BODY()

	UPROPERTY()
	FString SystemPath;

	/** No issue with severity Error. */
	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	int32 ErrorCount = 0;

	UPROPERTY()
	int32 WarningCount = 0;

	UPROPERTY()
	TArray<FNiagaraMcpIssue> Issues;
};

/** Particle count of one emitter. */
USTRUCT()
struct FNiagaraMcpEmitterCount
{
	GENERATED_BODY()

	UPROPERTY()
	FName Emitter;

	UPROPERTY()
	int32 Particles = 0;

	/** Active, Inactive, InactiveClear, Complete, ... */
	UPROPERTY()
	FString State;
};

/** Particle counts at one simulation time. */
USTRUCT()
struct FNiagaraMcpParticleSample
{
	GENERATED_BODY()

	UPROPERTY()
	float Time = 0.0f;

	UPROPERTY()
	int32 TotalParticles = 0;

	UPROPERTY()
	TArray<FNiagaraMcpEmitterCount> Emitters;
};

/** A curve key; Value holds one number per channel ("0.5" or "1, 0.2, 0.1, 1"). */
USTRUCT()
struct FNiagaraMcpCurveKey
{
	GENERATED_BODY()

	UPROPERTY()
	float Time = 0.0f;

	UPROPERTY()
	FString Value;
};

/** Curve data of a curve data interface (float, vector2/3/4 or color curve). */
USTRUCT()
struct FNiagaraMcpCurveInfo
{
	GENERATED_BODY()

	/** Data interface class, e.g. NiagaraDataInterfaceColorCurve. */
	UPROPERTY()
	FString DataInterfaceClass;

	/** Channel names in Value order, e.g. [RedCurve, GreenCurve, BlueCurve, AlphaCurve]. */
	UPROPERTY()
	TArray<FString> Channels;

	/** Keys at the union of all channel key times. */
	UPROPERTY()
	TArray<FNiagaraMcpCurveKey> Keys;

	/** Key interpolation: Cubic, Linear, Constant or Mixed. Pass it to SetCurveKeys to keep the curve shape. */
	UPROPERTY()
	FString Interpolation;

	UPROPERTY()
	bool bSaved = false;
};

/** An asset found by FindAssets. */
USTRUCT()
struct FNiagaraMcpAssetInfo
{
	GENERATED_BODY()

	/** Object path, usable as an asset parameter. */
	UPROPERTY()
	FString Path;

	UPROPERTY()
	FString Class;
};

/** One page of FindAssets results. */
USTRUCT()
struct FNiagaraMcpAssetPage
{
	GENERATED_BODY()

	UPROPERTY()
	int32 TotalMatched = 0;

	/** Offset for the next page, or -1 when this was the last page. */
	UPROPERTY()
	int32 NextOffset = -1;

	UPROPERTY()
	TArray<FNiagaraMcpAssetInfo> Assets;
};

/** Result of Undo / Redo. */
USTRUCT()
struct FNiagaraMcpUndoResult
{
	GENERATED_BODY()

	/** Titles of the transactions that were undone or redone, most recent first. */
	UPROPERTY()
	TArray<FString> Transactions;
};

/** One operation of ExecuteBatch. */
USTRUCT(BlueprintType)
struct FNiagaraMcpBatchOp
{
	GENERATED_BODY()

	/** Optional name for referencing this step's result later as "$<Id>.<field>", e.g. "node". Letters, digits, _. */
	UPROPERTY()
	FString Id;

	/**
	 * Tool to run: a tool of this plugin by short name ("SetModuleInput", "AddExpression") or any registered tool by
	 * full name ("NiagaraMcpEditorTools.MaterialMcpEditorTools.ConnectExpressions").
	 */
	UPROPERTY()
	FString Tool;

	/**
	 * The tool's arguments as a JSON object string, same names as when calling the tool directly, e.g.
	 * {"System": "/Game/FX/NS_A", "EmitterName": "Sparks", "Module": "Initialize Particle", "Input": "Lifetime", "Value": "1.5"}.
	 * A string value "$<Id or index>.<path>" is replaced by that part of an earlier step's result (any JSON type),
	 * e.g. "$expr.name" or "$0.nodeId"; "${expr.name}" inside a longer string is replaced by its text.
	 */
	UPROPERTY()
	FString Arguments;
};

/** Outcome of one ExecuteBatch step. */
USTRUCT()
struct FNiagaraMcpBatchStep
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Index = 0;

	UPROPERTY()
	FString Id;

	/** Full tool name that ran. */
	UPROPERTY()
	FString Tool;

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Error;

	/** Stable code from the error, e.g. EMITTER_NOT_FOUND, INPUT_NOT_FOUND, ASSET_EXISTS (see the authoring guide). */
	UPROPERTY()
	FString ErrorCode;

	/** The tool's result as JSON (long results are shortened; reference fields with "$<Id>.<path>" instead). */
	UPROPERTY()
	FString Result;
};

/** One ExecuteBatch post-condition (Expect). */
USTRUCT()
struct FNiagaraMcpBatchExpectation
{
	GENERATED_BODY()

	UPROPERTY()
	FString Expectation;

	UPROPERTY()
	bool bMet = false;

	/** What was measured, e.g. "Sparks: 0 particles at 0.5 s" or the first compile error. */
	UPROPERTY()
	FString Detail;
};

/** Result of ExecuteBatch. */
USTRUCT()
struct FNiagaraMcpBatchResult
{
	GENERATED_BODY()

	/** True when every step succeeded (or, for a dry run, every step is valid). */
	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	int32 Succeeded = 0;

	UPROPERTY()
	int32 Failed = 0;

	/** True when a failure undid the steps that had already run. */
	UPROPERTY()
	bool bRolledBack = false;

	/** New, never-saved assets that a rolled-back batch had created and that were deleted again (paths are free). */
	UPROPERTY()
	TArray<FString> DiscardedAssets;

	/** Undo transactions the batch added (pass to Undo to revert the whole batch). */
	UPROPERTY()
	int32 UndoCount = 0;

	/** Packages saved at the end of the batch. */
	UPROPERTY()
	TArray<FString> SavedPackages;

	/** Packages changed but not saved because Play In Editor was running (save them after stopping PIE). */
	UPROPERTY()
	TArray<FString> UnsavedPackages;

	UPROPERTY()
	TArray<FNiagaraMcpBatchStep> Steps;

	/** Post-conditions from Expect, checked after the steps (an unmet one fails the batch and triggers the rollback). */
	UPROPERTY()
	TArray<FNiagaraMcpBatchExpectation> Expectations;
};

/** A placed level actor. */
USTRUCT()
struct FNiagaraMcpActorInfo
{
	GENERATED_BODY()

	UPROPERTY()
	FString ActorName;

	UPROPERTY()
	FString ActorLabel;

	UPROPERTY()
	FString Location;
};

/** A simulation stage or event handler of an emitter. */
USTRUCT()
struct FNiagaraMcpStageInfo
{
	GENERATED_BODY()

	/** Position in the emitter's simulation stage (or event handler) list; use as StageIndex. */
	UPROPERTY()
	int32 StageIndex = INDEX_NONE;

	/** Stage name (simulation stages) or "Event Handler". */
	UPROPERTY()
	FString Name;

	/** Simulation stage class, or NiagaraEventHandler. */
	UPROPERTY()
	FString Class;

	UPROPERTY()
	bool bEnabled = true;

	/** Simulation stages: Particles or DataInterface. */
	UPROPERTY()
	FString IterationSource;

	/** Simulation stages iterating a data interface: the bound parameter, e.g. Emitter.Grid. */
	UPROPERTY()
	FString DataInterface;

	/** Event handlers: emitter and event that feed it, execution mode and spawn number. */
	UPROPERTY()
	FString SourceEmitter;

	UPROPERTY()
	FString SourceEvent;

	UPROPERTY()
	FString ExecutionMode;

	UPROPERTY()
	int32 SpawnNumber = 0;

	UPROPERTY()
	int32 MaxEventsPerFrame = 0;
};

/** Simulation stages and event handlers of an emitter, in execution order. */
USTRUCT()
struct FNiagaraMcpEmitterStages
{
	GENERATED_BODY()

	UPROPERTY()
	FName Emitter;

	UPROPERTY()
	FString SimTarget;

	UPROPERTY()
	TArray<FNiagaraMcpStageInfo> SimulationStages;

	UPROPERTY()
	TArray<FNiagaraMcpStageInfo> EventHandlers;
};

/** A local (Scratch Pad) module owned by a system. */
USTRUCT()
struct FNiagaraMcpLocalModuleInfo
{
	GENERATED_BODY()

	/** Object path of the module script; pass it as Script to the graph tools and as ModuleScript to AddModule. */
	UPROPERTY()
	FString ScriptPath;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	TArray<FString> AllowedStacks;

	/** Where CreateLocalModule placed it (empty for ListLocalModules). */
	UPROPERTY()
	FNiagaraMcpModuleInfo PlacedModule;
};

/** An entry of a script graph's "add node" menu. */
USTRUCT()
struct FNiagaraMcpNodeAction
{
	GENERATED_BODY()

	/** Pass as NodeType "Action:<Name>" to AddGraphNode (add "|<Category>" when the name is ambiguous). */
	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Category;

	UPROPERTY()
	FString Tooltip;
};

/** Output log lines returned by ReadOutputLog. */
USTRUCT()
struct FNiagaraMcpLogPage
{
	GENERATED_BODY()

	/** "[Category] Verbosity: text", oldest first. */
	UPROPERTY()
	TArray<FString> Lines;

	/** Pass as SinceCursor next time to get only newer lines. */
	UPROPERTY()
	int64 NextCursor = 0;
};

// ==== v0.4 Niagara extensions: begin ====

/** An editable reflected property (system, emitter, renderer, stage, ...) with its current value. */
USTRUCT()
struct FNiagaraMcpEditableProperty
{
	GENERATED_BODY()

	/** Property name; pass it as PropertyName to the matching Set...Property tool. Nested struct fields use dotted paths (FixedBounds.Min). */
	UPROPERTY()
	FString Name;

	/** C++ type, e.g. float, bool, int32, FBox, ENiagaraSimTarget, UNiagaraEffectType*. */
	UPROPERTY()
	FString Type;

	/** Current value as text (enum entry names, asset paths, UE struct text). Long values are truncated. */
	UPROPERTY()
	FString Value;

	/** Category shown in the details panel. */
	UPROPERTY()
	FString Category;

	/** Valid entries for enum properties. */
	UPROPERTY()
	TArray<FString> Options;

	/** Visible but not editable. */
	UPROPERTY()
	bool bReadOnly = false;

	/** First sentence of the property's tooltip. */
	UPROPERTY()
	FString Tooltip;
};

/** Editable properties of one object. */
USTRUCT()
struct FNiagaraMcpEditablePropertyList
{
	GENERATED_BODY()

	/** What the properties belong to, e.g. the system path or "Emitter Sparks". */
	UPROPERTY()
	FString Owner;

	/** Reflected class or struct, e.g. NiagaraSystem or VersionedNiagaraEmitterData. */
	UPROPERTY()
	FString Class;

	UPROPERTY()
	TArray<FNiagaraMcpEditableProperty> Properties;
};

/** An attribute binding of a renderer (which particle / user / emitter variable feeds a renderer input). */
USTRUCT()
struct FNiagaraMcpBindingInfo
{
	GENERATED_BODY()

	/** Binding property name; pass it as BindingName to SetRendererBinding, e.g. ColorBinding. */
	UPROPERTY()
	FString Name;

	/** Niagara type the binding expects, e.g. NiagaraFloat, Vector2f, LinearColor. */
	UPROPERTY()
	FString Type;

	/** Bound variable, e.g. Particles.Color or User.MySize. */
	UPROPERTY()
	FString Variable;

	/** Variable bound by default. */
	UPROPERTY()
	FString DefaultVariable;
};

/** A material parameter fed by a Niagara variable (renderer MaterialParameters). */
USTRUCT()
struct FNiagaraMcpMaterialBindingInfo
{
	GENERATED_BODY()

	UPROPERTY()
	FString MaterialParameter;

	UPROPERTY()
	FString Variable;
};

/** Properties, attribute bindings and material bindings of a renderer. */
USTRUCT()
struct FNiagaraMcpRendererDetails
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Index = INDEX_NONE;

	UPROPERTY()
	FString Class;

	UPROPERTY()
	bool bEnabled = true;

	/** Editable properties (bindings are listed separately in Bindings). */
	UPROPERTY()
	TArray<FNiagaraMcpEditableProperty> Properties;

	UPROPERTY()
	TArray<FNiagaraMcpBindingInfo> Bindings;

	UPROPERTY()
	TArray<FNiagaraMcpMaterialBindingInfo> MaterialBindings;
};

/** A dynamic input script found by FindDynamicInputs. */
USTRUCT()
struct FNiagaraMcpDynamicInputInfo
{
	GENERATED_BODY()

	/** Object path; use it in SetModuleInput as Value "Dynamic:<Path>". */
	UPROPERTY()
	FString Path;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Description;

	UPROPERTY()
	FString Category;

	/** Niagara type of the value it produces, e.g. NiagaraFloat, Vector3f, LinearColor. */
	UPROPERTY()
	FString OutputType;

	/** Exposed in the stack's dynamic input menu. */
	UPROPERTY()
	bool bLibraryExposed = false;
};

/** Function specifiers of a data interface function call node. */
USTRUCT()
struct FNiagaraMcpFunctionSpecifierInfo
{
	GENERATED_BODY()

	UPROPERTY()
	FString NodeId;

	/** Data interface function name, e.g. GetFloatByIndex. */
	UPROPERTY()
	FString Function;

	/** "Key=Value" per specifier, e.g. "Attribute=Age". */
	UPROPERTY()
	TArray<FString> Specifiers;
};

/** Compact JSON snapshot of a system (see ExportSystemSpec). */
USTRUCT()
struct FNiagaraMcpSystemSpec
{
	GENERATED_BODY()

	UPROPERTY()
	FString SystemPath;

	/** JSON: { system, userParameters, emitters: { <name>: { enabled, properties, stacks, renderers, simulationStages, eventHandlers } } }. Only non-default values are listed. */
	UPROPERTY()
	FString Spec;
};

/** One difference found by DiffSystems. */
USTRUCT()
struct FNiagaraMcpSpecDifference
{
	GENERATED_BODY()

	/** Spec path, e.g. emitters.Sparks.stacks.ParticleSpawn.Initialize Particle.inputs.Lifetime. */
	UPROPERTY()
	FString Path;

	/** Value in SystemA ("<missing>" when absent). */
	UPROPERTY()
	FString A;

	/** Value in SystemB ("<missing>" when absent). */
	UPROPERTY()
	FString B;
};

/** Result of DiffSystems. */
USTRUCT()
struct FNiagaraMcpSpecDiff
{
	GENERATED_BODY()

	UPROPERTY()
	FString SystemA;

	UPROPERTY()
	FString SystemB;

	UPROPERTY()
	bool bIdentical = false;

	UPROPERTY()
	TArray<FNiagaraMcpSpecDifference> Differences;
};

/** Statistics of one particle attribute. Vector values list one number per component ("x, y, z"). */
USTRUCT()
struct FNiagaraMcpAttributeStats
{
	GENERATED_BODY()

	/** Attribute name without the Particles. prefix, e.g. Age. */
	UPROPERTY()
	FString Attribute;

	/** Niagara type, e.g. NiagaraFloat, NiagaraPosition, LinearColor. */
	UPROPERTY()
	FString Type;

	/** Number of particles read. */
	UPROPERTY()
	int32 Count = 0;

	UPROPERTY()
	FString Min;

	UPROPERTY()
	FString Max;

	UPROPERTY()
	FString Mean;

	/** Values of the first particles (up to MaxSamples). */
	UPROPERTY()
	TArray<FString> Samples;
};

/** Particle attributes of one emitter at one time. */
USTRUCT()
struct FNiagaraMcpEmitterAttributes
{
	GENERATED_BODY()

	UPROPERTY()
	FName Emitter;

	/** CPUSim or GPUComputeSim. */
	UPROPERTY()
	FString SimTarget;

	UPROPERTY()
	int32 Particles = 0;

	/** Active, Inactive, Complete, ... */
	UPROPERTY()
	FString State;

	/** Why attributes are missing (GPU emitter, unknown attribute names, ...). */
	UPROPERTY()
	FString Note;

	UPROPERTY()
	TArray<FNiagaraMcpAttributeStats> Attributes;
};

/** Particle attributes of every emitter at one simulation time. */
USTRUCT()
struct FNiagaraMcpAttributeSample
{
	GENERATED_BODY()

	UPROPERTY()
	float Time = 0.0f;

	UPROPERTY()
	TArray<FNiagaraMcpEmitterAttributes> Emitters;
};

// ==== v0.4 Niagara extensions: end ====

// ==== v0.5 INFRA: begin ====
/** One journal entry (editor transaction, undo/redo or save). */
USTRUCT()
struct FNiagaraMcpChangeEntry
{
	GENERATED_BODY()

	UPROPERTY()
	int64 Cursor = 0;

	/** UTC, ISO 8601. */
	UPROPERTY()
	FString Time;

	/** Edit, Undo, Redo or Save. */
	UPROPERTY()
	FString Kind;

	/** Transaction title, e.g. "Niagara MCP: Set Module Input" (agent) or an editor action such as "Edit Spawn Rate" (user). */
	UPROPERTY()
	FString Title;

	/** Packages touched, e.g. /Game/FX/NS_Fire. */
	UPROPERTY()
	TArray<FString> Packages;

	/** True for changes made through these tools; false for edits made by someone in the editor. */
	UPROPERTY()
	bool bByAgent = false;
};

/** Result of GetChangesSince. */
USTRUCT()
struct FNiagaraMcpChangeLog
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FNiagaraMcpChangeEntry> Entries;

	/** Pass as SinceCursor next time. */
	UPROPERTY()
	int64 NextCursor = 0;

	/** Entries made in the editor (not by these tools). Re-read those assets before editing them. */
	UPROPERTY()
	int32 UserChangeCount = 0;

	/** More matching entries remain: call again with NextCursor. */
	UPROPERTY()
	bool bMore = false;

	/** SinceCursor is older than the journal (1000 entries) or from a previous editor session: some changes are not listed. */
	UPROPERTY()
	bool bHistoryTruncated = false;
};

/** A named point in the undo history. */
USTRUCT()
struct FNiagaraMcpRestorePointInfo
{
	GENERATED_BODY()

	UPROPERTY()
	FName Name;

	/** Undoable transactions when the point was made. */
	UPROPERTY()
	int32 UndoableCount = 0;

	/** Transactions added since (what a rollback would undo). */
	UPROPERTY()
	int32 StepsSince = 0;

	UPROPERTY()
	FString Created;
};

/** Result of RollbackToRestorePoint. */
USTRUCT()
struct FNiagaraMcpRollbackResult
{
	GENERATED_BODY()

	UPROPERTY()
	FName Name;

	UPROPERTY()
	bool bRolledBack = false;

	/** Titles of the undone transactions, most recent first. */
	UPROPERTY()
	TArray<FString> Undone;

	/** Editor (user) edits made after the restore point. */
	UPROPERTY()
	TArray<FString> UserChanges;
};

/** An engine Niagara template (emitter or system). */
USTRUCT()
struct FNiagaraMcpTemplateInfo
{
	GENERATED_BODY()

	/** Pass to AddEmitter TemplateEmitter (emitters) or DuplicateAsset (systems). */
	UPROPERTY()
	FString Path;

	/** Emitter or System. */
	UPROPERTY()
	FString Kind;

	/** Folder below /Niagara/DefaultAssets/Templates, e.g. Emitters or BehaviorExamples. */
	UPROPERTY()
	FString Category;

	UPROPERTY()
	FString Description;
};
// ==== v0.5 INFRA: end ====

// ==== v0.5 VISION: begin ====

/**
 * Image statistics of one rendered preview tile (one simulation time seen from one camera yaw). Pixel numbers are
 * measured on the 8-bit sRGB image you see; "covered" pixels are the ones that differ from the empty background.
 */
USTRUCT()
struct FNiagaraMcpFrameStats
{
	GENERATED_BODY()

	/** Tile index in the image (row * columns + column, row 0 at the top). */
	UPROPERTY()
	int32 Tile = 0;

	UPROPERTY()
	int32 Row = 0;

	UPROPERTY()
	int32 Column = 0;

	/** Simulation time in seconds since activation. */
	UPROPERTY()
	float Time = 0.0f;

	/** Camera yaw of this tile in degrees. */
	UPROPERTY()
	float CameraYaw = 0.0f;

	/** Particles alive at this time, all emitters (same continuous run as the pixels). */
	UPROPERTY()
	int32 Particles = 0;

	/** Particles per emitter at this time. */
	UPROPERTY()
	TArray<FNiagaraMcpEmitterCount> Emitters;

	/** Percent of the tile's pixels that differ from the empty background (0-100). 0 = nothing visible. */
	UPROPERTY()
	float Coverage = 0.0f;

	/** Mean luminance of the covered pixels, 0-1. */
	UPROPERTY()
	float MeanLuminance = 0.0f;

	/** Brightest covered pixel's luminance, 0-1. */
	UPROPERTY()
	float PeakLuminance = 0.0f;

	/** Percent of covered pixels with a channel at 255 (clipped / overexposed), 0-100. */
	UPROPERTY()
	float ClippedShare = 0.0f;

	/** Mean color of the covered pixels as "r, g, b" (0-255 sRGB); empty when nothing is drawn. */
	UPROPERTY()
	FString MeanColor;

	/** Screen rectangle of the covered pixels as "x0, y0, x1, y1", normalized 0-1 with 0,0 at the top left; empty when nothing is drawn. */
	UPROPERTY()
	FString ScreenBounds;

	/** True when no pixel differs from the background. */
	UPROPERTY()
	bool bEmpty = true;

	/** Diagnosis when the tile looks suspicious (empty, clipped, cut off, too dark, fills the frame); empty when it looks fine. */
	UPROPERTY()
	FString Hint;
};

/** Result of CapturePreview: the contact sheet plus per-tile numbers and a readable caption. */
USTRUCT()
struct FNiagaraMcpPreviewResult
{
	GENERATED_BODY()

	/** The contact sheet (PNG). */
	UPROPERTY()
	FToolsetImage Image;

	/** One entry per rendered tile, in tile order (the reference tile, if any, has no entry). */
	UPROPERTY()
	TArray<FNiagaraMcpFrameStats> Frames;

	/** Compact summary of every tile: layout, framing, particles, coverage, color and hints. */
	UPROPERTY()
	FString Caption;

	/** Radius in cm of the sphere the camera frames (union of the effect's bounds over all times). */
	UPROPERTY()
	float FramingRadius = 0.0f;

	/** Columns x rows of the sheet. */
	UPROPERTY()
	int32 Columns = 0;

	UPROPERTY()
	int32 Rows = 0;

	/** Niagara quality level the simulation ran at, e.g. "Epic". */
	UPROPERTY()
	FString QualityLevel;
};

/** Difference of one tile against the stored baseline. */
USTRUCT()
struct FNiagaraMcpTileDiff
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Tile = 0;

	UPROPERTY()
	float Time = 0.0f;

	UPROPERTY()
	float CameraYaw = 0.0f;

	/** Percent of pixels whose color changed noticeably (any channel by more than 12/255, or by more than the renderer's measured frame noise if that is higher), 0-100. */
	UPROPERTY()
	float ChangedPercent = 0.0f;

	/** Percent of 8x8 pixel blocks whose average color changed noticeably; less sensitive to random particle jitter. */
	UPROPERTY()
	float BlockChangedPercent = 0.0f;

	/** Mean color change "dr, dg, db" (new minus baseline, -255..255, averaged over all pixels). */
	UPROPERTY()
	FString MeanColorDelta;

	/** Coverage (percent of pixels differing from the background) of the baseline and of the new render. */
	UPROPERTY()
	float BaselineCoverage = 0.0f;

	UPROPERTY()
	float Coverage = 0.0f;

	UPROPERTY()
	int32 BaselineParticles = 0;

	UPROPERTY()
	int32 Particles = 0;
};

/** Result of ComparePreview. */
USTRUCT()
struct FNiagaraMcpPreviewDiff
{
	GENERATED_BODY()

	UPROPERTY()
	FString BaselineName;

	/** True when this call stored a new baseline (nothing compared yet); the image is then the baseline render. */
	UPROPERTY()
	bool bBaselineCreated = false;

	/** New render with changed pixels tinted red (same layout as the baseline). */
	UPROPERTY()
	FToolsetImage Image;

	/** Per tile differences (empty when the baseline was just created). */
	UPROPERTY()
	TArray<FNiagaraMcpTileDiff> Tiles;

	/** Largest ChangedPercent over all tiles. */
	UPROPERTY()
	float MaxChangedPercent = 0.0f;

	UPROPERTY()
	FString Caption;
};

/** Cost numbers of one tile of CaptureCostView. */
USTRUCT()
struct FNiagaraMcpCostFrame
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Tile = 0;

	UPROPERTY()
	float Time = 0.0f;

	UPROPERTY()
	float CameraYaw = 0.0f;

	UPROPERTY()
	int32 Particles = 0;

	/** Percent of pixels the effect touches (differ from the empty background), 0-100. */
	UPROPERTY()
	float Coverage = 0.0f;

	/** Percent of the covered pixels in the red band or above (expensive), 0-100. */
	UPROPERTY()
	float BadShare = 0.0f;

	/** Percent of the covered pixels in the pink / white band (extreme), 0-100. */
	UPROPERTY()
	float ExtremeShare = 0.0f;

	/** Mean cost index of the covered pixels, 0-1 on the view mode's color scale (red starts at 0.28, pink / white at 0.67). */
	UPROPERTY()
	float MeanComplexity = 0.0f;
};

/** Result of CaptureCostView. */
USTRUCT()
struct FNiagaraMcpCostResult
{
	GENERATED_BODY()

	/** View mode, ShaderComplexity. */
	UPROPERTY()
	FString Mode;

	UPROPERTY()
	FToolsetImage Image;

	UPROPERTY()
	TArray<FNiagaraMcpCostFrame> Frames;

	UPROPERTY()
	FString Caption;
};

/** Result of BakeFlipbook. */
USTRUCT()
struct FNiagaraMcpBakeResult
{
	GENERATED_BODY()

	/** Object path of the new texture, e.g. /Game/FX/T_Fire_SubUV.T_Fire_SubUV. */
	UPROPERTY()
	FString AssetPath;

	/** Atlas size in pixels. */
	UPROPERTY()
	int32 Width = 0;

	UPROPERTY()
	int32 Height = 0;

	UPROPERTY()
	int32 Columns = 0;

	UPROPERTY()
	int32 Rows = 0;

	UPROPERTY()
	int32 FrameSize = 0;

	/** Simulation time of each frame (frame i is at column i % Columns, row i / Columns). */
	UPROPERTY()
	TArray<float> FrameTimes;

	/** Coverage (percent of pixels drawn) of each frame; 0 means an empty frame. */
	UPROPERTY()
	TArray<float> FrameCoverage;

	/** How the alpha channel was made. */
	UPROPERTY()
	FString AlphaMode;

	/** How to use the texture with a sprite renderer (SubUV). */
	UPROPERTY()
	FString Usage;

	UPROPERTY()
	bool bSaved = false;
};

// ==== v0.5 VISION: end ====

// ==== v0.5 ANALYSIS: begin ====

class UNiagaraDataChannelAsset;

/** One step of DiagnoseVisibility's checklist. */
USTRUCT()
struct FNiagaraMcpVisibilityCheck
{
	GENERATED_BODY()

	/** Emitter the check belongs to; None for system-level checks (compile, effect type). */
	UPROPERTY()
	FName Emitter;

	/** Check id: Compile, EmitterEnabled, Scalability, EffectType, Spawn, Particles, Lifetime, Renderers, RendererFeedback, SimTarget, Material, MaterialUsage, Alpha, Size, Bounds, Space. */
	UPROPERTY()
	FString Check;

	/** Ok, Warning, Fail, or Skipped (not evaluated because an earlier check failed or does not apply). */
	UPROPERTY()
	FString Status;

	/** What was measured, e.g. "0 particles at 0.5 s", "Color.a mean 0, max 0". */
	UPROPERTY()
	FString Evidence;

	/** Suggested fix, usually the exact next tool call, e.g. SetModuleInput(System, "Sparks", "Initialize Particle", "Color", "1, 0.5, 0.1, 1"). Empty for Ok. */
	UPROPERTY()
	FString Fix;
};

/** Result of DiagnoseVisibility. */
USTRUCT()
struct FNiagaraMcpVisibilityReport
{
	GENERATED_BODY()

	UPROPERTY()
	FString System;

	/** Simulation time the particle checks were measured at (seconds since activation). */
	UPROPERTY()
	float Time = 0.0f;

	/** No check failed: every checked emitter should produce visible particles. */
	UPROPERTY()
	bool bVisible = false;

	/** The first failing check ("<Emitter>: <Check> - <Evidence>"), the stopping point to fix first. Empty when nothing failed. */
	UPROPERTY()
	FString FirstFail;

	/** Check id of the first failure (see Checks[].Check), e.g. Spawn or MaterialUsage. */
	UPROPERTY()
	FString FirstFailCheck;

	/** Emitter of the first failure. */
	UPROPERTY()
	FName FirstFailEmitter;

	/** Ordered checklist: system checks, then per emitter. After an emitter's first Fail its remaining checks are Skipped. */
	UPROPERTY()
	TArray<FNiagaraMcpVisibilityCheck> Checks;
};

/** Compiled cost of one script of an emitter (or of the system). */
USTRUCT()
struct FNiagaraMcpScriptCost
{
	GENERATED_BODY()

	/** Stack / script usage: SystemSpawn, SystemUpdate, EmitterSpawn, EmitterUpdate, ParticleSpawn, ParticleUpdate, ParticleEvent, ParticleGPUCompute. */
	UPROPERTY()
	FString Script;

	/** CPU (VectorVM) or GPU (compute shader). */
	UPROPERTY()
	FString Target;

	/** Compile status: UpToDate, Error, UpToDateWithWarnings, Dirty, Unknown, ... */
	UPROPERTY()
	FString CompileStatus;

	/** VectorVM instruction count from the last compile in this editor session (0 when the script came from the cache; run CompileSystem to refresh). CPU only. */
	UPROPERTY()
	int32 OpCount = 0;

	/** Temporary registers the VM needs per instance batch. CPU only. */
	UPROPERTY()
	int32 TempRegisters = 0;

	/** Size of the VM byte code in bytes. CPU only. */
	UPROPERTY()
	int32 ByteCodeBytes = 0;

	/** Length of the translated HLSL in characters (see GetCompiledHlsl). */
	UPROPERTY()
	int32 HlslLength = 0;

	/** Compile errors / warnings count. */
	UPROPERTY()
	int32 CompileErrors = 0;

	UPROPERTY()
	int32 CompileWarnings = 0;
};

/** Measured GPU time of one compute stage (spawn/update or a simulation stage) of an emitter. */
USTRUCT()
struct FNiagaraMcpGpuStageTiming
{
	GENERATED_BODY()

	/** Stage name as reported by the Niagara GPU profiler, e.g. ParticleGPUComputeScript or a simulation stage name. */
	UPROPERTY()
	FString Stage;

	/** Number of dispatches measured. */
	UPROPERTY()
	int32 Dispatches = 0;

	/** Average GPU time per dispatch in microseconds. */
	UPROPERTY()
	double AverageMicroseconds = 0.0;
};

/** Cost of one emitter. */
USTRUCT()
struct FNiagaraMcpEmitterCost
{
	GENERATED_BODY()

	UPROPERTY()
	FName Emitter;

	UPROPERTY()
	bool bEnabled = true;

	/** CPUSim, GPUComputeSim or Lightweight. */
	UPROPERTY()
	FString SimTarget;

	/** Particle count at each requested time (same order as Times). */
	UPROPERTY()
	TArray<int32> ParticleCounts;

	/** Largest particle count seen. */
	UPROPERTY()
	int32 PeakParticles = 0;

	/** Sum of the CPU VM op counts of the per-particle scripts (spawn + update + events), a rough per-particle CPU cost. */
	UPROPERTY()
	int32 ParticleOpCount = 0;

	UPROPERTY()
	TArray<FNiagaraMcpScriptCost> Scripts;

	/** GPU stage timings (GPU emitters, when the GPU profiler delivered results). */
	UPROPERTY()
	TArray<FNiagaraMcpGpuStageTiming> GpuStages;
};

/** CPU wall time of one simulation step of AnalyzeCost's run. */
USTRUCT()
struct FNiagaraMcpSimStepCost
{
	GENERATED_BODY()

	/** Simulation time reached by this step (seconds since activation). */
	UPROPERTY()
	float Time = 0.0f;

	/** Simulated seconds covered by the step. */
	UPROPERTY()
	float SimulatedSeconds = 0.0f;

	/** Game-thread wall time of the step in milliseconds (all emitters, 60 Hz sub-ticks). Approximate: includes the editor's own overhead. */
	UPROPERTY()
	double Milliseconds = 0.0;

	/** Milliseconds per simulated 1/60 s tick. */
	UPROPERTY()
	double MillisecondsPerTick = 0.0;
};

/** Result of AnalyzeCost. */
USTRUCT()
struct FNiagaraMcpCostReport
{
	GENERATED_BODY()

	UPROPERTY()
	FString System;

	UPROPERTY()
	TArray<float> Times;

	/** System spawn / update scripts. */
	UPROPERTY()
	TArray<FNiagaraMcpScriptCost> SystemScripts;

	UPROPERTY()
	TArray<FNiagaraMcpEmitterCost> Emitters;

	/** CPU time per simulation step of one continuous run through Times. */
	UPROPERTY()
	TArray<FNiagaraMcpSimStepCost> Steps;

	/** Emitters ranked by estimated CPU cost (ParticleOpCount x PeakParticles for CPU emitters), most expensive first. */
	UPROPERTY()
	TArray<FString> Ranking;

	/** Notes, e.g. why GPU timings are missing or op counts are 0. */
	UPROPERTY()
	TArray<FString> Notes;
};

/** Translated HLSL of a compiled script (see GetCompiledHlsl). */
USTRUCT()
struct FNiagaraMcpHlslResult
{
	GENERATED_BODY()

	UPROPERTY()
	FName Emitter;

	/** Script usage that was read, e.g. ParticleUpdateScript or ParticleGPUComputeScript. */
	UPROPERTY()
	FString Script;

	/** CPU or GPU. */
	UPROPERTY()
	FString Target;

	/** Total length of the HLSL in characters. */
	UPROPERTY()
	int32 TotalLength = 0;

	/** First character returned. */
	UPROPERTY()
	int32 Offset = 0;

	/** Characters returned. */
	UPROPERTY()
	int32 Length = 0;

	/** More text follows: call again with Offset = Offset + Length. */
	UPROPERTY()
	bool bTruncated = false;

	/** The HLSL text (the requested window). */
	UPROPERTY()
	FString Hlsl;
};

/** One operation of ApplySystemSpec. */
USTRUCT()
struct FNiagaraMcpSpecOperation
{
	GENERATED_BODY()

	/** What was done: SetSystemProperty, SetUserParameter, AddEmitter, RemoveEmitter, SetEmitterEnabled, SetEmitterProperty, AddModule, RemoveModule, SetModuleEnabled, SetModuleInput, AddRenderer, RemoveRenderer, SetRendererProperty, SetRendererBinding, SetMaterialBinding, AddSimulationStage, SetSimulationStageProperty, AddEventHandler. */
	UPROPERTY()
	FString Action;

	/** Spec path the operation came from, e.g. emitters.Sparks.stacks.ParticleSpawn.Initialize Particle.inputs.Lifetime. */
	UPROPERTY()
	FString Path;

	/** Value applied. */
	UPROPERTY()
	FString Value;

	/** Applied, WouldApply (dry run), Skipped or Failed. */
	UPROPERTY()
	FString Status;

	/** Why it was skipped or failed. */
	UPROPERTY()
	FString Reason;
};

/** Result of ApplySystemSpec. */
USTRUCT()
struct FNiagaraMcpApplySpecResult
{
	GENERATED_BODY()

	UPROPERTY()
	FString System;

	/** Merge or Replace. */
	UPROPERTY()
	FString Mode;

	UPROPERTY()
	bool bDryRun = false;

	UPROPERTY()
	int32 Applied = 0;

	UPROPERTY()
	int32 Skipped = 0;

	UPROPERTY()
	int32 Failed = 0;

	/** Operations already matching the spec (nothing to do). */
	UPROPERTY()
	int32 Unchanged = 0;

	UPROPERTY()
	bool bSaved = false;

	/** Every operation that changed (or would change) something, was skipped, or failed. */
	UPROPERTY()
	TArray<FNiagaraMcpSpecOperation> Operations;
};

/** A module of a lightweight (stateless) emitter. */
USTRUCT()
struct FNiagaraMcpStatelessModuleInfo
{
	GENERATED_BODY()

	/** Module name for SetStatelessModuleProperty, e.g. InitializeParticle, ShapeLocation, ScaleColor. */
	UPROPERTY()
	FString Name;

	/** Display name, e.g. "Initialize Particle". */
	UPROPERTY()
	FString DisplayName;

	UPROPERTY()
	FString Class;

	UPROPERTY()
	bool bEnabled = true;

	/** The module can be switched off (property bModuleEnabled). */
	UPROPERTY()
	bool bCanDisable = false;

	/** Editable properties; pass Name (dotted paths for struct fields, e.g. LifetimeDistribution.Min) to SetStatelessModuleProperty. */
	UPROPERTY()
	TArray<FNiagaraMcpEditableProperty> Properties;
};

/** A lightweight (stateless) emitter. */
USTRUCT()
struct FNiagaraMcpStatelessEmitterInfo
{
	GENERATED_BODY()

	UPROPERTY()
	FName Emitter;

	/** Stateless emitter template asset (defines the module list). */
	UPROPERTY()
	FString Template;

	UPROPERTY()
	TArray<FNiagaraMcpStatelessModuleInfo> Modules;

	/** Emitter-level properties (FixedBounds, RandomSeed, SpawnInfos, ...); set them with SetStatelessModuleProperty and Module "Emitter". */
	UPROPERTY()
	TArray<FNiagaraMcpEditableProperty> EmitterProperties;

	/** Renderer classes in order. */
	UPROPERTY()
	TArray<FString> Renderers;
};

/** One place a user parameter is used. */
USTRUCT()
struct FNiagaraMcpParameterUse
{
	GENERATED_BODY()

	/** ModuleInput, MapGet, RendererBinding, MaterialBinding, StageBinding, DataInterfaceBinding or EmitterProperty. */
	UPROPERTY()
	FString Kind;

	/** Emitter; None for the system stacks / system-level objects. */
	UPROPERTY()
	FName Emitter;

	/** Stack or owner, e.g. ParticleSpawn, Renderer 0 (NiagaraSpriteRendererProperties), SimulationStage 1. */
	UPROPERTY()
	FString Where;

	/** Module display name for ModuleInput / MapGet in a module script; empty otherwise. */
	UPROPERTY()
	FString Module;

	/** Input, binding or property name, e.g. "Color", "SpriteSizeBinding", "NumIterations". */
	UPROPERTY()
	FString Target;
};

/** Where one user parameter is used. */
USTRUCT()
struct FNiagaraMcpParameterUsage
{
	GENERATED_BODY()

	/** Parameter name with the User. prefix. */
	UPROPERTY()
	FString Parameter;

	UPROPERTY()
	FString Type;

	/** No use found: the parameter has no effect. */
	UPROPERTY()
	bool bUnused = true;

	UPROPERTY()
	TArray<FNiagaraMcpParameterUse> Uses;
};

/** A variable declaration (name + type), e.g. for data channels. */
USTRUCT(BlueprintType)
struct FNiagaraMcpVariableDecl
{
	GENERATED_BODY()

	/** Variable name, e.g. Position or Energy. */
	UPROPERTY()
	FName Name;

	/** float, int, bool, vec2, vec3, vec4, color, position, quat or a Niagara type name (see SetUserParameter). */
	UPROPERTY()
	FString Type;
};

/** A Niagara Data Channel asset. */
USTRUCT()
struct FNiagaraMcpDataChannelInfo
{
	GENERATED_BODY()

	UPROPERTY()
	FString Path;

	/** Channel class: Islands, Global or GameplayBurst (NiagaraDataChannel_*). */
	UPROPERTY()
	FString Type;

	/** Channel variables with their (data channel) types, e.g. Position: Vector (LWC position), Energy: float. */
	UPROPERTY()
	TArray<FNiagaraMcpVariableDecl> Variables;

	/** Editable channel settings (Mode, InitialExtents, bKeepPreviousFrameData, ...). */
	UPROPERTY()
	TArray<FNiagaraMcpEditableProperty> Properties;

	UPROPERTY()
	bool bSaved = false;
};

/** One color changed (or reported) by RecolorEffect. */
USTRUCT()
struct FNiagaraMcpColorChange
{
	GENERATED_BODY()

	/** ModuleInput, CurveKeys, UserParameter or MaterialParameter (reported only). */
	UPROPERTY()
	FString Kind;

	UPROPERTY()
	FName Emitter;

	/** Where: "Initialize Particle.Color", "Color.Curve (5 keys)", "User.Tint", material path + parameter. */
	UPROPERTY()
	FString Target;

	UPROPERTY()
	FString Before;

	UPROPERTY()
	FString After;

	/** False for items only reported (shared materials). */
	UPROPERTY()
	bool bChanged = true;
};

/** Result of RecolorEffect. */
USTRUCT()
struct FNiagaraMcpRecolorResult
{
	GENERATED_BODY()

	UPROPERTY()
	FString System;

	UPROPERTY()
	int32 Changed = 0;

	UPROPERTY()
	TArray<FNiagaraMcpColorChange> Changes;

	UPROPERTY()
	bool bSaved = false;
};

// ==== v0.5 ANALYSIS: end ====

/**
 * Niagara authoring, inspection and verification for AI agents (name-based, compact; prefer it over Epic's
 * NiagaraToolsets for building and editing effects; read the "Niagara VFX authoring" skill first).
 *
 * Workflow:
 *   1. Inspect: GetSystemSummary -> GetStack -> FindModuleScripts.
 *   2. Build systems: CreateSystem -> AddEmitter -> AddModule / RemoveModule -> SetModuleInput -> AddRenderer -> CompileSystem.
 *      Many steps at once: ExecuteBatch (one request, results referenced as "$Id.field", one save at the end).
 *   3. Custom modules: CreateModuleScript -> GetScriptGraph -> AddGraphNode / AddParameterPin / SetCustomHlsl /
 *      ConnectPins / SetPinDefaultValue -> ApplyScriptChanges -> AddModule (with the new script) -> CompileSystem.
 *   4. Verify: ValidateSystem, GetParticleCounts, and the top-level MCP tool niagara_capture_preview (returns a real
 *      image; CapturePreview through call_tool returns the same picture as base64 text).
 *   5. Document: SetModuleNote leaves a note on a module in the stack.
 *
 * Emitters are addressed by EmitterName; pass None (or "System") for the system stacks. Modules are addressed by
 * display name, function name or ModuleId, optionally narrowed with Stack (ParticleSpawn, ParticleUpdate,
 * EmitterSpawn, EmitterUpdate, SystemSpawn, SystemUpdate). Values read with GetStack use SetModuleInput syntax.
 * Every write runs in an undo transaction through the Niagara editor's own stack view models, so it also works
 * while the system is open in the Niagara editor. Lookup errors list the valid names and suggest close matches.
 */
UCLASS(BlueprintType, Hidden)
class UNiagaraMcpEditorTools : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	// ---------------------------------------------------------------- Inspection

	/**
	 * Summarizes a Niagara system: emitters, renderers, simulation stages and user parameters.
	 *
	 * @param System The Niagara System
	 * @return Summary of the system
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Inspection")
	static FNiagaraMcpSystemSummary GetSystemSummary(UNiagaraSystem* System);

	/**
	 * Lists the modules of an emitter's stacks (or of the system stacks) in execution order, with their notes,
	 * script versions and, optionally, every input's current value.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter name; None or System for the system stacks
	 * @param bIncludeInputs Include each module's inputs and values
	 * @param ModuleFilter Only modules whose display or function name contains these words (e.g. "color"), Any for all: keeps large stacks small
	 * @return Modules in stack order
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Inspection")
	static FNiagaraMcpStackInfo GetStack(UNiagaraSystem* System, FName EmitterName, bool bIncludeInputs = true, const FString& ModuleFilter = TEXT("Any"));

	/**
	 * Searches module script assets (engine and project) by name, description or category.
	 *
	 * @param Filter Words to match, e.g. "spawn burst" or "ribbon"; Any lists everything
	 * @param StackName Only modules allowed in this stack, e.g. ParticleUpdate; Any for any
	 * @param MaxResults Maximum number of results
	 * @return Matching module scripts
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Inspection")
	static TArray<FNiagaraMcpModuleScriptInfo> FindModuleScripts(const FString& Filter = TEXT("Any"), const FString& StackName = TEXT("Any"), int32 MaxResults = 40);

	/**
	 * Compiles a system and reports per-script status and every compile error and warning.
	 *
	 * @param System The Niagara System
	 * @param bWaitForCompletion Block until compilation finishes (recommended)
	 * @param bSave Save the system after compiling
	 * @return Compile status and messages
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "Niagara|Inspection")
	static FNiagaraMcpCompileResult CompileSystem(UNiagaraSystem* System, bool bWaitForCompletion = true, bool bSave = false);

	/**
	 * Renders the system off-screen at several simulation times and returns ONE contact-sheet image plus numbers
	 * per tile (particles per emitter, coverage = % of pixels drawn, luminance, clipped share, mean color, screen
	 * bounds, and a Hint when a tile looks wrong, e.g. "0 particles: spawn/lifetime problem" or "40 particles but
	 * nothing drawn: material, alpha, size, facing or bounds"). Read Caption first: it summarizes every tile.
	 * Nothing in the level changes; all tiles come from ONE continuous simulation run.
	 *
	 * Layout: with one camera yaw, tiles go left to right, top to bottom in the order of Times (up to 4 per row).
	 * With several CameraYaws, each row is one time (in the order of Times) and each column one yaw.
	 * A ReferenceImage is the first tile (with several yaws it gets a row of its own).
	 * Examples: Times [0.2, 0.5, 1.0]; Times [0.5] + CameraYaws "0, 90" (side and front); QualityLevel "Low" to see
	 * what low-end scalability leaves of the effect; ReferenceImage "D:/Refs/fire.png" to compare with concept art.
	 *
	 * @param System The Niagara System
	 * @param Times Seconds since activation for each frame, e.g. [0.1, 0.3, 0.6, 1.2]; empty uses 0.25, 0.5, 1, 2 (max 12)
	 * @param FrameSize Pixel size of each square frame (64-1024)
	 * @param CameraYaw Camera yaw in degrees; 0 is a side view looking along +X, 90 looks along +Y
	 * @param CameraPitch Camera pitch in degrees; negative looks down, -90 is top-down
	 * @param CameraDistance Distance from the effect's center; 0 frames its bounds automatically
	 * @param Background Black (default), Grey or White. Use Grey for dark or translucent smoke (invisible on black); additive effects look brighter on black than in a lit level
	 * @param CameraYaws Several yaws in degrees as text, e.g. "0, 90" or "0, 120, 240": every time is shot from each yaw (max 8). None uses CameraYaw
	 * @param QualityLevel Niagara quality level to simulate at: Current (the editor's), Low, Medium, High, Epic, Cinematic or 0-4. Emitters and modules disabled for that level by their scalability / platform sets disappear
	 * @param ReferenceImage Absolute path of a PNG / JPG / BMP / TGA / EXR image shown as the first tile for comparison, e.g. D:/Refs/fire.png; None for no reference
	 * @return Image (contact sheet), Frames (numbers per tile), Caption (summary), FramingRadius (cm), Columns, Rows, QualityLevel
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Preview")
	static FNiagaraMcpPreviewResult CapturePreview(UNiagaraSystem* System, const TArray<float>& Times, int32 FrameSize = 320, float CameraYaw = 0.0f, float CameraPitch = -15.0f, float CameraDistance = 0.0f, const FString& Background = TEXT("Black"), const FString& CameraYaws = TEXT("None"), const FString& QualityLevel = TEXT("Current"), const FString& ReferenceImage = TEXT("None"));

	// ---------------------------------------------------------------- Systems and emitters

	/**
	 * Creates an empty Niagara System asset.
	 *
	 * @param AssetPath New asset path, e.g. /Game/TestSample/NS_ChainStrike
	 * @param bSave Save the new asset
	 * @return The new asset
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|System")
	static FNiagaraMcpAssetResult CreateSystem(const FString& AssetPath, bool bSave = true);

	/**
	 * Adds an emitter to a system, copied from a template or existing emitter asset.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Name for the new emitter (made unique if taken)
	 * @param TemplateEmitter Emitter asset to copy; Minimal (default) uses /Niagara/DefaultAssets/Templates/Emitters/Minimal (Emitter State, Spawn Rate, Initialize Particle, Particle State, Sprite renderer)
	 * @param bSave Save the system after the change
	 * @return The new emitter
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|System")
	static FNiagaraMcpEmitterInfo AddEmitter(UNiagaraSystem* System, FName EmitterName, const FString& TemplateEmitter = TEXT("Minimal"), bool bSave = true);

	/**
	 * Removes an emitter from a system.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter to remove
	 * @param bSave Save the system after the change
	 * @return Summary after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|System")
	static FNiagaraMcpSystemSummary RemoveEmitter(UNiagaraSystem* System, FName EmitterName, bool bSave = true);

	/**
	 * Renames an emitter.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Current name
	 * @param NewName New name (made unique if taken)
	 * @param bSave Save the system after the change
	 * @return The renamed emitter
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|System")
	static FNiagaraMcpEmitterInfo RenameEmitter(UNiagaraSystem* System, FName EmitterName, FName NewName, bool bSave = true);

	/**
	 * Sets an emitter property (the emitter "Properties" section), e.g. SimTarget = GPUComputeSim,
	 * bLocalSpace = true, CalculateBoundsMode = Dynamic, bDeterminism = true. Dotted paths reach nested struct fields.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter to edit
	 * @param PropertyName Property of the emitter data, e.g. bLocalSpace
	 * @param Value New value as text
	 * @param bSave Save the system after the change
	 * @return The property's value after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|System")
	static FNiagaraMcpPropertyResult SetEmitterProperty(UNiagaraSystem* System, FName EmitterName, const FString& PropertyName, const FString& Value, bool bSave = true);

	/**
	 * Adds a renderer to an emitter.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter to edit
	 * @param RendererType Sprite, Ribbon, Mesh, Light, Component, Decal, Volume or a renderer class path
	 * @param Material Material asset path for the renderer (None = keep the renderer's default)
	 * @param bSave Save the system after the change
	 * @return The new renderer
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Renderers")
	static FNiagaraMcpRendererInfo AddRenderer(UNiagaraSystem* System, FName EmitterName, const FString& RendererType, const FString& Material = TEXT("None"), bool bSave = true);

	/**
	 * Sets a property of a renderer, e.g. Material, FacingMode, UV0Settings.TilingLength, bEnabled.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter that owns the renderer
	 * @param RendererIndex Index from GetSystemSummary
	 * @param PropertyName Renderer property (dotted path for nested fields)
	 * @param Value New value as text; asset properties take an asset path
	 * @param bSave Save the system after the change
	 * @return The property's value after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Renderers")
	static FNiagaraMcpPropertyResult SetRendererProperty(UNiagaraSystem* System, FName EmitterName, int32 RendererIndex, const FString& PropertyName, const FString& Value, bool bSave = true);

	/**
	 * Removes a renderer from an emitter.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter that owns the renderer
	 * @param RendererIndex Index from GetSystemSummary
	 * @param bSave Save the system after the change
	 * @return The emitter after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Renderers")
	static FNiagaraMcpEmitterInfo RemoveRenderer(UNiagaraSystem* System, FName EmitterName, int32 RendererIndex, bool bSave = true);

	/**
	 * Adds (or updates) a user parameter on the system.
	 *
	 * @param System The Niagara System
	 * @param ParameterName Name with or without the User. prefix
	 * @param Type float, int, bool, vec2, vec3, vec4, color, position, quat, or a data interface class
	 * @param DefaultValue Default value as text (ignored for data interfaces)
	 * @param bSave Save the system after the change
	 * @return The parameter after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|System")
	static FNiagaraMcpParameterInfo SetUserParameter(UNiagaraSystem* System, FName ParameterName, const FString& Type, const FString& DefaultValue, bool bSave = true);

	// ---------------------------------------------------------------- Stack modules

	/**
	 * Adds a module script to a stack, at its latest version.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter; None or System for SystemSpawn / SystemUpdate
	 * @param StackName ParticleSpawn, ParticleUpdate, EmitterSpawn, EmitterUpdate, SystemSpawn or SystemUpdate
	 * @param ModuleScript Module script asset path (see FindModuleScripts), e.g. /Niagara/Modules/Spawn/Beams/SpawnBeam.SpawnBeam
	 * @param TargetIndex Position in the stack; -1 appends
	 * @param bSave Save the system after the change
	 * @return The placed module with its inputs
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Stack")
	static FNiagaraMcpModuleInfo AddModule(UNiagaraSystem* System, FName EmitterName, const FString& StackName, const FString& ModuleScript, int32 TargetIndex = -1, bool bSave = true);

	/**
	 * Removes a module from a stack.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter; None or System for the system stacks
	 * @param Module Module display name, function name or ModuleId
	 * @param StackName Stack to disambiguate (Any = search all stacks), e.g. ParticleSpawn
	 * @param bSave Save the system after the change
	 * @return The remaining modules of the emitter
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Stack")
	static FNiagaraMcpStackInfo RemoveModule(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& StackName = TEXT("Any"), bool bSave = true);

	/**
	 * Enables or disables a module.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter; None or System for the system stacks
	 * @param Module Module display name, function name or ModuleId
	 * @param bEnabled New state
	 * @param StackName Stack to disambiguate (Any = search all stacks)
	 * @param bSave Save the system after the change
	 * @return The module after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Stack")
	static FNiagaraMcpModuleInfo SetModuleEnabled(UNiagaraSystem* System, FName EmitterName, const FString& Module, bool bEnabled, const FString& StackName = TEXT("Any"), bool bSave = true);

	/**
	 * Sets a module input the way the stack UI does. Value forms:
	 *   "1.35", "0, 0, 200", "true", "Self" (enum entry)   -> local value
	 *   "@Particles.Age" / "@User.Color"                   -> link to a parameter
	 *   "=Particles.NormalizedAge * 2"                     -> custom HLSL expression
	 *   "Dynamic:/Niagara/DynamicInputs/...Asset"          -> dynamic input script
	 *   "Data:NiagaraDataInterfaceCurve"                   -> data interface of that class
	 *   "Asset:/Game/Path/Asset"                           -> object asset input
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter; None or System for the system stacks
	 * @param Module Module display name, function name or ModuleId
	 * @param Input Input name, e.g. SpawnCount or "Lifetime"
	 * @param Value New value (see forms above)
	 * @param StackName Stack to disambiguate (Any = search all stacks)
	 * @param bSave Save the system after the change
	 * @return The input after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Stack")
	static FNiagaraMcpInputInfo SetModuleInput(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& Input, const FString& Value, const FString& StackName = TEXT("Any"), bool bSave = true);

	/**
	 * Resets a module input to its default.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter; None or System for the system stacks
	 * @param Module Module display name, function name or ModuleId
	 * @param Input Input name
	 * @param StackName Stack to disambiguate (Any = search all stacks)
	 * @param bSave Save the system after the change
	 * @return The input after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Stack")
	static FNiagaraMcpInputInfo ResetModuleInput(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& Input, const FString& StackName = TEXT("Any"), bool bSave = true);

	/**
	 * Sets a property on the data interface of a module input (e.g. a curve or texture sample input).
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter; None or System for the system stacks
	 * @param Module Module display name, function name or ModuleId
	 * @param Input Input whose value is a data interface
	 * @param PropertyName Data interface property, e.g. Texture
	 * @param Value New value as text
	 * @param StackName Stack to disambiguate (Any = search all stacks)
	 * @param bSave Save the system after the change
	 * @return The property's value after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Stack")
	static FNiagaraMcpPropertyResult SetDataInterfaceInputProperty(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& Input, const FString& PropertyName, const FString& Value, const FString& StackName = TEXT("Any"), bool bSave = true);

	/**
	 * Switches a module to the latest exposed version of its script (clears "script version upgrade" notices).
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter; None or System for the system stacks
	 * @param Module Module display name, function name or ModuleId
	 * @param StackName Stack to disambiguate (Any = search all stacks)
	 * @param bSave Save the system after the change
	 * @return The module after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Stack")
	static FNiagaraMcpModuleInfo UpgradeModuleVersion(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& StackName = TEXT("Any"), bool bSave = true);

	/**
	 * Attaches a stack note to a module (shown in the Niagara stack), e.g. to explain what an AI-made module does.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter; None or System for the system stacks
	 * @param Module Module display name, function name or ModuleId
	 * @param Header Note title
	 * @param Message Note body; empty Header and Message remove the note
	 * @param bInline Show the note inline in the module row
	 * @param StackName Stack to disambiguate (Any = search all stacks)
	 * @param bSave Save the system after the change
	 * @return The note after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Notes")
	static FNiagaraMcpNoteInfo SetModuleNote(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& Header, const FString& Message, bool bInline = false, const FString& StackName = TEXT("Any"), bool bSave = true);

	// ---------------------------------------------------------------- Module script graphs

	/**
	 * Creates a module script asset from the editor's default module template
	 * (Input Map -> Map Get -> Map Set -> Output Module). Edit its graph with the graph tools.
	 *
	 * @param AssetPath New asset path, e.g. /Game/TestSample/NM_ChainStrikeMotion
	 * @param Description Module description shown in the add menu
	 * @param AllowedStacks Stacks the module may be placed in, e.g. ["ParticleSpawn","ParticleUpdate"]; empty keeps the template's
	 * @param bSave Save the new asset
	 * @return The new script's graph
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Graph")
	static FNiagaraMcpGraphInfo CreateModuleScript(const FString& AssetPath, const FString& Description, const TArray<FString>& AllowedStacks, bool bSave = true);

	/**
	 * Reads a Niagara script graph (module, dynamic input or function): nodes, pins, types, defaults and links.
	 *
	 * @param Script The Niagara script
	 * @param NodeFilter Only nodes whose class or title contains these words (e.g. "MapGet" or "Custom"), Any for all
	 * @param Offset First node to return (paging; see NextOffset)
	 * @param MaxNodes Nodes per page (1-500)
	 * @return The graph
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Graph")
	static FNiagaraMcpGraphInfo GetScriptGraph(UNiagaraScript* Script, const FString& NodeFilter = TEXT("Any"), int32 Offset = 0, int32 MaxNodes = 200);

	/**
	 * Adds a node to a script graph. NodeType:
	 *   MapGet, MapSet, CustomHlsl, Reroute,
	 *   Op:<OpName> (e.g. Op:Numeric::Add, Op:Numeric::Mul), Function:<script path> (function / dynamic input call),
	 *   Action:<Name>[|<Category>] (any entry of ListNodeActions),
	 *   or a node class path such as /Script/NiagaraEditor.NiagaraNodeParameterMapGet.
	 *
	 * @param Script The Niagara script (its asset editor must be closed)
	 * @param NodeType Node to create
	 * @param PosX Graph X position
	 * @param PosY Graph Y position
	 * @return The new node
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Graph")
	static FNiagaraMcpGraphNode AddGraphNode(UNiagaraScript* Script, const FString& NodeType, int32 PosX = 0, int32 PosY = 0);

	/**
	 * Removes a node from a script graph.
	 *
	 * @param Script The Niagara script
	 * @param NodeId Node guid from GetScriptGraph
	 * @return The graph after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Graph")
	static FNiagaraMcpGraphInfo RemoveGraphNode(UNiagaraScript* Script, const FString& NodeId);

	/**
	 * Adds a parameter pin to a Map Get (output) or Map Set (input) node. Module inputs use the Module.
	 * namespace (Module.Length) and appear in the stack as module inputs; particle attributes use Particles.
	 *
	 * @param Script The Niagara script
	 * @param NodeId Map Get or Map Set node
	 * @param ParameterName Full parameter name, e.g. Module.LandingX or Particles.RibbonWidth
	 * @param Type Parameter type (float, vec3, position, ...)
	 * @return The node after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Graph")
	static FNiagaraMcpGraphNode AddParameterPin(UNiagaraScript* Script, const FString& NodeId, FName ParameterName, const FString& Type);

	/**
	 * Sets the HLSL body and the typed input/output pins of a Custom Hlsl node. Existing connections are kept for
	 * pins whose name did not change. Parameter map pins are kept unless redeclared.
	 *
	 * @param Script The Niagara script
	 * @param NodeId Custom Hlsl node
	 * @param Hlsl Function body; assign every output
	 * @param Inputs Input pins (name + type)
	 * @param Outputs Output pins (name + type)
	 * @return The node after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Graph")
	static FNiagaraMcpGraphNode SetCustomHlsl(UNiagaraScript* Script, const FString& NodeId, const FString& Hlsl, const TArray<FNiagaraMcpHlslPin>& Inputs, const TArray<FNiagaraMcpHlslPin>& Outputs);

	/**
	 * Connects an output pin to an input pin. Connecting to an "Add" pin creates a matching pin.
	 *
	 * @param Script The Niagara script
	 * @param FromNodeId Node with the output pin
	 * @param FromPin Output pin name
	 * @param ToNodeId Node with the input pin
	 * @param ToPin Input pin name
	 * @return The input node after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Graph")
	static FNiagaraMcpGraphNode ConnectPins(UNiagaraScript* Script, const FString& FromNodeId, FName FromPin, const FString& ToNodeId, FName ToPin);

	/**
	 * Breaks the link between two pins.
	 *
	 * @param Script The Niagara script
	 * @param FromNodeId Node with the output pin
	 * @param FromPin Output pin name
	 * @param ToNodeId Node with the input pin
	 * @param ToPin Input pin name
	 * @return The input node after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Graph")
	static FNiagaraMcpGraphNode DisconnectPins(UNiagaraScript* Script, const FString& FromNodeId, FName FromPin, const FString& ToNodeId, FName ToPin);

	/**
	 * Sets the default value of an input pin. On Map Get nodes the input pin with the same name as a Module.
	 * output holds that module input's default value.
	 *
	 * @param Script The Niagara script
	 * @param NodeId Node that owns the pin
	 * @param PinName Input pin name
	 * @param Value Default value in the pin's text format (as returned by GetScriptGraph), e.g. 0.34 or 0,0,1
	 * @return The node after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Graph")
	static FNiagaraMcpGraphNode SetPinDefaultValue(UNiagaraScript* Script, const FString& NodeId, FName PinName, const FString& Value);

	/**
	 * Sets a module script's description and allowed stacks.
	 *
	 * @param Script The module script
	 * @param Description New description; empty keeps the current one
	 * @param AllowedStacks Stacks the module may be placed in; empty keeps the current ones
	 * @param bSave Save the script after the change
	 * @return The script graph after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Graph")
	static FNiagaraMcpGraphInfo SetModuleScriptInfo(UNiagaraScript* Script, const FString& Description, const TArray<FString>& AllowedStacks, bool bSave = true);

	/**
	 * Pushes graph edits to every module node that calls the script (refreshing their inputs in the stacks),
	 * requests a recompile of the affected systems and saves. Call after a batch of graph edits.
	 *
	 * @param Script The edited script
	 * @param bSave Save the script and the affected systems
	 * @return What was refreshed and recompiled
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Graph")
	static FNiagaraMcpApplyInfo ApplyScriptChanges(UNiagaraScript* Script, bool bSave = true);

	// ---------------------------------------------------------------- Validation and statistics

	/**
	 * Checks a system and returns every problem in one list: compile errors/warnings, the issues the Niagara stack
	 * shows (with their automatic fixes) and plugin checks (renderer without material, GPU emitter with dynamic
	 * bounds or a light renderer, emitter without a spawn module).
	 *
	 * @param System The Niagara System
	 * @param bCompile Compile first so compile messages are current
	 * @return All issues; bOk is true when there is no error
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Validation")
	static FNiagaraMcpValidationResult ValidateSystem(UNiagaraSystem* System, bool bCompile = true);

	/**
	 * Applies one of the automatic fixes the Niagara stack offers for an issue (see ValidateSystem).
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter of the issue (None for the system)
	 * @param IssueId Issue id from ValidateSystem
	 * @param FixIndex Index into the issue's Fixes
	 * @param bSave Save the system after the change
	 * @return The issues remaining afterwards
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Validation")
	static FNiagaraMcpValidationResult ApplyStackIssueFix(UNiagaraSystem* System, FName EmitterName, const FString& IssueId, int32 FixIndex = 0, bool bSave = true);

	/**
	 * Simulates the system off-screen and reports particle counts per emitter at each time. Use it to check that
	 * spawn counts, lifetimes and timing behave as intended (for example 40 particles at 0.5 s, 0 after 1.4 s).
	 *
	 * @param System The Niagara System
	 * @param Times Seconds since activation, e.g. [0.1, 0.5, 1.0, 2.0]; empty uses 0.25, 0.5, 1, 2
	 * @param QualityLevel Niagara quality level to simulate at: Current (the editor's), Low, Medium, High, Epic, Cinematic or 0-4 (compare Low vs Epic to check scalability)
	 * @return One sample per time
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Validation")
	static TArray<FNiagaraMcpParticleSample> GetParticleCounts(UNiagaraSystem* System, const TArray<float>& Times, const FString& QualityLevel = TEXT("Current"));

	// ---------------------------------------------------------------- Curves

	/**
	 * Reads the keys of a curve data interface used by a module input (float, vector2/3/4 or color curve).
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter; None or System for the system stacks
	 * @param Module Module display name, function name or ModuleId
	 * @param Input Input whose value is a curve data interface
	 * @param StackName Stack to disambiguate (Any = search all stacks)
	 * @return Channels and keys
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Curves")
	static FNiagaraMcpCurveInfo GetCurveKeys(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& Input, const FString& StackName = TEXT("Any"));

	/**
	 * Replaces the keys of a curve data interface used by a module input. If the input does not hold a curve yet,
	 * first call SetModuleInput with Value "Data:NiagaraDataInterfaceColorCurve" (or Curve, VectorCurve, ...).
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter; None or System for the system stacks
	 * @param Module Module display name, function name or ModuleId
	 * @param Input Input whose value is a curve data interface
	 * @param Keys Keys; each Value has one number per channel (1 for float, 3 for vector, 4 for color RGBA)
	 * @param Interpolation Cubic (default), Linear or Constant
	 * @param StackName Stack to disambiguate (Any = search all stacks)
	 * @param bSave Save the system after the change
	 * @return The curve after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Curves")
	static FNiagaraMcpCurveInfo SetCurveKeys(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& Input, const TArray<FNiagaraMcpCurveKey>& Keys, const FString& Interpolation = TEXT("Cubic"), const FString& StackName = TEXT("Any"), bool bSave = true);

	// ---------------------------------------------------------------- Editor

	/**
	 * Searches assets by class, name and folder, with paging. Use it to find textures, meshes, materials, systems
	 * or module scripts by name before referencing them.
	 *
	 * @param ClassName Asset class, e.g. Texture2D, Material, MaterialInstanceConstant, NiagaraSystem, StaticMesh; Any for any
	 * @param NameFilter Case-insensitive substring of the asset name; Any for any
	 * @param Path Folder to search recursively, e.g. /Game/Tex; default /Game
	 * @param MaxResults Page size
	 * @param Offset Index of the first result (use NextOffset of the previous page)
	 * @return One page of results
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Editor")
	static FNiagaraMcpAssetPage FindAssets(const FString& ClassName = TEXT("Any"), const FString& NameFilter = TEXT("Any"), const FString& Path = TEXT("/Game"), int32 MaxResults = 50, int32 Offset = 0);

	/**
	 * Places a Niagara system in the current editor level, selects it and moves the viewport camera to it.
	 *
	 * @param System The Niagara System
	 * @param Location World location "X, Y, Z"; default the origin
	 * @param Label Actor label in the outliner; Auto uses the system name
	 * @return The new actor
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Editor")
	static FNiagaraMcpActorInfo PlaceInLevel(UNiagaraSystem* System, const FString& Location = TEXT("0, 0, 0"), const FString& Label = TEXT("Auto"));

	/**
	 * Undoes the most recent editor transactions (every write tool of this plugin is one transaction).
	 *
	 * @param Count Number of transactions to undo
	 * @return Titles of the undone transactions
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Editor")
	static FNiagaraMcpUndoResult Undo(int32 Count = 1);

	/**
	 * Redoes transactions undone with Undo.
	 *
	 * @param Count Number of transactions to redo
	 * @return Titles of the redone transactions
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Editor")
	static FNiagaraMcpUndoResult Redo(int32 Count = 1);

	/**
	 * Runs several tool calls in one request, in order: build or edit a whole effect or material graph without a
	 * round trip per call. Steps may use earlier results: "$<Id>.<path>" (e.g. "$mul.name" after an AddExpression
	 * step with Id "mul"). Each step's own save is skipped; every package the batch dirtied is saved once at the end.
	 *
	 * Example Operations:
	 *   [{"Id": "n", "Tool": "AddExpression", "Arguments": "{"Material": "/Game/M_A", "ExpressionClass": "Multiply"}"},
	 *    {"Tool": "ConnectToMaterialProperty", "Arguments": "{"Material": "/Game/M_A", "FromExpression": "$n.name", "MaterialProperty": "EmissiveColor"}"}]
	 *
	 * When a step fails the tool reports an error whose text ends with the full batch result as JSON (which steps ran,
	 * their errors, whether it rolled back), so you can see exactly what was applied.
	 *
	 * @param Operations Steps to run (max 200). Undo, Redo and ExecuteBatch cannot be nested.
	 * @param bStopOnError Stop at the first failing step
	 * @param bRollbackOnError When a step fails, undo every step that already ran (nothing is saved)
	 * @param bDryRun Only check tool names and argument names (no step runs)
	 * @param bSave Save the dirtied packages at the end
	 * @param Expect Post-conditions checked after the steps, separated by ';' (None = no checks):
	 *   "Compiles:/Game/FX/NS_A"                     the system compiles without errors
	 *   "Valid:/Game/FX/NS_A"                        ValidateSystem reports no errors
	 *   "Particles:/Game/FX/NS_A:Sparks@0.5>=1"      emitter Sparks (or Any) has at least 1 particle at 0.5 s
	 *   "MaterialCompiles:/Game/FX/M_A"              the material (or function) compiles without errors
	 *   An unmet expectation fails the batch (and rolls it back when bRollbackOnError).
	 * @return Per-step results, expectations, undo count and saved packages
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Editor")
	static FNiagaraMcpBatchResult ExecuteBatch(const TArray<FNiagaraMcpBatchOp>& Operations, bool bStopOnError = true, bool bRollbackOnError = true, bool bDryRun = false, bool bSave = true, const FString& Expect = TEXT("None"));

	/**
	 * Reads recent editor output log lines (kept since the plugin loaded, newest 4000). Use it for the details behind
	 * compile failures, e.g. Categories ["LogNiagara", "LogShaderCompilers", "LogMaterial"], MinVerbosity Warning.
	 *
	 * @param Categories Log categories to include; empty for all
	 * @param MinVerbosity Least severe level to include: Error, Warning, Display or Log (default)
	 * @param Contains Only lines containing this text; Any for all
	 * @param SinceCursor Only lines after this cursor (NextCursor of a previous call); 0 for everything kept
	 * @param MaxLines Maximum number of (newest) lines
	 * @return Lines and the cursor for the next call
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Editor")
	static FNiagaraMcpLogPage ReadOutputLog(const TArray<FString>& Categories, const FString& MinVerbosity = TEXT("Log"), const FString& Contains = TEXT("Any"), int64 SinceCursor = 0, int32 MaxLines = 200);

	// ---------------------------------------------------------------- Simulation stages, event handlers, local modules

	/**
	 * Lists an emitter's simulation stages and event handlers.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter name
	 * @return Stages in execution order
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Stages")
	static FNiagaraMcpEmitterStages GetEmitterStages(UNiagaraSystem* System, FName EmitterName);

	/**
	 * Adds a simulation stage to an emitter (like its "+ Stage" menu). For grid solvers iterate a data interface,
	 * e.g. IterationSource DataInterface, DataInterfaceName Emitter.Grid, DataInterfaceClass NiagaraDataInterfaceGrid2DCollection.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter to add the stage to
	 * @param StageName Stage name shown in the stack, e.g. Advect
	 * @param IterationSource Particles (default) or DataInterface
	 * @param DataInterfaceName Data interface parameter to iterate (DataInterface iteration only)
	 * @param DataInterfaceClass Class of that data interface (DataInterface iteration only; None otherwise)
	 * @param TargetIndex Position in the stage list; -1 appends
	 * @param bSave Save the system after the change
	 * @return The emitter's stages after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Stages")
	static FNiagaraMcpEmitterStages AddSimulationStage(UNiagaraSystem* System, FName EmitterName, FName StageName, const FString& IterationSource = TEXT("Particles"), FName DataInterfaceName = NAME_None, const FString& DataInterfaceClass = TEXT("None"), int32 TargetIndex = -1, bool bSave = true);

	/**
	 * Adds an event handler to a CPU emitter (like "+ Stage > Event Handler"). Events are written by modules such as
	 * Generate Death Event / Generate Collision Event on the source emitter.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter that receives the events
	 * @param SourceEmitterName Emitter that generates the events; None means EmitterName
	 * @param SourceEventName Event name, e.g. DeathEvent or CollisionEvent
	 * @param ExecutionMode EveryParticle (default; run on existing particles) or SpawnedParticles (spawn new ones per event)
	 * @param SpawnNumber Particles spawned per event in SpawnedParticles mode
	 * @param MaxEventsPerFrame Events processed per frame; 0 means no limit
	 * @param bSave Save the system after the change
	 * @return The emitter's stages after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Stages")
	static FNiagaraMcpEmitterStages AddEventHandler(UNiagaraSystem* System, FName EmitterName, FName SourceEmitterName, FName SourceEventName, const FString& ExecutionMode = TEXT("EveryParticle"), int32 SpawnNumber = 0, int32 MaxEventsPerFrame = 0, bool bSave = true);

	/**
	 * Places a module script in a simulation stage or event handler stack.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter that owns the stage
	 * @param StageKind SimulationStage or EventHandler
	 * @param StageIndex Index from GetEmitterStages
	 * @param ModuleScript Module script path (asset or local module)
	 * @param TargetIndex Position in the stack; -1 appends
	 * @param bSave Save the system after the change
	 * @return The placed module with its inputs
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Stages")
	static FNiagaraMcpModuleInfo AddModuleToStage(UNiagaraSystem* System, FName EmitterName, const FString& StageKind, int32 StageIndex, const FString& ModuleScript, int32 TargetIndex = -1, bool bSave = true);

	/**
	 * Creates a local (Scratch Pad) module inside the system from the default module template and places it in a
	 * stack. Edit its graph with the graph tools using the returned ScriptPath, then call ApplyScriptChanges.
	 *
	 * @param System The Niagara System (its Niagara editor must be closed while editing the module graph)
	 * @param EmitterName Emitter whose stack receives the module; None for system stacks
	 * @param StackName ParticleSpawn, ParticleUpdate, EmitterSpawn, EmitterUpdate, SystemSpawn, SystemUpdate, SimulationStage or ParticleEvent
	 * @param StageIndex Stage index for SimulationStage / ParticleEvent stacks, otherwise ignored
	 * @param ModuleName Name for the module (made unique)
	 * @param bSave Save the system after the change
	 * @return The new module
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Stages")
	static FNiagaraMcpLocalModuleInfo CreateLocalModule(UNiagaraSystem* System, FName EmitterName, const FString& StackName, int32 StageIndex, FName ModuleName, bool bSave = true);

	/**
	 * Lists the local (Scratch Pad) modules of a system.
	 *
	 * @param System The Niagara System
	 * @return Local modules
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Stages")
	static TArray<FNiagaraMcpLocalModuleInfo> ListLocalModules(UNiagaraSystem* System);

	// ---------------------------------------------------------------- Graph discovery and layout

	/**
	 * Lists the script graph's "add node" menu (functions, operators, data interface functions, parameters...).
	 * Pass a Name to AddGraphNode as NodeType "Action:<Name>".
	 *
	 * @param Script The Niagara script
	 * @param Filter Case-insensitive words to match in name or category, e.g. "sine" or "vector length"; Any lists everything
	 * @param MaxResults Maximum number of entries
	 * @return Matching menu entries
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Graph")
	static TArray<FNiagaraMcpNodeAction> ListNodeActions(UNiagaraScript* Script, const FString& Filter = TEXT("Any"), int32 MaxResults = 50);

	/**
	 * Arranges a script graph left to right by data flow (each node one column right of its inputs).
	 *
	 * @param Script The Niagara script
	 * @param bSave Save after the change
	 * @return The graph after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Graph")
	static FNiagaraMcpGraphInfo LayoutScriptGraph(UNiagaraScript* Script, bool bSave = true);
	// ==== v0.4 Niagara extensions: begin ====

	// ---------------------------------------------------------------- System, emitter and renderer properties

	/**
	 * Lists the editable properties of a system (the System Properties panel): warmup, fixed bounds, determinism,
	 * random seed, fixed tick delta, effect type, scalability overrides, ... with current values and enum options.
	 *
	 * @param System The Niagara System
	 * @param Filter Case-insensitive words matched against name, category and type, e.g. "warmup" or "bounds"; "Any" lists all
	 * @return Properties with their values
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|System")
	static FNiagaraMcpEditablePropertyList GetSystemProperties(UNiagaraSystem* System, const FString& Filter = TEXT("Any"));

	/**
	 * Sets a system property (see GetSystemProperties), e.g. WarmupTime = 1.5, bFixedBounds = true,
	 * FixedBounds = "(Min=(X=-200,Y=-200,Z=0),Max=(X=200,Y=200,Z=400),IsValid=True)", bDeterminism = true, RandomSeed = 42,
	 * EffectType = /Niagara/DefaultAssets/... (asset path) or None. Dotted paths reach nested struct fields (FixedBounds.Max).
	 * Works while the system is open in the Niagara editor.
	 *
	 * @param System The Niagara System
	 * @param PropertyName Property name, e.g. WarmupTime
	 * @param Value New value as text
	 * @param bSave Save the system after the change
	 * @return The property's value after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|System")
	static FNiagaraMcpPropertyResult SetSystemProperty(UNiagaraSystem* System, const FString& PropertyName, const FString& Value, bool bSave = true);

	/**
	 * Lists the editable properties of an emitter (the emitter "Properties" section: SimTarget, bLocalSpace,
	 * CalculateBoundsMode, AllocationMode, bDeterminism, ...) with current values and enum options. Use the names with
	 * SetEmitterProperty.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter name
	 * @param Filter Case-insensitive words matched against name, category and type, e.g. "bounds"; "Any" lists all
	 * @return Properties with their values
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|System")
	static FNiagaraMcpEditablePropertyList GetEmitterProperties(UNiagaraSystem* System, FName EmitterName, const FString& Filter = TEXT("Any"));

	/**
	 * Reads a renderer: editable properties with values and enum options, every attribute binding (e.g. ColorBinding ->
	 * Particles.Color, SpriteSizeBinding -> Particles.SpriteSize) and the material parameter bindings.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter that owns the renderer
	 * @param RendererIndex Index from GetSystemSummary
	 * @param Filter Case-insensitive words to filter properties and bindings, e.g. "size"; "Any" lists all
	 * @return The renderer details
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Renderers")
	static FNiagaraMcpRendererDetails GetRendererProperties(UNiagaraSystem* System, FName EmitterName, int32 RendererIndex, const FString& Filter = TEXT("Any"));

	/**
	 * Binds a renderer attribute to a variable, like picking it in the binding dropdown of the renderer, e.g.
	 * BindingName "SpriteSizeBinding", Variable "Particles.SpriteSize" or "User.MySize"; "ColorBinding" -> "Particles.MyTint".
	 * The variable must exist (particle attributes come from the last compile, user parameters from the system) and its
	 * type must match the binding's type.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter that owns the renderer
	 * @param RendererIndex Index from GetSystemSummary
	 * @param BindingName Binding property from GetRendererProperties, e.g. ColorBinding
	 * @param Variable Full variable name, e.g. Particles.Color, User.Tint, Emitter.Age
	 * @param bSave Save the system after the change
	 * @return The binding and its bound variable after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Renderers")
	static FNiagaraMcpPropertyResult SetRendererBinding(UNiagaraSystem* System, FName EmitterName, int32 RendererIndex, const FString& BindingName, const FString& Variable, bool bSave = true);

	/**
	 * Moves a renderer to another position in the emitter's renderer list (renderers draw in list order).
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter that owns the renderer
	 * @param RendererIndex Current index
	 * @param NewIndex Index after the move (0 = first)
	 * @param bSave Save the system after the change
	 * @return The emitter with its renderers after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Renderers")
	static FNiagaraMcpEmitterInfo MoveRenderer(UNiagaraSystem* System, FName EmitterName, int32 RendererIndex, int32 NewIndex, bool bSave = true);

	// ---------------------------------------------------------------- Module order

	/**
	 * Moves a module to another position (and optionally another stack of the same emitter), through the stack's
	 * drag-and-drop path, so input overrides move with it. Static switch inputs (bStatic in GetStack, e.g.
	 * "Lifetime Mode") are set with SetModuleInput like any other input.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter; None or System for the system stacks
	 * @param Module Module display name, function name or ModuleId
	 * @param StackName Stack the module is in, to disambiguate, e.g. ParticleUpdate; "Any" searches every stack
	 * @param TargetStackName Stack to move to, e.g. ParticleSpawn; "Any" keeps the module's stack
	 * @param TargetIndex Position in the target stack after the move (0 = first); -1 moves it to the end
	 * @param bSave Save the system after the change
	 * @return The moved module with its inputs
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Stack")
	static FNiagaraMcpModuleInfo MoveModule(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& StackName = TEXT("Any"), const FString& TargetStackName = TEXT("Any"), int32 TargetIndex = -1, bool bSave = true);

	/**
	 * Duplicates a module, keeping every input override (values, links, dynamic inputs, data interfaces), and places the
	 * copy in the same or another stack of the same emitter.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter; None or System for the system stacks
	 * @param Module Module display name, function name or ModuleId
	 * @param StackName Stack the module is in, to disambiguate; "Any" searches every stack
	 * @param TargetStackName Stack for the copy, e.g. ParticleUpdate; "Any" uses the module's stack
	 * @param TargetIndex Position of the copy in the target stack (0 = first); -1 places it at the end
	 * @param bSave Save the system after the change
	 * @return The new module with its inputs
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Stack")
	static FNiagaraMcpModuleInfo DuplicateModule(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& StackName = TEXT("Any"), const FString& TargetStackName = TEXT("Any"), int32 TargetIndex = -1, bool bSave = true);

	// ---------------------------------------------------------------- Emitters

	/**
	 * Enables or disables an emitter (the checkbox on the emitter in the system overview). Disabled emitters are not
	 * compiled or simulated.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter name
	 * @param bEnabled New state
	 * @param bSave Save the system after the change
	 * @return The emitter after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|System")
	static FNiagaraMcpEmitterInfo SetEmitterEnabled(UNiagaraSystem* System, FName EmitterName, bool bEnabled, bool bSave = true);

	/**
	 * Duplicates an emitter inside the system with all its modules, inputs, renderers, stages and event handlers
	 * (same as Duplicate in the system overview).
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter to copy
	 * @param NewName Name of the copy (made unique if taken); None appends a number
	 * @param bSave Save the system after the change
	 * @return The new emitter
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|System")
	static FNiagaraMcpEmitterInfo DuplicateEmitter(UNiagaraSystem* System, FName EmitterName, FName NewName = NAME_None, bool bSave = true);

	// ---------------------------------------------------------------- Simulation stages and event handlers

	/**
	 * Sets a property of a simulation stage, e.g. NumIterations = 4, ExecuteBehavior = OnSimulationReset,
	 * IterationSource = DataInterface, bEnabled = false, SimulationStageName = Advect, bSpawnOnly = true.
	 * Parameter-binding properties (NumIterations, EnabledBinding, ElementCountX/Y/Z, ...) take a constant ("4") or a
	 * parameter ("@User.Iterations", "@Emitter.Count"). An invalid name lists the stage's properties.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter that owns the stage
	 * @param StageIndex Index from GetEmitterStages
	 * @param PropertyName Stage property (dotted path for nested fields)
	 * @param Value New value as text
	 * @param bSave Save the system after the change
	 * @return The property's value after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Stages")
	static FNiagaraMcpPropertyResult SetSimulationStageProperty(UNiagaraSystem* System, FName EmitterName, int32 StageIndex, const FString& PropertyName, const FString& Value, bool bSave = true);

	/**
	 * Removes a simulation stage and its modules (like Delete on the stage in the stack).
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter that owns the stage
	 * @param StageIndex Index from GetEmitterStages
	 * @param bSave Save the system after the change
	 * @return The emitter's stages after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Stages")
	static FNiagaraMcpEmitterStages RemoveSimulationStage(UNiagaraSystem* System, FName EmitterName, int32 StageIndex, bool bSave = true);

	/**
	 * Moves a simulation stage to another position in the execution order.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter that owns the stage
	 * @param StageIndex Current index
	 * @param NewIndex Index after the move (0 = first)
	 * @param bSave Save the system after the change
	 * @return The emitter's stages after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Stages")
	static FNiagaraMcpEmitterStages MoveSimulationStage(UNiagaraSystem* System, FName EmitterName, int32 StageIndex, int32 NewIndex, bool bSave = true);

	/**
	 * Removes an event handler and its modules (like Delete on the event handler in the stack).
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter that owns the handler
	 * @param HandlerIndex Index into EventHandlers from GetEmitterStages
	 * @param bSave Save the system after the change
	 * @return The emitter's stages after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Stages")
	static FNiagaraMcpEmitterStages RemoveEventHandler(UNiagaraSystem* System, FName EmitterName, int32 HandlerIndex, bool bSave = true);

	/**
	 * Sets a property of an event handler: ExecutionMode (EveryParticle, SpawnedParticles, SingleParticle), SpawnNumber,
	 * MaxEventsPerFrame, bRandomSpawnNumber, MinSpawnNumber, UpdateAttributeInitialValues, SourceEventName, or
	 * SourceEmitter (an emitter name of this system).
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter that owns the handler
	 * @param HandlerIndex Index into EventHandlers from GetEmitterStages
	 * @param PropertyName Property name, e.g. SpawnNumber
	 * @param Value New value as text
	 * @param bSave Save the system after the change
	 * @return The property's value after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Stages")
	static FNiagaraMcpPropertyResult SetEventHandlerProperty(UNiagaraSystem* System, FName EmitterName, int32 HandlerIndex, const FString& PropertyName, const FString& Value, bool bSave = true);

	// ---------------------------------------------------------------- User parameters

	/**
	 * Removes a user parameter. Module inputs linked to it are reset and renderer bindings to it fall back to their
	 * defaults (the user parameter panel's delete).
	 *
	 * @param System The Niagara System
	 * @param ParameterName Name with or without the User. prefix
	 * @param bSave Save the system after the change
	 * @return The remaining user parameters
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|System")
	static TArray<FNiagaraMcpParameterInfo> RemoveUserParameter(UNiagaraSystem* System, FName ParameterName, bool bSave = true);

	/**
	 * Renames a user parameter and updates everything that references it (linked module inputs, map get nodes,
	 * renderer bindings), like renaming it in the user parameter panel.
	 *
	 * @param System The Niagara System
	 * @param ParameterName Current name, with or without the User. prefix
	 * @param NewName New name, with or without the User. prefix
	 * @param bSave Save the system after the change
	 * @return The renamed parameter
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|System")
	static FNiagaraMcpParameterInfo RenameUserParameter(UNiagaraSystem* System, FName ParameterName, FName NewName, bool bSave = true);

	/**
	 * Sets a property of the data interface held by a user parameter, e.g. a texture DI's Texture
	 * ("/Game/Tex/T_Noise"), an array DI's values (FloatData = "(1.0,2.5,4.0)"), a curve DI's settings.
	 *
	 * @param System The Niagara System
	 * @param ParameterName User parameter holding a data interface, with or without the User. prefix
	 * @param PropertyName Data interface property (dotted path for nested fields)
	 * @param Value New value as text; asset properties take an asset path
	 * @param bSave Save the system after the change
	 * @return The property's value after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|System")
	static FNiagaraMcpPropertyResult SetUserDataInterfaceProperty(UNiagaraSystem* System, FName ParameterName, const FString& PropertyName, const FString& Value, bool bSave = true);

	// ---------------------------------------------------------------- Graph helpers and discovery

	/**
	 * Sets a function specifier on a data interface function call node of a script graph. Some functions need one to
	 * work, e.g. Particle Attribute Reader's GetFloatByIndex needs Attribute=Age (without it the call silently returns 0).
	 * Call ApplyScriptChanges afterwards.
	 *
	 * @param Script The Niagara script (its asset editor must be closed)
	 * @param NodeId Function call node from GetScriptGraph
	 * @param Specifier Specifier name, e.g. Attribute
	 * @param Value Specifier value, e.g. Age or Position
	 * @return The node's specifiers after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Graph")
	static FNiagaraMcpFunctionSpecifierInfo SetFunctionSpecifier(UNiagaraScript* Script, const FString& NodeId, FName Specifier, FName Value);

	/**
	 * Searches dynamic input scripts (engine and project) by name, description or category, optionally by output type.
	 * Use the result in SetModuleInput as Value "Dynamic:<Path>".
	 *
	 * @param Filter Words to match, e.g. "random range" or "curve"; "Any" matches everything
	 * @param OutputType Only dynamic inputs producing this type: float, int, bool, vec2, vec3, vec4, color (LinearColor), position, quat or a Niagara type name; "Any" for any type
	 * @param MaxResults Maximum number of results
	 * @return Matching dynamic inputs
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Inspection")
	static TArray<FNiagaraMcpDynamicInputInfo> FindDynamicInputs(const FString& Filter = TEXT("Any"), const FString& OutputType = TEXT("Any"), int32 MaxResults = 40);

	// ---------------------------------------------------------------- Specs, diffs and particle data

	/**
	 * Exports one compact JSON snapshot of a system: system properties that differ from the defaults, user parameters,
	 * and per emitter its enabled state, non-default properties, every stack's modules (script, enabled, overridden
	 * inputs in SetModuleInput syntax), renderers (class, non-default properties, bindings, material), simulation
	 * stages and event handlers. Use it to review a whole effect in one call or to compare effects.
	 *
	 * @param System The Niagara System
	 * @return The spec as a JSON string
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Inspection")
	static FNiagaraMcpSystemSpec ExportSystemSpec(UNiagaraSystem* System);

	/**
	 * Compares two systems through their specs (see ExportSystemSpec) and lists every difference as {path, a, b}, e.g.
	 * a changed module input, a module only one of them has, a different renderer material.
	 *
	 * @param SystemA First Niagara System
	 * @param SystemB Second Niagara System
	 * @return Differences
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Inspection")
	static FNiagaraMcpSpecDiff DiffSystems(UNiagaraSystem* SystemA, UNiagaraSystem* SystemB);

	/**
	 * Simulates the system off-screen (like GetParticleCounts) and reads particle attributes at each time: per emitter and
	 * attribute the count, min, max, mean and a few sample values. Use it to check motion, sizes, colors and lifetimes
	 * numerically. CPU emitters are read directly; GPU and lightweight (stateless) emitters are read back from the GPU
	 * through a transient sim cache captured during the same run (same output; Note says so).
	 *
	 * @param System The Niagara System
	 * @param Times Seconds since activation, e.g. [0.2, 0.6, 1.0]; empty uses 0.25, 0.5, 1, 2
	 * @param Attributes Attribute names with or without Particles., e.g. ["Position", "Velocity", "Color", "SpriteSize", "Age", "NormalizedAge"]; empty uses those six
	 * @param EmitterName Only this emitter; None for all
	 * @param MaxSamples Sample values per attribute (0-64)
	 * @return One sample per time
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Validation")
	static TArray<FNiagaraMcpAttributeSample> GetParticleAttributes(UNiagaraSystem* System, const TArray<float>& Times, const TArray<FString>& Attributes, FName EmitterName = NAME_None, int32 MaxSamples = 8);

	// ==== v0.4 Niagara extensions: end ====
	// ==== v0.5 INFRA: begin ====
	/**
	 * What changed since a cursor: editor transactions (with the packages they touched), undo/redo and saves, tagged
	 * bByAgent. Call it at the start of each turn (SinceCursor = the previous NextCursor) to notice edits the artist made
	 * in the editor between your calls, and re-read those assets before changing them so you don't overwrite their work.
	 *
	 * @param SinceCursor 0 for everything still in the journal (last 1000 entries), else a previous NextCursor
	 * @param bIncludeAgentChanges Also list changes made through these tools
	 * @param Asset Only entries touching this asset (path), or Any
	 * @param MaxEntries Maximum entries returned (1-1000)
	 * @return Entries, NextCursor and the number of user (non-agent) changes
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Editor")
	static FNiagaraMcpChangeLog GetChangesSince(int64 SinceCursor = 0, bool bIncludeAgentChanges = false, const FString& Asset = TEXT("Any"), int32 MaxEntries = 100);

	/**
	 * Marks the current point of the undo history so a series of experimental edits can be reverted in one call with
	 * RollbackToRestorePoint (e.g. "try a variation, compare, revert"). Asset creation is not undoable and is not reverted.
	 *
	 * @param Name Name of the point, e.g. before_color_pass (an existing point with this name is replaced)
	 * @return The restore point
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Editor")
	static FNiagaraMcpRestorePointInfo CreateRestorePoint(FName Name);

	/** Lists restore points and how many undo steps each would revert. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Editor")
	static TArray<FNiagaraMcpRestorePointInfo> ListRestorePoints();

	/**
	 * Undoes every transaction made after the restore point. Refuses when the history was undone/cleared in between,
	 * during Play In Editor, or when the user edited something in the editor in between (unless bAllowUserChanges).
	 *
	 * @param Name Restore point name
	 * @param bAllowUserChanges Also undo edits the user made in the editor after the point
	 * @return What was undone
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Editor")
	static FNiagaraMcpRollbackResult RollbackToRestorePoint(FName Name, bool bAllowUserChanges = false);

	/**
	 * Renders the editor thumbnails of up to 16 assets into one image (4 per row, in the given order) so you can pick
	 * between candidates (textures, materials, meshes, systems) before using one. Prefer the top-level MCP tool
	 * asset_thumbnails, which returns a real image.
	 *
	 * @param AssetPaths Asset paths, e.g. from FindAssets
	 * @param Size Tile size in pixels (32-512)
	 * @return Contact sheet image
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Editor")
	static FToolsetImage PreviewAssets(const TArray<FString>& AssetPaths, int32 Size = 128);

	/**
	 * Lists the engine's Niagara templates (emitters and systems under /Niagara/DefaultAssets/Templates) with their
	 * descriptions: good starting points for AddEmitter (TemplateEmitter) or DuplicateAsset.
	 *
	 * @param Kind Emitter, System or Any
	 * @param Filter Words to match in name, folder or description, e.g. "fountain" or "ribbon"; Any for all
	 * @return Templates
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Editor")
	static TArray<FNiagaraMcpTemplateInfo> ListNiagaraTemplates(const FString& Kind = TEXT("Any"), const FString& Filter = TEXT("Any"));
	// ==== v0.5 INFRA: end ====
	// ==== v0.5 VISION: begin ====

	/**
	 * Visual before/after check. The first call with a new BaselineName renders the system (like CapturePreview)
	 * and stores the pixels in memory together with the camera and framing. Later calls with the same name
	 * re-render with exactly the stored settings (the arguments after BaselineName are then ignored) and report per
	 * tile how many pixels changed, the mean color change and coverage before/after; the image is the new render
	 * with changed pixels tinted red. Typical use: ComparePreview(S, "before_color", [0.5, 1.0]) -> edit the
	 * system -> ComparePreview(S, "before_color", [0.5, 1.0]). Up to 8 baselines are kept (least recently used
	 * dropped); nothing is saved to disk.
	 * Emitters without Determinism re-randomize on every render, so a few percent of changed pixels can be
	 * noise; compare BlockChangedPercent / MeanColorDelta, or enable Determinism on the emitters for exact diffs.
	 *
	 * @param System The Niagara System
	 * @param BaselineName Name of the baseline, e.g. "before_color"
	 * @param Times Seconds since activation for each frame, e.g. [0.5, 1.0]; empty uses 0.25, 0.5, 1, 2 (only used when the baseline is created)
	 * @param FrameSize Pixel size of each square frame (64-1024)
	 * @param CameraYaw Camera yaw in degrees
	 * @param CameraPitch Camera pitch in degrees; negative looks down
	 * @param CameraDistance Distance from the effect's center; 0 frames its bounds automatically
	 * @param Background Black, Grey or White
	 * @param CameraYaws Several yaws as text, e.g. "0, 90"; None uses CameraYaw
	 * @param QualityLevel Current, Low, Medium, High, Epic, Cinematic or 0-4
	 * @param bResetBaseline True replaces an existing baseline of that name with a fresh render
	 * @return Baseline state, image, per tile differences and a caption
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Preview")
	static FNiagaraMcpPreviewDiff ComparePreview(UNiagaraSystem* System, const FString& BaselineName, const TArray<float>& Times, int32 FrameSize = 256, float CameraYaw = 0.0f, float CameraPitch = -15.0f, float CameraDistance = 0.0f, const FString& Background = TEXT("Black"), const FString& CameraYaws = TEXT("None"), const FString& QualityLevel = TEXT("Current"), bool bResetBaseline = false);

	/**
	 * Renders the system off-screen in the ShaderComplexity view mode and measures it: each pixel is colored by the
	 * pixel shader cost of everything drawn on it (green cheap -> red expensive -> pink / white extreme); for
	 * particles that is mostly overdraw of large translucent sprites. Empty space is the first (green) ramp color.
	 * Returns the image plus, per tile, coverage, the share of covered pixels in the red-or-worse band, the share in
	 * the pink / white band and the mean cost index (0-1). Fix high numbers with fewer / smaller particles, cheaper
	 * materials, cutout sprites or fewer overlapping layers. (QuadOverdraw cannot be rendered off-screen.)
	 *
	 * @param System The Niagara System
	 * @param Times Seconds since activation for each frame, e.g. [0.5, 1.0]; empty uses 0.25, 0.5, 1, 2 (max 12)
	 * @param Mode ShaderComplexity (the only mode available off-screen)
	 * @param FrameSize Pixel size of each square frame (64-1024)
	 * @param CameraYaw Camera yaw in degrees
	 * @param CameraPitch Camera pitch in degrees; negative looks down
	 * @param CameraDistance Distance from the effect's center; 0 frames its bounds automatically
	 * @param CameraYaws Several yaws as text, e.g. "0, 90"; None uses CameraYaw
	 * @param QualityLevel Current, Low, Medium, High, Epic, Cinematic or 0-4
	 * @return Mode, image, numbers per tile and a caption
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Preview")
	static FNiagaraMcpCostResult CaptureCostView(UNiagaraSystem* System, const TArray<float>& Times, const FString& Mode = TEXT("ShaderComplexity"), int32 FrameSize = 320, float CameraYaw = 0.0f, float CameraPitch = -15.0f, float CameraDistance = 0.0f, const FString& CameraYaws = TEXT("None"), const FString& QualityLevel = TEXT("Current"));

	/**
	 * Bakes the effect into a flipbook (SubUV) texture: renders Columns x Rows frames of ONE continuous simulation
	 * run with a fixed camera that frames the union of the bounds of all frames, and packs them into one new texture
	 * asset (frame i at column i % Columns, row i / Columns, left to right, top to bottom). Frame i is at
	 * StartTime + i * (EndTime - StartTime) / (Columns * Rows), so the end time itself is excluded and a looping
	 * effect loops seamlessly. Use the result on a sprite renderer with SubImageSize = (Columns, Rows) and a
	 * SubUV Animation module (or Particles.SubImageIndex 0..Columns*Rows-1).
	 * Alpha: with Background Black the alpha channel is the brightest color channel of each pixel (so black is
	 * transparent; RGB is the render over black, i.e. premultiplied - use additive or premultiplied blending);
	 * with Grey or White the texture is opaque.
	 *
	 * @param System The Niagara System
	 * @param NewTextureAssetPath Path of the new texture, e.g. /Game/FX/Textures/T_Explosion_SubUV
	 * @param Columns Frames per row (1-16)
	 * @param Rows Rows of frames (1-16)
	 * @param StartTime Simulation time of the first frame in seconds
	 * @param EndTime End of the baked range in seconds (> StartTime)
	 * @param FrameSize Pixel size of each square frame (32-1024; the atlas must stay within 8192 px)
	 * @param Background Black (alpha from brightness), Grey or White (opaque)
	 * @param CameraYaw Camera yaw in degrees
	 * @param CameraPitch Camera pitch in degrees (0 = side view, the usual choice for camera-facing sprites)
	 * @param bSave Save the new texture
	 * @return Asset path, atlas size, frame layout and times, per frame coverage and usage notes
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Preview")
	static FNiagaraMcpBakeResult BakeFlipbook(UNiagaraSystem* System, const FString& NewTextureAssetPath, int32 Columns, int32 Rows, float StartTime, float EndTime, int32 FrameSize = 128, const FString& Background = TEXT("Black"), float CameraYaw = 0.0f, float CameraPitch = 0.0f, bool bSave = true);

	// ==== v0.5 VISION: end ====
	// ==== v0.5 ANALYSIS: begin ====

	/**
	 * "Why don't I see anything?" Runs an ordered checklist per emitter and stops at the first failure, with evidence and the
	 * exact next tool call to fix it. Checks in order: compile errors; emitter enabled; scalability (emitter platform set at the
	 * current effects quality, effect type culling); a spawn module present; particle count at Time (off-screen continuous run);
	 * lifetime > 0; renderers present and enabled, renderer feedback errors, sim target supported; renderer materials set,
	 * compiled, Niagara usage flag for the renderer type (bUsedWithNiagaraSprites / Ribbons / MeshParticles), blend mode;
	 * Color alpha, SpriteSize / Scale near zero; GPU fixed bounds vs the particles' actual extent; local / world space.
	 * Read the first Fail (FirstFail / FirstFailCheck), apply its Fix, run again.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Only this emitter; None checks every emitter
	 * @param Time Seconds since activation at which particles are measured, e.g. 0.5 (use a time where the effect should be visible)
	 * @return Ordered checklist with the first failure
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Validation")
	static FNiagaraMcpVisibilityReport DiagnoseVisibility(UNiagaraSystem* System, FName EmitterName = NAME_None, float Time = 0.5f);

	/**
	 * Performance overview of a system: per emitter and script the compile status, VectorVM op count, temp registers and byte
	 * code size (CPU scripts), HLSL size, the simulation target, particle counts at each time (one continuous off-screen run),
	 * game-thread milliseconds per simulation step, and for GPU emitters per-stage GPU timings from the Niagara GPU profiler when
	 * it delivers results (otherwise a note). Ranking lists the emitters by estimated CPU cost (ops x particles).
	 *
	 * @param System The Niagara System
	 * @param Times Seconds since activation to measure at, e.g. [0.5, 1, 2]; empty uses 0.5, 1, 2
	 * @param EmitterName Only this emitter; None for all
	 * @return Cost report
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Validation")
	static FNiagaraMcpCostReport AnalyzeCost(UNiagaraSystem* System, const TArray<float>& Times, FName EmitterName = NAME_None);

	/**
	 * Returns the HLSL the Niagara compiler generated for a script, paged: read Length characters from Offset and call again
	 * with Offset + Length while bTruncated. CPU scripts (VectorVM) keep their HLSL only after a compile in this editor session:
	 * when it is missing the system is recompiled once (forced) to regenerate it. GPU emitters: Script ParticleGPUCompute (or
	 * any particle script name) with Target GPU returns the combined compute shader.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter; None (or System) for the system scripts (Script SystemSpawn / SystemUpdate)
	 * @param Script ParticleSpawn, ParticleUpdate, EmitterSpawn, EmitterUpdate, SystemSpawn, SystemUpdate or ParticleGPUCompute
	 * @param Target CPU (VectorVM HLSL) or GPU (compute shader HLSL of a GPU emitter)
	 * @param Offset First character to return
	 * @param MaxLength Characters to return (1-200000), e.g. 20000
	 * @return The HLSL window with the total length
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Inspection")
	static FNiagaraMcpHlslResult GetCompiledHlsl(UNiagaraSystem* System, FName EmitterName, const FString& Script = TEXT("ParticleUpdate"), const FString& Target = TEXT("CPU"), int32 Offset = 0, int32 MaxLength = 20000);

	/**
	 * Applies a spec JSON (the format ExportSystemSpec returns) to a system: the inverse of ExportSystemSpec. Applies system
	 * properties, user parameters, emitters (missing emitters are added from the Minimal template), emitter enabled state,
	 * sim target and properties, every stack's modules (missing modules are added by script path at the spec's index,
	 * enabled state, overridden inputs incl. dynamic input sub-inputs "Parent/Child"), renderers (by index and class:
	 * properties, attribute bindings, material bindings), simulation stages and their properties, and event handlers.
	 * Merge keeps emitters / modules / renderers that the spec does not mention; Replace removes them (and resets inputs
	 * the spec does not override). Typical round trip: ExportSystemSpec(A) -> CreateSystem(B) -> ApplySystemSpec(B, spec)
	 * -> DiffSystems(A, B). Each step is one undo transaction. Items that cannot be applied (scratch pad modules, truncated
	 * values, unknown classes) are listed as Skipped with the reason; nothing stops at the first problem.
	 *
	 * @param System Target Niagara System
	 * @param SpecJson The spec string (FNiagaraMcpSystemSpec.spec from ExportSystemSpec), possibly edited
	 * @param Mode Merge (default) or Replace
	 * @param bDryRun Only list what would change (Status WouldApply); nothing is modified
	 * @param bSave Save the system at the end
	 * @return Every operation with its status
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|System")
	static FNiagaraMcpApplySpecResult ApplySystemSpec(UNiagaraSystem* System, const FString& SpecJson, const FString& Mode = TEXT("Merge"), bool bDryRun = false, bool bSave = true);

	/**
	 * Saves an emitter of a system as a standalone, reusable emitter asset (like "Create Asset from this Emitter" in the
	 * system editor, without dialogs). The new asset is not linked to the system; use it as TemplateEmitter in AddEmitter.
	 * Emitters that use system scratch pad modules keep references into the system and are refused.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Emitter to save
	 * @param NewAssetPath Path of the new emitter asset, e.g. /Game/FX/Emitters/NE_Sparks (must not exist)
	 * @param bSave Save the new asset
	 * @return The new asset
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|System")
	static FNiagaraMcpAssetResult SaveEmitterAsAsset(UNiagaraSystem* System, FName EmitterName, const FString& NewAssetPath, bool bSave = true);

	/**
	 * Adds a lightweight (stateless) emitter: a cheap GPU emitter with a fixed module list (Initialize Particle, Shape Location,
	 * Scale Color, Add Velocity, ...) and no scripts. Copied from an engine lightweight template system. Edit it with
	 * GetStatelessEmitterInfo / SetStatelessModuleProperty; stack tools (AddModule, SetModuleInput) do not apply to it.
	 *
	 * @param System The Niagara System
	 * @param EmitterName Name for the new emitter (made unique if taken)
	 * @param Template MinimalLightweight (default), FountainLightweight, DirectionalBurstLightweight, or a system asset path whose first lightweight emitter is copied
	 * @param bSave Save the system after the change
	 * @return The new emitter
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Lightweight")
	static FNiagaraMcpEmitterInfo AddLightweightEmitter(UNiagaraSystem* System, FName EmitterName, const FString& Template = TEXT("MinimalLightweight"), bool bSave = true);

	/**
	 * Describes a lightweight (stateless) emitter: its modules with enabled state and editable properties (current values),
	 * emitter-level properties (fixed bounds, random seed, spawn infos) and renderers.
	 *
	 * @param System The Niagara System
	 * @param EmitterName A lightweight emitter
	 * @return Modules and properties
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|Lightweight")
	static FNiagaraMcpStatelessEmitterInfo GetStatelessEmitterInfo(UNiagaraSystem* System, FName EmitterName);

	/**
	 * Sets a property of a lightweight (stateless) emitter module, e.g. Module InitializeParticle, PropertyName
	 * LifetimeDistribution.Min, Value 2; Module ScaleColor, bModuleEnabled true; Module Emitter (the emitter itself),
	 * PropertyName FixedBounds, Value "(Min=(X=-200,Y=-200,Z=-200),Max=(X=200,Y=200,Z=200),IsValid=True)". Values use the
	 * same text forms as SetEmitterProperty (numbers, "1, 0, 0, 1", enum names, UE struct text). Distribution structs
	 * (FNiagaraDistributionRange*) take Mode (UniformConstant, NonUniformConstant, UniformRange, NonUniformRange, ...) plus
	 * Min / Max (LifetimeDistribution.Min = 2); color distributions take Values (ColorDistribution.Values = "((R=0,G=1,B=0,A=1))").
	 *
	 * @param System The Niagara System
	 * @param EmitterName A lightweight emitter
	 * @param Module Module name (InitializeParticle, "Initialize Particle", class name) or Emitter
	 * @param PropertyName Property (dotted path for struct fields)
	 * @param Value New value as text
	 * @param bSave Save the system after the change
	 * @return The property after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|Lightweight")
	static FNiagaraMcpPropertyResult SetStatelessModuleProperty(UNiagaraSystem* System, FName EmitterName, const FString& Module, const FString& PropertyName, const FString& Value, bool bSave = true);

	/**
	 * Lists where user parameters are used: module inputs linked to them (emitter / stack / module / input), Map Get reads in
	 * scratch pad and system / emitter graphs, renderer attribute and material bindings, simulation stage bindings, data
	 * interface user parameter bindings and emitter property bindings. bUnused flags parameters that affect nothing.
	 *
	 * @param System The Niagara System
	 * @param ParameterName One user parameter (with or without User.); None for all
	 * @return One entry per user parameter
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|System")
	static TArray<FNiagaraMcpParameterUsage> GetUserParameterUsage(UNiagaraSystem* System, FName ParameterName = NAME_None);

	/**
	 * Creates a Niagara Data Channel asset (a typed, shared particle data stream that systems write to and read from, e.g.
	 * impact points written by gameplay and read by an effect). Hook it up to a system with a user parameter of data
	 * interface type: SetUserParameter(System, "ImpactWriter", "NiagaraDataInterfaceDataChannelWrite") then
	 * SetUserDataInterfaceProperty(System, "ImpactWriter", "Channel", "<channel path>") (Read: NiagaraDataInterfaceDataChannelRead).
	 *
	 * @param AssetPath Path of the new asset, e.g. /Game/FX/DataChannels/NDC_Impacts
	 * @param Variables Channel variables, e.g. [{"name": "Position", "type": "position"}, {"name": "Energy", "type": "float"}, {"name": "Color", "type": "color"}]
	 * @param Type Islands (default; spatially partitioned, per-island systems), Global (one shared buffer) or GameplayBurst
	 * @param bSave Save the new asset
	 * @return The new data channel
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|DataChannels")
	static FNiagaraMcpDataChannelInfo CreateNiagaraDataChannel(const FString& AssetPath, const TArray<FNiagaraMcpVariableDecl>& Variables, const FString& Type = TEXT("Islands"), bool bSave = true);

	/**
	 * Describes a Niagara Data Channel asset: channel type, variables and settings.
	 *
	 * @param DataChannel The data channel asset
	 * @return Type, variables and settings
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Niagara|DataChannels")
	static FNiagaraMcpDataChannelInfo GetNiagaraDataChannel(UNiagaraDataChannelAsset* DataChannel);

	/**
	 * Recolors a whole effect: rotates the hue (HueShift) and/or multiplies by a tint color (Tint) of every color in it:
	 * local LinearColor module inputs (e.g. Initialize Particle Color), Color From Curve keys (the gradient is kept, every key
	 * recolored), and user color parameters. Saturation, brightness and alpha are kept by HueShift. Material colors are only
	 * reported (shared materials are never modified); change them with SetMaterialInstanceParameter if needed.
	 * Example: HueShiftDegrees 120 turns orange fire into green.
	 *
	 * @param System The Niagara System
	 * @param Mode HueShift (rotate hue by HueShiftDegrees, then apply Tint if set) or Tint (multiply by Tint only)
	 * @param HueShiftDegrees Hue rotation in degrees, e.g. 120 (orange -> green), -60, 180 (complement)
	 * @param Tint RGB multiplier "r, g, b" (linear), e.g. "1, 0.6, 0.6"; None for no tint
	 * @param EmitterName Only this emitter (user parameters are skipped then); None for the whole effect
	 * @param bSave Save the system after the change
	 * @return Every color changed with before / after values
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Niagara|System")
	static FNiagaraMcpRecolorResult RecolorEffect(UNiagaraSystem* System, const FString& Mode = TEXT("HueShift"), float HueShiftDegrees = 0.0f, const FString& Tint = TEXT("None"), FName EmitterName = NAME_None, bool bSave = true);

	// ==== v0.5 ANALYSIS: end ====
};
