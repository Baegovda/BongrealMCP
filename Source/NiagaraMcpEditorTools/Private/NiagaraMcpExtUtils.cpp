#include "NiagaraMcpExtUtils.h"

#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraParameterBinding.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "UObject/UnrealType.h"

namespace NiagaraMcpExt
{
	// Thin wrappers over the shared NiagaraMcpCommon helpers (kept so the extension files read the same).
	bool IsUnsetArg(const FString& S)
	{
		return NiagaraMcp::IsUnset(S);
	}

	FString ArgOrEmpty(const FString& S)
	{
		return IsUnsetArg(S) ? FString() : S.TrimStartAndEnd();
	}

	/** "'A', 'B'" closest candidates (no surrounding text), empty when nothing is close. */
	FString SuggestNames(const FString& Query, const TArray<FString>& Candidates)
	{
		FString Hint = NiagaraMcp::DidYouMean(Query, Candidates);
		Hint.RemoveFromStart(TEXT(" Did you mean "));
		Hint.RemoveFromEnd(TEXT("?"));
		return Hint;
	}

	FString UnknownNameMessage(const TCHAR* What, const FString& Query, const TArray<FString>& Candidates)
	{
		if (Candidates.Num() == 0)
		{
			return FString::Printf(TEXT("Unknown %s '%s'. There are none to choose from."), What, *Query);
		}
		return FString::Printf(TEXT("Unknown %s '%s'.%s Valid: %s"), What, *Query, *NiagaraMcp::DidYouMean(Query, Candidates), *FString::Join(Candidates, TEXT(", ")));
	}

	bool ParseBoolText(const FString& Text, bool& bOut)
	{
		return NiagaraMcp::ParseBool(Text, bOut);
	}

	TArray<FString> SplitFilter(const FString& Filter)
	{
		TArray<FString> Words;
		ArgOrEmpty(Filter).ToLower().ParseIntoArrayWS(Words);
		return Words;
	}

	bool MatchesWords(const FString& Haystack, const TArray<FString>& Words)
	{
		const FString Lower = Haystack.ToLower();
		for (const FString& Word : Words)
		{
			if (!Lower.Contains(Word))
			{
				return false;
			}
		}
		return true;
	}

