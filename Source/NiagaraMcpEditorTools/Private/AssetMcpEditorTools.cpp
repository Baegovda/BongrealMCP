#include "AssetMcpEditorTools.h"

#include "Algo/LevenshteinDistance.h"
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "IAssetTools.h"
#include "ImageCore.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/StringOutputDevice.h"
#include "NiagaraEffectType.h"
#include "NiagaraEmitter.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraParameterCollection.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "RenderUtils.h"
#include "RenderingThread.h"
#include "ScopedTransaction.h"
#include "ShaderCompiler.h"
#include "TextureCompiler.h"
#include "UObject/UnrealType.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AssetMcpEditorTools)

#define LOCTEXT_NAMESPACE "AssetMcpEditorTools"

using namespace NiagaraMcp;

/**
 * File-local helpers. They live in a named namespace (not an anonymous one) and are called qualified from the tool
 * bodies, so unity builds that also pull in other toolsets' "using namespace" directives cannot make them ambiguous.
 */
namespace AssetMcp
{
	// Above 2048 px UTextureRenderTarget2D::PostEditChangeProperty asks a modal "are you sure" question, and Undo/Redo
	// run that PostEditChange too (UObject::PostEditUndo). A modal blocks every MCP request, so stay at or below it.
	static constexpr int32 MaxRenderTargetSize = 2048;
	static constexpr int32 MaxAtlasSize = 16384;
	static constexpr int32 MaxValueTextLength = 1024;

	/** Shared helpers (NiagaraMcpCommon): optional-argument words and did-you-mean suggestions. */
	static bool IsUnsetArg(const FString& S)
	{
		return NiagaraMcp::IsUnset(S);
	}

	FString UnknownNameMessage(const FString& What, const FString& Query, const TArray<FString>& Candidates)
	{
		if (Candidates.Num() == 0)
		{
			return FString::Printf(TEXT("Unknown %s '%s'. There are none."), *What, *Query);
		}
		return FString::Printf(TEXT("Unknown %s '%s'.%s Valid: %s"), *What, *Query,
			*NiagaraMcp::DidYouMean(Query, Candidates), *FString::Join(Candidates, TEXT(", ")));
	}

