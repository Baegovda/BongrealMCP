#include "NiagaraMcpCommon.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/UserDefinedEnum.h"
#include "Blueprint/BlueprintExceptionInfo.h"
#include "UObject/Script.h"
#include "UObject/Stack.h"
#include "Misc/PackageName.h"
#include "Misc/StringOutputDevice.h"
#include "NiagaraDataInterface.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/SavePackage.h"
#include "Algo/LevenshteinDistance.h"
#include "HAL/FileManager.h"
#include "ISourceControlModule.h"
#include "SourceControlHelpers.h"
#include "NiagaraMcpJournal.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY(LogNiagaraMcp);

namespace NiagaraMcp
{
	void ReportError(const FString& Message)
	{
		UE_LOG(LogNiagaraMcp, Warning, TEXT("%s"), *Message);
		// Same as UKismetSystemLibrary::RaiseScriptError (not exported in every engine version): raise a
		// user error on the script frame of the tool call. The Toolset Registry reports it as the tool's error.
		if (FFrame* TopFrame = FFrame::GetThreadLocalTopStackFrame())
		{
			const FBlueprintExceptionInfo ExceptionInfo(EBlueprintExceptionType::UserRaisedError, FText::FromString(Message));
			FBlueprintCoreDelegates::ThrowScriptException(TopFrame->Object, *TopFrame, ExceptionInfo);
		}
	}

	bool SaveAssetSilently(UObject* Asset)
	{
		if (Asset == nullptr)
		{
			return false;
		}

		if (GEditor && GEditor->PlayWorld)
		{
			// Saving during Play In Editor silently no-ops or saves PIE-modified state; refuse clearly instead.
			ReportError(FString::Printf(TEXT("[PIE_ACTIVE] Not saving '%s' while Play In Editor is running. Stop PIE and save again (the change itself is applied)."), *Asset->GetOutermost()->GetName()));
			return false;
		}
		Journal::FAgentSaveScope AgentScope;
		UPackage* Package = Asset->GetOutermost();
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

		// With source control (e.g. Perforce) files are read-only until checked out: check out silently, or
		// mark for add when new. Without source control a read-only file stays an error below.
		if (ISourceControlModule::Get().IsEnabled() && ISourceControlModule::Get().GetProvider().IsAvailable())
		{
			const FString FullPath = FPaths::ConvertRelativePathToFull(Filename);
			if (!USourceControlHelpers::CheckOutOrAddFile(FullPath, /*bSilent*/ true))
			{
				UE_LOG(LogNiagaraMcp, Warning, TEXT("Source control could not check out or add '%s': %s"), *FullPath, *USourceControlHelpers::LastErrorMsg().ToString());
			}
		}

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		SaveArgs.bSlowTask = false;
		// GError is fatal in the editor; route save errors to the log instead.
		SaveArgs.Error = GWarn;

		if (!UPackage::SavePackage(Package, Asset, *Filename, SaveArgs))
		{
			ReportError(FString::Printf(TEXT("[SAVE_FAILED] Failed to save '%s' to '%s'. The file may be read-only or checked in."), *Package->GetName(), *Filename));
			return false;
		}
		return true;
	}

	bool SplitNewAssetPath(const FString& AssetPath, FString& OutPackageName, FString& OutAssetName)
	{
		FString PackageName = AssetPath.TrimStartAndEnd();
		if (PackageName.Contains(TEXT(".")))
		{
			PackageName = FPackageName::ObjectPathToPackageName(PackageName);
		}

		FText Reason;
		if (!FPackageName::IsValidLongPackageName(PackageName, false, &Reason))
		{
			ReportError(FString::Printf(TEXT("[INVALID_PATH] '%s' is not a valid asset path (%s). Example: /Game/TestSample/NS_ChainStrike"), *AssetPath, *Reason.ToString()));
			return false;
		}

		// A failed load of a missing asset can leave an empty package in memory, so only an asset object inside
		// the package (or a file on disk) counts as "exists".
		const FString AssetName = FPackageName::GetShortName(PackageName);
		const UPackage* InMemory = FindPackage(nullptr, *PackageName);
		const UObject* Existing = InMemory ? StaticFindObject(UObject::StaticClass(), const_cast<UPackage*>(InMemory), *AssetName) : nullptr;
		const bool bAssetInMemory = IsValid(Existing); // deleted (garbage) assets, e.g. discarded by a batch rollback, don't count
		if (bAssetInMemory || FPackageName::DoesPackageExist(PackageName))
		{
			ReportError(FString::Printf(TEXT("[ASSET_EXISTS] An asset already exists at '%s'. Pick a new path (creation tools never overwrite), or edit the existing asset."), *PackageName));
			return false;
		}

		OutPackageName = PackageName;
		OutAssetName = FPackageName::GetShortName(PackageName);
		return true;
	}