	FProperty* FindPropertyLoose(const UStruct* Struct, const FString& Name)
	{
		if (Struct == nullptr)
		{
			return nullptr;
		}
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
			if (NiagaraMcp::LooseNameEquals(It->GetName(), Name) || (!DisplayName.IsEmpty() && NiagaraMcp::LooseNameEquals(DisplayName, Name))
				|| (CastField<FBoolProperty>(*It) && NiagaraMcp::LooseNameEquals(It->GetName(), TEXT("b") + Name)))
			{
				return *It;
			}
		}
		return nullptr;
	}

	static bool IsListedProperty(const FProperty* Property)
	{
		return Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_Deprecated) && !Property->GetName().EndsWith(TEXT("_DEPRECATED"));
	}

	TArray<FString> EditablePropertyNames(const UStruct* Struct)
	{
		TArray<FString> Names;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (IsListedProperty(*It))
			{
				Names.Add(It->GetName());
			}
		}
		return Names;
	}

	TArray<FString> EnumOptions(const UEnum* Enum)
	{
		TArray<FString> Options;
		if (Enum == nullptr)
		{
			return Options;
		}
		for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
		{
			const FString Name = Enum->GetNameStringByIndex(Index);
			if (Name.EndsWith(TEXT("_MAX")) || Name == TEXT("MAX"))
			{
				continue;
			}
#if WITH_EDITORONLY_DATA
			if (Enum->HasMetaData(TEXT("Hidden"), Index))
			{
				continue;
			}
#endif
			Options.Add(Name);
		}
		return Options;
	}

	static const UEnum* EnumOf(const FProperty* Property)
	{
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return EnumProperty->GetEnum();
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			return ByteProperty->Enum;
		}
		return nullptr;
	}

	bool IsParameterBindingProperty(const FProperty* Property)
	{
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		return StructProperty && StructProperty->Struct && StructProperty->Struct->IsChildOf(FNiagaraParameterBinding::StaticStruct());
	}

	FString ParameterBindingText(const FStructProperty& Property, const void* Container)
	{
		FNiagaraParameterBinding& Binding = *const_cast<FNiagaraParameterBinding*>(Property.ContainerPtrToValuePtr<FNiagaraParameterBinding>(Container));
		const FNiagaraVariable& Default = Binding.GetDefaultAliasedParameter();
		if (Binding.AliasedParameter.GetName() != Default.GetName() && !Binding.AliasedParameter.GetName().IsNone())
		{
			return TEXT("@") + Binding.AliasedParameter.GetName().ToString();
		}
		if (Binding.HasDefaultValueEditorOnly())
		{
			const TConstArrayView<uint8> Bytes = Binding.GetDefaultValueEditorOnly();
			const FNiagaraTypeDefinition& Type = Default.GetType();
			if (Type.IsValid() && Type.GetScriptStruct() && Bytes.Num() == Type.GetSize())
			{
				return NiagaraMcp::ExportStructValue(Type.GetScriptStruct(), Bytes.GetData(), Type.GetEnum());
			}
		}
		return Binding.AliasedParameter.GetName().IsNone() ? FString(TEXT("None")) : TEXT("@") + Binding.AliasedParameter.GetName().ToString();
	}

	bool SetParameterBindingFromText(const FStructProperty& Property, void* Container, const FString& InValue, const FString& EmitterUniqueName, FString& OutError)
	{
		FNiagaraParameterBinding& Binding = *Property.ContainerPtrToValuePtr<FNiagaraParameterBinding>(Container);
		const FNiagaraTypeDefinition Type = Binding.GetDefaultAliasedParameter().GetType();
		const FString Value = InValue.TrimStartAndEnd();
		if (Value.StartsWith(TEXT("@")))
		{
			const FString Name = Value.Mid(1).TrimStartAndEnd();
			const bool bUser = Name.StartsWith(TEXT("User."));
			const bool bSystem = Name.StartsWith(TEXT("System."));
			const bool bEmitter = Name.StartsWith(TEXT("Emitter."));
			const bool bParticle = Name.StartsWith(TEXT("Particles."));
			if (!((bUser && Binding.AllowUserParameters()) || (bSystem && Binding.AllowSystemParameters()) || (bEmitter && Binding.AllowEmitterParameters()) || (bParticle && Binding.AllowParticleParameters())))
			{
				OutError = FString::Printf(TEXT("'%s' cannot be bound here. Allowed namespaces:%s%s%s%s"), *Name,
					Binding.AllowUserParameters() ? TEXT(" User.") : TEXT(""), Binding.AllowSystemParameters() ? TEXT(" System.") : TEXT(""),
					Binding.AllowEmitterParameters() ? TEXT(" Emitter.") : TEXT(""), Binding.AllowParticleParameters() ? TEXT(" Particles.") : TEXT(""));
				return false;
			}
			if (!Type.IsValid())
			{
				OutError = TEXT("The binding has no type to bind with.");
				return false;
			}
			Binding.AliasedParameter = FNiagaraVariableBase(Type, FName(*Name));
			Binding.ResolvedParameter = FNiagaraVariableBase(Type, FName(*(bEmitter && !EmitterUniqueName.IsEmpty() ? EmitterUniqueName + Name.Mid(7) : Name)));
			return true;
		}
		if (!Binding.HasDefaultValueEditorOnly() || !Type.IsValid() || Type.GetScriptStruct() == nullptr)
		{
			OutError = TEXT("This binding only takes a parameter; use @User.Name (or @System. / @Emitter.).");
			return false;
		}
		FStructOnScope Parsed(Type.GetScriptStruct());
		if (!NiagaraMcp::ImportStructValue(Type.GetScriptStruct(), Parsed.GetStructMemory(), Value, Type.GetEnum(), OutError))
		{
			OutError += TEXT(" (or bind a parameter with @User.Name)");
			return false;
		}
		Binding.SetToDefault();
		Binding.SetDefaultValueEditorOnly(MakeArrayView(Parsed.GetStructMemory(), Type.GetSize()));
		return true;
	}

	FString PropertyValueText(const FProperty* Property, const void* Container, int32 MaxLen)
	{
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
		FString Out;
		if (IsParameterBindingProperty(Property))
		{
			Out = ParameterBindingText(*CastField<FStructProperty>(Property), Container);
		}
		else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			Out = NiagaraMcp::EnumValueToString(EnumProperty->GetEnum(), EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr));
		}
		else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property); ByteProperty && ByteProperty->Enum)
		{
			Out = NiagaraMcp::EnumValueToString(ByteProperty->Enum, ByteProperty->GetPropertyValue(ValuePtr));
		}
		else if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			Out = BoolProperty->GetPropertyValue(ValuePtr) ? TEXT("true") : TEXT("false");
		}
		else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			const UObject* Object = ObjectProperty->GetObjectPropertyValue(ValuePtr);
			Out = Object ? Object->GetPathName() : FString(TEXT("None"));
		}
		else
		{
			Property->ExportTextItem_Direct(Out, ValuePtr, nullptr, nullptr, PPF_None);
		}
		if (MaxLen > 0 && Out.Len() > MaxLen)
		{
			Out = Out.Left(MaxLen) + TEXT("...");
		}
		return Out;
	}

	static FString FirstSentence(const FString& Text, int32 MaxLen)
	{
		FString Out = Text.Replace(TEXT("\n"), TEXT(" ")).Replace(TEXT("\r"), TEXT(""));
		int32 Dot = INDEX_NONE;
		if (Out.FindChar(TEXT('.'), Dot) && Dot > 10)
		{
			Out = Out.Left(Dot + 1);
		}
		if (Out.Len() > MaxLen)
		{
			Out = Out.Left(MaxLen) + TEXT("...");
		}
		return Out;
	}

	void DescribeEditableProperties(const UStruct* Struct, const void* Container, const FString& Filter, TArray<FNiagaraMcpEditableProperty>& OutProperties,
		TFunctionRef<bool(const FProperty*)> Skip)
	{
		const TArray<FString> Words = SplitFilter(Filter);
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Property = *It;
			if (!IsListedProperty(Property) || Skip(Property))
			{
				continue;
			}
			FNiagaraMcpEditableProperty Info;
			Info.Name = Property->GetName();
			Info.Type = Property->GetCPPType();
#if WITH_EDITORONLY_DATA
			Info.Category = Property->GetMetaData(TEXT("Category"));
			Info.Tooltip = FirstSentence(Property->GetToolTipText().ToString(), 160);
#endif
			if (!MatchesWords(Info.Name + TEXT(" ") + Info.Category + TEXT(" ") + Info.Type, Words))
			{
				continue;
			}
			Info.Value = PropertyValueText(Property, Container);
			Info.Options = EnumOptions(EnumOf(Property));
			Info.bReadOnly = Property->HasAnyPropertyFlags(CPF_EditConst);
			OutProperties.Add(MoveTemp(Info));
		}
	}

	bool DryRunSetProperty(UStruct* Struct, const void* Container, UObject* OwnerObject, const FString& PropertyPath, const FString& Value, FString& OutError)
	{
		TArray<FString> Segments;
		PropertyPath.ParseIntoArray(Segments, TEXT("."));
		if (Segments.Num() == 0)
		{
			OutError = TEXT("PropertyName is empty.");
			return false;
		}
		FProperty* TopLevel = FindPropertyLoose(Struct, Segments[0].TrimStartAndEnd());
		if (TopLevel == nullptr)
		{
			OutError = UnknownNameMessage(TEXT("property"), Segments[0], EditablePropertyNames(Struct));
			return false;
		}
		if (!TopLevel->HasAnyPropertyFlags(CPF_Edit) || TopLevel->HasAnyPropertyFlags(CPF_EditConst))
		{
			OutError = FString::Printf(TEXT("Property '%s' is not editable. Editable: %s"), *TopLevel->GetName(), *FString::Join(EditablePropertyNames(Struct), TEXT(", ")));
			return false;
		}

		// Scratch copy of the top-level value, addressed through a fake container so the regular setter can be reused.
		void* Scratch = FMemory::Malloc(TopLevel->GetSize(), TopLevel->GetMinAlignment());
		TopLevel->InitializeValue(Scratch);
		TopLevel->CopyCompleteValue(Scratch, TopLevel->ContainerPtrToValuePtr<void>(Container));
		void* FakeContainer = static_cast<uint8*>(Scratch) - TopLevel->GetOffset_ForInternal();
		FString Error;
		const bool bOk = NiagaraMcp::SetPropertyFromText(Struct, FakeContainer, OwnerObject, PropertyPath, Value, Error) != nullptr;
		TopLevel->DestroyValue(Scratch);
		FMemory::Free(Scratch);
		if (!bOk)
		{
			OutError = Error;
		}
		return bOk;
	}

	bool IsPropertyDefault(const FProperty* Property, const void* Container, const void* DefaultContainer)
	{
		return DefaultContainer != nullptr && Property->Identical_InContainer(Container, DefaultContainer, 0, PPF_None);
	}

	FNiagaraEmitterHandle* FindHandleById(UNiagaraSystem& System, const FGuid& Id)
	{
		for (FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
		{
			if (Handle.GetId() == Id)
			{
				return &Handle;
			}
		}
		return nullptr;
	}

	FNiagaraMcpEmitterInfo DescribeEmitterInfo(const FNiagaraEmitterHandle& Handle)
	{
		FNiagaraMcpEmitterInfo Info;
		Info.Name = Handle.GetName();
		Info.bEnabled = Handle.GetIsEnabled();
		if (FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData())
		{
			Info.SimTarget = StaticEnum<ENiagaraSimTarget>()->GetNameStringByValue(static_cast<int64>(EmitterData->SimTarget));
			Info.bLocalSpace = EmitterData->bLocalSpace;
			const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
			for (int32 Index = 0; Index < Renderers.Num(); ++Index)
			{
				FNiagaraMcpRendererInfo& RendererInfo = Info.Renderers.AddDefaulted_GetRef();
				RendererInfo.Index = Index;
				if (UNiagaraRendererProperties* Renderer = Renderers[Index])
				{
					RendererInfo.Class = Renderer->GetClass()->GetName();
					RendererInfo.bEnabled = Renderer->GetIsEnabled();
					RendererInfo.Material = NiagaraMcp::GetPropertyText(Renderer->GetClass(), Renderer, TEXT("Material"));
				}
			}
			for (const UNiagaraSimulationStageBase* Stage : EmitterData->GetSimulationStages())
			{
				if (Stage)
				{
					Info.SimulationStages.Add(Stage->SimulationStageName.ToString());
				}
			}
			Info.EventHandlerCount = EmitterData->GetEventHandlers().Num();
		}
		return Info;
	}

	FNiagaraMcpEmitterStages DescribeEmitterStages(UNiagaraSystem& System, FNiagaraEmitterHandle& Handle)
	{
		FNiagaraMcpEmitterStages Info;
		Info.Emitter = Handle.GetName();
		const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		if (EmitterData == nullptr)
		{
			return Info;
		}
		Info.SimTarget = StaticEnum<ENiagaraSimTarget>()->GetNameStringByValue(static_cast<int64>(EmitterData->SimTarget));
		const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
		for (int32 Index = 0; Index < Stages.Num(); ++Index)
		{
			const UNiagaraSimulationStageBase* Stage = Stages[Index];
			if (Stage == nullptr)
			{
				continue;
			}
			FNiagaraMcpStageInfo& StageInfo = Info.SimulationStages.AddDefaulted_GetRef();
			StageInfo.StageIndex = Index;
			StageInfo.Name = Stage->SimulationStageName.ToString();
			StageInfo.Class = Stage->GetClass()->GetName();
			StageInfo.bEnabled = Stage->bEnabled != 0;
			if (const UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage))
			{
				StageInfo.IterationSource = StaticEnum<ENiagaraIterationSource>()->GetNameStringByValue(static_cast<int64>(Generic->IterationSource));
				StageInfo.DataInterface = Generic->DataInterface.BoundVariable.GetName().ToString();
			}
		}
		const TArray<FNiagaraEventScriptProperties>& Handlers = EmitterData->GetEventHandlers();
		for (int32 Index = 0; Index < Handlers.Num(); ++Index)
		{
			const FNiagaraEventScriptProperties& Props = Handlers[Index];
			FNiagaraMcpStageInfo& HandlerInfo = Info.EventHandlers.AddDefaulted_GetRef();
			HandlerInfo.StageIndex = Index;
			HandlerInfo.Name = TEXT("Event Handler");
			HandlerInfo.Class = TEXT("NiagaraEventHandler");
			const FNiagaraEmitterHandle* Source = Props.SourceEmitterID.IsValid() ? FindHandleById(System, Props.SourceEmitterID) : nullptr;
			HandlerInfo.SourceEmitter = Source ? Source->GetName().ToString() : Handle.GetName().ToString();
			HandlerInfo.SourceEvent = Props.SourceEventName.ToString();
			HandlerInfo.ExecutionMode = StaticEnum<EScriptExecutionMode>()->GetNameStringByValue(static_cast<int64>(Props.ExecutionMode));
			HandlerInfo.SpawnNumber = static_cast<int32>(Props.SpawnNumber);
			HandlerInfo.MaxEventsPerFrame = static_cast<int32>(Props.MaxEventsPerFrame);
		}
		return Info;
	}

	UNiagaraRendererProperties* FindRendererByIndex(FNiagaraEmitterHandle& Handle, int32 RendererIndex)
	{
		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		const TArray<UNiagaraRendererProperties*>* Renderers = EmitterData ? &EmitterData->GetRenderers() : nullptr;
		if (Renderers == nullptr || !Renderers->IsValidIndex(RendererIndex) || (*Renderers)[RendererIndex] == nullptr)
		{
			TArray<FString> Names;
			for (int32 Index = 0; Renderers && Index < Renderers->Num(); ++Index)
			{
				Names.Add(FString::Printf(TEXT("%d=%s"), Index, (*Renderers)[Index] ? *(*Renderers)[Index]->GetClass()->GetName() : TEXT("null")));
			}
			NiagaraMcp::ReportError(FString::Printf(TEXT("Emitter '%s' has no renderer at index %d. Renderers: %s"), *Handle.GetName().ToString(), RendererIndex,
				Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("(none)")));
			return nullptr;
		}
		return (*Renderers)[RendererIndex];
	}

	FString ParameterValueText(const FNiagaraParameterStore& Store, const FNiagaraVariable& Variable)
	{
		const FNiagaraTypeDefinition& Type = Variable.GetType();
		if (Type.IsDataInterface())
		{
			const UNiagaraDataInterface* DataInterface = Store.GetDataInterface(Variable);
			return DataInterface ? DataInterface->GetClass()->GetName() : FString(TEXT("None"));
		}
		if (Type.IsUObject())
		{
			const int32 Offset = Store.IndexOf(Variable);
			const UObject* Object = Offset != INDEX_NONE ? Store.GetUObject(Offset).Get() : nullptr;
			return Object ? Object->GetPathName() : FString(TEXT("None"));
		}
		const uint8* Data = Store.GetParameterData(Variable);
		return Data ? NiagaraMcp::ExportStructValue(Type.GetScriptStruct(), Data, Type.GetEnum()) : FString();
	}

	FName UserParameterName(FName ParameterName)
	{
		const FString Name = ParameterName.ToString().TrimStartAndEnd();
		return Name.StartsWith(TEXT("User."), ESearchCase::IgnoreCase) ? FName(*(TEXT("User.") + Name.Mid(5))) : FName(*(TEXT("User.") + Name));
	}

	bool FindUserParameter(UNiagaraSystem& System, FName ParameterName, FNiagaraVariable& OutVariable, bool bReport)
	{
		const FString Wanted = UserParameterName(ParameterName).ToString();
		TArray<FNiagaraVariable> UserParameters;
		System.GetExposedParameters().GetUserParameters(UserParameters);
		TArray<FString> Names;
		for (const FNiagaraVariable& Parameter : UserParameters)
		{
			FString Name = Parameter.GetName().ToString();
			// GetUserParameters can return the redirected names without the namespace.
			if (!Name.StartsWith(TEXT("User.")))
			{
				Name = TEXT("User.") + Name;
			}
			if (Name.Equals(Wanted, ESearchCase::IgnoreCase))
			{
				OutVariable = FNiagaraVariable(Parameter.GetType(), FName(*Name));
				return true;
			}
			Names.Add(Name);
		}
		if (bReport)
		{
			NiagaraMcp::ReportError(UnknownNameMessage(TEXT("user parameter"), Wanted, Names));
		}
		return false;
	}

	TArray<FNiagaraMcpParameterInfo> DescribeUserParameters(UNiagaraSystem& System)
	{
		TArray<FNiagaraMcpParameterInfo> Result;
		TArray<FNiagaraVariable> UserParameters;
		System.GetExposedParameters().GetUserParameters(UserParameters);
		for (const FNiagaraVariable& Parameter : UserParameters)
		{
			FNiagaraMcpParameterInfo& Info = Result.AddDefaulted_GetRef();
			// GetUserParameters strips the namespace; report the full name ("User.Color"), the form links and the
			// user-parameter tools take, so a name can be copied into "@User.Color" as is.
			Info.Name = Parameter.GetName().ToString();
			if (!Info.Name.StartsWith(TEXT("User.")))
			{
				Info.Name = TEXT("User.") + Info.Name;
			}
			Info.Type = Parameter.GetType().GetName();
			Info.Value = ParameterValueText(System.GetExposedParameters(), Parameter);
		}
		return Result;
	}
}