	bool IsEditableProperty(const FProperty* Property)
	{
		return Property && Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated);
	}

	TArray<FString> EditablePropertyNames(const UStruct* Struct)
	{
		TArray<FString> Names;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (IsEditableProperty(*It))
			{
				Names.Add(It->GetName());
			}
		}
		return Names;
	}

	/** Exact name, then name / display name ignoring case, spaces and underscores, then bool "b" prefix. */
	FProperty* FindPropertyByLooseName(const UStruct* Struct, const FString& Name)
	{
		if (FProperty* Exact = Struct->FindPropertyByName(FName(*Name)))
		{
			return Exact;
		}
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FString DisplayName = It->GetDisplayNameText().ToString();
			if (LooseNameEquals(It->GetName(), Name) || (!DisplayName.IsEmpty() && LooseNameEquals(DisplayName, Name))
				|| (CastField<FBoolProperty>(*It) && LooseNameEquals(It->GetName(), TEXT("b") + Name)))
			{
				return *It;
			}
		}
		return nullptr;
	}

	/** Splits "Settings[2]" into "Settings" and 2. Index is INDEX_NONE without brackets. */
	bool ParsePathSegment(const FString& Segment, FString& OutName, int32& OutIndex, FString& OutError)
	{
		OutIndex = INDEX_NONE;
		int32 Open = INDEX_NONE;
		if (!Segment.FindChar(TEXT('['), Open))
		{
			OutName = Segment.TrimStartAndEnd();
			return !OutName.IsEmpty();
		}
		const FString IndexText = Segment.Mid(Open + 1).TrimEnd();
		if (!IndexText.EndsWith(TEXT("]")))
		{
			OutError = FString::Printf(TEXT("'%s' is missing a closing ']'."), *Segment);
			return false;
		}
		const FString Number = IndexText.LeftChop(1).TrimStartAndEnd();
		if (Number.IsEmpty() || !Number.IsNumeric() || Number.Contains(TEXT(".")) || Number.StartsWith(TEXT("-")))
		{
			OutError = FString::Printf(TEXT("'%s' has an invalid array index. Use Name[0]."), *Segment);
			return false;
		}
		OutName = Segment.Left(Open).TrimStartAndEnd();
		OutIndex = FCString::Atoi(*Number);
		return !OutName.IsEmpty();
	}

	/** A property path resolved against live memory. */
	struct FResolvedProperty
	{
		/** Properties from the object's member property down to the leaf (for the edit property chain). */
		TArray<FProperty*> Chain;

		/** Leaf property as named in the path (the array property when the last segment is indexed). */
		FProperty* Leaf = nullptr;

		/** Struct / class that declares Leaf, and the memory that contains it (set when the leaf is not indexed). */
		const UStruct* LeafOwner = nullptr;
		void* LeafContainer = nullptr;

		/** Property describing the value and the value memory (an array element when indexed). */
		FProperty* ValueProperty = nullptr;
		void* ValuePtr = nullptr;

		bool bIndexedLeaf = false;
	};

	/**
	 * Walks "A.B[2].C" from an object. With bAllowAppend, Name[Count] appends a default element to a dynamic array
	 * (callers snapshot the member property first, see SetAssetProperty).
	 */
	bool ResolvePropertyPath(UObject* Object, const FString& Path, bool bAllowAppend, FResolvedProperty& Out, FString& OutError)
	{
		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("."));
		if (Segments.Num() == 0)
		{
			OutError = TEXT("PropertyName is empty.");
			return false;
		}

		const UStruct* Struct = Object->GetClass();
		void* Container = Object;
		for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
		{
			FString Name;
			int32 Index = INDEX_NONE;
			if (!ParsePathSegment(Segments[SegmentIndex], Name, Index, OutError))
			{
				if (OutError.IsEmpty())
				{
					OutError = FString::Printf(TEXT("'%s' is not a valid property path."), *Path);
				}
				return false;
			}

			FProperty* Property = FindPropertyByLooseName(Struct, Name);
			if (Property == nullptr)
			{
				OutError = UnknownNameMessage(FString::Printf(TEXT("property of %s"), *Struct->GetName()), Name, EditablePropertyNames(Struct));
				return false;
			}
			if (!IsEditableProperty(Property))
			{
				OutError = FString::Printf(TEXT("'%s' on %s is not editable (read-only, deprecated or internal). Editable: %s"),
					*Property->GetName(), *Struct->GetName(), *FString::Join(EditablePropertyNames(Struct), TEXT(", ")));
				return false;
			}
			Out.Chain.Add(Property);
			const bool bLast = SegmentIndex == Segments.Num() - 1;

			FProperty* ElementProperty = Property;
			void* ElementPtr = nullptr;
			if (Index != INDEX_NONE)
			{
				if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
				{
					FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Container));
					if (bAllowAppend && Index == Helper.Num())
					{
						Helper.AddValue();
					}
					if (Index >= Helper.Num())
					{
						OutError = FString::Printf(TEXT("Index %d is out of range: '%s' has %d elements (use index %d to append one)."), Index, *Property->GetName(), Helper.Num(), Helper.Num());
						return false;
					}
					ElementProperty = ArrayProperty->Inner;
					ElementPtr = Helper.GetRawPtr(Index);
				}
				else if (Property->ArrayDim > 1)
				{
					if (Index >= Property->ArrayDim)
					{
						OutError = FString::Printf(TEXT("Index %d is out of range: '%s' has %d elements."), Index, *Property->GetName(), Property->ArrayDim);
						return false;
					}
					ElementPtr = Property->ContainerPtrToValuePtr<void>(Container, Index);
				}
				else
				{
					OutError = FString::Printf(TEXT("'%s' is not an array, so [%d] cannot be used."), *Property->GetName(), Index);
					return false;
				}
			}
			else
			{
				ElementPtr = Property->ContainerPtrToValuePtr<void>(Container);
			}

			if (bLast)
			{
				Out.Leaf = Property;
				Out.ValueProperty = ElementProperty;
				Out.ValuePtr = ElementPtr;
				Out.bIndexedLeaf = Index != INDEX_NONE;
				Out.LeafOwner = Struct;
				Out.LeafContainer = Container;
				return true;
			}

			FStructProperty* StructProperty = CastField<FStructProperty>(ElementProperty);
			if (StructProperty == nullptr)
			{
				OutError = FString::Printf(TEXT("'%s' is not a struct, so '%s' cannot be reached through it."), *Segments[SegmentIndex], *Path);
				return false;
			}
			Struct = StructProperty->Struct;
			Container = ElementPtr;
		}
		return false;
	}

	/** True when every leaf of the struct is a number (FVector, FLinearColor, FIntPoint...). */
	bool IsNumericStruct(const UStruct* Struct, int32 Depth = 0)
	{
		if (Struct == nullptr || Depth > 3)
		{
			return false;
		}
		int32 Count = 0;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			++Count;
			if (CastField<FNumericProperty>(*It) && !CastField<FByteProperty>(*It))
			{
				continue;
			}
			const FStructProperty* Inner = CastField<FStructProperty>(*It);
			if (Inner == nullptr || !IsNumericStruct(Inner->Struct, Depth + 1))
			{
				return false;
			}
		}
		return Count > 0;
	}

	FString ValueToText(const FProperty* Property, const void* ValuePtr)
	{
		if (Property == nullptr || ValuePtr == nullptr)
		{
			return FString();
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return EnumValueToString(EnumProperty->GetEnum(), EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr));
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property); ByteProperty && ByteProperty->Enum)
		{
			return EnumValueToString(ByteProperty->Enum, ByteProperty->GetPropertyValue(ValuePtr));
		}
		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			return BoolProperty->GetPropertyValue(ValuePtr) ? TEXT("true") : TEXT("false");
		}
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			const UObject* Object = ObjectProperty->GetObjectPropertyValue(ValuePtr);
			return Object ? Object->GetPathName() : FString(TEXT("None"));
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property); StructProperty && IsNumericStruct(StructProperty->Struct))
		{
			return ExportStructValue(StructProperty->Struct, ValuePtr, nullptr);
		}
		FString Out;
		Property->ExportTextItem_Direct(Out, ValuePtr, nullptr, nullptr, PPF_None);
		return Out;
	}

	FString TruncateValue(const FString& Text)
	{
		return Text.Len() <= MaxValueTextLength ? Text : Text.Left(MaxValueTextLength) + FString::Printf(TEXT("... (%d characters)"), Text.Len());
	}

	TArray<FString> EnumEntryNames(const UEnum* Enum)
	{
		TArray<FString> Names;
		for (int32 Index = 0; Enum && Index < Enum->NumEnums() - 1; ++Index) // last entry is _MAX
		{
			if (!Enum->HasMetaData(TEXT("Hidden"), Index))
			{
				Names.Add(EnumValueToString(Enum, Enum->GetValueByIndex(Index)));
			}
		}
		return Names;
	}

	/** Sets one array element (or static array entry) from text. */
	bool SetValueFromText(FProperty* Property, void* ValuePtr, UObject* Owner, const FString& Value, FString& OutError)
	{
		const FString Trimmed = Value.TrimStartAndEnd();
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property); StructProperty && !Trimmed.StartsWith(TEXT("(")))
		{
			FString StructError;
			if (ImportStructValue(StructProperty->Struct, ValuePtr, Trimmed, nullptr, StructError))
			{
				return true;
			}
		}
		const UEnum* Enum = nullptr;
		FNumericProperty* Underlying = nullptr;
		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			Enum = EnumProperty->GetEnum();
			Underlying = EnumProperty->GetUnderlyingProperty();
		}
		else if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property); ByteProperty && ByteProperty->Enum)
		{
			Enum = ByteProperty->Enum;
			Underlying = ByteProperty;
		}
		if (Enum && Underlying)
		{
			int64 EnumValue = 0;
			if (!ParseEnumText(Enum, Trimmed, EnumValue))
			{
				OutError = FString::Printf(TEXT("'%s' is not an entry of %s. Entries: %s"), *Trimmed, *Enum->GetName(), *FString::Join(EnumEntryNames(Enum), TEXT(", ")));
				return false;
			}
			Underlying->SetIntPropertyValue(ValuePtr, EnumValue);
			return true;
		}
		FStringOutputDevice Errors;
		if (Property->ImportText_Direct(*Trimmed, ValuePtr, Owner, PPF_None, &Errors) == nullptr || !Errors.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Could not set '%s' to '%s'. %s"), *Property->GetName(), *Trimmed, *Errors);
			return false;
		}
		return true;
	}

	/** Resolves the path (appending array elements if asked) and writes the value. No notifications. */
	bool ApplyPropertyText(UObject* Asset, const FString& Path, const FString& Value, FResolvedProperty& Out, FString& OutError)
	{
		Out = FResolvedProperty();
		if (!ResolvePropertyPath(Asset, Path, true, Out, OutError))
		{
			return false;
		}
		if (Out.bIndexedLeaf)
		{
			return SetValueFromText(Out.ValueProperty, Out.ValuePtr, Asset, Value, OutError);
		}
		return SetPropertyFromText(const_cast<UStruct*>(Out.LeafOwner), Out.LeafContainer, Asset, Out.Leaf->GetName(), Value, OutError) != nullptr;
	}

	/** Graph assets whose editors work on a copy: edits made behind an open editor are lost on its next apply. */
	bool UsesCopyingEditor(const UObject* Asset)
	{
		return Asset->IsA<UMaterial>() || Asset->IsA<UMaterialFunctionInterface>() || Asset->IsA<UNiagaraSystem>()
			|| Asset->IsA<UNiagaraEmitter>() || Asset->IsA<UNiagaraScript>();
	}

	/** Copy of a property value (all ArrayDim entries), restored on demand. */
	struct FPropertySnapshot
	{
		FPropertySnapshot(FProperty* InProperty, void* InValuePtr)
			: Property(InProperty), ValuePtr(InValuePtr)
		{
			Memory = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
			Property->InitializeValue(Memory);
			Property->CopyCompleteValue(Memory, ValuePtr);
		}
		~FPropertySnapshot()
		{
			Property->DestroyValue(Memory);
			FMemory::Free(Memory);
		}
		void Restore() const
		{
			Property->CopyCompleteValue(ValuePtr, Memory);
		}

		FProperty* Property;
		void* ValuePtr;
		void* Memory = nullptr;
	};

	bool ParseCompression(const FString& Text, TextureCompressionSettings& OutSettings)
	{
		const UEnum* Enum = StaticEnum<TextureCompressionSettings>();
		const FString Trimmed = Text.TrimStartAndEnd();
		int64 Value = 0;
		if (ParseEnumText(Enum, Trimmed, Value) || ParseEnumText(Enum, TEXT("TC_") + Trimmed, Value))
		{
			OutSettings = static_cast<TextureCompressionSettings>(Value);
			return true;
		}
		ReportError(UnknownNameMessage(TEXT("compression setting"), Trimmed, EnumEntryNames(Enum)));
		return false;
	}

	bool ParseRenderTargetFormat(const FString& Text, ETextureRenderTargetFormat& OutFormat)
	{
		const UEnum* Enum = StaticEnum<ETextureRenderTargetFormat>();
		const FString Trimmed = Text.TrimStartAndEnd();
		int64 Value = 0;
		if (ParseEnumText(Enum, Trimmed, Value) || ParseEnumText(Enum, TEXT("RTF_") + Trimmed, Value))
		{
			OutFormat = static_cast<ETextureRenderTargetFormat>(Value);
			return true;
		}
		ReportError(UnknownNameMessage(TEXT("render target format"), Trimmed, EnumEntryNames(Enum)));
		return false;
	}

	/** Splits "1, 0.5 0" into numbers. False when a part is not numeric. */
	bool ParseNumbers(const FString& Text, TArray<double>& OutNumbers)
	{
		TArray<FString> Parts;
		Text.Replace(TEXT(","), TEXT(" ")).Replace(TEXT(";"), TEXT(" ")).Replace(TEXT("("), TEXT(" ")).Replace(TEXT(")"), TEXT(" ")).ParseIntoArrayWS(Parts);
		OutNumbers.Reset();
		for (const FString& Part : Parts)
		{
			if (!Part.IsNumeric())
			{
				return false;
			}
			OutNumbers.Add(FCString::Atod(*Part));
		}
		return OutNumbers.Num() > 0;
	}

	/** "R, G, B" or "R, G, B, A" (alpha defaults to 1), or a single value for all four channels. */
	bool ParseColor(const FString& Text, FLinearColor& OutColor, const TCHAR* What)
	{
		TArray<double> Numbers;
		if (!ParseNumbers(Text, Numbers) || (Numbers.Num() != 1 && Numbers.Num() != 3 && Numbers.Num() != 4))
		{
			ReportError(FString::Printf(TEXT("%s '%s' is not a color. Use \"R, G, B\" or \"R, G, B, A\", e.g. \"0, 0, 0, 1\"."), What, *Text));
			return false;
		}
		if (Numbers.Num() == 1)
		{
			OutColor = FLinearColor(Numbers[0], Numbers[0], Numbers[0], Numbers[0]);
		}
		else
		{
			OutColor = FLinearColor(Numbers[0], Numbers[1], Numbers[2], Numbers.Num() == 4 ? Numbers[3] : 1.0);
		}
		return true;
	}

	FString ColorToText(const FLinearColor& Color)
	{
		return FString::Printf(TEXT("%s, %s, %s, %s"), *FString::SanitizeFloat(Color.R), *FString::SanitizeFloat(Color.G), *FString::SanitizeFloat(Color.B), *FString::SanitizeFloat(Color.A));
	}

	void WaitForTexture(UTexture* Texture)
	{
		if (Texture)
		{
			FTextureCompilingManager::Get().FinishCompilation({ Texture });
		}
	}

	FAssetMcpTextureInfo DescribeTexture(UTexture* Texture)
	{
		FAssetMcpTextureInfo Info;
		if (Texture == nullptr)
		{
			return Info;
		}
		WaitForTexture(Texture);
		UClass* Class = Texture->GetClass();
		Info.Path = Texture->GetPathName();
		Info.Class = Class->GetName();
		Info.bSRGB = Texture->SRGB;
		Info.CompressionSettings = GetPropertyText(Class, Texture, TEXT("CompressionSettings"));
		Info.LODGroup = GetPropertyText(Class, Texture, TEXT("LODGroup"));
		Info.MipGenSettings = GetPropertyText(Class, Texture, TEXT("MipGenSettings"));
		Info.AddressX = GetPropertyText(Class, Texture, TEXT("AddressX"));
		Info.AddressY = GetPropertyText(Class, Texture, TEXT("AddressY"));
		Info.Filter = GetPropertyText(Class, Texture, TEXT("Filter"));
		Info.bNeverStream = Texture->NeverStream;

		const UEnum* PixelFormatEnum = UTexture::GetPixelFormatEnum();
		if (UTextureRenderTarget2D* RenderTarget = Cast<UTextureRenderTarget2D>(Texture))
		{
			Info.Width = RenderTarget->SizeX;
			Info.Height = RenderTarget->SizeY;
			Info.PixelFormat = PixelFormatEnum ? PixelFormatEnum->GetNameStringByValue(static_cast<int64>(RenderTarget->GetFormat())) : FString();
			Info.NumMips = RenderTarget->bAutoGenerateMips ? FMath::FloorLog2(FMath::Max(RenderTarget->SizeX, RenderTarget->SizeY)) + 1 : 1;
			Info.RenderTargetFormat = GetPropertyText(Class, Texture, TEXT("RenderTargetFormat"));
			Info.ClearColor = ColorToText(RenderTarget->ClearColor);
			return Info;
		}
		if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
		{
			Info.Width = Texture2D->GetSizeX();
			Info.Height = Texture2D->GetSizeY();
			Info.PixelFormat = PixelFormatEnum ? PixelFormatEnum->GetNameStringByValue(static_cast<int64>(Texture2D->GetPixelFormat())) : FString();
			Info.NumMips = Texture2D->GetNumMips();
		}
		else
		{
			Info.Width = FMath::RoundToInt(Texture->GetSurfaceWidth());
			Info.Height = FMath::RoundToInt(Texture->GetSurfaceHeight());
		}
		if (Texture->Source.IsValid())
		{
			Info.SourceWidth = static_cast<int32>(Texture->Source.GetSizeX());
			Info.SourceHeight = static_cast<int32>(Texture->Source.GetSizeY());
			Info.SourceFormat = StaticEnum<ETextureSourceFormat>()->GetNameStringByValue(static_cast<int64>(Texture->Source.GetFormat()));
		}
		return Info;
	}

	FAssetMcpReference DescribePackage(IAssetRegistry& Registry, FName PackageName)
	{
		FAssetMcpReference Reference;
		Reference.PackageName = PackageName.ToString();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(PackageName, Assets);
		const FString ShortName = FPackageName::GetShortName(Reference.PackageName);
		const FAssetData* Main = Assets.FindByPredicate([&ShortName](const FAssetData& Data) { return Data.AssetName.ToString() == ShortName; });
		if (Main == nullptr && Assets.Num() > 0)
		{
			Main = &Assets[0];
		}
		if (Main)
		{
			Reference.AssetPath = Main->GetObjectPathString();
			Reference.Class = Main->AssetClassPath.GetAssetName().ToString();
		}
		return Reference;
	}

	bool IsValidNamespace(const FString& Namespace)
	{
		if (Namespace.IsEmpty() || FChar::IsDigit(Namespace[0]))
		{
			return false;
		}
		for (TCHAR C : Namespace)
		{
			if (!FChar::IsAlnum(C) && C != TEXT('_'))
			{
				return false;
			}
		}
		return true;
	}

	FAssetMcpMaterialParameterCollectionInfo DescribeMaterialCollection(UMaterialParameterCollection* Collection)
	{
		FAssetMcpMaterialParameterCollectionInfo Info;
		Info.Path = Collection->GetPathName();
		if (const UMaterialParameterCollection* Base = Collection->GetBaseParameterCollection())
		{
			Info.Base = Base->GetPathName();
		}
		for (const FCollectionScalarParameter& Parameter : Collection->ScalarParameters)
		{
			FAssetMcpCollectionParameter& Out = Info.Parameters.AddDefaulted_GetRef();
			Out.Name = Parameter.ParameterName.ToString();
			Out.Type = TEXT("Scalar");
			Out.Value = FString::SanitizeFloat(Parameter.DefaultValue);
		}
		for (const FCollectionVectorParameter& Parameter : Collection->VectorParameters)
		{
			FAssetMcpCollectionParameter& Out = Info.Parameters.AddDefaulted_GetRef();
			Out.Name = Parameter.ParameterName.ToString();
			Out.Type = TEXT("Vector");
			Out.Value = ColorToText(Parameter.DefaultValue);
		}
		return Info;
	}

	FAssetMcpNiagaraParameterCollectionInfo DescribeNiagaraCollection(UNiagaraParameterCollection* Collection)
	{
		FAssetMcpNiagaraParameterCollectionInfo Info;
		Info.Path = Collection->GetPathName();
		Info.Namespace = Collection->GetNamespace().ToString();
		if (const UMaterialParameterCollection* Source = Collection->GetSourceCollection())
		{
			Info.SourceMaterialCollection = Source->GetPathName();
		}
		const UNiagaraParameterCollectionInstance* Instance = Collection->GetDefaultInstance();
		for (const FNiagaraVariable& Parameter : Collection->GetParameters())
		{
			FAssetMcpCollectionParameter& Out = Info.Parameters.AddDefaulted_GetRef();
			Out.FullName = Parameter.GetName().ToString();
			Out.Name = Collection->FriendlyNameFromParameterName(Parameter.GetName()).ToString();
			const FNiagaraTypeDefinition& Type = Parameter.GetType();
			Out.Type = Type.GetName();
			const uint8* Data = Instance ? Instance->GetParameterStore().GetParameterData(Parameter) : nullptr;
			if (Data && Type.GetScriptStruct() && !Type.IsDataInterface() && !Type.IsUObject())
			{
				Out.Value = ExportStructValue(Type.GetScriptStruct(), Data, Type.GetEnum());
			}
		}
		return Info;
	}

	bool CheckArg(const UObject* Object, const TCHAR* What, const TCHAR* Example)
	{
		if (Object == nullptr)
		{
			ReportError(FString::Printf(TEXT("%s is null or could not be loaded. Pass an asset path such as %s."), What, Example));
			return false;
		}
		return true;
	}

	IAssetTools& GetAssetTools()
	{
		return FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	}
}

