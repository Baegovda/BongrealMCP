#pragma once

#include "CoreMinimal.h"
#include "NiagaraMcpEditorTools.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "ToolsetRegistry/ToolsetImage.h"
#include "MaterialMcpEditorTools.generated.h"

class UMaterial;
class UMaterialInterface;
class UMaterialInstanceConstant;

/** A material expression node. */
USTRUCT()
struct FMaterialMcpExpressionInfo
{
	GENERATED_BODY()

	/** Expression object name; use as Expression in the other tools. Parameter names, FunctionInput / FunctionOutput names also work. */
	UPROPERTY()
	FString Name;

	/** Expression class without the MaterialExpression prefix, e.g. Custom, ScalarParameter, FunctionInput, MaterialFunctionCall. */
	UPROPERTY()
	FString Class;

	/** Parameter name, FunctionInput / FunctionOutput name, or the node description. */
	UPROPERTY()
	FString Label;

	UPROPERTY()
	int32 PosX = 0;

	UPROPERTY()
	int32 PosY = 0;

	/** "<InputName>" or "<InputName> <- <Expression>:<Output>" when connected. MaterialFunctionCall nodes list the function's inputs. */
	UPROPERTY()
	TArray<FString> Inputs;

	/** Output names; an empty first output is listed as "". MaterialFunctionCall nodes list the function's outputs. */
	UPROPERTY()
	TArray<FString> Outputs;

	/** HLSL code of Custom expressions. */
	UPROPERTY()
	FString Code;

	/** MaterialFunctionCall nodes: path of the called material function (empty when none is set). */
	UPROPERTY()
	FString MaterialFunction;
};

/** A material or material function graph. */
USTRUCT()
struct FMaterialMcpGraphInfo
{
	GENERATED_BODY()

	/** Path of the material or material function. */
	UPROPERTY()
	FString MaterialPath;

	/** Material or MaterialFunction. */
	UPROPERTY()
	FString AssetType;

	/** Materials only. */
	UPROPERTY()
	FString MaterialDomain;

	/** Materials only. */
	UPROPERTY()
	FString BlendMode;

	/** Materials only. */
	UPROPERTY()
	FString ShadingModel;

	/** Materials only. */
	UPROPERTY()
	bool bTwoSided = false;

	/** Material functions: Default, MaterialLayer or MaterialLayerBlend. */
	UPROPERTY()
	FString FunctionUsage;

	/** Material functions: the description shown as tooltip where the function is used. */
	UPROPERTY()
	FString Description;

	/** Material functions: "<Name> (<Type>)" of each FunctionInput in pin order (SortPriority). */
	UPROPERTY()
	TArray<FString> FunctionInputs;

	/** Material functions: name of each FunctionOutput in pin order (SortPriority). */
	UPROPERTY()
	TArray<FString> FunctionOutputs;

	/** Materials: "<MaterialProperty> <- <Expression>:<Output>" for every connected material output. */
	UPROPERTY()
	TArray<FString> PropertyConnections;

	UPROPERTY()
	TArray<FMaterialMcpExpressionInfo> Expressions;

	/** Expressions matching the filter (all pages). */
	UPROPERTY()
	int32 TotalExpressions = 0;

	/** Offset of the next page, -1 when this is the last. */
	UPROPERTY()
	int32 NextOffset = -1;
};

/** Result of compiling a material or material function. */
USTRUCT()
struct FMaterialMcpCompileResult
{
	GENERATED_BODY()

	UPROPERTY()
	FString MaterialPath;

	/** No shader compile errors on the editor's shader platform. */
	UPROPERTY()
	bool bSucceeded = false;

	/** Compile errors. For a material function: errors of the loaded materials that use it, prefixed with the material name. */
	UPROPERTY()
	TArray<FString> Errors;

	/** Material functions: loaded materials that use the function and were recompiled. */
	UPROPERTY()
	TArray<FString> DependentMaterials;

	UPROPERTY()
	bool bSaved = false;
};

/** One problem found by ValidateMaterial. */
USTRUCT()
struct FMaterialMcpIssue
{
	GENERATED_BODY()

	/** Error, Warning or Info. */
	UPROPERTY()
	FString Severity;

	UPROPERTY()
	FString Message;

	/** Expression the issue is about, if any. */
	UPROPERTY()
	FString Expression;
};

/** Result of ValidateMaterial. */
USTRUCT()
struct FMaterialMcpValidation
{
	GENERATED_BODY()

	UPROPERTY()
	FString MaterialPath;

