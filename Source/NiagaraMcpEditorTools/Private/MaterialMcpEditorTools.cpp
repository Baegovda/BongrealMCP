#include "MaterialMcpEditorTools.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture.h"
#include "Engine/TextureCube.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMaterialLayerOutput.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticBool.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionTextureObject.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTextureSampleParameterCube.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInstance.h"
#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Materials/MaterialFunctionMaterialLayerBlend.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraMcpPreview.h"
#include "RHI.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MaterialMcpEditorTools)

#define LOCTEXT_NAMESPACE "MaterialMcpEditorTools"

using namespace NiagaraMcp;

namespace MaterialMcp
{
	// ------------------------------------------------------------------ Generic helpers

	/** Shared helpers (NiagaraMcpCommon): did-you-mean suggestions, optional-argument words, bool parsing. */
	FString SuggestNames(const FString& Query, const TArray<FString>& Candidates)
	{
		return NiagaraMcp::DidYouMean(Query, Candidates);
	}

	static bool IsUnsetArg(const FString& S)
	{
		return NiagaraMcp::IsUnset(S);
	}

	/** Pin name argument: empty when unset (the library then takes the first pin). */
	FString PinArg(const FString& S)
	{
		return IsUnsetArg(S) ? FString() : S.TrimStartAndEnd();
	}

	bool ParseBoolText(const FString& Text, bool& bOut)
	{
		return NiagaraMcp::ParseBool(Text, bOut);
	}

	/** Loads an asset without raising a tool error or logging load warnings (for validation passes). */
	UObject* QuietLoad(const FString& InPath, UClass* Class)
	{
		FString Path = InPath.TrimStartAndEnd();
		if (!Path.StartsWith(TEXT("/")))
		{
			return nullptr;
		}
		FString PackageName = Path;
		FString ObjectName;
		if (!Path.Split(TEXT("."), &PackageName, &ObjectName))
		{
			Path = Path + TEXT(".") + FPackageName::GetShortName(Path);
		}
		UObject* Object = StaticFindObject(Class, nullptr, *Path);
		if (Object == nullptr && FPackageName::DoesPackageExist(PackageName))
		{
			Object = StaticLoadObject(Class, nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		return Object && Object->IsA(Class) ? Object : nullptr;
	}

	// ------------------------------------------------------------------ Material or material function graph

	/** The graph a tool works on: exactly one of Material / Function is set. */
	struct FGraph
	{
		UMaterial* Material = nullptr;
		UMaterialFunction* Function = nullptr;

		UObject* Asset() const { return Material ? static_cast<UObject*>(Material) : static_cast<UObject*>(Function); }

		TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions() const
		{
			return Material ? Material->GetExpressions() : Function->GetExpressions();
		}
	};

	bool ResolveGraph(UObject* Object, bool bForWrite, FGraph& Out)
	{
		if (Object == nullptr)
		{
			ReportError(TEXT("Material is null or could not be loaded. Pass a material or material function path such as /Game/FX/M_Example or /Game/FX/Functions/MF_Example."));
			return false;
		}
		if (UMaterial* Material = Cast<UMaterial>(Object))
		{
			Out.Material = Material;
		}
		else if (UMaterialFunction* Function = Cast<UMaterialFunction>(Object))
		{
			Out.Function = Function;
		}
		else if (Object->IsA<UMaterialInstance>())
		{
			ReportError(FString::Printf(TEXT("'%s' is a material instance, which has no graph. Edit its parent material, or change its values with SetMaterialInstanceParameters."), *Object->GetPathName()));
			return false;
		}
		else if (Object->IsA<UMaterialFunctionInstance>())
		{
			ReportError(FString::Printf(TEXT("'%s' is a material function instance, which has no graph. Edit its parent function."), *Object->GetPathName()));
			return false;
		}
		else
		{
			ReportError(FString::Printf(TEXT("'%s' is a %s, not a material or material function."), *Object->GetPathName(), *Object->GetClass()->GetName()));
			return false;
		}
		// The material (function) editor edits a preview copy and writes it back on Apply, which would discard our edits.
		return !bForWrite || EnsureNoOpenEditor(Object);
	}

	/** Material-only tools (material outputs). */
	UMaterial* ResolveMaterialOnly(UObject* Object, bool bForWrite)
	{
		FGraph Graph;
		if (!ResolveGraph(Object, bForWrite, Graph))
		{
			return nullptr;
		}
		if (Graph.Function)
		{
			ReportError(FString::Printf(TEXT("'%s' is a material function: it has no material outputs. Connect to a FunctionOutput node with ConnectExpressions instead."), *Graph.Function->GetPathName()));
			return nullptr;
		}
		return Graph.Material;
	}

	void MarkChanged(const FGraph& Graph)
	{
		Graph.Asset()->MarkPackageDirty();
	}

	FString ShortClassName(const UClass* Class)
	{
		FString Name = Class ? Class->GetName() : FString();
		Name.RemoveFromStart(TEXT("MaterialExpression"));
		return Name;
	}

	FString ExpressionLabel(const UMaterialExpression* Expression)
	{
		if (const UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(Expression))
		{
			return Input->InputName.ToString();
		}
		if (const UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
		{
			return Output->OutputName.ToString();
		}
		const FName ParameterName = Expression->GetParameterName();
		return ParameterName.IsNone() ? Expression->Desc : ParameterName.ToString();
	}

	FString OutputName(UMaterialExpression* Expression, int32 OutputIndex)
	{
		if (Expression == nullptr)
		{
			return FString();
		}
		TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
		return Outputs.IsValidIndex(OutputIndex) ? Outputs[OutputIndex].OutputName.ToString() : FString();
	}

	FMaterialMcpExpressionInfo DescribeExpression(UMaterialExpression* Expression)
	{
		FMaterialMcpExpressionInfo Info;
		if (Expression == nullptr)
		{
			return Info;
		}
		Info.Name = Expression->GetName();
		Info.Class = ShortClassName(Expression->GetClass());
		Info.Label = ExpressionLabel(Expression);
		Info.PosX = Expression->MaterialExpressionEditorX;
		Info.PosY = Expression->MaterialExpressionEditorY;

		// Function calls report the function's input names (without the type suffix shown in the editor).
		const TArray<FString> InputNames = UMaterialEditingLibrary::GetMaterialExpressionInputNames(Expression);
		for (int32 Index = 0; Index < InputNames.Num(); ++Index)
		{
			const FExpressionInput* Input = Expression->GetInput(Index);
			if (Input && Input->Expression)
			{
				Info.Inputs.Add(FString::Printf(TEXT("%s <- %s:%s"), *InputNames[Index], *Input->Expression->GetName(), *OutputName(Input->Expression, Input->OutputIndex)));
			}
			else
			{
				Info.Inputs.Add(InputNames[Index]);
			}
		}
		for (const FExpressionOutput& Output : Expression->GetOutputs())
		{
			Info.Outputs.Add(Output.OutputName.ToString());
		}
		if (const UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expression))
		{
			Info.Code = Custom->Code;
		}
		if (const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
		{
			Info.MaterialFunction = Call->MaterialFunction ? Call->MaterialFunction->GetPathName() : FString();
		}
		return Info;
	}

	static const EMaterialProperty DescribedProperties[] =
	{
		MP_BaseColor, MP_Metallic, MP_Specular, MP_Roughness, MP_Anisotropy, MP_EmissiveColor, MP_Opacity, MP_OpacityMask,
		MP_Normal, MP_Tangent, MP_WorldPositionOffset, MP_SubsurfaceColor, MP_AmbientOcclusion, MP_Refraction, MP_PixelDepthOffset,
	};

	FString PropertyName(EMaterialProperty Property)
	{
		FString Name = StaticEnum<EMaterialProperty>()->GetNameStringByValue(static_cast<int64>(Property));
		Name.RemoveFromStart(TEXT("MP_"));
		return Name;
	}

	FString FunctionUsageName(EMaterialFunctionUsage Usage)
	{
		switch (Usage)
		{
		case EMaterialFunctionUsage::MaterialLayer: return TEXT("MaterialLayer");
		case EMaterialFunctionUsage::MaterialLayerBlend: return TEXT("MaterialLayerBlend");
		default: return TEXT("Default");
		}
	}

	void GetFunctionPins(UMaterialFunction* Function, TArray<FFunctionExpressionInput>& OutInputs, TArray<FFunctionExpressionOutput>& OutOutputs)
	{
		// Through the interface: the override is not exported, the virtual call is.
		const UMaterialFunctionInterface* Interface = Function;
		Interface->GetInputsAndOutputs(OutInputs, OutOutputs);
	}

	FMaterialMcpGraphInfo DescribeGraph(const FGraph& Graph)
	{
		FMaterialMcpGraphInfo Info;
		Info.MaterialPath = Graph.Asset()->GetPathName();
		if (UMaterial* Material = Graph.Material)
		{
			Info.AssetType = TEXT("Material");
			Info.MaterialDomain = GetPropertyText(UMaterial::StaticClass(), Material, TEXT("MaterialDomain"));
			Info.BlendMode = GetPropertyText(UMaterial::StaticClass(), Material, TEXT("BlendMode"));
			Info.ShadingModel = GetPropertyText(UMaterial::StaticClass(), Material, TEXT("ShadingModel"));
			Info.bTwoSided = Material->IsTwoSided();
			for (EMaterialProperty Property : DescribedProperties)
			{
				if (UMaterialExpression* Source = UMaterialEditingLibrary::GetMaterialPropertyInputNode(Material, Property))
				{
					Info.PropertyConnections.Add(FString::Printf(TEXT("%s <- %s:%s"), *PropertyName(Property), *Source->GetName(),
						*UMaterialEditingLibrary::GetMaterialPropertyInputNodeOutputName(Material, Property)));
				}
			}
		}
		else
		{
			UMaterialFunction* Function = Graph.Function;
			Info.AssetType = TEXT("MaterialFunction");
			Info.Description = Function->Description;
			Info.FunctionUsage = FunctionUsageName(Function->GetMaterialFunctionUsage());
			TArray<FFunctionExpressionInput> Inputs;
			TArray<FFunctionExpressionOutput> Outputs;
			GetFunctionPins(Function, Inputs, Outputs);
			for (const FFunctionExpressionInput& Input : Inputs)
			{
				if (const UMaterialExpressionFunctionInput* Expression = Input.ExpressionInput)
				{
					Info.FunctionInputs.Add(FString::Printf(TEXT("%s (%s)"), *Expression->InputName.ToString(),
						*UMaterialExpressionFunctionInput::GetInputTypeDisplayName(static_cast<EFunctionInputType>(Expression->InputType.GetValue()))));
				}
			}
			for (const FFunctionExpressionOutput& Output : Outputs)
			{
				if (const UMaterialExpressionFunctionOutput* Expression = Output.ExpressionOutput)
				{
					Info.FunctionOutputs.Add(Expression->OutputName.ToString());
				}
			}
		}
		for (UMaterialExpression* Expression : Graph.Expressions())
		{
			if (Expression)
			{
				Info.Expressions.Add(DescribeExpression(Expression));
			}
		}
		return Info;
	}

	UMaterialExpression* FindExpression(const FGraph& Graph, const FString& InName)
	{
		const FString Name = InName.TrimStartAndEnd();
		TArray<UMaterialExpression*> Matches;
		TArray<FString> Available;
		TArray<FString> Candidates;
		for (UMaterialExpression* Expression : Graph.Expressions())
		{
			if (Expression == nullptr)
			{
				continue;
			}
			if (Expression->GetName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Expression; // object names are unique
			}
			const FString Label = ExpressionLabel(Expression);
			if (!Label.IsEmpty() && Label.Equals(Name, ESearchCase::IgnoreCase))
			{
				Matches.Add(Expression);
			}
			Candidates.Add(Expression->GetName());
			if (!Label.IsEmpty())
			{
				Candidates.Add(Label);
			}
			Available.Add(Label.IsEmpty() ? Expression->GetName() : FString::Printf(TEXT("%s (%s)"), *Expression->GetName(), *Label));
		}
		if (Matches.Num() == 1)
		{
			return Matches[0];
		}
		ReportError(Matches.Num() == 0
			? FString::Printf(TEXT("Expression '%s' not found.%s Expressions: %s"), *InName, *SuggestNames(Name, Candidates), Available.Num() ? *FString::Join(Available, TEXT(", ")) : TEXT("(none)"))
			: FString::Printf(TEXT("'%s' matches %d expressions; use the expression name."), *InName, Matches.Num()));
		return nullptr;
	}

	UClass* ResolveExpressionClass(const FString& InName)
	{
		const FString Name = InName.TrimStartAndEnd();
		UClass* Class = Name.StartsWith(TEXT("/")) ? LoadObject<UClass>(nullptr, *Name) : FindFirstObject<UClass>(*(FString(TEXT("MaterialExpression")) + Name), EFindFirstObjectOptions::NativeFirst);
		if (Class == nullptr && !Name.StartsWith(TEXT("/")))
		{
			Class = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst);
		}
		if (Class == nullptr || !Class->IsChildOf(UMaterialExpression::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
		{
			TArray<FString> Candidates;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->IsChildOf(UMaterialExpression::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
				{
					Candidates.Add(ShortClassName(*It));
				}
			}
			ReportError(FString::Printf(TEXT("Unknown material expression '%s'.%s Examples: Custom, ScalarParameter, VectorParameter, TextureObjectParameter, TextureSampleParameter2D, TextureCoordinate, Time, Multiply, Add, Lerp, MaterialFunctionCall, FunctionInput, FunctionOutput."),
				*InName, *SuggestNames(Name, Candidates)));
			return nullptr;
		}
		return Class;
	}

	bool ParseMaterialProperty(const FString& InName, EMaterialProperty& OutProperty)
	{
		const UEnum* Enum = StaticEnum<EMaterialProperty>();
		FString Name = InName.TrimStartAndEnd();
		Name.RemoveFromStart(TEXT("MP_"));
		for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
		{
			FString EntryName = Enum->GetNameStringByIndex(Index);
			EntryName.RemoveFromStart(TEXT("MP_"));
			if (LooseNameEquals(EntryName, Name) || LooseNameEquals(Enum->GetDisplayNameTextByIndex(Index).ToString(), Name))
			{
				OutProperty = static_cast<EMaterialProperty>(Enum->GetValueByIndex(Index));
				return true;
			}
		}
		TArray<FString> Candidates;
		for (EMaterialProperty Property : DescribedProperties)
		{
			Candidates.Add(PropertyName(Property));
		}
		ReportError(FString::Printf(TEXT("Unknown material output '%s'.%s Valid: %s."), *InName, *SuggestNames(Name, Candidates), *FString::Join(Candidates, TEXT(", "))));
		return false;
	}

	/** Every FunctionInput (bInputs) or FunctionOutput of the graph other than Except. */
	TArray<UMaterialExpression*> FunctionPinExpressions(const FGraph& Graph, bool bInputs, const UMaterialExpression* Except = nullptr)
	{
		TArray<UMaterialExpression*> Result;
		for (UMaterialExpression* Expression : Graph.Expressions())
		{
			if (Expression && Expression != Except && (bInputs ? Expression->IsA<UMaterialExpressionFunctionInput>() : Expression->IsA<UMaterialExpressionFunctionOutput>()))
			{
				Result.Add(Expression);
			}
		}
		return Result;
	}

	FName UniqueFunctionPinName(const FGraph& Graph, bool bInputs, const FString& Base, const UMaterialExpression* Except)
	{
		const TArray<UMaterialExpression*> Others = FunctionPinExpressions(Graph, bInputs, Except);
		for (int32 Suffix = 1;; ++Suffix)
		{
			const FString Candidate = Suffix == 1 ? Base : FString::Printf(TEXT("%s_%d"), *Base, Suffix);
			if (!Others.ContainsByPredicate([&Candidate](const UMaterialExpression* E) { return ExpressionLabel(E).Equals(Candidate, ESearchCase::IgnoreCase); }))
			{
				return FName(*Candidate);
			}
		}
	}

	FString VectorText(const FLinearColor& Color)
	{
		return FString::Printf(TEXT("%s, %s, %s, %s"), *FString::SanitizeFloat(Color.R, 0), *FString::SanitizeFloat(Color.G, 0), *FString::SanitizeFloat(Color.B, 0), *FString::SanitizeFloat(Color.A, 0));
	}

	FMaterialMcpParameters DescribeParameters(UMaterialInterface* Material)
	{
		FMaterialMcpParameters Info;
		Info.Path = Material->GetPathName();
		UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Material);
		UMaterial* Base = Cast<UMaterial>(Material);
		if (Instance && Instance->Parent)
		{
			Info.Parent = Instance->Parent->GetPathName();
		}
		auto Add = [&Info, Instance](const FName& Name, const TCHAR* Type, const FString& Value)
		{
			FMaterialMcpParameter& Parameter = Info.Parameters.AddDefaulted_GetRef();
			Parameter.Name = Name.ToString();
			Parameter.Type = Type;
			Parameter.Value = Value;
			Parameter.bOverridden = Instance && UMaterialEditingLibrary::IsMaterialInstanceParameterOverridden(Instance, Name);
		};

		TArray<FName> Names;
		UMaterialEditingLibrary::GetScalarParameterNames(Material, Names);
		for (const FName& Name : Names)
		{
			const float Value = Instance ? UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, Name)
				: Base ? UMaterialEditingLibrary::GetMaterialDefaultScalarParameterValue(Base, Name) : 0.0f;
			Add(Name, TEXT("Scalar"), FString::SanitizeFloat(Value));
		}
		Names.Reset();
		UMaterialEditingLibrary::GetVectorParameterNames(Material, Names);
		for (const FName& Name : Names)
		{
			const FLinearColor Value = Instance ? UMaterialEditingLibrary::GetMaterialInstanceVectorParameterValue(Instance, Name)
				: Base ? UMaterialEditingLibrary::GetMaterialDefaultVectorParameterValue(Base, Name) : FLinearColor::Black;
			Add(Name, TEXT("Vector"), VectorText(Value));
		}
		Names.Reset();
		UMaterialEditingLibrary::GetTextureParameterNames(Material, Names);
		for (const FName& Name : Names)
		{
			const UTexture* Value = Instance ? UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, Name)
				: Base ? UMaterialEditingLibrary::GetMaterialDefaultTextureParameterValue(Base, Name) : nullptr;
			Add(Name, TEXT("Texture"), Value ? Value->GetPathName() : FString(TEXT("None")));
		}
		Names.Reset();
		UMaterialEditingLibrary::GetStaticSwitchParameterNames(Material, Names);
		for (const FName& Name : Names)
		{
			const bool bValue = Instance ? UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, Name)
				: Base ? UMaterialEditingLibrary::GetMaterialDefaultStaticSwitchParameterValue(Base, Name) : false;
			Add(Name, TEXT("StaticSwitch"), bValue ? TEXT("true") : TEXT("false"));
		}
		return Info;
	}