// ---------------------------------------------------------------------------------------------------- Generic assets

FAssetMcpPropertyList UAssetMcpEditorTools::GetAssetProperties(UObject* Asset, const FString& Filter)
{
	if (!AssetMcp::CheckArg(Asset, TEXT("Asset"), TEXT("/Game/FX/Textures/T_Noise.T_Noise")))
	{
		return {};
	}
	FAssetMcpPropertyList List;
	List.AssetPath = Asset->GetPathName();
	List.Class = Asset->GetClass()->GetName();
	const FString TrimmedFilter = AssetMcp::IsUnsetArg(Filter) ? FString() : Filter.TrimStartAndEnd();
	for (TFieldIterator<FProperty> It(Asset->GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (!AssetMcp::IsEditableProperty(Property))
		{
			continue;
		}
		const FString Category = Property->GetMetaData(TEXT("Category"));
		if (!TrimmedFilter.IsEmpty() && !Property->GetName().Contains(TrimmedFilter) && !Category.Contains(TrimmedFilter)
			&& !Property->GetDisplayNameText().ToString().Contains(TrimmedFilter))
		{
			continue;
		}
		FAssetMcpPropertyInfo& Info = List.Properties.AddDefaulted_GetRef();
		Info.Name = Property->GetName();
		Info.Type = Property->GetCPPType();
		Info.Category = Category;
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Asset);
		if (Property->ArrayDim > 1)
		{
			TArray<FString> Elements;
			for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
			{
				Elements.Add(AssetMcp::ValueToText(Property, Property->ContainerPtrToValuePtr<void>(Asset, Index)));
			}
			Info.Value = AssetMcp::TruncateValue(FString::Printf(TEXT("[%s]"), *FString::Join(Elements, TEXT("; "))));
		}
		else
		{
			Info.Value = AssetMcp::TruncateValue(AssetMcp::ValueToText(Property, ValuePtr));
		}

		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			Info.ValidValues = AssetMcp::EnumEntryNames(EnumProperty->GetEnum());
		}
		else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property); ByteProperty && ByteProperty->Enum)
		{
			Info.ValidValues = AssetMcp::EnumEntryNames(ByteProperty->Enum);
		}
		else if (CastField<FBoolProperty>(Property))
		{
			Info.ValidValues = { TEXT("true"), TEXT("false") };
		}
	}
	return List;
}