	UPackage* CreateNewAssetPackage(const FString& AssetPath, FString& OutAssetName)
	{
		FString PackageName;
		if (!SplitNewAssetPath(AssetPath, PackageName, OutAssetName))
		{
			return nullptr;
		}
		UPackage* Package = CreatePackage(*PackageName);
		if (Package == nullptr)
		{
			ReportError(FString::Printf(TEXT("Could not create package '%s'."), *PackageName));
			return nullptr;
		}
		Package->FullyLoad();
		return Package;
	}

	UObject* LoadObjectByPath(const FString& InPath, UClass* ExpectedClass, const TCHAR* What)
	{
		FString Path = InPath.TrimStartAndEnd();
		if (Path.IsEmpty())
		{
			ReportError(FString::Printf(TEXT("[ASSET_NOT_FOUND] %s path is empty."), What));
			return nullptr;
		}

		// "/Game/Folder/Asset" -> "/Game/Folder/Asset.Asset"
		if (!Path.Contains(TEXT(".")) && Path.StartsWith(TEXT("/")))
		{
			Path = Path + TEXT(".") + FPackageName::GetShortName(Path);
		}

		UObject* Object = StaticFindObject(ExpectedClass ? ExpectedClass : UObject::StaticClass(), nullptr, *Path);
		if (Object == nullptr)
		{
			Object = StaticLoadObject(ExpectedClass ? ExpectedClass : UObject::StaticClass(), nullptr, *Path, nullptr, LOAD_NoWarn);
		}
		if (Object == nullptr)
		{
			ReportError(FString::Printf(TEXT("[ASSET_NOT_FOUND] %s '%s' could not be loaded%s."), What, *InPath,
				ExpectedClass ? *FString::Printf(TEXT(" as %s"), *ExpectedClass->GetName()) : TEXT("")));
			return nullptr;
		}
		if (ExpectedClass && !Object->IsA(ExpectedClass))
		{
			ReportError(FString::Printf(TEXT("[ASSET_NOT_FOUND] %s '%s' is a %s, expected %s."), What, *InPath, *Object->GetClass()->GetName(), *ExpectedClass->GetName()));
			return nullptr;
		}
		return Object;
	}

	bool IsAssetEditorOpen(UObject* Asset)
	{
		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		return Asset && AssetEditorSubsystem && AssetEditorSubsystem->FindEditorsForAsset(Asset).Num() > 0;
	}

	bool EnsureNoOpenEditor(UObject* Asset)
	{
		if (Asset == nullptr)
		{
			ReportError(TEXT("Asset is null."));
			return false;
		}
		if (IsAssetEditorOpen(Asset))
		{
			// An open editor keeps its own working copy; edits made behind its back are lost on its next apply.
			ReportError(FString::Printf(TEXT("[EDITOR_OPEN] Close the asset editor for '%s' before running this tool."), *Asset->GetPathName()));
			return false;
		}
		return true;
	}

	static FString NormalizeName(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		for (TCHAR C : In)
		{
			if (C != TEXT(' ') && C != TEXT('_') && C != TEXT('-'))
			{
				Out.AppendChar(FChar::ToLower(C));
			}
		}
		return Out;
	}

	bool LooseNameEquals(const FString& A, const FString& B)
	{
		return A.Equals(B, ESearchCase::IgnoreCase) || NormalizeName(A) == NormalizeName(B);
	}