	// ------------------------------------------------------------------ Material instance parameter values

	/** A parameter value parsed and checked against the instance's parameter list. */
	struct FParameterValue
	{
		FName Name;
		FString Type;
		float Scalar = 0.0f;
		FLinearColor Vector = FLinearColor::Black;
		UTexture* Texture = nullptr;
		bool bSwitch = false;
	};

	const FMaterialMcpParameter* FindParameter(const FMaterialMcpParameters& Described, const FString& Name, FString& OutError)
	{
		const FString Trimmed = Name.TrimStartAndEnd();
		const FMaterialMcpParameter* Parameter = Described.Parameters.FindByPredicate([&Trimmed](const FMaterialMcpParameter& P) { return P.Name.Equals(Trimmed, ESearchCase::IgnoreCase); });
		if (Parameter == nullptr)
		{
			TArray<FString> Names;
			for (const FMaterialMcpParameter& P : Described.Parameters)
			{
				Names.Add(P.Name);
			}
			OutError = FString::Printf(TEXT("Unknown parameter '%s'.%s Parameters: %s"), *Name, *SuggestNames(Trimmed, Names),
				Described.Parameters.Num() ? *FString::JoinBy(Described.Parameters, TEXT(", "), [](const FMaterialMcpParameter& P) { return FString::Printf(TEXT("%s (%s)"), *P.Name, *P.Type); }) : TEXT("(none)"));
		}
		return Parameter;
	}