FAssetMcpSetPropertyResult UAssetMcpEditorTools::SetAssetProperty(UObject* Asset, const FString& PropertyName, const FString& Value, bool bSave)
{
	if (!AssetMcp::CheckArg(Asset, TEXT("Asset"), TEXT("/Game/FX/Textures/T_Noise.T_Noise")))
	{
		return {};
	}
	if (Asset->IsA<UNiagaraSystem>() || Asset->IsA<UNiagaraEmitter>())
	{
		// Niagara systems must be recompiled and their stacks refreshed after an edit; the Niagara toolset does that.
		ReportError(TEXT("Use the Niagara toolset for Niagara assets: SetSystemProperty (system), SetEmitterProperty (emitter data), SetRendererProperty / SetRendererBinding (renderers)."));
		return {};
	}
	if (AssetMcp::UsesCopyingEditor(Asset) && !EnsureNoOpenEditor(Asset))
	{
		return {};
	}

	// The member property (first path segment) is snapshotted so the value can be validated by writing it once
	// without notifications, then restored before the real, notified edit. Nothing stays changed on failure.
	FString FirstName;
	int32 FirstIndex = INDEX_NONE;
	FString PathError;
	TArray<FString> Segments;
	PropertyName.ParseIntoArray(Segments, TEXT("."));
	if (Segments.Num() == 0 || !AssetMcp::ParsePathSegment(Segments[0], FirstName, FirstIndex, PathError))
	{
		ReportError(PathError.IsEmpty() ? FString::Printf(TEXT("'%s' is not a valid property name. Example: CompressionSettings or SystemScalabilitySettings.Settings[0].MaxDistance"), *PropertyName) : PathError);
		return {};
	}
	FProperty* MemberProperty = AssetMcp::FindPropertyByLooseName(Asset->GetClass(), FirstName);
	if (MemberProperty == nullptr)
	{
		ReportError(AssetMcp::UnknownNameMessage(FString::Printf(TEXT("property of %s"), *Asset->GetClass()->GetName()), FirstName, AssetMcp::EditablePropertyNames(Asset->GetClass())));
		return {};
	}

	AssetMcp::FResolvedProperty Resolved;
	{
		const AssetMcp::FPropertySnapshot Snapshot(MemberProperty, MemberProperty->ContainerPtrToValuePtr<void>(Asset));
		FString ApplyError;
		bool bValid = AssetMcp::ApplyPropertyText(Asset, PropertyName, Value, Resolved, ApplyError);
		if (const UTextureRenderTarget2D* SizedTarget = Cast<UTextureRenderTarget2D>(Asset);
			bValid && SizedTarget && (SizedTarget->SizeX > AssetMcp::MaxRenderTargetSize || SizedTarget->SizeY > AssetMcp::MaxRenderTargetSize))
		{
			bValid = false;
			ApplyError = FString::Printf(TEXT("Render targets are limited to %d px here (larger sizes make the editor ask a modal question on edit and undo)."), AssetMcp::MaxRenderTargetSize);
		}
		Snapshot.Restore();
		if (!bValid)
		{
			ReportError(ApplyError);
			return {};
		}
	}

	FScopedTransaction Transaction(LOCTEXT("SetAssetProperty", "Asset MCP: Set Asset Property"));
	Asset->Modify();

	FEditPropertyChain Chain;
	for (FProperty* Property : Resolved.Chain)
	{
		Chain.AddTail(Property);
	}
	Chain.SetActiveMemberPropertyNode(Resolved.Chain[0]);
	Chain.SetActivePropertyNode(Resolved.Chain.Last());
	Asset->PreEditChange(Chain);

	FString ApplyError;
	if (!AssetMcp::ApplyPropertyText(Asset, PropertyName, Value, Resolved, ApplyError))
	{
		// Validated above; only reachable if the asset changed itself in PreEditChange.
		ReportError(ApplyError);
	}

	FPropertyChangedEvent ChangedEvent(Resolved.Chain.Last(), EPropertyChangeType::ValueSet);
	ChangedEvent.SetActiveMemberProperty(Resolved.Chain[0]);
	UTextureRenderTarget2D* RenderTarget = Cast<UTextureRenderTarget2D>(Asset);
	if (RenderTarget && (RenderTarget->SizeX > 2048 || RenderTarget->SizeY > 2048))
	{
		// UTextureRenderTarget2D::PostEditChangeProperty asks a modal "are you sure" question above 2048 pixels.
		RenderTarget->SizeX = FMath::Clamp(RenderTarget->SizeX, 1, AssetMcp::MaxRenderTargetSize);
		RenderTarget->SizeY = FMath::Clamp(RenderTarget->SizeY, 1, AssetMcp::MaxRenderTargetSize);
		RenderTarget->UpdateResourceImmediate(true);
	}
	else
	{
		FPropertyChangedChainEvent ChainEvent(Chain, ChangedEvent);
		Asset->PostEditChangeChainProperty(ChainEvent);
	}
	Asset->MarkPackageDirty();

	FAssetMcpSetPropertyResult Result;
	Result.AssetPath = Asset->GetPathName();
	Result.Property = PropertyName;
	AssetMcp::FResolvedProperty ReadBack;
	FString ReadError;
	if (AssetMcp::ResolvePropertyPath(Asset, PropertyName, false, ReadBack, ReadError))
	{
		Result.Value = AssetMcp::TruncateValue(AssetMcp::ValueToText(ReadBack.ValueProperty, ReadBack.ValuePtr));
	}
	if (UTexture* Texture = Cast<UTexture>(Asset))
	{
		AssetMcp::WaitForTexture(Texture);
	}
	Result.bSaved = bSave && SaveAssetSilently(Asset);
	return Result;
}

FAssetMcpAssetResult UAssetMcpEditorTools::DuplicateAsset(UObject* Source, const FString& NewAssetPath, bool bSave)
{
	if (!AssetMcp::CheckArg(Source, TEXT("Source"), TEXT("/Game/FX/NS_Sparks")))
	{
		return {};
	}
	if (!Source->IsAsset())
	{
		ReportError(FString::Printf(TEXT("'%s' is not an asset (it is a sub-object). Pass the asset itself."), *Source->GetPathName()));
		return {};
	}
	FString PackageName;
	FString AssetName;
	if (!SplitNewAssetPath(NewAssetPath, PackageName, AssetName))
	{
		return {};
	}
	UObject* Copy = AssetMcp::GetAssetTools().DuplicateAsset(AssetName, FPackageName::GetLongPackagePath(PackageName), Source);
	if (Copy == nullptr)
	{
		ReportError(FString::Printf(TEXT("Could not duplicate '%s' to '%s'. See the output log."), *Source->GetPathName(), *PackageName));
		return {};
	}
	FAssetMcpAssetResult Result;
	Result.Path = Copy->GetPathName();
	Result.Class = Copy->GetClass()->GetName();
	Result.bSaved = bSave && SaveAssetSilently(Copy);
	return Result;
}