	bool ParseUsage(const FString& InText, ENiagaraScriptUsage& OutUsage)
	{
		const FString Text = NormalizeName(InText);
		struct FUsageAlias { const TCHAR* Name; ENiagaraScriptUsage Usage; };
		static const FUsageAlias Aliases[] =
		{
			{ TEXT("particlespawn"), ENiagaraScriptUsage::ParticleSpawnScript },
			{ TEXT("particlespawnscript"), ENiagaraScriptUsage::ParticleSpawnScript },
			{ TEXT("particleupdate"), ENiagaraScriptUsage::ParticleUpdateScript },
			{ TEXT("particleupdatescript"), ENiagaraScriptUsage::ParticleUpdateScript },
			{ TEXT("emitterspawn"), ENiagaraScriptUsage::EmitterSpawnScript },
			{ TEXT("emitterspawnscript"), ENiagaraScriptUsage::EmitterSpawnScript },
			{ TEXT("emitterupdate"), ENiagaraScriptUsage::EmitterUpdateScript },
			{ TEXT("emitterupdatescript"), ENiagaraScriptUsage::EmitterUpdateScript },
			{ TEXT("systemspawn"), ENiagaraScriptUsage::SystemSpawnScript },
			{ TEXT("systemspawnscript"), ENiagaraScriptUsage::SystemSpawnScript },
			{ TEXT("systemupdate"), ENiagaraScriptUsage::SystemUpdateScript },
			{ TEXT("systemupdatescript"), ENiagaraScriptUsage::SystemUpdateScript },
			{ TEXT("particleevent"), ENiagaraScriptUsage::ParticleEventScript },
			{ TEXT("particleeventscript"), ENiagaraScriptUsage::ParticleEventScript },
			{ TEXT("eventhandler"), ENiagaraScriptUsage::ParticleEventScript },
			{ TEXT("event"), ENiagaraScriptUsage::ParticleEventScript },
			{ TEXT("simulationstage"), ENiagaraScriptUsage::ParticleSimulationStageScript },
			{ TEXT("particlesimulationstage"), ENiagaraScriptUsage::ParticleSimulationStageScript },
			{ TEXT("particlesimulationstagescript"), ENiagaraScriptUsage::ParticleSimulationStageScript },
			{ TEXT("simstage"), ENiagaraScriptUsage::ParticleSimulationStageScript },
			{ TEXT("stage"), ENiagaraScriptUsage::ParticleSimulationStageScript },
			{ TEXT("module"), ENiagaraScriptUsage::Module },
			{ TEXT("function"), ENiagaraScriptUsage::Function },
			{ TEXT("dynamicinput"), ENiagaraScriptUsage::DynamicInput },
		};
		for (const FUsageAlias& Alias : Aliases)
		{
			if (Text == Alias.Name)
			{
				OutUsage = Alias.Usage;
				return true;
			}
		}
		ReportError(FString::Printf(TEXT("[INVALID_VALUE] Unknown stack '%s'. Use SystemSpawn, SystemUpdate, EmitterSpawn, EmitterUpdate, ParticleSpawn, ParticleUpdate, ParticleEvent or SimulationStage."), *InText));
		return false;
	}

	FString UsageToString(ENiagaraScriptUsage Usage)
	{
		return StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(static_cast<int64>(Usage));
	}

	bool IsSystemStack(ENiagaraScriptUsage Usage)
	{
		return Usage == ENiagaraScriptUsage::SystemSpawnScript || Usage == ENiagaraScriptUsage::SystemUpdateScript;
	}

	bool IsStandardStack(ENiagaraScriptUsage Usage)
	{
		switch (Usage)
		{
		case ENiagaraScriptUsage::ParticleSpawnScript:
		case ENiagaraScriptUsage::ParticleUpdateScript:
		case ENiagaraScriptUsage::EmitterSpawnScript:
		case ENiagaraScriptUsage::EmitterUpdateScript:
		case ENiagaraScriptUsage::SystemSpawnScript:
		case ENiagaraScriptUsage::SystemUpdateScript:
			return true;
		default:
			return false;
		}
	}