	/** No issue with severity Error. */
	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	TArray<FMaterialMcpIssue> Issues;
};

/** Shader cost of a material (editor shader platform). */
USTRUCT()
struct FMaterialMcpStats
{
	GENERATED_BODY()

	UPROPERTY()
	FString MaterialPath;

	UPROPERTY()
	int32 PixelShaderInstructions = 0;

	UPROPERTY()
	int32 VertexShaderInstructions = 0;

	UPROPERTY()
	int32 Samplers = 0;

	UPROPERTY()
	int32 PixelTextureSamples = 0;

	UPROPERTY()
	int32 VertexTextureSamples = 0;

	UPROPERTY()
	int32 VirtualTextureSamples = 0;

	UPROPERTY()
	int32 InterpolatorScalars = 0;
};

/** A material parameter and its value. */
USTRUCT()
struct FMaterialMcpParameter
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	/** Scalar, Vector, Texture or StaticSwitch. */
	UPROPERTY()
	FString Type;

	/** "0.5", "1, 0.2, 0.1, 1", an asset path or true/false. */
	UPROPERTY()
	FString Value;

	/** Material instances: the instance overrides this parameter (false = inherited from the parent). */
	UPROPERTY()
	bool bOverridden = false;
};

/** A parameter value to set on a material instance. */
USTRUCT()
struct FMaterialMcpParameterValue
{
	GENERATED_BODY()

	/** Parameter name, e.g. Tint. */
	UPROPERTY()
	FString Name;

	/** Scalar "0.2", vector "1, 0.3, 0.1, 1", texture asset path, or static switch true/false. */
	UPROPERTY()
	FString Value;
};

/** A material or material instance with its parameters. */
USTRUCT()
struct FMaterialMcpParameters
{
	GENERATED_BODY()

	UPROPERTY()
	FString Path;

	/** Parent material (instances only). */
	UPROPERTY()
	FString Parent;

	UPROPERTY()
	TArray<FMaterialMcpParameter> Parameters;

	UPROPERTY()
	bool bSaved = false;
};

/**
 * Material graph authoring for AI agents (used for Niagara renderer materials and general effects).
 *
 * Material workflow: CreateMaterial -> SetMaterialProperty (MaterialDomain, BlendMode, ShadingModel, TwoSided, usage
 * flags) -> AddExpression (ScalarParameter, VectorParameter, TextureObjectParameter, TextureCoordinate, Time, Custom,
 * MaterialFunctionCall, ...) -> SetExpressionProperty / SetCustomExpression -> ConnectExpressions -> ConnectToMaterialProperty
 * -> LayoutMaterial -> CompileMaterial -> ValidateMaterial -> CapturePreview (FrameCount > 1 shows animation over time).
 * DisconnectExpressions / DisconnectMaterialProperty / RemoveExpression undo wiring; PromoteToParameter turns a constant
 * or texture node into a parameter.
 *
 * Material functions (reusable sub-graphs such as a flipbook or radial mask): CreateMaterialFunction, then the same graph
 * tools (pass the function as Material): add FunctionInput nodes (InputName, InputType, SortPriority, Preview pin) and
 * FunctionOutput nodes (OutputName), wire them, CompileMaterial. Use a function from a material with
 * AddExpression("MaterialFunctionCall") + SetExpressionProperty(..., "MaterialFunction", "/Game/.../MF_X").
 *
 * Material instances: CreateMaterialInstance, SetMaterialInstanceParameters, SetMaterialInstanceParent,
 * ClearMaterialInstanceParameters, GetMaterialParameters. The asset's editor must be closed while editing.
 */