	bool ParseParameterValue(const FMaterialMcpParameters& Described, const FString& Name, const FString& Value, FParameterValue& Out, FString& OutError)
	{
		const FMaterialMcpParameter* Parameter = FindParameter(Described, Name, OutError);
		if (Parameter == nullptr)
		{
			return false;
		}
		Out.Name = FName(*Parameter->Name);
		Out.Type = Parameter->Type;
		const FString Trimmed = Value.TrimStartAndEnd();
		if (Out.Type == TEXT("Scalar"))
		{
			if (!Trimmed.IsNumeric())
			{
				OutError = FString::Printf(TEXT("Scalar parameter '%s' takes a number (got '%s')."), *Parameter->Name, *Value);
				return false;
			}
			Out.Scalar = FCString::Atof(*Trimmed);
			return true;
		}
		if (Out.Type == TEXT("Vector"))
		{
			FString ValueError;
			if (!ImportStructValue(TBaseStructure<FLinearColor>::Get(), &Out.Vector, Trimmed, nullptr, ValueError))
			{
				OutError = FString::Printf(TEXT("Vector parameter '%s' takes \"R, G, B, A\" (got '%s'). %s"), *Parameter->Name, *Value, *ValueError);
				return false;
			}
			return true;
		}
		if (Out.Type == TEXT("Texture"))
		{
			Out.Texture = Cast<UTexture>(QuietLoad(Trimmed, UTexture::StaticClass()));
			if (Out.Texture == nullptr)
			{
				OutError = FString::Printf(TEXT("Texture parameter '%s' takes a texture asset path, e.g. /Engine/EngineResources/DefaultTexture (could not load '%s' as a texture)."), *Parameter->Name, *Value);
				return false;
			}
			return true;
		}
		if (Out.Type == TEXT("StaticSwitch"))
		{
			if (!ParseBoolText(Trimmed, Out.bSwitch))
			{
				OutError = FString::Printf(TEXT("Static switch parameter '%s' takes true or false (got '%s')."), *Parameter->Name, *Value);
				return false;
			}
			return true;
		}
		OutError = FString::Printf(TEXT("Parameter '%s' has unsupported type %s."), *Parameter->Name, *Out.Type);
		return false;
	}

	/** Current value of a parameter on the instance (inherited or overridden). */
	FParameterValue ReadParameterValue(UMaterialInstanceConstant* Instance, const FParameterValue& Like)
	{
		FParameterValue Current = Like;
		if (Like.Type == TEXT("Scalar"))
		{
			Current.Scalar = UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, Like.Name);
		}
		else if (Like.Type == TEXT("Vector"))
		{
			Current.Vector = UMaterialEditingLibrary::GetMaterialInstanceVectorParameterValue(Instance, Like.Name);
		}
		else if (Like.Type == TEXT("Texture"))
		{
			Current.Texture = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, Like.Name);
		}
		else if (Like.Type == TEXT("StaticSwitch"))
		{
			Current.bSwitch = UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, Like.Name);
		}
		return Current;
	}

	/**
	 * Writes an override and verifies it by reading it back. The UMaterialEditingLibrary setters do not report success
	 * (SetMaterialInstanceVectorParameterValue returns false even when it worked), so the read-back is the only check.
	 * The instance is not updated here; the caller runs UpdateMaterialInstance once after all values are written.
	 */
	bool ApplyParameterValue(UMaterialInstanceConstant* Instance, const FParameterValue& Value)
	{
		const FMaterialParameterInfo Info(Value.Name);
		if (Value.Type == TEXT("Scalar"))
		{
			Instance->SetScalarParameterValueEditorOnly(Info, Value.Scalar);
			return FMath::IsNearlyEqual(UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, Value.Name), Value.Scalar, 1.e-4f);
		}
		if (Value.Type == TEXT("Vector"))
		{
			Instance->SetVectorParameterValueEditorOnly(Info, Value.Vector);
			return UMaterialEditingLibrary::GetMaterialInstanceVectorParameterValue(Instance, Value.Name).Equals(Value.Vector, 1.e-4f);
		}
		if (Value.Type == TEXT("Texture"))
		{
			Instance->SetTextureParameterValueEditorOnly(Info, Value.Texture);
			return UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, Value.Name) == Value.Texture;
		}
		if (Value.Type == TEXT("StaticSwitch"))
		{
			UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, Value.Name, Value.bSwitch, EMaterialParameterAssociation::GlobalParameter, false);
			return UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, Value.Name) == Value.bSwitch;
		}
		return false;
	}

	// ------------------------------------------------------------------ Material functions

	/** Loaded (non-preview) materials whose graph uses Function directly or through other functions. */
	TArray<UMaterial*> FindDependentMaterials(UMaterialFunctionInterface* Function)
	{
		TArray<UMaterial*> Result;
		for (TObjectIterator<UMaterial> It(RF_ClassDefaultObject, true, EInternalObjectFlags::Garbage); It; ++It)
		{
			UMaterial* Material = *It;
			if (Material->bIsPreviewMaterial || Material->bIsFunctionPreviewMaterial || Material->GetOutermost() == GetTransientPackage())
			{
				continue;
			}
			bool bUses = false;
			const UMaterialInterface* AsInterface = Material;
			AsInterface->IterateDependentFunctions([Function, &bUses](UMaterialFunctionInterface* Dependent)
			{
				bUses = Dependent == Function;
				return !bUses;
			});
			if (bUses)
			{
				Result.Add(Material);
			}
		}
		return Result;
	}

	/** Compile errors of the last compile of a material; never triggers a recompile. */
	void ReadExistingCompileErrors(UMaterial* Material, TArray<FString>& OutErrors, bool& bOutCompiled)
	{
		bOutCompiled = false;
		FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform);
		if (Resource == nullptr)
		{
			return;
		}
		if (!Resource->IsCompilationFinished())
		{
			Resource->FinishCompilation();
		}
		OutErrors = Resource->GetCompileErrors();
		bOutCompiled = OutErrors.Num() > 0 || Resource->GetGameThreadShaderMap() != nullptr;
	}

	/** Expressions of the graph that link to Target (they are modified when Target is replaced or removed). */
	void ModifyReferencers(const FGraph& Graph, UMaterialExpression* Target)
	{
		for (UMaterialExpression* Expression : Graph.Expressions())
		{
			if (Expression == nullptr || Expression == Target)
			{
				continue;
			}
			for (FExpressionInputIterator It{ Expression }; It; ++It)
			{
				if (It->Expression == Target)
				{
					Expression->Modify();
					break;
				}
			}
		}
	}
}

using namespace MaterialMcp;

FMaterialMcpGraphInfo UMaterialMcpEditorTools::CreateMaterial(const FString& AssetPath, bool bSave)
{
	FString AssetName;
	UPackage* Package = CreateNewAssetPackage(AssetPath, AssetName);
	if (Package == nullptr)
	{
		return {};
	}
	// No transaction: undoing a recorded NewObject leaves an empty, broken asset in memory that crashes the next
	// tool using it (the editor's asset factories do not record asset creation either).
	UMaterial* Material = NewObject<UMaterial>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Material);
	Package->MarkPackageDirty();
	if (bSave)
	{
		SaveAssetSilently(Material);
	}
	FGraph Graph;
	Graph.Material = Material;
	return DescribeGraph(Graph);
}

FMaterialMcpGraphInfo UMaterialMcpEditorTools::CreateMaterialFunction(const FString& AssetPath, const FString& Description, const FString& Usage, bool bExposeToLibrary, bool bSave)
{
	const FString UsageText = Usage.TrimStartAndEnd();
	EMaterialFunctionUsage FunctionUsage;
	UClass* FunctionClass = nullptr;
	if (IsUnsetArg(UsageText) || LooseNameEquals(UsageText, TEXT("Function")))
	{
		FunctionUsage = EMaterialFunctionUsage::Default;
		FunctionClass = UMaterialFunction::StaticClass();
	}
	else if (LooseNameEquals(UsageText, TEXT("MaterialLayer")) || LooseNameEquals(UsageText, TEXT("Layer")))
	{
		FunctionUsage = EMaterialFunctionUsage::MaterialLayer;
		FunctionClass = UMaterialFunctionMaterialLayer::StaticClass();
	}
	else if (LooseNameEquals(UsageText, TEXT("MaterialLayerBlend")) || LooseNameEquals(UsageText, TEXT("LayerBlend")) || LooseNameEquals(UsageText, TEXT("Blend")))
	{
		FunctionUsage = EMaterialFunctionUsage::MaterialLayerBlend;
		FunctionClass = UMaterialFunctionMaterialLayerBlend::StaticClass();
	}
	else
	{
		ReportError(FString::Printf(TEXT("Unknown Usage '%s'.%s Valid: Default, MaterialLayer, MaterialLayerBlend."), *Usage,
			*SuggestNames(UsageText, { TEXT("Default"), TEXT("MaterialLayer"), TEXT("MaterialLayerBlend") })));
		return {};
	}

	FString AssetName;
	UPackage* Package = CreateNewAssetPackage(AssetPath, AssetName);
	if (Package == nullptr)
	{
		return {};
	}
	// No transaction: undoing a recorded NewObject leaves an empty, broken asset in memory that crashes the next
	// tool using it (the editor's asset factories do not record asset creation either).
	// Same as UMaterialFunctionFactoryNew / UMaterialFunctionMaterialLayer(Blend)Factory.
	UMaterialFunction* Function = NewObject<UMaterialFunction>(Package, FunctionClass, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	Function->SetMaterialFunctionUsage(FunctionUsage);
	Function->Description = IsUnsetArg(Description) ? FString() : Description;
	Function->bExposeToLibrary = bExposeToLibrary;

	// Starting nodes as created by the material editor for a new function.
	FGraph Graph;
	Graph.Function = Function;
	if (FunctionUsage == EMaterialFunctionUsage::Default)
	{
		if (UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(
			UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, UMaterialExpressionFunctionOutput::StaticClass(), 200, 0)))
		{
			Output->OutputName = TEXT("Result");
		}
	}
	else
	{
		UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, UMaterialExpressionMaterialLayerOutput::StaticClass(), 300, 0);
		auto AddAttributesInput = [Function](const TCHAR* Name, int32 PosY, int32 SortPriority, EBlendInputRelevance Relevance)
		{
			if (UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(
				UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, UMaterialExpressionFunctionInput::StaticClass(), -350, PosY)))
			{
				Input->InputType = FunctionInput_MaterialAttributes;
				Input->InputName = Name;
				Input->bUsePreviewValueAsDefault = true;
				Input->SortPriority = SortPriority;
				Input->BlendInputRelevance = Relevance;
			}
		};
		if (FunctionUsage == EMaterialFunctionUsage::MaterialLayer)
		{
			AddAttributesInput(TEXT("Material Attributes"), 0, 0, EBlendInputRelevance::General);
		}
		else
		{
			// "Top layer" sits below "bottom layer", matching B on blend nodes.
			AddAttributesInput(BottomMaterialBlendInputName, -100, 0, EBlendInputRelevance::Bottom);
			AddAttributesInput(TopMaterialBlendInputName, 100, 1, EBlendInputRelevance::Top);
		}
	}

	FAssetRegistryModule::AssetCreated(Function);
	Package->MarkPackageDirty();
	if (bSave)
	{
		SaveAssetSilently(Function);
	}
	return DescribeGraph(Graph);
}