FAssetMcpReferences UAssetMcpEditorTools::GetAssetReferences(const FString& AssetPath, bool bReferencers, bool bDependencies, int32 MaxResults)
{
	FString PackageName = AssetPath.TrimStartAndEnd();
	if (PackageName.Contains(TEXT(".")))
	{
		PackageName = FPackageName::ObjectPathToPackageName(PackageName);
	}
	FText Reason;
	if (!FPackageName::IsValidLongPackageName(PackageName, false, &Reason))
	{
		ReportError(FString::Printf(TEXT("'%s' is not a valid asset path (%s). Example: /Game/FX/Textures/T_Noise"), *AssetPath, *Reason.ToString()));
		return {};
	}
	if (MaxResults < 1 || MaxResults > 1000)
	{
		ReportError(FString::Printf(TEXT("MaxResults must be 1-1000 (got %d)."), MaxResults));
		return {};
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	if (Registry.IsLoadingAssets())
	{
		// Referencer lists are only complete once the initial scan has finished.
		Registry.WaitForCompletion();
	}

	// Re-gather the queried package from disk so its dependency list matches its last save, even when the editor's
	// file watcher has not caught up yet (e.g. right after a tool saved it).
	FString PackageFilename;
	if (FPackageName::DoesPackageExist(PackageName, &PackageFilename))
	{
		Registry.ScanFilesSynchronous({ FPaths::ConvertRelativePathToFull(PackageFilename) }, true);
	}

	const FName PackageFName(*PackageName);
	TArray<FAssetData> OwnAssets;
	Registry.GetAssetsByPackageName(PackageFName, OwnAssets);
	if (OwnAssets.Num() == 0 && !FPackageName::DoesPackageExist(PackageName))
	{
		ReportError(FString::Printf(TEXT("No asset found at '%s'."), *PackageName));
		return {};
	}

	FAssetMcpReferences Result;
	Result.PackageName = PackageName;
	auto Collect = [&](const TArray<FName>& Names, TArray<FAssetMcpReference>& Out)
	{
		TArray<FName> Sorted;
		for (const FName& Name : Names)
		{
			const FString NameString = Name.ToString();
			if (Name != PackageFName && !NameString.StartsWith(TEXT("/Script/")))
			{
				Sorted.AddUnique(Name);
			}
		}
		Sorted.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
		for (const FName& Name : Sorted)
		{
			if (Out.Num() >= MaxResults)
			{
				Result.bTruncated = true;
				break;
			}
			Out.Add(AssetMcp::DescribePackage(Registry, Name));
		}
	};

	if (bReferencers)
	{
		TArray<FName> Referencers;
		Registry.GetReferencers(PackageFName, Referencers);
		Collect(Referencers, Result.Referencers);
	}
	if (bDependencies)
	{
		TArray<FName> Dependencies;
		Registry.GetDependencies(PackageFName, Dependencies);
		Collect(Dependencies, Result.Dependencies);
	}
	return Result;
}

// ---------------------------------------------------------------------------------------------------- Textures

FAssetMcpTextureInfo UAssetMcpEditorTools::ImportTexture(const FString& FilePath, const FString& DestinationAssetPath, bool bSRGB, const FString& CompressionSettings, bool bSave)
{
	FString File = FilePath.TrimStartAndEnd().TrimQuotes();
	FPaths::NormalizeFilename(File);
	static const TCHAR* Extensions[] = { TEXT("png"), TEXT("tga"), TEXT("exr"), TEXT("jpg"), TEXT("jpeg"), TEXT("hdr"), TEXT("bmp"), TEXT("tif"), TEXT("tiff"), TEXT("psd") };
	const FString Extension = FPaths::GetExtension(File).ToLower();
	bool bKnownExtension = false;
	for (const TCHAR* Known : Extensions)
	{
		bKnownExtension |= Extension == Known;
	}
	if (!bKnownExtension)
	{
		ReportError(FString::Printf(TEXT("'%s' is not an image file. Supported: png, tga, exr, jpg, jpeg, hdr, bmp, tif, tiff, psd."), *FilePath));
		return {};
	}
	if (FPaths::IsRelative(File) || !FPaths::FileExists(File))
	{
		ReportError(FString::Printf(TEXT("File '%s' does not exist. Pass an absolute path, e.g. D:/Textures/noise.png."), *FilePath));
		return {};
	}
	TextureCompressionSettings Compression = TC_Default;
	const bool bSetCompression = !AssetMcp::IsUnsetArg(CompressionSettings);
	if (bSetCompression && !AssetMcp::ParseCompression(CompressionSettings, Compression))
	{
		return {};
	}
	FString PackageName;
	FString AssetName;
	if (!SplitNewAssetPath(DestinationAssetPath, PackageName, AssetName))
	{
		return {};
	}

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = File;
	Task->DestinationPath = FPackageName::GetLongPackagePath(PackageName);
	Task->DestinationName = AssetName;
	Task->bAutomated = true; // no dialogs
	Task->bReplaceExisting = false;
	Task->bSave = false;
	Task->bAsync = false;
	AssetMcp::GetAssetTools().ImportAssetTasks({ Task });

	UTexture* Texture = nullptr;
	for (UObject* Object : Task->GetObjects())
	{
		Texture = Cast<UTexture>(Object);
		if (Texture)
		{
			break;
		}
	}
	for (int32 Index = 0; Texture == nullptr && Index < Task->ImportedObjectPaths.Num(); ++Index)
	{
		Texture = LoadObject<UTexture>(nullptr, *Task->ImportedObjectPaths[Index], nullptr, LOAD_NoWarn);
	}
	if (Texture == nullptr)
	{
		ReportError(FString::Printf(TEXT("Importing '%s' produced no texture. See the output log for the importer's message."), *File));
		return {};
	}

	AssetMcp::WaitForTexture(Texture);
	Texture->PreEditChange(nullptr);
	Texture->SRGB = bSRGB;
	if (bSetCompression)
	{
		Texture->CompressionSettings = Compression;
	}
	Texture->PostEditChange();
	Texture->MarkPackageDirty();

	FAssetMcpTextureInfo Info = AssetMcp::DescribeTexture(Texture);
	Info.bSaved = bSave && SaveAssetSilently(Texture);
	return Info;
}

FAssetMcpTextureInfo UAssetMcpEditorTools::GetTextureInfo(UTexture* Texture)
{
	if (!AssetMcp::CheckArg(Texture, TEXT("Texture"), TEXT("/Game/FX/Textures/T_Noise.T_Noise")))
	{
		return {};
	}
	return AssetMcp::DescribeTexture(Texture);
}

FAssetMcpTextureInfo UAssetMcpEditorTools::CreateFlipbookTexture(const TArray<UTexture2D*>& Frames, int32 Columns, const FString& NewAssetPath, bool bSave)
{
	if (Frames.Num() == 0)
	{
		ReportError(TEXT("Frames is empty. Pass the frame textures in order."));
		return {};
	}
	for (int32 Index = 0; Index < Frames.Num(); ++Index)
	{
		if (Frames[Index] == nullptr)
		{
			ReportError(FString::Printf(TEXT("Frame %d is null or could not be loaded."), Index));
			return {};
		}
		if (!Frames[Index]->Source.IsValid() || Frames[Index]->Source.GetNumSlices() != 1 || Frames[Index]->Source.GetNumBlocks() != 1)
		{
			ReportError(FString::Printf(TEXT("Frame %d ('%s') has no plain 2D source image (render targets and UDIM / array textures are not supported)."), Index, *Frames[Index]->GetPathName()));
			return {};
		}
	}
	if (Columns < 1 || Columns > Frames.Num())
	{
		ReportError(FString::Printf(TEXT("Columns must be 1-%d (the frame count), got %d."), Frames.Num(), Columns));
		return {};
	}
	const int32 FrameWidth = static_cast<int32>(Frames[0]->Source.GetSizeX());
	const int32 FrameHeight = static_cast<int32>(Frames[0]->Source.GetSizeY());
	for (int32 Index = 1; Index < Frames.Num(); ++Index)
	{
		if (Frames[Index]->Source.GetSizeX() != FrameWidth || Frames[Index]->Source.GetSizeY() != FrameHeight)
		{
			ReportError(FString::Printf(TEXT("Frame %d ('%s') is %lldx%lld; all frames must be %dx%d like frame 0."), Index, *Frames[Index]->GetPathName(),
				Frames[Index]->Source.GetSizeX(), Frames[Index]->Source.GetSizeY(), FrameWidth, FrameHeight));
			return {};
		}
	}
	const int32 Rows = FMath::DivideAndRoundUp(Frames.Num(), Columns);
	const int32 AtlasWidth = FrameWidth * Columns;
	const int32 AtlasHeight = FrameHeight * Rows;
	if (AtlasWidth > AssetMcp::MaxAtlasSize || AtlasHeight > AssetMcp::MaxAtlasSize)
	{
		ReportError(FString::Printf(TEXT("The atlas would be %dx%d; the maximum is %d. Use fewer or smaller frames."), AtlasWidth, AtlasHeight, AssetMcp::MaxAtlasSize));
		return {};
	}

	// Read every frame before creating anything, converted to frame 0's source format. Pow22 (legacy gamma) sources
	// become sRGB: ImageCore cannot write Pow22 and asserts (editor exit) when asked to.
	TArray<FImage> Images;
	Images.SetNum(Frames.Num());
	ERawImageFormat::Type TargetFormat = ERawImageFormat::Invalid;
	EGammaSpace TargetGamma = EGammaSpace::sRGB;
	for (int32 Index = 0; Index < Frames.Num(); ++Index)
	{
		FImage Image;
		if (!Frames[Index]->Source.GetMipImage(Image, 0, 0, 0))
		{
			ReportError(FString::Printf(TEXT("Could not read the source image of frame %d ('%s')."), Index, *Frames[Index]->GetPathName()));
			return {};
		}
		if (Index == 0)
		{
			TargetFormat = Image.Format;
			TargetGamma = Image.GammaSpace == EGammaSpace::Pow22 ? EGammaSpace::sRGB : Image.GammaSpace;
		}
		if (Image.Format != TargetFormat || Image.GammaSpace != TargetGamma)
		{
			Image.CopyTo(Images[Index], TargetFormat, TargetGamma);
		}
		else
		{
			Images[Index] = MoveTemp(Image);
		}
	}

	FString AssetName;
	UPackage* Package = CreateNewAssetPackage(NewAssetPath, AssetName);
	if (Package == nullptr)
	{
		return {};
	}

	FImage Atlas(AtlasWidth, AtlasHeight, Images[0].Format, Images[0].GammaSpace);
	FMemory::Memzero(Atlas.RawData.GetData(), Atlas.RawData.Num());
	const int64 BytesPerPixel = Atlas.GetBytesPerPixel();
	const int64 FrameStride = FrameWidth * BytesPerPixel;
	for (int32 Index = 0; Index < Images.Num(); ++Index)
	{
		const int32 Column = Index % Columns;
		const int32 Row = Index / Columns;
		for (int32 Y = 0; Y < FrameHeight; ++Y)
		{
			uint8* Destination = Atlas.RawData.GetData() + ((static_cast<int64>(Row) * FrameHeight + Y) * AtlasWidth + static_cast<int64>(Column) * FrameWidth) * BytesPerPixel;
			FMemory::Memcpy(Destination, Images[Index].RawData.GetData() + Y * FrameStride, FrameStride);
		}
	}

	// No transaction: undoing a recorded NewObject leaves a broken asset in memory (asset factories do not record creation either).
	UTexture2D* Texture = NewObject<UTexture2D>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	Texture->PreEditChange(nullptr);
	Texture->Source.Init(FImageView(Atlas, Atlas.RawData.GetData()));
	Texture->SRGB = Frames[0]->SRGB;
	Texture->CompressionSettings = Frames[0]->CompressionSettings;
	Texture->LODGroup = Frames[0]->LODGroup;
	Texture->MipGenSettings = Frames[0]->MipGenSettings;
	Texture->PostEditChange();
	FAssetRegistryModule::AssetCreated(Texture);
	Package->MarkPackageDirty();

	FAssetMcpTextureInfo Info = AssetMcp::DescribeTexture(Texture);
	Info.bSaved = bSave && SaveAssetSilently(Texture);
	return Info;
}

// ---------------------------------------------------------------------------------------------------- Render targets

FAssetMcpTextureInfo UAssetMcpEditorTools::CreateRenderTarget2D(const FString& AssetPath, int32 Width, int32 Height, const FString& Format, const FString& ClearColor, bool bSave)
{
	if (Width < 1 || Height < 1 || Width > AssetMcp::MaxRenderTargetSize || Height > AssetMcp::MaxRenderTargetSize)
	{
		ReportError(FString::Printf(TEXT("Width and Height must be 1-%d (got %dx%d)."), AssetMcp::MaxRenderTargetSize, Width, Height));
		return {};
	}
	ETextureRenderTargetFormat RenderTargetFormat = RTF_RGBA16f;
	if (!AssetMcp::ParseRenderTargetFormat(AssetMcp::IsUnsetArg(Format) ? FString(TEXT("RTF_RGBA16f")) : Format, RenderTargetFormat))
	{
		return {};
	}
	FLinearColor Color = FLinearColor::Black;
	if (!AssetMcp::ParseColor(AssetMcp::IsUnsetArg(ClearColor) ? FString(TEXT("0, 0, 0, 1")) : ClearColor, Color, TEXT("ClearColor")))
	{
		return {};
	}
	FString AssetName;
	UPackage* Package = CreateNewAssetPackage(AssetPath, AssetName);
	if (Package == nullptr)
	{
		return {};
	}

	// No transaction: undoing a recorded NewObject leaves a broken asset in memory (asset factories do not record creation either).
	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	RenderTarget->RenderTargetFormat = RenderTargetFormat;
	RenderTarget->ClearColor = Color;
	RenderTarget->InitAutoFormat(Width, Height);
	RenderTarget->UpdateResourceImmediate(true);
	FAssetRegistryModule::AssetCreated(RenderTarget);
	Package->MarkPackageDirty();

	FAssetMcpTextureInfo Info = AssetMcp::DescribeTexture(RenderTarget);
	Info.bSaved = bSave && SaveAssetSilently(RenderTarget);
	return Info;
}

FAssetMcpTextureInfo UAssetMcpEditorTools::ResizeRenderTarget2D(UTextureRenderTarget2D* RenderTarget, int32 Width, int32 Height, bool bSave)
{
	if (!AssetMcp::CheckArg(RenderTarget, TEXT("RenderTarget"), TEXT("/Game/FX/RT_Noise.RT_Noise")))
	{
		return {};
	}
	if (Width < 1 || Height < 1 || Width > AssetMcp::MaxRenderTargetSize || Height > AssetMcp::MaxRenderTargetSize)
	{
		ReportError(FString::Printf(TEXT("Width and Height must be 1-%d (got %dx%d)."), AssetMcp::MaxRenderTargetSize, Width, Height));
		return {};
	}
	FScopedTransaction Transaction(LOCTEXT("ResizeRenderTarget2D", "Asset MCP: Resize Render Target"));
	RenderTarget->Modify();
	// Not through PostEditChangeProperty: it opens a modal confirmation above 2048 pixels.
	RenderTarget->ResizeTarget(Width, Height);
	RenderTarget->UpdateResourceImmediate(true);
	RenderTarget->MarkPackageDirty();

	FAssetMcpTextureInfo Info = AssetMcp::DescribeTexture(RenderTarget);
	Info.bSaved = bSave && SaveAssetSilently(RenderTarget);
	return Info;
}

FAssetMcpDrawResult UAssetMcpEditorTools::DrawMaterialToRenderTarget(UTextureRenderTarget2D* RenderTarget, UMaterialInterface* Material)
{
	if (!AssetMcp::CheckArg(RenderTarget, TEXT("RenderTarget"), TEXT("/Game/FX/RT_Noise.RT_Noise"))
		|| !AssetMcp::CheckArg(Material, TEXT("Material"), TEXT("/Game/FX/M_NoiseGen.M_NoiseGen")))
	{
		return {};
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (World == nullptr)
	{
		ReportError(TEXT("No editor world is available to draw with."));
		return {};
	}

	// Wait for the material's shaders and textures, otherwise the default material is drawn.
	if (UMaterial* BaseMaterial = Material->GetMaterial())
	{
		if (FMaterialResource* Resource = BaseMaterial->GetMaterialResource(GMaxRHIShaderPlatform))
		{
			Resource->FinishCompilation();
		}
	}
	if (GShaderCompilingManager)
	{
		GShaderCompilingManager->FinishAllCompilation();
	}
	FTextureCompilingManager::Get().FinishAllCompilation();
	if (RenderTarget->GetResource() == nullptr)
	{
		RenderTarget->UpdateResourceImmediate(true);
	}

	UKismetRenderingLibrary::DrawMaterialToRenderTarget(World, RenderTarget, Material);
	FlushRenderingCommands();

	FAssetMcpDrawResult Result;
	Result.RenderTarget = RenderTarget->GetPathName();
	Result.Material = Material->GetPathName();
	Result.Width = RenderTarget->SizeX;
	Result.Height = RenderTarget->SizeY;
	Result.CenterColor = AssetMcp::ColorToText(UKismetRenderingLibrary::ReadRenderTargetRawUV(World, RenderTarget, 0.5f, 0.5f));
	return Result;
}

FAssetMcpTextureInfo UAssetMcpEditorTools::CreateTextureFromRenderTarget(UTextureRenderTarget2D* RenderTarget, const FString& NewAssetPath, const FString& CompressionSettings, bool bSave)
{
	if (!AssetMcp::CheckArg(RenderTarget, TEXT("RenderTarget"), TEXT("/Game/FX/RT_Noise.RT_Noise")))
	{
		return {};
	}
	if (RenderTarget->GetResource() == nullptr)
	{
		ReportError(TEXT("The render target has no GPU resource. Draw into it first (DrawMaterialToRenderTarget)."));
		return {};
	}
	TextureCompressionSettings Compression = TC_Default;
	const bool bSetCompression = !AssetMcp::IsUnsetArg(CompressionSettings);
	if (bSetCompression && !AssetMcp::ParseCompression(CompressionSettings, Compression))
	{
		return {};
	}
	FString AssetName;
	UPackage* Package = CreateNewAssetPackage(NewAssetPath, AssetName);
	if (Package == nullptr)
	{
		return {};
	}

	// No transaction: undoing a recorded NewObject leaves a broken asset in memory (asset factories do not record creation either).
	FlushRenderingCommands();
	FText ConstructError;
	UTexture* Texture = RenderTarget->ConstructTexture(Package, AssetName, RF_Public | RF_Standalone | RF_Transactional,
		static_cast<EConstructTextureFlags>(CTF_Default | CTF_AllowMips | CTF_SkipPostEdit), nullptr, &ConstructError);
	if (Texture == nullptr)
	{
		ReportError(FString::Printf(TEXT("Could not create a texture from '%s': %s"), *RenderTarget->GetPathName(), *ConstructError.ToString()));
		return {};
	}
	if (bSetCompression)
	{
		Texture->CompressionSettings = Compression;
	}
	Texture->PostEditChange();
	FAssetRegistryModule::AssetCreated(Texture);
	Package->MarkPackageDirty();

	FAssetMcpTextureInfo Info = AssetMcp::DescribeTexture(Texture);
	Info.bSaved = bSave && SaveAssetSilently(Texture);
	return Info;
}

// ---------------------------------------------------------------------------------------------------- Material parameter collections

FAssetMcpMaterialParameterCollectionInfo UAssetMcpEditorTools::CreateMaterialParameterCollection(const FString& AssetPath, bool bSave)
{
	FString AssetName;
	UPackage* Package = CreateNewAssetPackage(AssetPath, AssetName);
	if (Package == nullptr)
	{
		return {};
	}
	// No transaction: undoing a recorded NewObject leaves a broken asset in memory (asset factories do not record creation either).
	UMaterialParameterCollection* Collection = NewObject<UMaterialParameterCollection>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Collection);
	Package->MarkPackageDirty();

	FAssetMcpMaterialParameterCollectionInfo Info = AssetMcp::DescribeMaterialCollection(Collection);
	Info.bSaved = bSave && SaveAssetSilently(Collection);
	return Info;
}

FAssetMcpMaterialParameterCollectionInfo UAssetMcpEditorTools::SetMaterialParameterCollectionParameter(UMaterialParameterCollection* Collection, FName ParameterName, const FString& Value, bool bSave)
{
	if (!AssetMcp::CheckArg(Collection, TEXT("Collection"), TEXT("/Game/FX/MPC_Weather.MPC_Weather")))
	{
		return {};
	}
	if (ParameterName.IsNone())
	{
		ReportError(TEXT("ParameterName is empty."));
		return {};
	}
	TArray<double> Numbers;
	if (!AssetMcp::ParseNumbers(Value, Numbers))
	{
		ReportError(FString::Printf(TEXT("'%s' is not a parameter value. Use \"0.5\" for a scalar or \"1, 0.5, 0, 1\" for a vector."), *Value));
		return {};
	}

	const int32 ScalarIndex = Collection->ScalarParameters.IndexOfByPredicate([ParameterName](const FCollectionScalarParameter& P) { return P.ParameterName == ParameterName; });
	const int32 VectorIndex = Collection->VectorParameters.IndexOfByPredicate([ParameterName](const FCollectionVectorParameter& P) { return P.ParameterName == ParameterName; });
	if (ScalarIndex == INDEX_NONE && VectorIndex == INDEX_NONE)
	{
		for (const UMaterialParameterCollection* Base = Collection->GetBaseParameterCollection(); Base; Base = Base->GetBaseParameterCollection())
		{
			if (Base->GetScalarParameterByName(ParameterName) || Base->GetVectorParameterByName(ParameterName))
			{
				ReportError(FString::Printf(TEXT("'%s' is inherited from the base collection '%s'. Edit the base collection instead."), *ParameterName.ToString(), *Base->GetPathName()));
				return {};
			}
		}
	}

	bool bVector = false;
	if (ScalarIndex != INDEX_NONE)
	{
		if (Numbers.Num() != 1)
		{
			ReportError(FString::Printf(TEXT("'%s' is a scalar parameter; pass one number (got '%s')."), *ParameterName.ToString(), *Value));
			return {};
		}
	}
	else if (VectorIndex != INDEX_NONE)
	{
		bVector = true;
		if (Numbers.Num() != 1 && Numbers.Num() != 3 && Numbers.Num() != 4)
		{
			ReportError(FString::Printf(TEXT("'%s' is a vector parameter; pass \"R, G, B\" or \"R, G, B, A\" (got '%s')."), *ParameterName.ToString(), *Value));
			return {};
		}
	}
	else if (Numbers.Num() == 3 || Numbers.Num() == 4)
	{
		bVector = true;
	}
	else if (Numbers.Num() != 1)
	{
		ReportError(FString::Printf(TEXT("'%s' has %d numbers. Use 1 for a scalar or 3-4 for a vector."), *Value, Numbers.Num()));
		return {};
	}

	FLinearColor Color = FLinearColor::Black;
	if (bVector && !AssetMcp::ParseColor(Value, Color, TEXT("Value")))
	{
		return {};
	}

	FScopedTransaction Transaction(LOCTEXT("SetMaterialParameterCollectionParameter", "Asset MCP: Set Material Parameter Collection Parameter"));
	Collection->Modify();
	FProperty* ArrayProperty = FindFProperty<FProperty>(UMaterialParameterCollection::StaticClass(), bVector ? TEXT("VectorParameters") : TEXT("ScalarParameters"));
	// PreEditChange records the uniform buffer layout; PostEditChangeProperty compares against it and recompiles the
	// materials that use the collection when a parameter was added, or just refreshes the buffer values otherwise.
	Collection->PreEditChange(ArrayProperty);
	bool bAdded = false;
	if (bVector)
	{
		if (VectorIndex != INDEX_NONE)
		{
			Collection->VectorParameters[VectorIndex].DefaultValue = Color;
		}
		else
		{
			FCollectionVectorParameter Parameter;
			Parameter.ParameterName = ParameterName;
			Parameter.DefaultValue = Color;
			Collection->VectorParameters.Add(Parameter);
			bAdded = true;
		}
	}
	else
	{
		if (ScalarIndex != INDEX_NONE)
		{
			Collection->ScalarParameters[ScalarIndex].DefaultValue = static_cast<float>(Numbers[0]);
		}
		else
		{
			FCollectionScalarParameter Parameter;
			Parameter.ParameterName = ParameterName;
			Parameter.DefaultValue = static_cast<float>(Numbers[0]);
			Collection->ScalarParameters.Add(Parameter);
			bAdded = true;
		}
	}
	FPropertyChangedEvent ChangedEvent(ArrayProperty, bAdded ? EPropertyChangeType::ArrayAdd : EPropertyChangeType::ValueSet);
	Collection->PostEditChangeProperty(ChangedEvent);
	Collection->MarkPackageDirty();

	FAssetMcpMaterialParameterCollectionInfo Info = AssetMcp::DescribeMaterialCollection(Collection);
	Info.bSaved = bSave && SaveAssetSilently(Collection);
	return Info;
}

FAssetMcpMaterialParameterCollectionInfo UAssetMcpEditorTools::GetMaterialParameterCollection(UMaterialParameterCollection* Collection)
{
	if (!AssetMcp::CheckArg(Collection, TEXT("Collection"), TEXT("/Game/FX/MPC_Weather.MPC_Weather")))
	{
		return {};
	}
	return AssetMcp::DescribeMaterialCollection(Collection);
}

// ---------------------------------------------------------------------------------------------------- Niagara parameter collections

FAssetMcpNiagaraParameterCollectionInfo UAssetMcpEditorTools::CreateNiagaraParameterCollection(const FString& AssetPath, const FString& Namespace, bool bSave)
{
	const FString TrimmedNamespace = AssetMcp::IsUnsetArg(Namespace) ? FString() : Namespace.TrimStartAndEnd();
	if (!TrimmedNamespace.IsEmpty() && !AssetMcp::IsValidNamespace(TrimmedNamespace))
	{
		ReportError(FString::Printf(TEXT("Namespace '%s' is invalid. Use letters, digits and underscores, starting with a letter, e.g. Weather."), *Namespace));
		return {};
	}
	FString AssetName;
	UPackage* Package = CreateNewAssetPackage(AssetPath, AssetName);
	if (Package == nullptr)
	{
		return {};
	}

	// No transaction: undoing a recorded NewObject leaves a broken asset in memory (asset factories do not record creation either).
	// The constructor takes the namespace from the object name and builds the transient full namespace
	// (NPC.<Namespace>.) from it; editing Namespace afterwards does not rebuild that until the asset is reloaded.
	// So a custom namespace is created as the object name inside the new (empty) package, then renamed to the asset name.
	const bool bCustomNamespace = !TrimmedNamespace.IsEmpty() && TrimmedNamespace != AssetName;
	UNiagaraParameterCollection* Collection = NewObject<UNiagaraParameterCollection>(Package, FName(bCustomNamespace ? *TrimmedNamespace : *AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (bCustomNamespace)
	{
		Collection->Rename(*AssetName, Package, REN_DontCreateRedirectors | REN_NonTransactional);
	}
	Collection->RefreshCompileId();
	FAssetRegistryModule::AssetCreated(Collection);

	// Like a details panel edit of Namespace: makes it unique among the project's collections (rebuilding the full
	// namespace when it had to be changed).
	FProperty* NamespaceProperty = FindFProperty<FProperty>(UNiagaraParameterCollection::StaticClass(), TEXT("Namespace"));
	Collection->PreEditChange(NamespaceProperty);
	FPropertyChangedEvent ChangedEvent(NamespaceProperty, EPropertyChangeType::ValueSet);
	Collection->PostEditChangeProperty(ChangedEvent);
	Package->MarkPackageDirty();

	FAssetMcpNiagaraParameterCollectionInfo Info = AssetMcp::DescribeNiagaraCollection(Collection);
	Info.bSaved = bSave && SaveAssetSilently(Collection);
	return Info;
}

FAssetMcpNiagaraParameterCollectionInfo UAssetMcpEditorTools::AddNiagaraParameterCollectionParameter(UNiagaraParameterCollection* Collection, FName ParameterName, const FString& Type, const FString& DefaultValue, bool bSave)
{
	if (!AssetMcp::CheckArg(Collection, TEXT("Collection"), TEXT("/Game/FX/NPC_Weather.NPC_Weather")))
	{
		return {};
	}
	UNiagaraParameterCollectionInstance* DefaultInstance = Collection->GetDefaultInstance();
	if (DefaultInstance == nullptr)
	{
		ReportError(FString::Printf(TEXT("'%s' has no default instance."), *Collection->GetPathName()));
		return {};
	}
	FString FriendlyName = ParameterName.ToString().TrimStartAndEnd();
	const FString FullNamespace = Collection->GetFullNamespaceName().ToString();
	if (FriendlyName.StartsWith(FullNamespace))
	{
		FriendlyName.RightChopInline(FullNamespace.Len());
	}
	else if (FriendlyName.StartsWith(TEXT("NPC."), ESearchCase::IgnoreCase))
	{
		ReportError(FString::Printf(TEXT("'%s' is in another collection's namespace. This collection's parameters are %s<Name>; pass just the name."), *FriendlyName, *FullNamespace));
		return {};
	}
	if (FriendlyName.IsEmpty() || FriendlyName.Contains(TEXT(".")) || FriendlyName.Contains(TEXT(" ")))
	{
		ReportError(FString::Printf(TEXT("'%s' is not a valid parameter name. Use a name without dots or spaces, e.g. WindStrength."), *ParameterName.ToString()));
		return {};
	}

	FNiagaraTypeDefinition TypeDef;
	if (!ResolveNiagaraType(Type, TypeDef))
	{
		return {};
	}
	if (TypeDef.IsDataInterface() || TypeDef.IsUObject() || TypeDef == FNiagaraTypeDefinition::GetParameterMapDef() || TypeDef.GetScriptStruct() == nullptr)
	{
		ReportError(FString::Printf(TEXT("Type '%s' cannot be added with this tool. Use float, int, bool, vec2, vec3, vec4, color, position, quat or an enum."), *Type));
		return {};
	}

	const FNiagaraVariable Variable(TypeDef, Collection->ConditionalAddFullNamespace(FName(*FriendlyName)));
	const FNiagaraVariable* Existing = Collection->GetParameters().FindByPredicate([&Variable](const FNiagaraVariable& P) { return P.GetName() == Variable.GetName(); });
	if (Existing && Existing->GetType() != TypeDef)
	{
		ReportError(FString::Printf(TEXT("'%s' already exists as %s. Use that type or another name."), *FriendlyName, *Existing->GetType().GetName()));
		return {};
	}

	TArray<uint8> Data;
	Data.SetNumZeroed(TypeDef.GetSize());
	if (!AssetMcp::IsUnsetArg(DefaultValue))
	{
		FString ValueError;
		if (!ImportStructValue(TypeDef.GetScriptStruct(), Data.GetData(), DefaultValue, TypeDef.GetEnum(), ValueError))
		{
			ReportError(ValueError);
			return {};
		}
	}
	else if (Existing)
	{
		ReportError(FString::Printf(TEXT("'%s' already exists; pass a DefaultValue (not \"Default\") to change its default."), *FriendlyName));
		return {};
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetQuatDef())
	{
		*reinterpret_cast<FQuat4f*>(Data.GetData()) = FQuat4f::Identity;
	}

	FScopedTransaction Transaction(LOCTEXT("AddNiagaraParameterCollectionParameter", "Asset MCP: Add Niagara Parameter Collection Parameter"));
	Collection->Modify();
	DefaultInstance->Modify();
	if (Existing == nullptr)
	{
		Collection->AddParameter(Variable);
	}
	DefaultInstance->GetParameterStore().SetParameterData(Data.GetData(), Variable);

	// Same as the collection editor: bring every instance of this collection in line with the new layout / defaults.
	for (TObjectIterator<UNiagaraParameterCollectionInstance> It; It; ++It)
	{
		if (*It != DefaultInstance && It->GetParent() == Collection)
		{
			It->SyncWithCollection();
		}
	}
#if WITH_EDITOR
	const FName ChangedName = Variable.GetName();
	Collection->OnParameterValuesEdited.Broadcast(MakeArrayView(&ChangedName, 1));
#endif
	Collection->MarkPackageDirty();

	FAssetMcpNiagaraParameterCollectionInfo Info = AssetMcp::DescribeNiagaraCollection(Collection);
	Info.bSaved = bSave && SaveAssetSilently(Collection);
	return Info;
}

FAssetMcpNiagaraParameterCollectionInfo UAssetMcpEditorTools::GetNiagaraParameterCollection(UNiagaraParameterCollection* Collection)
{
	if (!AssetMcp::CheckArg(Collection, TEXT("Collection"), TEXT("/Game/FX/NPC_Weather.NPC_Weather")))
	{
		return {};
	}
	return AssetMcp::DescribeNiagaraCollection(Collection);
}

// ---------------------------------------------------------------------------------------------------- Niagara effect types

FAssetMcpAssetResult UAssetMcpEditorTools::CreateNiagaraEffectType(const FString& AssetPath, bool bSave)
{
	FString AssetName;
	UPackage* Package = CreateNewAssetPackage(AssetPath, AssetName);
	if (Package == nullptr)
	{
		return {};
	}
	// No transaction: undoing a recorded NewObject leaves a broken asset in memory (asset factories do not record creation either).
	UNiagaraEffectType* EffectType = NewObject<UNiagaraEffectType>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(EffectType);
	Package->MarkPackageDirty();

	FAssetMcpAssetResult Result;
	Result.Path = EffectType->GetPathName();
	Result.Class = EffectType->GetClass()->GetName();
	Result.bSaved = bSave && SaveAssetSilently(EffectType);
	return Result;
}

#undef LOCTEXT_NAMESPACE