UCLASS(BlueprintType, Hidden)
class UMaterialMcpEditorTools : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Creates an empty material asset.
	 *
	 * @param AssetPath New asset path, e.g. /Game/TestSample/M_ChainRibbon
	 * @param bSave Save the new asset
	 * @return The new material graph
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Material")
	static FMaterialMcpGraphInfo CreateMaterial(const FString& AssetPath, bool bSave = true);

	/**
	 * Creates a material function asset: a reusable sub-graph that materials call through a MaterialFunctionCall node.
	 * A Default function starts with one FunctionOutput named "Result"; a MaterialLayer starts with a "Material
	 * Attributes" input and a layer output; a MaterialLayerBlend starts with "Top Layer" / "Bottom Layer" inputs and a
	 * layer output. Add FunctionInput / FunctionOutput nodes with AddExpression and edit the graph with the other tools.
	 *
	 * @param AssetPath New asset path, e.g. /Game/FX/Functions/MF_Flipbook
	 * @param Description Tooltip shown where the function is used, e.g. "Samples a flipbook frame from UV and Time"; None for no description
	 * @param Usage Default, MaterialLayer or MaterialLayerBlend
	 * @param bExposeToLibrary List the function in the material editor's function library
	 * @param bSave Save the new asset
	 * @return The new function graph
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Material|Functions")
	static FMaterialMcpGraphInfo CreateMaterialFunction(const FString& AssetPath, const FString& Description = TEXT("None"), const FString& Usage = TEXT("Default"), bool bExposeToLibrary = true, bool bSave = true);

	/**
	 * Reads a material or material function graph: settings, expressions, their connections, the material output
	 * connections (materials) or the function inputs / outputs (functions).
	 *
	 * @param Material Material or material function, e.g. /Game/FX/M_Fire or /Game/FX/Functions/MF_Flipbook
	 * @param ClassFilter Only expressions whose class, name or label contains these words (e.g. "Parameter" or "Custom"), Any for all
	 * @param Offset First expression to return (paging; see NextOffset)
	 * @param MaxExpressions Expressions per page (1-500)
	 * @return The graph
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Material")
	static FMaterialMcpGraphInfo GetMaterialGraph(UObject* Material, const FString& ClassFilter = TEXT("Any"), int32 Offset = 0, int32 MaxExpressions = 200);

	/**
	 * Sets a material property, e.g. MaterialDomain = Surface, BlendMode = Masked, ShadingModel = Unlit,
	 * TwoSided = true, bUsedWithNiagaraRibbons = true, OpacityMaskClipValue = 0.333. On a material function:
	 * Description, UserExposedCaption, bExposeToLibrary, LibraryCategoriesText.
	 *
	 * @param Material Material or material function
	 * @param PropertyName Material (or function) property
	 * @param Value New value as text (enum entries by name or display name)
	 * @param bSave Save the asset after the change
	 * @return The property's value after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Material")
	static FNiagaraMcpPropertyResult SetMaterialProperty(UObject* Material, const FString& PropertyName, const FString& Value, bool bSave = true);

	/**
	 * Adds an expression node to a material or material function.
	 *
	 * @param Material Material or material function
	 * @param ExpressionClass Class name without the MaterialExpression prefix (Custom, ScalarParameter, VectorParameter,
	 *        TextureObjectParameter, TextureSampleParameter2D, TextureCoordinate, Time, Multiply, Add, Lerp,
	 *        MaterialFunctionCall; FunctionInput / FunctionOutput in material functions only) or a class path
	 * @param PosX Graph X position
	 * @param PosY Graph Y position
	 * @return The new expression (FunctionInput / FunctionOutput get a unique name, e.g. In, In_2)
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Material")
	static FMaterialMcpExpressionInfo AddExpression(UObject* Material, const FString& ExpressionClass, int32 PosX = 0, int32 PosY = 0);

	/**
	 * Sets a property of an expression, e.g. ParameterName, DefaultValue, Texture, Group, SortPriority, Desc,
	 * CoordinateIndex, ConstA. Asset properties take an asset path. Special cases: MaterialFunctionCall
	 * "MaterialFunction" = "/Game/FX/Functions/MF_Flipbook" (rebuilds the node's pins from the function);
	 * FunctionInput InputName / InputType (Scalar, Vector2, Vector3, Vector4, Texture2D, TextureCube, StaticBool,
	 * MaterialAttributes, Bool) / Description / SortPriority / PreviewValue / bUsePreviewValueAsDefault;
	 * FunctionOutput OutputName / Description / SortPriority. Input and output names must be unique.
	 *
	 * @param Material Material or material function
	 * @param Expression Expression name, parameter name or FunctionInput / FunctionOutput name
	 * @param PropertyName Expression property (dotted path for nested fields)
	 * @param Value New value as text
	 * @return The expression after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Material")
	static FMaterialMcpExpressionInfo SetExpressionProperty(UObject* Material, const FString& Expression, const FString& PropertyName, const FString& Value);

	/**
	 * Sets the code, description, inputs and output type of a Custom expression. Inputs keep their connections
	 * when the name is unchanged. Texture object inputs are sampled in code with Texture2DSample(Name, NameSampler, UV).
	 *
	 * @param Material Material or material function
	 * @param Expression Custom expression name
	 * @param Code HLSL function body (must return a value)
	 * @param Description Node title; None keeps the current title
	 * @param InputNames Input pin names in order
	 * @param OutputType Float1, Float2, Float3, Float4 or MaterialAttributes; None keeps the current type
	 * @return The expression after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Material")
	static FMaterialMcpExpressionInfo SetCustomExpression(UObject* Material, const FString& Expression, const FString& Code, const FString& Description, const TArray<FString>& InputNames, const FString& OutputType);

	/**
	 * Connects an expression output to an input of another expression (replacing an existing link on that input).
	 *
	 * @param Material Material or material function
	 * @param FromExpression Source expression
	 * @param FromOutput Output name, e.g. RGB, R; Auto for the first output
	 * @param ToExpression Target expression
	 * @param ToInput Input name on the target, e.g. UV, Alpha, or a function input name; Auto for the first input
	 * @return The target expression after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Material")
	static FMaterialMcpExpressionInfo ConnectExpressions(UObject* Material, const FString& FromExpression, const FString& FromOutput, const FString& ToExpression, const FString& ToInput = TEXT("Auto"));

	/**
	 * Breaks the link into one input of an expression.
	 *
	 * @param Material Material or material function
	 * @param ToExpression Expression whose input is disconnected
	 * @param ToInput Input name, e.g. B, Alpha, Coordinates; Auto for the first input
	 * @return The expression after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Material")
	static FMaterialMcpExpressionInfo DisconnectExpressions(UObject* Material, const FString& ToExpression, const FString& ToInput = TEXT("Auto"));

	/**
	 * Connects an expression output to a material output (BaseColor, EmissiveColor, Opacity, OpacityMask, Normal,
	 * Roughness, Metallic, WorldPositionOffset, ...). Materials only: in a material function connect to a
	 * FunctionOutput node instead.
	 *
	 * @param Material The material
	 * @param FromExpression Source expression
	 * @param FromOutput Output name, e.g. RGB, R; Auto for the first output
	 * @param MaterialProperty Material output, e.g. EmissiveColor
	 * @return The material graph after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Material")
	static FMaterialMcpGraphInfo ConnectToMaterialProperty(UObject* Material, const FString& FromExpression, const FString& FromOutput, const FString& MaterialProperty);

	/**
	 * Breaks the link into a material output. Materials only.
	 *
	 * @param Material The material
	 * @param MaterialProperty Material output, e.g. EmissiveColor, Opacity
	 * @return The material graph after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Material")
	static FMaterialMcpGraphInfo DisconnectMaterialProperty(UObject* Material, const FString& MaterialProperty);

	/**
	 * Removes an expression and every link to it.
	 *
	 * @param Material Material or material function
	 * @param Expression Expression name or parameter name
	 * @return The graph after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Material")
	static FMaterialMcpGraphInfo RemoveExpression(UObject* Material, const FString& Expression);

	/**
	 * Replaces a constant or texture node with the matching parameter node, keeping its value, position and every
	 * link: Constant -> ScalarParameter, Constant3Vector / Constant4Vector -> VectorParameter, StaticBool ->
	 * StaticBoolParameter, TextureSample -> TextureSampleParameter2D (Cube for cube textures), TextureObject ->
	 * TextureObjectParameter.
	 *
	 * @param Material Material or material function
	 * @param Expression Expression name of the constant / texture node
	 * @param ParameterName New parameter name, e.g. Scroll Speed
	 * @param bSave Save the asset after the change
	 * @return The new parameter expression
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Material")
	static FMaterialMcpExpressionInfo PromoteToParameter(UObject* Material, const FString& Expression, FName ParameterName, bool bSave = true);

	/**
	 * Arranges the expression nodes in columns.
	 *
	 * @param Material Material or material function
	 * @return The graph after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Material")
	static FMaterialMcpGraphInfo LayoutMaterial(UObject* Material);

	/**
	 * Recompiles a material, waits for its shaders and reports compile errors. For a material function: updates the
	 * function (refreshing its inputs / outputs) and recompiles every loaded material that uses it; their errors are
	 * reported (a function on its own has no shaders).
	 *
	 * @param Material Material or material function
	 * @param bSave Save the asset after compiling
	 * @return Compile result
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "Material")
	static FMaterialMcpCompileResult CompileMaterial(UObject* Material, bool bSave = true);

	/**
	 * Renders a material (or material instance) off-screen on a preview mesh and returns the image. Plane is viewed
	 * from above so UV0 0-1 fills the frame. With FrameCount > 1 the frames are rendered at times 0, FrameInterval,
	 * 2 * FrameInterval, ... seconds into one contact sheet (left to right, top to bottom, 4 per row), so panners,
	 * flipbooks and erosion can be checked. A single frame shows time 0.
	 *
	 * @param Material The material or material instance
	 * @param Mesh Plane (default), Sphere, Cube or a static mesh path
	 * @param Size Size of each frame in pixels (64-1024)
	 * @param FrameCount Number of frames (1-16), e.g. 4
	 * @param FrameInterval Seconds between frames, e.g. 0.25
	 * @return Preview image
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Material")
	static FToolsetImage CapturePreview(UMaterialInterface* Material, const FString& Mesh = TEXT("Plane"), int32 Size = 512, int32 FrameCount = 1, float FrameInterval = 0.25f);

	/**
	 * Checks a material or material function: shader compile errors, expressions not connected to any output,
	 * duplicate parameter / function input / output names, texture parameters without a texture, Custom expressions
	 * with unconnected inputs, MaterialFunctionCall nodes without a function or with required inputs unconnected.
	 * Without bCompile the errors of the last compile are read (nothing is recompiled or modified).
	 *
	 * @param Material Material or material function
	 * @param bCompile Recompile first (marks the material dirty and updates its instances); use after graph edits
	 * @return All issues; bOk is true when there is no error
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Material")
	static FMaterialMcpValidation ValidateMaterial(UObject* Material, bool bCompile = false);

	/**
	 * Returns the shader cost of a material or instance: instruction counts, samplers and texture samples.
	 *
	 * @param Material The material or material instance
	 * @return Statistics
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Material")
	static FMaterialMcpStats GetMaterialStats(UMaterialInterface* Material);

	/**
	 * Lists the parameters of a material or material instance with their current values.
	 *
	 * @param Material The material or material instance
	 * @return Parameters (bOverridden tells which ones an instance overrides)
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Material|Instances")
	static FMaterialMcpParameters GetMaterialParameters(UMaterialInterface* Material);

	/**
	 * Creates a material instance (constant) of a material or of another instance.
	 *
	 * @param Parent Parent material or instance
	 * @param AssetPath New asset path, e.g. /Game/TestSample/MI_MimicEye_Blue
	 * @param bSave Save the new asset
	 * @return The new instance with its parameters
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Material|Instances")
	static FMaterialMcpParameters CreateMaterialInstance(UMaterialInterface* Parent, const FString& AssetPath, bool bSave = true);

	/**
	 * Overrides a parameter on a material instance. The type is taken from the parent's parameter of that name.
	 *
	 * @param Instance The material instance
	 * @param ParameterName Parameter name, e.g. Gaze Travel
	 * @param Value Scalar "0.2", vector "1, 0.3, 0.1, 1", texture asset path, or static switch true/false
	 * @param bSave Save the instance after the change
	 * @return The instance's parameters after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Material|Instances")
	static FMaterialMcpParameters SetMaterialInstanceParameter(UMaterialInstanceConstant* Instance, const FString& ParameterName, const FString& Value, bool bSave = true);

	/**
	 * Overrides several parameters on a material instance in one step. Every name and value is checked before any is
	 * applied; if one is invalid nothing changes.
	 *
	 * @param Instance The material instance
	 * @param Parameters Name / Value pairs, e.g. [{"name": "Tint", "value": "0, 0.3, 1, 1"}, {"name": "Speed", "value": "2"}]
	 * @param bSave Save the instance after the change
	 * @return The instance's parameters after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Material|Instances")
	static FMaterialMcpParameters SetMaterialInstanceParameters(UMaterialInstanceConstant* Instance, const TArray<FMaterialMcpParameterValue>& Parameters, bool bSave = true);

	/**
	 * Changes the parent of a material instance. Overrides of parameters the new parent also has keep their values.
	 *
	 * @param Instance The material instance
	 * @param Parent New parent material or instance, e.g. /Game/FX/M_Fire_Master
	 * @param bSave Save the instance after the change
	 * @return The instance's parameters after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Material|Instances")
	static FMaterialMcpParameters SetMaterialInstanceParent(UMaterialInstanceConstant* Instance, UMaterialInterface* Parent, bool bSave = true);

	/**
	 * Removes parameter overrides from a material instance so they inherit the parent's values again.
	 *
	 * @param Instance The material instance
	 * @param ParameterNames Parameters to reset, e.g. ["Tint", "Speed"]; empty resets every override
	 * @param bSave Save the instance after the change
	 * @return The instance's parameters after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Material|Instances")
	static FMaterialMcpParameters ClearMaterialInstanceParameters(UMaterialInstanceConstant* Instance, const TArray<FString>& ParameterNames, bool bSave = true);
};