FMaterialMcpGraphInfo UMaterialMcpEditorTools::GetMaterialGraph(UObject* Material, const FString& ClassFilter, int32 Offset, int32 MaxExpressions)
{
	FGraph Graph;
	if (!ResolveGraph(Material, false, Graph))
	{
		return {};
	}
	FMaterialMcpGraphInfo Info = DescribeGraph(Graph);
	if (!NiagaraMcp::IsUnset(ClassFilter))
	{
		TArray<FString> Words;
		ClassFilter.ToLower().ParseIntoArrayWS(Words);
		Info.Expressions.RemoveAll([&Words](const FMaterialMcpExpressionInfo& Expression)
		{
			const FString Haystack = (Expression.Class + TEXT(" ") + Expression.Name + TEXT(" ") + Expression.Label).ToLower();
			return Words.ContainsByPredicate([&Haystack](const FString& Word) { return !Haystack.Contains(Word); });
		});
	}
	Info.TotalExpressions = Info.Expressions.Num();
	const int32 First = FMath::Clamp(Offset, 0, Info.Expressions.Num());
	const int32 Count = FMath::Clamp(MaxExpressions, 1, 500);
	Info.Expressions = TArray<FMaterialMcpExpressionInfo>(Info.Expressions.GetData() + First, FMath::Min(Count, Info.Expressions.Num() - First));
	Info.NextOffset = First + Count < Info.TotalExpressions ? First + Count : -1;
	return Info;
}

FNiagaraMcpPropertyResult UMaterialMcpEditorTools::SetMaterialProperty(UObject* Material, const FString& PropertyName, const FString& Value, bool bSave)
{
	FGraph Graph;
	if (!ResolveGraph(Material, true, Graph))
	{
		return {};
	}
	UObject* Asset = Graph.Asset();
	FScopedTransaction Transaction(LOCTEXT("SetMaterialProperty", "Material MCP: Set Material Property"));
	Asset->Modify();
	// SetPropertyFromText validates the path and the value before it writes anything.
	FString PropertyError;
	FProperty* Changed = SetPropertyFromText(Asset->GetClass(), Asset, Asset, PropertyName, Value, PropertyError);
	if (Changed == nullptr)
	{
		ReportError(PropertyError);
		Transaction.Cancel();
		return {};
	}
	FPropertyChangedEvent ChangedEvent(Changed, EPropertyChangeType::ValueSet);
	Asset->PostEditChangeProperty(ChangedEvent);
	MarkChanged(Graph);

	FNiagaraMcpPropertyResult Result;
	Result.Property = PropertyName;
	Result.Value = GetPropertyText(Asset->GetClass(), Asset, PropertyName);
	Result.bSaved = bSave && SaveAssetSilently(Asset);
	return Result;
}

FMaterialMcpExpressionInfo UMaterialMcpEditorTools::AddExpression(UObject* Material, const FString& ExpressionClass, int32 PosX, int32 PosY)
{
	FGraph Graph;
	if (!ResolveGraph(Material, true, Graph))
	{
		return {};
	}
	UClass* Class = ResolveExpressionClass(ExpressionClass);
	if (Class == nullptr)
	{
		return {};
	}
	if (!Class->GetDefaultObject<UMaterialExpression>()->IsAllowedIn(Graph.Asset()))
	{
		ReportError(Class->IsChildOf(UMaterialExpressionFunctionInput::StaticClass()) || Class->IsChildOf(UMaterialExpressionFunctionOutput::StaticClass())
			? FString::Printf(TEXT("%s nodes only exist in material functions. Create one with CreateMaterialFunction and call it with a MaterialFunctionCall node."), *ShortClassName(Class))
			: FString::Printf(TEXT("%s is not allowed in '%s'."), *ShortClassName(Class), *Graph.Asset()->GetPathName()));
		return {};
	}
	FScopedTransaction Transaction(LOCTEXT("AddExpression", "Material MCP: Add Expression"));
	Graph.Asset()->Modify();
	UMaterialExpression* Expression = Graph.Material
		? UMaterialEditingLibrary::CreateMaterialExpression(Graph.Material, Class, PosX, PosY)
		: UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Graph.Function, Class, PosX, PosY);
	if (Expression == nullptr)
	{
		ReportError(FString::Printf(TEXT("Could not create a %s expression."), *Class->GetName()));
		return {};
	}
	// Function pins need unique names (the engine only enforces this inside the material editor) and keep the
	// order they were added in.
	if (UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(Expression))
	{
		Input->InputName = UniqueFunctionPinName(Graph, true, Input->InputName.IsNone() ? FString(TEXT("In")) : Input->InputName.ToString(), Input);
		Input->SortPriority = FunctionPinExpressions(Graph, true, Input).Num();
	}
	else if (UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
	{
		Output->OutputName = UniqueFunctionPinName(Graph, false, Output->OutputName.IsNone() ? FString(TEXT("Result")) : Output->OutputName.ToString(), Output);
		Output->SortPriority = FunctionPinExpressions(Graph, false, Output).Num();
	}
	MarkChanged(Graph);
	return DescribeExpression(Expression);
}

FMaterialMcpExpressionInfo UMaterialMcpEditorTools::SetExpressionProperty(UObject* Material, const FString& Expression, const FString& PropertyName, const FString& Value)
{
	FGraph Graph;
	if (!ResolveGraph(Material, true, Graph))
	{
		return {};
	}
	UMaterialExpression* Target = FindExpression(Graph, Expression);
	if (Target == nullptr)
	{
		return {};
	}

	// MaterialFunctionCall.MaterialFunction: SetMaterialFunction rebuilds the node's pins from the function.
	if (UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Target); Call && LooseNameEquals(PropertyName.TrimStartAndEnd(), TEXT("MaterialFunction")))
	{
		const FString Path = Value.TrimStartAndEnd();
		UMaterialFunctionInterface* NewFunction = nullptr;
		if (!Path.IsEmpty() && !Path.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			NewFunction = LoadAssetByPath<UMaterialFunctionInterface>(Path, TEXT("MaterialFunction"));
			if (NewFunction == nullptr)
			{
				return {};
			}
		}
		// The engine answers a circular call with a modal dialog; refuse it up front.
		if (NewFunction && Graph.Function && (NewFunction == Graph.Function || NewFunction->IsDependent(Graph.Function)))
		{
			ReportError(FString::Printf(TEXT("'%s' calls '%s' (directly or indirectly); calling it from there would be circular."), *NewFunction->GetPathName(), *Graph.Function->GetPathName()));
			return {};
		}
		FScopedTransaction Transaction(LOCTEXT("SetMaterialFunction", "Material MCP: Set Material Function"));
		Graph.Asset()->Modify();
		Call->Modify();
		Call->SetMaterialFunction(NewFunction);
		if (Call->MaterialFunction != NewFunction)
		{
			ReportError(FString::Printf(TEXT("Could not set the material function of %s to '%s'."), *Call->GetName(), *Path));
			return {};
		}
		MarkChanged(Graph);
		return DescribeExpression(Call);
	}

	FString NewValue = Value;
	const FString Property = PropertyName.TrimStartAndEnd();
	if (Target->IsA<UMaterialExpressionFunctionInput>() || Target->IsA<UMaterialExpressionFunctionOutput>())
	{
		const bool bInput = Target->IsA<UMaterialExpressionFunctionInput>();
		if (LooseNameEquals(Property, bInput ? TEXT("InputName") : TEXT("OutputName")))
		{
			const FString NewName = Value.TrimStartAndEnd();
			if (NewName.IsEmpty())
			{
				ReportError(TEXT("Function input / output names cannot be empty."));
				return {};
			}
			for (const UMaterialExpression* Other : FunctionPinExpressions(Graph, bInput, Target))
			{
				if (ExpressionLabel(Other).Equals(NewName, ESearchCase::IgnoreCase))
				{
					ReportError(FString::Printf(TEXT("Function %s names must be unique: '%s' is already used by %s."), bInput ? TEXT("input") : TEXT("output"), *NewName, *Other->GetName()));
					return {};
				}
			}
			NewValue = NewName;
		}
		else if (bInput && LooseNameEquals(Property, TEXT("InputType")) && !NewValue.TrimStartAndEnd().StartsWith(TEXT("FunctionInput_"), ESearchCase::IgnoreCase) && !NewValue.TrimStartAndEnd().IsNumeric())
		{
			NewValue = TEXT("FunctionInput_") + NewValue.TrimStartAndEnd();
		}
	}

	FScopedTransaction Transaction(LOCTEXT("SetExpressionProperty", "Material MCP: Set Expression Property"));
	Graph.Asset()->Modify();
	Target->Modify();
	// SetPropertyFromText validates the path and the value before it writes anything.
	FString PropertyError;
	FProperty* Changed = SetPropertyFromText(Target->GetClass(), Target, Target, PropertyName, NewValue, PropertyError);
	if (Changed == nullptr)
	{
		ReportError(PropertyError);
		Transaction.Cancel();
		return {};
	}
	FPropertyChangedEvent ChangedEvent(Changed, EPropertyChangeType::ValueSet);
	Target->PostEditChangeProperty(ChangedEvent);
	MarkChanged(Graph);
	return DescribeExpression(Target);
}