	bool ResolveNiagaraType(const FString& InTypeName, FNiagaraTypeDefinition& OutType)
	{
		const FString TypeName = InTypeName.TrimStartAndEnd();
		struct FAlias
		{
			const TCHAR* Name;
			const FNiagaraTypeDefinition& (*Get)();
		};
		static const FAlias Aliases[] =
		{
			{ TEXT("float"), &FNiagaraTypeDefinition::GetFloatDef },
			{ TEXT("float1"), &FNiagaraTypeDefinition::GetFloatDef },
			{ TEXT("int"), &FNiagaraTypeDefinition::GetIntDef },
			{ TEXT("int32"), &FNiagaraTypeDefinition::GetIntDef },
			{ TEXT("bool"), &FNiagaraTypeDefinition::GetBoolDef },
			{ TEXT("vec2"), &FNiagaraTypeDefinition::GetVec2Def },
			{ TEXT("float2"), &FNiagaraTypeDefinition::GetVec2Def },
			{ TEXT("vector2d"), &FNiagaraTypeDefinition::GetVec2Def },
			{ TEXT("vec3"), &FNiagaraTypeDefinition::GetVec3Def },
			{ TEXT("float3"), &FNiagaraTypeDefinition::GetVec3Def },
			{ TEXT("vector"), &FNiagaraTypeDefinition::GetVec3Def },
			{ TEXT("vec4"), &FNiagaraTypeDefinition::GetVec4Def },
			{ TEXT("float4"), &FNiagaraTypeDefinition::GetVec4Def },
			{ TEXT("vector4"), &FNiagaraTypeDefinition::GetVec4Def },
			{ TEXT("color"), &FNiagaraTypeDefinition::GetColorDef },
			{ TEXT("linearcolor"), &FNiagaraTypeDefinition::GetColorDef },
			{ TEXT("position"), &FNiagaraTypeDefinition::GetPositionDef },
			{ TEXT("quat"), &FNiagaraTypeDefinition::GetQuatDef },
			{ TEXT("matrix"), &FNiagaraTypeDefinition::GetMatrix4Def },
			{ TEXT("matrix4"), &FNiagaraTypeDefinition::GetMatrix4Def },
			{ TEXT("map"), &FNiagaraTypeDefinition::GetParameterMapDef },
			{ TEXT("parametermap"), &FNiagaraTypeDefinition::GetParameterMapDef },
		};
		for (const FAlias& Alias : Aliases)
		{
			const FNiagaraTypeDefinition& Candidate = Alias.Get();
			if (TypeName.Equals(Alias.Name, ESearchCase::IgnoreCase) || TypeName.Equals(Candidate.GetName(), ESearchCase::IgnoreCase))
			{
				OutType = Candidate;
				return true;
			}
		}

		// Enums by path or short name (e.g. /Script/Niagara.ENiagaraExecutionState).
		if (UEnum* Enum = TypeName.StartsWith(TEXT("/")) ? LoadObject<UEnum>(nullptr, *TypeName) : FindFirstObject<UEnum>(*TypeName, EFindFirstObjectOptions::NativeFirst))
		{
			OutType = FNiagaraTypeDefinition(Enum);
			return true;
		}

		// Data interface classes by path or short name.
		UClass* Class = TypeName.StartsWith(TEXT("/"))
			? LoadObject<UClass>(nullptr, *TypeName)
			: FindFirstObject<UClass>(*TypeName, EFindFirstObjectOptions::NativeFirst);
		if (Class == nullptr && !TypeName.StartsWith(TEXT("/")))
		{
			Class = FindFirstObject<UClass>(*(FString(TEXT("NiagaraDataInterface")) + TypeName), EFindFirstObjectOptions::NativeFirst);
		}
		if (Class && Class->IsChildOf(UNiagaraDataInterface::StaticClass()) && !Class->HasAnyClassFlags(CLASS_Abstract))
		{
			OutType = FNiagaraTypeDefinition(Class);
			return true;
		}

		ReportError(FString::Printf(TEXT("Unknown Niagara type '%s'. Use float, int, bool, vec2, vec3, vec4, color, position, quat, matrix, map, an enum, or a data interface class such as /Script/Niagara.NiagaraDataInterfaceCurve."), *InTypeName));
		return false;
	}