FMaterialMcpExpressionInfo UMaterialMcpEditorTools::SetCustomExpression(UObject* Material, const FString& Expression, const FString& Code, const FString& Description, const TArray<FString>& InputNames, const FString& OutputType)
{
	FGraph Graph;
	if (!ResolveGraph(Material, true, Graph))
	{
		return {};
	}
	UMaterialExpression* Found = FindExpression(Graph, Expression);
	if (Found == nullptr)
	{
		return {};
	}
	UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Found);
	if (Custom == nullptr)
	{
		ReportError(FString::Printf(TEXT("'%s' is a %s expression, not a Custom expression."), *Expression, *ShortClassName(Found->GetClass())));
		return {};
	}

	// Validate everything before touching the node: a cancelled transaction does not roll back changes.
	TSet<FString> Seen;
	for (const FString& InputName : InputNames)
	{
		if (InputName.TrimStartAndEnd().IsEmpty() || Seen.Contains(InputName))
		{
			ReportError(FString::Printf(TEXT("Input names must be set and unique (got '%s')."), *InputName));
			return {};
		}
		Seen.Add(InputName);
	}
	TOptional<ECustomMaterialOutputType> NewOutputType;
	if (!IsUnsetArg(OutputType))
	{
		FString Normalized = OutputType.TrimStartAndEnd();
		if (!Normalized.StartsWith(TEXT("CMOT_"), ESearchCase::IgnoreCase) && !Normalized.IsNumeric())
		{
			Normalized = TEXT("CMOT_") + Normalized;
		}
		const UEnum* Enum = StaticEnum<ECustomMaterialOutputType>();
		int64 Parsed = 0;
		if (!ParseEnumText(Enum, Normalized, Parsed) || !Enum->IsValidEnumValue(Parsed) || Parsed == Enum->GetMaxEnumValue())
		{
			ReportError(FString::Printf(TEXT("Unknown OutputType '%s'. Valid: Float1, Float2, Float3, Float4, MaterialAttributes."), *OutputType));
			return {};
		}
		NewOutputType = static_cast<ECustomMaterialOutputType>(Parsed);
	}

	FScopedTransaction Transaction(LOCTEXT("SetCustomExpression", "Material MCP: Set Custom Expression"));
	Graph.Asset()->Modify();
	Custom->Modify();

	Custom->Code = Code;
	if (!IsUnsetArg(Description))
	{
		Custom->Description = Description;
	}
	TArray<FCustomInput> NewInputs;
	for (const FString& InputName : InputNames)
	{
		const FCustomInput* Existing = Custom->Inputs.FindByPredicate([&InputName](const FCustomInput& Input) { return Input.InputName.ToString().Equals(InputName); });
		if (Existing)
		{
			NewInputs.Add(*Existing); // keeps its connection
		}
		else
		{
			FCustomInput& Input = NewInputs.AddDefaulted_GetRef();
			Input.InputName = FName(*InputName);
		}
	}
	Custom->Inputs = MoveTemp(NewInputs);
	if (NewOutputType.IsSet())
	{
		Custom->OutputType = NewOutputType.GetValue();
	}

	// Rebuilds the node's input pins and outputs from the new declarations.
	FProperty* InputsProperty = FindFProperty<FProperty>(UMaterialExpressionCustom::StaticClass(), TEXT("Inputs"));
	FPropertyChangedEvent ChangedEvent(InputsProperty, EPropertyChangeType::ValueSet);
	Custom->PostEditChangeProperty(ChangedEvent);
	MarkChanged(Graph);
	return DescribeExpression(Custom);
}

FMaterialMcpExpressionInfo UMaterialMcpEditorTools::ConnectExpressions(UObject* Material, const FString& FromExpression, const FString& InFromOutput, const FString& ToExpression, const FString& InToInput)
{
	const FString FromOutput = PinArg(InFromOutput);
	const FString ToInput = PinArg(InToInput);
	FGraph Graph;
	if (!ResolveGraph(Material, true, Graph))
	{
		return {};
	}
	UMaterialExpression* From = FindExpression(Graph, FromExpression);
	UMaterialExpression* To = From ? FindExpression(Graph, ToExpression) : nullptr;
	if (To == nullptr)
	{
		return {};
	}
	FScopedTransaction Transaction(LOCTEXT("ConnectExpressions", "Material MCP: Connect Expressions"));
	Graph.Asset()->Modify();
	To->Modify();
	// The library looks the pins up before it links anything, so a failure leaves the graph unchanged.
	if (!UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromOutput, To, ToInput))
	{
		TArray<FString> Outputs;
		for (const FExpressionOutput& Output : From->GetOutputs())
		{
			Outputs.Add(FString::Printf(TEXT("'%s'"), *Output.OutputName.ToString()));
		}
		const TArray<FString> Inputs = UMaterialEditingLibrary::GetMaterialExpressionInputNames(To);
		TArray<FString> OutputNames;
		for (const FExpressionOutput& Output : From->GetOutputs())
		{
			OutputNames.Add(Output.OutputName.ToString());
		}
		ReportError(FString::Printf(TEXT("Could not connect %s:'%s' to %s:'%s'.%s Outputs of %s: %s. Inputs of %s: %s."),
			*From->GetName(), *FromOutput, *To->GetName(), *ToInput,
			*SuggestNames(ToInput, Inputs).Append(SuggestNames(FromOutput, OutputNames)),
			*From->GetName(), Outputs.Num() ? *FString::Join(Outputs, TEXT(", ")) : TEXT("(none)"),
			*To->GetName(), Inputs.Num() ? *FString::Join(Inputs, TEXT(", ")) : TEXT("(none)")));
		Transaction.Cancel();
		return {};
	}
	MarkChanged(Graph);
	return DescribeExpression(To);
}

FMaterialMcpExpressionInfo UMaterialMcpEditorTools::DisconnectExpressions(UObject* Material, const FString& ToExpression, const FString& InToInput)
{
	const FString ToInput = PinArg(InToInput);
	FGraph Graph;
	if (!ResolveGraph(Material, true, Graph))
	{
		return {};
	}
	UMaterialExpression* To = FindExpression(Graph, ToExpression);
	if (To == nullptr)
	{
		return {};
	}
	const TArray<FString> InputNames = UMaterialEditingLibrary::GetMaterialExpressionInputNames(To);
	if (InputNames.Num() == 0)
	{
		ReportError(FString::Printf(TEXT("%s has no inputs."), *To->GetName()));
		return {};
	}
	const FString Wanted = ToInput.TrimStartAndEnd();
	const int32 InputIndex = Wanted.IsEmpty() ? 0 : InputNames.IndexOfByPredicate([&Wanted](const FString& Name) { return Name.Equals(Wanted, ESearchCase::IgnoreCase); });
	if (InputIndex == INDEX_NONE)
	{
		ReportError(FString::Printf(TEXT("%s has no input '%s'.%s Inputs: %s."), *To->GetName(), *ToInput, *SuggestNames(Wanted, InputNames), *FString::Join(InputNames, TEXT(", "))));
		return {};
	}
	FExpressionInput* Input = To->GetInput(InputIndex);
	if (Input == nullptr || Input->Expression == nullptr)
	{
		ReportError(FString::Printf(TEXT("Input '%s' of %s is not connected."), *InputNames[InputIndex], *To->GetName()));
		return {};
	}
	FScopedTransaction Transaction(LOCTEXT("DisconnectExpressions", "Material MCP: Disconnect Expressions"));
	Graph.Asset()->Modify();
	To->Modify();
	Input->Expression = nullptr;
	Input->OutputIndex = 0;
	MarkChanged(Graph);
	return DescribeExpression(To);
}

FMaterialMcpGraphInfo UMaterialMcpEditorTools::ConnectToMaterialProperty(UObject* Material, const FString& FromExpression, const FString& InFromOutput, const FString& MaterialProperty)
{
	const FString FromOutput = PinArg(InFromOutput);
	UMaterial* Target = ResolveMaterialOnly(Material, true);
	if (Target == nullptr)
	{
		return {};
	}
	FGraph Graph;
	Graph.Material = Target;
	UMaterialExpression* From = FindExpression(Graph, FromExpression);
	EMaterialProperty Property;
	if (From == nullptr || !ParseMaterialProperty(MaterialProperty, Property))
	{
		return {};
	}
	FScopedTransaction Transaction(LOCTEXT("ConnectToMaterialProperty", "Material MCP: Connect To Material Output"));
	Target->Modify();
	if (!UMaterialEditingLibrary::ConnectMaterialProperty(From, FromOutput, Property))
	{
		TArray<FString> Outputs;
		for (const FExpressionOutput& Output : From->GetOutputs())
		{
			Outputs.Add(FString::Printf(TEXT("'%s'"), *Output.OutputName.ToString()));
		}
		ReportError(FString::Printf(TEXT("Could not connect %s:'%s' to %s. Outputs of %s: %s."), *From->GetName(), *FromOutput, *MaterialProperty,
			*From->GetName(), Outputs.Num() ? *FString::Join(Outputs, TEXT(", ")) : TEXT("(none)")));
		Transaction.Cancel();
		return {};
	}
	MarkChanged(Graph);
	return DescribeGraph(Graph);
}

FMaterialMcpGraphInfo UMaterialMcpEditorTools::DisconnectMaterialProperty(UObject* Material, const FString& MaterialProperty)
{
	UMaterial* Target = ResolveMaterialOnly(Material, true);
	EMaterialProperty Property;
	if (Target == nullptr || !ParseMaterialProperty(MaterialProperty, Property))
	{
		return {};
	}
	FGraph Graph;
	Graph.Material = Target;
	const FExpressionInput* Input = Target->GetExpressionInputForProperty(Property);
	if (Input == nullptr || !Input->IsConnected())
	{
		const FMaterialMcpGraphInfo Info = DescribeGraph(Graph);
		ReportError(FString::Printf(TEXT("Material output %s of '%s' is not connected. Connected outputs: %s"), *MaterialProperty, *Target->GetName(),
			Info.PropertyConnections.Num() ? *FString::Join(Info.PropertyConnections, TEXT("; ")) : TEXT("(none)")));
		return {};
	}
	FScopedTransaction Transaction(LOCTEXT("DisconnectMaterialProperty", "Material MCP: Disconnect Material Output"));
	Target->Modify();
	UMaterialEditingLibrary::DisconnectMaterialProperty(Target, Property);
	MarkChanged(Graph);
	return DescribeGraph(Graph);
}

FMaterialMcpGraphInfo UMaterialMcpEditorTools::RemoveExpression(UObject* Material, const FString& Expression)
{
	FGraph Graph;
	if (!ResolveGraph(Material, true, Graph))
	{
		return {};
	}
	UMaterialExpression* Target = FindExpression(Graph, Expression);
	if (Target == nullptr)
	{
		return {};
	}
	{
		FScopedTransaction Transaction(LOCTEXT("RemoveExpression", "Material MCP: Remove Expression"));
		Graph.Asset()->Modify();
		ModifyReferencers(Graph, Target);
		if (Graph.Material)
		{
			UMaterialEditingLibrary::DeleteMaterialExpression(Graph.Material, Target);
		}
		else
		{
			UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Graph.Function, Target);
		}
	}
	MarkChanged(Graph);
	return DescribeGraph(Graph);
}

FMaterialMcpExpressionInfo UMaterialMcpEditorTools::PromoteToParameter(UObject* Material, const FString& Expression, FName ParameterName, bool bSave)
{
	FGraph Graph;
	if (!ResolveGraph(Material, true, Graph))
	{
		return {};
	}
	if (ParameterName.IsNone() || ParameterName.ToString().TrimStartAndEnd().IsEmpty())
	{
		ReportError(TEXT("ParameterName is empty."));
		return {};
	}
	UMaterialExpression* Old = FindExpression(Graph, Expression);
	if (Old == nullptr)
	{
		return {};
	}

	// Pick the parameter class; exact classes only (parameters derive from some of these).
	const UClass* OldClass = Old->GetClass();
	UClass* NewClass = nullptr;
	if (OldClass == UMaterialExpressionConstant::StaticClass())
	{
		NewClass = UMaterialExpressionScalarParameter::StaticClass();
	}
	else if (OldClass == UMaterialExpressionConstant3Vector::StaticClass() || OldClass == UMaterialExpressionConstant4Vector::StaticClass())
	{
		NewClass = UMaterialExpressionVectorParameter::StaticClass();
	}
	else if (OldClass == UMaterialExpressionStaticBool::StaticClass())
	{
		NewClass = UMaterialExpressionStaticBoolParameter::StaticClass();
	}
	else if (OldClass == UMaterialExpressionTextureObject::StaticClass())
	{
		NewClass = UMaterialExpressionTextureObjectParameter::StaticClass();
	}
	else if (OldClass == UMaterialExpressionTextureSample::StaticClass())
	{
		const UMaterialExpressionTextureSample* Sample = CastChecked<UMaterialExpressionTextureSample>(Old);
		if (Sample->TextureObject.Expression)
		{
			ReportError(FString::Printf(TEXT("%s samples the texture object wired into its TextureObject input; promote that TextureObject node instead."), *Old->GetName()));
			return {};
		}
		NewClass = Sample->Texture && Sample->Texture->IsA<UTextureCube>()
			? UMaterialExpressionTextureSampleParameterCube::StaticClass()
			: UMaterialExpressionTextureSampleParameter2D::StaticClass();
	}
	if (NewClass == nullptr)
	{
		ReportError(FString::Printf(TEXT("%s (%s) cannot be promoted%s. Supported: Constant, Constant3Vector, Constant4Vector, StaticBool, TextureSample, TextureObject."),
			*Old->GetName(), *ShortClassName(OldClass), Old->HasAParameterName() ? TEXT(" (it already is a parameter; rename it with SetExpressionProperty ParameterName)") : TEXT("")));
		return {};
	}
	for (UMaterialExpression* Other : Graph.Expressions())
	{
		if (Other && Other->HasAParameterName() && Other->GetParameterName() == ParameterName && Other->GetClass() != NewClass)
		{
			ReportError(FString::Printf(TEXT("Parameter name '%s' is already used by %s (%s); pick another name."), *ParameterName.ToString(), *Other->GetName(), *ShortClassName(Other->GetClass())));
			return {};
		}
	}

	FScopedTransaction Transaction(LOCTEXT("PromoteToParameter", "Material MCP: Promote To Parameter"));
	Graph.Asset()->Modify();
	Old->Modify();
	ModifyReferencers(Graph, Old);

	UMaterialExpression* New = Graph.Material
		? UMaterialEditingLibrary::CreateMaterialExpression(Graph.Material, NewClass, Old->MaterialExpressionEditorX, Old->MaterialExpressionEditorY)
		: UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Graph.Function, NewClass, Old->MaterialExpressionEditorX, Old->MaterialExpressionEditorY);
	if (New == nullptr)
	{
		ReportError(FString::Printf(TEXT("Could not create a %s expression."), *ShortClassName(NewClass)));
		return {};
	}

	// Copy every property both classes share below UMaterialExpression (texture, sampler settings, input links).
	const UClass* Common = OldClass;
	while (Common && !NewClass->IsChildOf(Common))
	{
		Common = Common->GetSuperClass();
	}
	for (TFieldIterator<FProperty> It(Common); It; ++It)
	{
		const UClass* Owner = It->GetOwnerClass();
		if (Owner && Owner != UMaterialExpression::StaticClass() && Owner->IsChildOf(UMaterialExpression::StaticClass()))
		{
			It->CopyCompleteValue_InContainer(New, Old);
		}
	}
	if (const UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(Old))
	{
		CastChecked<UMaterialExpressionScalarParameter>(New)->DefaultValue = Constant->R;
	}
	else if (const UMaterialExpressionConstant3Vector* Constant3 = Cast<UMaterialExpressionConstant3Vector>(Old))
	{
		CastChecked<UMaterialExpressionVectorParameter>(New)->DefaultValue = Constant3->Constant;
	}
	else if (const UMaterialExpressionConstant4Vector* Constant4 = Cast<UMaterialExpressionConstant4Vector>(Old))
	{
		CastChecked<UMaterialExpressionVectorParameter>(New)->DefaultValue = Constant4->Constant;
	}
	else if (const UMaterialExpressionStaticBool* StaticBool = Cast<UMaterialExpressionStaticBool>(Old))
	{
		CastChecked<UMaterialExpressionStaticBoolParameter>(New)->DefaultValue = StaticBool->Value;
	}
	New->Desc = Old->Desc;
	New->SetParameterName(ParameterName);
	if (Graph.Material)
	{
		Graph.Material->UpdateExpressionParameterName(New);
	}

	// Outputs: same name when the old output is named (Constant4Vector "RGBA" -> VectorParameter "RGBA"), else same index.
	auto MapOutput = [Old, New](int32 OldIndex)
	{
		const TArray<FExpressionOutput>& OldOutputs = Old->GetOutputs();
		const TArray<FExpressionOutput>& NewOutputs = New->GetOutputs();
		if (OldOutputs.IsValidIndex(OldIndex) && !OldOutputs[OldIndex].OutputName.IsNone())
		{
			const int32 ByName = NewOutputs.IndexOfByPredicate([&OldOutputs, OldIndex](const FExpressionOutput& Output) { return Output.OutputName == OldOutputs[OldIndex].OutputName; });
			if (ByName != INDEX_NONE)
			{
				return ByName;
			}
		}
		return NewOutputs.IsValidIndex(OldIndex) ? OldIndex : 0;
	};
	for (UMaterialExpression* Other : Graph.Expressions())
	{
		if (Other == nullptr || Other == Old || Other == New)
		{
			continue;
		}
		for (FExpressionInputIterator It{ Other }; It; ++It)
		{
			if (It->Expression == Old)
			{
				It->Connect(MapOutput(It->OutputIndex), New);
			}
		}
	}
	if (Graph.Material)
	{
		for (int32 PropertyIndex = 0; PropertyIndex < MP_MAX; ++PropertyIndex)
		{
			FExpressionInput* Input = Graph.Material->GetExpressionInputForProperty(static_cast<EMaterialProperty>(PropertyIndex));
			if (Input && Input->Expression == Old)
			{
				Input->Connect(MapOutput(Input->OutputIndex), New);
			}
		}
		UMaterialEditingLibrary::DeleteMaterialExpression(Graph.Material, Old);
	}
	else
	{
		UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Graph.Function, Old);
	}
	// Like SetExpressionProperty: lets the owning material refresh its cached parameter list.
	New->PostEditChange();
	MarkChanged(Graph);
	if (bSave)
	{
		SaveAssetSilently(Graph.Asset());
	}
	return DescribeExpression(New);
}

FMaterialMcpGraphInfo UMaterialMcpEditorTools::LayoutMaterial(UObject* Material)
{
	FGraph Graph;
	if (!ResolveGraph(Material, true, Graph))
	{
		return {};
	}
	{
		FScopedTransaction Transaction(LOCTEXT("LayoutMaterial", "Material MCP: Layout Material"));
		Graph.Asset()->Modify();
		for (UMaterialExpression* Expression : Graph.Expressions())
		{
			if (Expression)
			{
				Expression->Modify();
			}
		}
		if (Graph.Material)
		{
			UMaterialEditingLibrary::LayoutMaterialExpressions(Graph.Material);
		}
		else
		{
			UMaterialEditingLibrary::LayoutMaterialFunctionExpressions(Graph.Function);
		}
	}
	MarkChanged(Graph);
	return DescribeGraph(Graph);
}

FMaterialMcpCompileResult UMaterialMcpEditorTools::CompileMaterial(UObject* Material, bool bSave)
{
	FMaterialMcpCompileResult Result;
	FGraph Graph;
	if (!ResolveGraph(Material, false, Graph))
	{
		return Result;
	}
	Result.MaterialPath = Graph.Asset()->GetPathName();

	if (UMaterial* Target = Graph.Material)
	{
		// Function call nodes keep a copy of the called function's pins; refresh them in case the function changed.
		for (UMaterialExpression* Expression : Target->GetExpressions())
		{
			if (UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression); Call && Call->MaterialFunction)
			{
				Call->UpdateFromFunctionResource();
			}
		}
		UMaterialEditingLibrary::RecompileMaterial(Target);
		if (FMaterialResource* Resource = Target->GetMaterialResource(GMaxRHIShaderPlatform))
		{
			Resource->FinishCompilation();
			Result.Errors = Resource->GetCompileErrors();
		}
	}
	else
	{
		UMaterialFunction* Function = Graph.Function;
		const TArray<UMaterial*> Dependents = FindDependentMaterials(Function);
		for (UMaterial* Dependent : Dependents)
		{
			for (UMaterialExpression* Expression : Dependent->GetExpressions())
			{
				if (UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression); Call && Call->MaterialFunction == Function)
				{
					Call->UpdateFromFunctionResource();
				}
			}
		}
		// Refreshes the function's input / output types and state id, and marks the materials using it for recompile.
		UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
		for (UMaterial* Dependent : Dependents)
		{
			UMaterialEditingLibrary::RecompileMaterial(Dependent);
			Result.DependentMaterials.Add(Dependent->GetPathName());
			if (FMaterialResource* Resource = Dependent->GetMaterialResource(GMaxRHIShaderPlatform))
			{
				Resource->FinishCompilation();
				for (const FString& Error : Resource->GetCompileErrors())
				{
					Result.Errors.Add(FString::Printf(TEXT("%s: %s"), *Dependent->GetName(), *Error));
				}
			}
		}
	}
	Result.bSucceeded = Result.Errors.Num() == 0;
	if (bSave)
	{
		Result.bSaved = SaveAssetSilently(Graph.Asset());
	}
	return Result;
}