	/** Numeric leaf properties of a struct in declaration order (nested structs are flattened). */
	static void CollectNumericLeaves(const UStruct* Struct, uint8* Base, TArray<TPair<FNumericProperty*, void*>>& Out)
	{
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* Property = *It;
			for (int32 ArrayIndex = 0; ArrayIndex < Property->ArrayDim; ++ArrayIndex)
			{
				void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Base, ArrayIndex);
				if (FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
				{
					Out.Emplace(Numeric, ValuePtr);
				}
				else if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
				{
					CollectNumericLeaves(StructProperty->Struct, static_cast<uint8*>(ValuePtr), Out);
				}
			}
		}
	}

	bool ParseBool(const FString& Text, bool& bOut)
	{
		const FString T = Text.TrimStartAndEnd();
		if (T.Equals(TEXT("true"), ESearchCase::IgnoreCase) || T == TEXT("1") || T.Equals(TEXT("yes"), ESearchCase::IgnoreCase) || T.Equals(TEXT("on"), ESearchCase::IgnoreCase))
		{
			bOut = true;
			return true;
		}
		if (T.Equals(TEXT("false"), ESearchCase::IgnoreCase) || T == TEXT("0") || T.Equals(TEXT("no"), ESearchCase::IgnoreCase) || T.Equals(TEXT("off"), ESearchCase::IgnoreCase))
		{
			bOut = false;
			return true;
		}
		return false;
	}

	static bool ParseEnumValue(const UEnum* Enum, const FString& InText, int64& OutValue)
	{
		const FString Text = InText.TrimStartAndEnd();
		if (Text.IsNumeric())
		{
			OutValue = FCString::Atoi64(*Text);
			return true;
		}
		FString ShortName = Text;
		int32 ColonIndex = INDEX_NONE;
		if (ShortName.FindLastChar(TEXT(':'), ColonIndex))
		{
			ShortName = ShortName.Mid(ColonIndex + 1);
		}
		for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
		{
			const FString EntryShort = Enum->GetNameStringByIndex(Index);
			const FString EntryDisplay = Enum->GetDisplayNameTextByIndex(Index).ToString();
			if (LooseNameEquals(EntryShort, ShortName) || LooseNameEquals(EntryDisplay, ShortName))
			{
				OutValue = Enum->GetValueByIndex(Index);
				return true;
			}
		}
		return false;
	}

	bool ParseEnumText(const UEnum* Enum, const FString& Text, int64& OutValue)
	{
		return Enum && ParseEnumValue(Enum, Text, OutValue);
	}

	static FString EnumEntries(const UEnum* Enum)
	{
		TArray<FString> Names;
		for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index) // last entry is _MAX
		{
			Names.Add(Enum->GetNameStringByIndex(Index));
		}
		return FString::Join(Names, TEXT(", "));
	}

	/**
	 * Enum value as text. Blueprint (user-defined) enums store entries as NewEnumerator0, NewEnumerator1, ...,
	 * so their display name ("Direct Set") is returned instead; native enums keep their entry name.
	 */
	FString EnumValueToString(const UEnum* Enum, int64 Value)
	{
		if (Enum == nullptr)
		{
			return FString::Printf(TEXT("%lld"), Value);
		}
		if (Enum->IsA<UUserDefinedEnum>())
		{
			const FText Display = Enum->GetDisplayNameTextByValue(Value);
			if (!Display.IsEmpty())
			{
				return Display.ToString();
			}
		}
		const FString Name = Enum->GetNameStringByValue(Value);
		return Name.IsEmpty() ? FString::Printf(TEXT("%lld"), Value) : Name;
	}

	bool ImportStructValue(const UScriptStruct* Struct, void* Memory, const FString& InText, const UEnum* Enum, FString& OutError)
	{
		const FString Text = InText.TrimStartAndEnd();
		if (Struct == nullptr || Memory == nullptr)
		{
			OutError = TEXT("Value has no struct layout (data interfaces and objects are not set with text values).");
			return false;
		}

		TArray<TPair<FNumericProperty*, void*>> Leaves;
		CollectNumericLeaves(Struct, static_cast<uint8*>(Memory), Leaves);

		if (Enum)
		{
			int64 EnumValue = 0;
			if (!ParseEnumValue(Enum, Text, EnumValue))
			{
				OutError = FString::Printf(TEXT("'%s' is not an entry of %s. Entries: %s"), *Text, *Enum->GetName(), *EnumEntries(Enum));
				return false;
			}
			if (Leaves.Num() == 0)
			{
				OutError = TEXT("Enum value has no integer storage.");
				return false;
			}
			Leaves[0].Key->SetIntPropertyValue(Leaves[0].Value, EnumValue);
			return true;
		}

		if (Struct == FNiagaraBool::StaticStruct())
		{
			bool bValue = false;
			if (!ParseBool(Text, bValue))
			{
				OutError = FString::Printf(TEXT("'%s' is not a bool. Use true or false."), *Text);
				return false;
			}
			static_cast<FNiagaraBool*>(Memory)->SetValue(bValue);
			return true;
		}

		if (Text.StartsWith(TEXT("(")))
		{
			FStringOutputDevice Errors;
			const TCHAR* Result = Struct->ImportText(*Text, Memory, nullptr, PPF_None, &Errors, Struct->GetName());
			if (Result == nullptr || !Errors.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Could not parse '%s' as %s: %s"), *Text, *Struct->GetName(), *Errors);
				return false;
			}
			return true;
		}

		TArray<FString> Parts;
		Text.Replace(TEXT(","), TEXT(" ")).Replace(TEXT(";"), TEXT(" ")).ParseIntoArrayWS(Parts);
		if (Parts.Num() == 0 || Leaves.Num() == 0)
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid %s value."), *Text, *Struct->GetName());
			return false;
		}
		if (Parts.Num() != 1 && Parts.Num() != Leaves.Num())
		{
			OutError = FString::Printf(TEXT("%s expects %d numbers (or 1 to fill all), got %d: '%s'."), *Struct->GetName(), Leaves.Num(), Parts.Num(), *Text);
			return false;
		}
		for (int32 Index = 0; Index < Leaves.Num(); ++Index)
		{
			const FString& Part = Parts.Num() == 1 ? Parts[0] : Parts[Index];
			if (!Part.IsNumeric())
			{
				OutError = FString::Printf(TEXT("'%s' is not a number."), *Part);
				return false;
			}
			FNumericProperty* Leaf = Leaves[Index].Key;
			if (Leaf->IsFloatingPoint())
			{
				Leaf->SetFloatingPointPropertyValue(Leaves[Index].Value, FCString::Atod(*Part));
			}
			else
			{
				Leaf->SetIntPropertyValue(Leaves[Index].Value, static_cast<int64>(FCString::Atod(*Part)));
			}
		}
		return true;
	}

	FString ExportStructValue(const UScriptStruct* Struct, const void* Memory, const UEnum* Enum)
	{
		if (Struct == nullptr || Memory == nullptr)
		{
			return FString();
		}

		TArray<TPair<FNumericProperty*, void*>> Leaves;
		CollectNumericLeaves(Struct, static_cast<uint8*>(const_cast<void*>(Memory)), Leaves);

		if (Enum && Leaves.Num() > 0)
		{
			return EnumValueToString(Enum, Leaves[0].Key->GetSignedIntPropertyValue(Leaves[0].Value));
		}
		if (Struct == FNiagaraBool::StaticStruct())
		{
			return static_cast<const FNiagaraBool*>(Memory)->GetValue() ? TEXT("true") : TEXT("false");
		}
		if (Leaves.Num() > 0)
		{
			TArray<FString> Parts;
			for (const TPair<FNumericProperty*, void*>& Leaf : Leaves)
			{
				Parts.Add(Leaf.Key->IsFloatingPoint()
					? FString::SanitizeFloat(Leaf.Key->GetFloatingPointPropertyValue(Leaf.Value))
					: FString::Printf(TEXT("%lld"), Leaf.Key->GetSignedIntPropertyValue(Leaf.Value)));
			}
			return FString::Join(Parts, TEXT(", "));
		}

		FString Out;
		Struct->ExportText(Out, Memory, nullptr, nullptr, PPF_None, nullptr);
		return Out;
	}

	static FProperty* FindPropertyLoose(const UStruct* Struct, const FString& Name)
	{
		if (FProperty* Exact = Struct->FindPropertyByName(FName(*Name)))
		{
			return Exact;
		}
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FString DisplayName;
#if WITH_EDITORONLY_DATA
			DisplayName = It->GetDisplayNameText().ToString();
#endif
			if (LooseNameEquals(It->GetName(), Name) || (!DisplayName.IsEmpty() && LooseNameEquals(DisplayName, Name))
				|| (CastField<FBoolProperty>(*It) && LooseNameEquals(It->GetName(), TEXT("b") + Name)))
			{
				return *It;
			}
		}
		return nullptr;
	}

	bool IsUnset(const FString& Value)
	{
		const FString Trimmed = Value.TrimStartAndEnd();
		return Trimmed.IsEmpty() || Trimmed == TEXT("*")
			|| Trimmed.Equals(TEXT("Any"), ESearchCase::IgnoreCase) || Trimmed.Equals(TEXT("None"), ESearchCase::IgnoreCase)
			|| Trimmed.Equals(TEXT("Auto"), ESearchCase::IgnoreCase) || Trimmed.Equals(TEXT("Default"), ESearchCase::IgnoreCase);
	}

	FString DidYouMean(const FString& Query, const TArray<FString>& Candidates, int32 MaxSuggestions)
	{
		auto Normalize = [](const FString& In)
		{
			FString Out = In.ToLower();
			Out.ReplaceInline(TEXT(" "), TEXT(""));
			Out.ReplaceInline(TEXT("_"), TEXT(""));
			return Out;
		};
		const FString Needle = Normalize(Query);
		if (Needle.IsEmpty())
		{
			return FString();
		}
		struct FScored { FString Name; int32 Score; };
		TArray<FScored> Scored;
		for (const FString& Candidate : Candidates)
		{
			const FString Hay = Normalize(Candidate);
			int32 Score = Algo::LevenshteinDistance(Needle, Hay);
			if (Hay.Contains(Needle) || Needle.Contains(Hay))
			{
				Score = FMath::Min(Score, 1); // substring hits rank first
			}
			// Keep only reasonably close names: within ~40% of the longer name.
			if (Score <= FMath::Max(2, FMath::Max(Needle.Len(), Hay.Len()) * 2 / 5))
			{
				Scored.Add({ Candidate, Score });
			}
		}
		if (Scored.Num() == 0)
		{
			return FString();
		}
		Scored.StableSort([](const FScored& A, const FScored& B) { return A.Score < B.Score; });
		TArray<FString> Names;
		for (int32 Index = 0; Index < Scored.Num() && Names.Num() < MaxSuggestions; ++Index)
		{
			Names.AddUnique(FString::Printf(TEXT("'%s'"), *Scored[Index].Name));
		}
		return FString::Printf(TEXT(" Did you mean %s?"), *FString::Join(Names, TEXT(" or ")));
	}

	FString ListEditableProperties(const UStruct* Struct)
	{
		TArray<FString> Names;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Edit))
			{
				Names.Add(It->GetName());
			}
		}
		return FString::Join(Names, TEXT(", "));
	}

	FProperty* SetPropertyFromText(UStruct* Struct, void* Container, UObject* OwnerObject, const FString& PropertyPath, const FString& Value, FString& OutError)
	{
		TArray<FString> Segments;
		PropertyPath.ParseIntoArray(Segments, TEXT("."));
		if (Segments.Num() == 0)
		{
			OutError = TEXT("PropertyName is empty.");
			return nullptr;
		}

		FProperty* TopLevel = nullptr;
		const UStruct* CurrentStruct = Struct;
		void* CurrentContainer = Container;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FProperty* Property = FindPropertyLoose(CurrentStruct, Segments[Index]);
			if (Property == nullptr)
			{
				TArray<FString> Editable;
				ListEditableProperties(CurrentStruct).ParseIntoArray(Editable, TEXT(", "));
				OutError = FString::Printf(TEXT("[PROPERTY_NOT_FOUND] %s has no property '%s'.%s Editable properties: %s"), *CurrentStruct->GetName(), *Segments[Index],
					*DidYouMean(Segments[Index], Editable), *ListEditableProperties(CurrentStruct));
				return nullptr;
			}
			if (TopLevel == nullptr)
			{
				TopLevel = Property;
			}

			if (Index < Segments.Num() - 1)
			{
				FStructProperty* StructProperty = CastField<FStructProperty>(Property);
				if (StructProperty == nullptr)
				{
					OutError = FString::Printf(TEXT("'%s' is not a struct, so '%s' cannot be reached through it."), *Segments[Index], *PropertyPath);
					return nullptr;
				}
				CurrentContainer = StructProperty->ContainerPtrToValuePtr<void>(CurrentContainer);
				CurrentStruct = StructProperty->Struct;
				continue;
			}

			// Leaf.
			if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				const FString Trimmed = Value.TrimStartAndEnd();
				UObject* NewValue = nullptr;
				if (!Trimmed.IsEmpty() && !Trimmed.Equals(TEXT("None"), ESearchCase::IgnoreCase))
				{
					UClass* PropertyClass = ObjectProperty->PropertyClass;
					if (PropertyClass && PropertyClass->IsChildOf(UClass::StaticClass()))
					{
						NewValue = LoadObject<UClass>(nullptr, *Trimmed);
					}
					else
					{
						NewValue = LoadObjectByPath(Trimmed, PropertyClass, *Property->GetName());
					}
					if (NewValue == nullptr)
					{
						OutError = FString::Printf(TEXT("Could not load '%s' for property '%s'."), *Trimmed, *Property->GetName());
						return nullptr;
					}
				}
				ObjectProperty->SetObjectPropertyValue_InContainer(CurrentContainer, NewValue);
				return TopLevel;
			}

			if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
			{
				bool bValue = false;
				if (!ParseBool(Value, bValue))
				{
					OutError = FString::Printf(TEXT("'%s' is not a bool. Use true or false."), *Value);
					return nullptr;
				}
				BoolProperty->SetPropertyValue_InContainer(CurrentContainer, bValue);
				return TopLevel;
			}

			// Enums: accept loose entry names.
			const UEnum* Enum = nullptr;
			FNumericProperty* UnderlyingNumeric = nullptr;
			if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				Enum = EnumProperty->GetEnum();
				UnderlyingNumeric = EnumProperty->GetUnderlyingProperty();
			}
			else if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
			{
				Enum = ByteProperty->Enum;
				UnderlyingNumeric = ByteProperty;
			}
			if (Enum && UnderlyingNumeric)
			{
				int64 EnumValue = 0;
				if (!ParseEnumValue(Enum, Value, EnumValue))
				{
					OutError = FString::Printf(TEXT("'%s' is not an entry of %s. Entries: %s"), *Value, *Enum->GetName(), *EnumEntries(Enum));
					return nullptr;
				}
				UnderlyingNumeric->SetIntPropertyValue(Property->ContainerPtrToValuePtr<void>(CurrentContainer), EnumValue);
				return TopLevel;
			}

			// Plain numeric / vector style structs: reuse the friendly number parser.
			if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				if (!Value.TrimStart().StartsWith(TEXT("(")))
				{
					FString StructError;
					if (ImportStructValue(StructProperty->Struct, StructProperty->ContainerPtrToValuePtr<void>(CurrentContainer), Value, nullptr, StructError))
					{
						return TopLevel;
					}
				}
			}

			FStringOutputDevice Errors;
			const TCHAR* Result = Property->ImportText_InContainer(*Value, CurrentContainer, OwnerObject, PPF_None, &Errors);
			if (Result == nullptr)
			{
				OutError = FString::Printf(TEXT("Could not set '%s' to '%s'. %s"), *Property->GetName(), *Value, *Errors);
				return nullptr;
			}
			return TopLevel;
		}
		return TopLevel;
	}

	FString GetPropertyText(const UStruct* Struct, const void* Container, const FString& PropertyPath)
	{
		TArray<FString> Segments;
		PropertyPath.ParseIntoArray(Segments, TEXT("."));
		const UStruct* CurrentStruct = Struct;
		const void* CurrentContainer = Container;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FProperty* Property = FindPropertyLoose(CurrentStruct, Segments[Index]);
			if (Property == nullptr)
			{
				return FString();
			}
			if (Index < Segments.Num() - 1)
			{
				FStructProperty* StructProperty = CastField<FStructProperty>(Property);
				if (StructProperty == nullptr)
				{
					return FString();
				}
				CurrentContainer = StructProperty->ContainerPtrToValuePtr<void>(CurrentContainer);
				CurrentStruct = StructProperty->Struct;
				continue;
			}
			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				return EnumValueToString(EnumProperty->GetEnum(), EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr));
			}
			if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property); ByteProperty && ByteProperty->Enum)
			{
				return EnumValueToString(ByteProperty->Enum, ByteProperty->GetPropertyValue(ValuePtr));
			}
			FString Out;
			Property->ExportTextItem_Direct(Out, ValuePtr, nullptr, nullptr, PPF_None);
			return Out;
		}
		return FString();
	}
}