FToolsetImage UMaterialMcpEditorTools::CapturePreview(UMaterialInterface* Material, const FString& Mesh, int32 Size, int32 FrameCount, float FrameInterval)
{
	FToolsetImage Image;
	if (Material == nullptr)
	{
		ReportError(TEXT("Material is null or could not be loaded. Pass a material or material instance (material functions cannot be rendered on their own)."));
		return Image;
	}
	FString PreviewError;
	if (!NiagaraMcp::Preview::CaptureMaterial(*Material, Mesh, Size, NiagaraMcp::Preview::FCameraSettings(), FrameCount, FrameInterval, Image, PreviewError))
	{
		ReportError(PreviewError);
		return FToolsetImage();
	}
	return Image;
}

FMaterialMcpValidation UMaterialMcpEditorTools::ValidateMaterial(UObject* Material, bool bCompile)
{
	FMaterialMcpValidation Result;
	FGraph Graph;
	if (!ResolveGraph(Material, false, Graph))
	{
		return Result;
	}
	Result.MaterialPath = Graph.Asset()->GetPathName();
	auto Add = [&Result](const TCHAR* Severity, const FString& Message, const FString& Expression)
	{
		Result.Issues.Add({ Severity, Message, Expression });
	};

	// Compile errors. Without bCompile only the results of the last compile are read (nothing is dirtied).
	if (bCompile)
	{
		const FMaterialMcpCompileResult Compile = CompileMaterial(Graph.Asset(), false);
		for (const FString& CompileError : Compile.Errors)
		{
			Add(TEXT("Error"), CompileError, FString());
		}
		if (Graph.Function && Compile.DependentMaterials.Num() == 0)
		{
			Add(TEXT("Info"), TEXT("No loaded material uses this function, so its shader code was not compiled. Call it from a material and compile that material to check it."), FString());
		}
	}
	else if (Graph.Material)
	{
		TArray<FString> Errors;
		bool bCompiled = false;
		ReadExistingCompileErrors(Graph.Material, Errors, bCompiled);
		for (const FString& CompileError : Errors)
		{
			Add(TEXT("Error"), CompileError, FString());
		}
		if (!bCompiled)
		{
			Add(TEXT("Info"), TEXT("The material is not compiled yet; pass bCompile=true (or run CompileMaterial) to check its shaders."), FString());
		}
		else
		{
			Add(TEXT("Info"), TEXT("Compile errors are from the last compile; pass bCompile=true after graph edits."), FString());
		}
	}
	else
	{
		const TArray<UMaterial*> Dependents = FindDependentMaterials(Graph.Function);
		for (UMaterial* Dependent : Dependents)
		{
			TArray<FString> Errors;
			bool bCompiled = false;
			ReadExistingCompileErrors(Dependent, Errors, bCompiled);
			for (const FString& CompileError : Errors)
			{
				Add(TEXT("Error"), FString::Printf(TEXT("%s: %s"), *Dependent->GetName(), *CompileError), FString());
			}
		}
		Add(TEXT("Info"), Dependents.Num() > 0
			? FString::Printf(TEXT("Compile errors are read from the last compile of the %d loaded material(s) using this function; pass bCompile=true after graph edits."), Dependents.Num())
			: FString(TEXT("No loaded material uses this function, so no compile errors are known. Call it from a material and compile that material to check it.")), FString());
	}

	// Reachability from the outputs: material outputs, or the FunctionOutput nodes of a function.
	TSet<UMaterialExpression*> Reached;
	TArray<UMaterialExpression*> Pending;
	if (UMaterial* Target = Graph.Material)
	{
		// Only the user-facing material outputs: querying internal properties (MP_MaterialAttributes, CustomOutput...)
		// dereferences inputs that do not exist on every material.
		static const EMaterialProperty Outputs[] =
		{
			MP_BaseColor, MP_Metallic, MP_Specular, MP_Roughness, MP_Anisotropy, MP_EmissiveColor, MP_Opacity, MP_OpacityMask,
			MP_Normal, MP_Tangent, MP_WorldPositionOffset, MP_SubsurfaceColor, MP_AmbientOcclusion, MP_Refraction,
			MP_PixelDepthOffset, MP_ShadingModel, MP_CustomData0, MP_CustomData1, MP_Displacement,
		};
		for (EMaterialProperty Property : Outputs)
		{
			if (UMaterialExpression* Source = UMaterialEditingLibrary::GetMaterialPropertyInputNode(Target, Property))
			{
				Pending.Add(Source);
			}
		}
		if (Target->bUseMaterialAttributes)
		{
			if (const FExpressionInput* Attributes = Target->GetExpressionInputForProperty(MP_MaterialAttributes); Attributes && Attributes->Expression)
			{
				Pending.Add(Attributes->Expression);
			}
		}
		if (Pending.Num() == 0)
		{
			Add(TEXT("Warning"), TEXT("No material output is connected."), FString());
		}
	}
	else
	{
		const TArray<UMaterialExpression*> FunctionOutputs = FunctionPinExpressions(Graph, false);
		if (FunctionOutputs.Num() == 0)
		{
			Add(TEXT("Error"), TEXT("The material function has no FunctionOutput node."), FString());
		}
		for (UMaterialExpression* Output : FunctionOutputs)
		{
			Pending.Add(Output);
			const FExpressionInput* Input = Output->GetInput(0);
			if (Input == nullptr || Input->Expression == nullptr)
			{
				Add(TEXT("Error"), FString::Printf(TEXT("Function output '%s' is not connected."), *ExpressionLabel(Output)), Output->GetName());
			}
		}
		for (const bool bInputs : { true, false })
		{
			TMap<FString, int32> Counts;
			for (const UMaterialExpression* Pin : FunctionPinExpressions(Graph, bInputs))
			{
				++Counts.FindOrAdd(ExpressionLabel(Pin).ToLower());
			}
			for (const TPair<FString, int32>& Entry : Counts)
			{
				if (Entry.Value > 1)
				{
					Add(TEXT("Error"), FString::Printf(TEXT("%d function %s are named '%s'; names must be unique."), Entry.Value, bInputs ? TEXT("inputs") : TEXT("outputs"), *Entry.Key), FString());
				}
			}
		}
	}
	while (Pending.Num() > 0)
	{
		UMaterialExpression* Expression = Pending.Pop();
		if (Expression == nullptr || Reached.Contains(Expression))
		{
			continue;
		}
		Reached.Add(Expression);
		for (FExpressionInputIterator It{ Expression }; It; ++It)
		{
			if (It->Expression)
			{
				Pending.Add(It->Expression);
			}
		}
	}

	TMap<FString, TArray<UMaterialExpression*>> ByParameter;
	for (UMaterialExpression* Expression : Graph.Expressions())
	{
		if (Expression == nullptr || Expression->GetClass()->GetName().Contains(TEXT("Comment")))
		{
			continue;
		}
		if (!Reached.Contains(Expression))
		{
			Add(TEXT("Warning"), Expression->IsA<UMaterialExpressionFunctionInput>()
				? FString::Printf(TEXT("Function input '%s' is not used by any output."), *ExpressionLabel(Expression))
				: FString(Graph.Material ? TEXT("Expression is not connected to any material output.") : TEXT("Expression is not connected to any function output.")),
				Expression->GetName());
		}
		const FName ParameterName = Expression->GetParameterName();
		if (!ParameterName.IsNone())
		{
			ByParameter.FindOrAdd(ParameterName.ToString()).Add(Expression);
		}
		if (FindFProperty<FObjectPropertyBase>(Expression->GetClass(), TEXT("Texture")))
		{
			const FString Texture = GetPropertyText(Expression->GetClass(), Expression, TEXT("Texture"));
			const UMaterialExpressionTextureSample* Sample = Cast<UMaterialExpressionTextureSample>(Expression);
			const bool bFedByObject = Sample && Sample->TextureObject.Expression;
			if (!bFedByObject && (Texture.IsEmpty() || Texture.Equals(TEXT("None"), ESearchCase::IgnoreCase)))
			{
				Add(TEXT("Error"), TEXT("Texture expression has no texture."), Expression->GetName());
			}
		}
		if (const UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expression))
		{
			for (const FCustomInput& Input : Custom->Inputs)
			{
				if (Input.Input.Expression == nullptr)
				{
					Add(TEXT("Warning"), FString::Printf(TEXT("Custom input '%s' is not connected (it reads 0)."), *Input.InputName.ToString()), Expression->GetName());
				}
			}
		}
		if (const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
		{
			if (Call->MaterialFunction == nullptr)
			{
				Add(TEXT("Error"), TEXT("MaterialFunctionCall has no function; set its MaterialFunction property."), Expression->GetName());
			}
			for (const FFunctionExpressionInput& Input : Call->FunctionInputs)
			{
				const UMaterialExpressionFunctionInput* Declared = Input.ExpressionInput;
				if (Input.Input.Expression == nullptr && Declared && !Declared->bUsePreviewValueAsDefault)
				{
					Add(TEXT("Error"), FString::Printf(TEXT("Function input '%s' is not connected and has no default (set bUsePreviewValueAsDefault on the function's input to make it optional)."),
						*Declared->InputName.ToString()), Expression->GetName());
				}
			}
		}
	}
	for (const TPair<FString, TArray<UMaterialExpression*>>& Entry : ByParameter)
	{
		if (Entry.Value.Num() > 1)
		{
			const bool bSameClass = !Entry.Value.ContainsByPredicate([&Entry](const UMaterialExpression* E) { return E->GetClass() != Entry.Value[0]->GetClass(); });
			Add(bSameClass ? TEXT("Info") : TEXT("Error"),
				FString::Printf(TEXT("Parameter '%s' is used by %d expressions%s."), *Entry.Key, Entry.Value.Num(), bSameClass ? TEXT(" (they share one value)") : TEXT(" of different types")),
				Entry.Value[0]->GetName());
		}
	}

	Result.bOk = !Result.Issues.ContainsByPredicate([](const FMaterialMcpIssue& Issue) { return Issue.Severity == TEXT("Error"); });
	return Result;
}

FMaterialMcpStats UMaterialMcpEditorTools::GetMaterialStats(UMaterialInterface* Material)
{
	FMaterialMcpStats Stats;
	if (Material == nullptr)
	{
		ReportError(TEXT("Material is null or could not be loaded."));
		return Stats;
	}
	Stats.MaterialPath = Material->GetPathName();
	const FMaterialStatistics Raw = UMaterialEditingLibrary::GetStatistics(Material);
	Stats.PixelShaderInstructions = Raw.NumPixelShaderInstructions;
	Stats.VertexShaderInstructions = Raw.NumVertexShaderInstructions;
	Stats.Samplers = Raw.NumSamplers;
	Stats.PixelTextureSamples = Raw.NumPixelTextureSamples;
	Stats.VertexTextureSamples = Raw.NumVertexTextureSamples;
	Stats.VirtualTextureSamples = Raw.NumVirtualTextureSamples;
	Stats.InterpolatorScalars = Raw.NumInterpolatorScalars;
	return Stats;
}

FMaterialMcpParameters UMaterialMcpEditorTools::GetMaterialParameters(UMaterialInterface* Material)
{
	if (Material == nullptr)
	{
		ReportError(TEXT("Material is null or could not be loaded."));
		return {};
	}
	return DescribeParameters(Material);
}

FMaterialMcpParameters UMaterialMcpEditorTools::CreateMaterialInstance(UMaterialInterface* Parent, const FString& AssetPath, bool bSave)
{
	if (Parent == nullptr)
	{
		ReportError(TEXT("Parent is null or could not be loaded."));
		return {};
	}
	FString AssetName;
	UPackage* Package = CreateNewAssetPackage(AssetPath, AssetName);
	if (Package == nullptr)
	{
		return {};
	}
	// No transaction: undoing a recorded NewObject leaves an empty, broken asset in memory that crashes the next
	// tool using it (the editor's asset factories do not record asset creation either).
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	Instance->SetParentEditorOnly(Parent);
	Instance->PostEditChange();
	FAssetRegistryModule::AssetCreated(Instance);
	Package->MarkPackageDirty();

	FMaterialMcpParameters Info = DescribeParameters(Instance);
	Info.bSaved = bSave && SaveAssetSilently(Instance);
	return Info;
}

FMaterialMcpParameters UMaterialMcpEditorTools::SetMaterialInstanceParameter(UMaterialInstanceConstant* Instance, const FString& ParameterName, const FString& Value, bool bSave)
{
	FMaterialMcpParameterValue Entry;
	Entry.Name = ParameterName;
	Entry.Value = Value;
	return SetMaterialInstanceParameters(Instance, { Entry }, bSave);
}

FMaterialMcpParameters UMaterialMcpEditorTools::SetMaterialInstanceParameters(UMaterialInstanceConstant* Instance, const TArray<FMaterialMcpParameterValue>& Parameters, bool bSave)
{
	if (Instance == nullptr)
	{
		ReportError(TEXT("Instance is null or not a material instance constant."));
		return {};
	}
	if (!EnsureNoOpenEditor(Instance))
	{
		return {};
	}
	if (Parameters.Num() == 0)
	{
		ReportError(TEXT("Parameters is empty. Pass Name / Value pairs, e.g. [{\"name\": \"Tint\", \"value\": \"1, 0.3, 0.1, 1\"}]."));
		return {};
	}

	// Parse and check every entry first; nothing is written unless all of them are valid.
	const FMaterialMcpParameters Before = DescribeParameters(Instance);
	TArray<FParameterValue> Values;
	TArray<FString> Problems;
	TSet<FName> Seen;
	for (const FMaterialMcpParameterValue& Entry : Parameters)
	{
		FParameterValue Parsed;
		FString ParseError;
		if (!ParseParameterValue(Before, Entry.Name, Entry.Value, Parsed, ParseError))
		{
			Problems.Add(ParseError);
			continue;
		}
		if (Seen.Contains(Parsed.Name))
		{
			Problems.Add(FString::Printf(TEXT("Parameter '%s' is listed more than once."), *Parsed.Name.ToString()));
			continue;
		}
		Seen.Add(Parsed.Name);
		Values.Add(Parsed);
	}
	if (Problems.Num() > 0)
	{
		ReportError(FString::Printf(TEXT("Nothing was changed on '%s': %s"), *Instance->GetName(), *FString::Join(Problems, TEXT(" "))));
		return {};
	}

	FScopedTransaction Transaction(LOCTEXT("SetMaterialInstanceParameters", "Material MCP: Set Material Instance Parameters"));
	Instance->Modify();
	struct FBackup
	{
		FParameterValue Value;
		bool bOverridden = false;
	};
	TArray<FBackup> Backups;
	for (const FParameterValue& Value : Values)
	{
		Backups.Add({ ReadParameterValue(Instance, Value), UMaterialEditingLibrary::IsMaterialInstanceParameterOverridden(Instance, Value.Name) });
	}

	for (int32 Index = 0; Index < Values.Num(); ++Index)
	{
		if (!ApplyParameterValue(Instance, Values[Index]))
		{
			// A cancelled transaction does not undo anything: put the previous values back explicitly.
			for (int32 Undo = Index; Undo >= 0; --Undo)
			{
				if (Backups[Undo].bOverridden)
				{
					ApplyParameterValue(Instance, Backups[Undo].Value);
				}
				else
				{
					UMaterialEditingLibrary::SetMaterialInstanceParameterOverride(Instance, Values[Undo].Name, false);
				}
			}
			UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
			ReportError(FString::Printf(TEXT("Could not set %s parameter '%s' on '%s' (the value did not read back); no parameter was changed."),
				*Values[Index].Type, *Values[Index].Name.ToString(), *Instance->GetName()));
			return {};
		}
	}
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);

	FMaterialMcpParameters Info = DescribeParameters(Instance);
	Info.bSaved = bSave && SaveAssetSilently(Instance);
	return Info;
}

FMaterialMcpParameters UMaterialMcpEditorTools::SetMaterialInstanceParent(UMaterialInstanceConstant* Instance, UMaterialInterface* Parent, bool bSave)
{
	if (Instance == nullptr)
	{
		ReportError(TEXT("Instance is null or not a material instance constant."));
		return {};
	}
	if (Parent == nullptr)
	{
		ReportError(TEXT("Parent is null or could not be loaded. Pass a material or material instance path."));
		return {};
	}
	if (!EnsureNoOpenEditor(Instance))
	{
		return {};
	}
	for (UMaterialInterface* Ancestor = Parent; Ancestor; )
	{
		if (Ancestor == Instance)
		{
			ReportError(FString::Printf(TEXT("'%s' is '%s' itself or one of its children; a material instance cannot be its own ancestor."), *Parent->GetPathName(), *Instance->GetPathName()));
			return {};
		}
		const UMaterialInstance* AncestorInstance = Cast<UMaterialInstance>(Ancestor);
		Ancestor = AncestorInstance ? AncestorInstance->Parent.Get() : nullptr;
	}

	FScopedTransaction Transaction(LOCTEXT("SetMaterialInstanceParent", "Material MCP: Set Material Instance Parent"));
	Instance->Modify();
	UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Parent);
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	if (Instance->Parent != Parent)
	{
		ReportError(FString::Printf(TEXT("Could not set the parent of '%s' to '%s'."), *Instance->GetPathName(), *Parent->GetPathName()));
		return {};
	}

	FMaterialMcpParameters Info = DescribeParameters(Instance);
	Info.bSaved = bSave && SaveAssetSilently(Instance);
	return Info;
}

FMaterialMcpParameters UMaterialMcpEditorTools::ClearMaterialInstanceParameters(UMaterialInstanceConstant* Instance, const TArray<FString>& ParameterNames, bool bSave)
{
	if (Instance == nullptr)
	{
		ReportError(TEXT("Instance is null or not a material instance constant."));
		return {};
	}
	if (!EnsureNoOpenEditor(Instance))
	{
		return {};
	}
	const FMaterialMcpParameters Before = DescribeParameters(Instance);
	TArray<FName> Targets;
	TArray<FString> Problems;
	for (const FString& Name : ParameterNames)
	{
		FString FindError;
		if (const FMaterialMcpParameter* Parameter = FindParameter(Before, Name, FindError))
		{
			if (Parameter->bOverridden)
			{
				Targets.AddUnique(FName(*Parameter->Name));
			}
		}
		else
		{
			Problems.Add(FindError);
		}
	}
	if (Problems.Num() > 0)
	{
		ReportError(FString::Printf(TEXT("Nothing was changed on '%s': %s"), *Instance->GetName(), *FString::Join(Problems, TEXT(" "))));
		return {};
	}
	if (ParameterNames.Num() == 0)
	{
		for (const FMaterialMcpParameter& Parameter : Before.Parameters)
		{
			if (Parameter.bOverridden)
			{
				Targets.AddUnique(FName(*Parameter.Name));
			}
		}
	}

	if (Targets.Num() > 0)
	{
		FScopedTransaction Transaction(LOCTEXT("ClearMaterialInstanceParameters", "Material MCP: Clear Material Instance Parameters"));
		Instance->Modify();
		for (const FName& Name : Targets)
		{
			UMaterialEditingLibrary::SetMaterialInstanceParameterOverride(Instance, Name, false);
		}
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	}

	FMaterialMcpParameters Info = DescribeParameters(Instance);
	TArray<FString> StillOverridden;
	for (const FMaterialMcpParameter& Parameter : Info.Parameters)
	{
		if (Parameter.bOverridden && Targets.Contains(FName(*Parameter.Name)))
		{
			StillOverridden.Add(Parameter.Name);
		}
	}
	if (StillOverridden.Num() > 0)
	{
		ReportError(FString::Printf(TEXT("Could not clear the override of %s on '%s'."), *FString::Join(StillOverridden, TEXT(", ")), *Instance->GetName()));
		return {};
	}
	Info.bSaved = bSave && SaveAssetSilently(Instance);
	return Info;
}

#undef LOCTEXT_NAMESPACE
