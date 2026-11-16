// v0.4 extensions: function specifiers, dynamic input search, system specs and diffs, particle attribute readback.

#include "NiagaraMcpEditorTools.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "NiagaraComponent.h"
#include "NiagaraDataSet.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraGraph.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraMcpPreview.h"
#include "NiagaraMcpEditorToolsInternal.h"
#include "NiagaraMcpExtUtils.h"
#include "NiagaraMcpStackUtils.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSimCache.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "PreviewScene.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StrongObjectPtr.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

#define LOCTEXT_NAMESPACE "NiagaraMcpEditorTools"

using namespace NiagaraMcp;

namespace NiagaraMcpExtB
{
	// UNiagaraNode::ReallocatePins is exported but protected; reach it through a member pointer of a derived type
	// (never instantiated), as the graph tools do.
	struct FExtNiagaraNodeAccess : public UNiagaraNode
	{
		using FReallocatePinsFn = bool (UNiagaraNode::*)(bool);
		static FReallocatePinsFn ReallocatePinsFn() { return &FExtNiagaraNodeAccess::ReallocatePins; }
	};

	FNiagaraMcpFunctionSpecifierInfo DescribeSpecifiers(const UNiagaraNodeFunctionCall& Node)
	{
		FNiagaraMcpFunctionSpecifierInfo Info;
		Info.NodeId = Node.NodeGuid.ToString(EGuidFormats::Digits);
		Info.Function = Node.Signature.Name.ToString();
		TArray<FName> Keys;
		Node.FunctionSpecifiers.GetKeys(Keys);
		Keys.Sort(FNameLexicalLess());
		for (const FName& Key : Keys)
		{
			Info.Specifiers.Add(FString::Printf(TEXT("%s=%s"), *Key.ToString(), *Node.FunctionSpecifiers.FindRef(Key).ToString()));
		}
		return Info;
	}

	/** Output value type of a dynamic input script (first non parameter-map output of its output node). */
	FNiagaraTypeDefinition DynamicInputOutputType(UNiagaraScript& Script)
	{
		UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script.GetLatestSource());
		UNiagaraGraph* Graph = Source ? Source->NodeGraph.Get() : nullptr;
		if (Graph == nullptr)
		{
			return FNiagaraTypeDefinition();
		}
		TArray<UNiagaraNodeOutput*> OutputNodes;
		Graph->GetNodesOfClass(OutputNodes);
		for (const UNiagaraNodeOutput* OutputNode : OutputNodes)
		{
			for (const FNiagaraVariable& Output : OutputNode->GetOutputs())
			{
				if (Output.GetType() != FNiagaraTypeDefinition::GetParameterMapDef())
				{
					return Output.GetType();
				}
			}
		}
		return FNiagaraTypeDefinition();
	}

	// ---------------------------------------------------------------- Spec building

	void SetIfAny(const TSharedRef<FJsonObject>& Parent, const FString& Key, const TSharedRef<FJsonObject>& Child)
	{
		if (Child->Values.Num() > 0)
		{
			Parent->SetObjectField(Key, Child);
		}
	}

	/** Non-default editable properties of Struct at Container, compared with DefaultContainer. */
	TSharedRef<FJsonObject> NonDefaultProperties(const UStruct* Struct, const void* Container, const void* DefaultContainer, TFunctionRef<bool(const FProperty*)> Skip)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Deprecated) || Property->GetName().EndsWith(TEXT("_DEPRECATED")) || Skip(Property))
			{
				continue;
			}
			if (NiagaraMcpExt::IsPropertyDefault(Property, Container, DefaultContainer))
			{
				continue;
			}
			Out->SetStringField(Property->GetName(), NiagaraMcpExt::PropertyValueText(Property, Container, 400));
		}
		return Out;
	}

	bool IsBindingProperty(const FProperty* Property)
	{
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		if (StructProperty && (StructProperty->Struct == FNiagaraVariableAttributeBinding::StaticStruct() || StructProperty->Struct == FNiagaraRendererMaterialParameters::StaticStruct()))
		{
			return true;
		}
		const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
		const FStructProperty* Inner = ArrayProperty ? CastField<FStructProperty>(ArrayProperty->Inner) : nullptr;
		return Inner && (Inner->Struct == FNiagaraMaterialAttributeBinding::StaticStruct() || Inner->Struct == FNiagaraVariableAttributeBinding::StaticStruct());
	}

	/** Overridden inputs of a module (and of its dynamic inputs, as "Parent/Child"). */
	void CollectOverriddenInputs(UNiagaraStackEntry& Entry, const FString& Prefix, const TSharedRef<FJsonObject>& Out, int32 Depth)
	{
		if (Depth > 4)
		{
			return;
		}
		TArray<UNiagaraStackFunctionInput*> Inputs;
		if (UNiagaraStackModuleItem* Module = Cast<UNiagaraStackModuleItem>(&Entry))
		{
			Stack::GetModuleInputs(*Module, Inputs);
		}
		else
		{
			// Inputs of a dynamic input: its direct function-input descendants.
			TArray<UNiagaraStackEntry*> Pending;
			Entry.GetUnfilteredChildren(Pending);
			while (Pending.Num() > 0)
			{
				UNiagaraStackEntry* Child = Pending.Pop(EAllowShrinking::No);
				if (UNiagaraStackFunctionInput* Input = Cast<UNiagaraStackFunctionInput>(Child))
				{
					Inputs.Add(Input);
				}
				else if (Child)
				{
					Child->GetUnfilteredChildren(Pending);
				}
			}
			Inputs.Sort([](const UNiagaraStackFunctionInput& A, const UNiagaraStackFunctionInput& B) { return A.GetInputParameterHandle().GetName().LexicalLess(B.GetInputParameterHandle().GetName()); });
		}
		for (UNiagaraStackFunctionInput* Input : Inputs)
		{
			if (Input == nullptr || !Input->CanReset())
			{
				continue;
			}
			const FString Name = Prefix + Input->GetInputParameterHandle().GetName().ToString();
			Out->SetStringField(Name, Stack::InputValueToString(*Input));
			if (Input->GetValueMode() == UNiagaraStackFunctionInput::EValueMode::Dynamic)
			{
				CollectOverriddenInputs(*Input, Name + TEXT("/"), Out, Depth + 1);
			}
		}
	}

	FString StackKeyFor(const UNiagaraStackModuleItem& Item, const FVersionedNiagaraEmitterData* EmitterData)
	{
		const UNiagaraNodeOutput* OutputNode = Item.GetOutputNode();
		const ENiagaraScriptUsage Usage = Stack::GetModuleUsage(Item);
		FString Key = UsageToString(Usage);
		Key.RemoveFromEnd(TEXT("Script"));
		if (EmitterData && OutputNode && Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
		{
			for (const UNiagaraSimulationStageBase* Stage : EmitterData->GetSimulationStages())
			{
				if (Stage && Stage->Script && Stage->Script->GetUsageId() == OutputNode->GetUsageId())
				{
					return TEXT("SimulationStage:") + Stage->SimulationStageName.ToString();
				}
			}
		}
		if (EmitterData && OutputNode && Usage == ENiagaraScriptUsage::ParticleEventScript)
		{
			const TArray<FNiagaraEventScriptProperties>& Handlers = EmitterData->GetEventHandlers();
			for (int32 Index = 0; Index < Handlers.Num(); ++Index)
			{
				if (Handlers[Index].Script && Handlers[Index].Script->GetUsageId() == OutputNode->GetUsageId())
				{
					return FString::Printf(TEXT("EventHandler:%d"), Index);
				}
			}
		}
		return Key;
	}

	TSharedRef<FJsonObject> DescribeStacks(UNiagaraStackViewModel& StackViewModel, const FVersionedNiagaraEmitterData* EmitterData)
	{
		TSharedRef<FJsonObject> Stacks = MakeShared<FJsonObject>();
		TArray<UNiagaraStackModuleItem*> Items;
		Stack::CollectModuleItems(StackViewModel, Items);
		TMap<FString, int32> Counters;
		for (UNiagaraStackModuleItem* Item : Items)
		{
			const FString StackKey = StackKeyFor(*Item, EmitterData);
			TSharedPtr<FJsonObject> StackObject = Stacks->HasField(StackKey) ? Stacks->GetObjectField(StackKey) : nullptr;
			if (!StackObject.IsValid())
			{
				StackObject = MakeShared<FJsonObject>();
				Stacks->SetObjectField(StackKey, StackObject);
			}
			int32& Index = Counters.FindOrAdd(StackKey);
			FString ModuleKey = Item->GetDisplayName().ToString();
			for (int32 Suffix = 2; StackObject->HasField(ModuleKey); ++Suffix)
			{
				ModuleKey = FString::Printf(TEXT("%s #%d"), *Item->GetDisplayName().ToString(), Suffix);
			}
			TSharedRef<FJsonObject> ModuleObject = MakeShared<FJsonObject>();
			ModuleObject->SetNumberField(TEXT("index"), Index++);
			if (const UNiagaraScript* Script = Item->GetModuleNode().FunctionScript)
			{
				ModuleObject->SetStringField(TEXT("script"), Script->GetPathName());
			}
			if (!Item->GetIsEnabled())
			{
				ModuleObject->SetBoolField(TEXT("enabled"), false);
			}
			TSharedRef<FJsonObject> Inputs = MakeShared<FJsonObject>();
			CollectOverriddenInputs(*Item, FString(), Inputs, 0);
			SetIfAny(ModuleObject, TEXT("inputs"), Inputs);
			StackObject->SetObjectField(ModuleKey, ModuleObject);
		}
		return Stacks;
	}

	TSharedRef<FJsonObject> DescribeRendererSpec(UNiagaraRendererProperties& Renderer)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!Renderer.GetIsEnabled())
		{
			Out->SetBoolField(TEXT("enabled"), false);
		}
		const UObject* Defaults = Renderer.GetClass()->GetDefaultObject();
		SetIfAny(Out, TEXT("properties"), NonDefaultProperties(Renderer.GetClass(), &Renderer, Defaults, [](const FProperty* Property) { return IsBindingProperty(Property) || Property->GetFName() == TEXT("bIsEnabled"); }));
		TSharedRef<FJsonObject> Bindings = MakeShared<FJsonObject>();
		for (TFieldIterator<FStructProperty> It(Renderer.GetClass()); It; ++It)
		{
			if (It->Struct != FNiagaraVariableAttributeBinding::StaticStruct() || It->HasAnyPropertyFlags(CPF_Deprecated))
			{
				continue;
			}
			const FNiagaraVariableAttributeBinding* Binding = It->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(&Renderer);
			const FNiagaraVariableAttributeBinding* Default = It->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(Defaults);
			const FName Bound = Binding->GetParamMapBindableVariable().GetName();
			if (Default == nullptr || Default->GetParamMapBindableVariable().GetName() != Bound)
			{
				Bindings->SetStringField(It->GetName(), Bound.ToString());
			}
		}
		SetIfAny(Out, TEXT("bindings"), Bindings);
		TSharedRef<FJsonObject> MaterialBindings = MakeShared<FJsonObject>();
		for (TFieldIterator<FStructProperty> It(Renderer.GetClass()); It; ++It)
		{
			if (It->Struct == FNiagaraRendererMaterialParameters::StaticStruct())
			{
				for (const FNiagaraMaterialAttributeBinding& Binding : It->ContainerPtrToValuePtr<FNiagaraRendererMaterialParameters>(&Renderer)->AttributeBindings)
				{
					MaterialBindings->SetStringField(Binding.MaterialParameterName.ToString(), Binding.NiagaraVariable.GetName().ToString());
				}
			}
		}
		SetIfAny(Out, TEXT("materialBindings"), MaterialBindings);
		return Out;
	}

	TSharedRef<FJsonObject> BuildSpec(UNiagaraSystem& System)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> SystemObject = MakeShared<FJsonObject>();
		SystemObject->SetStringField(TEXT("path"), System.GetPathName());
		SetIfAny(SystemObject, TEXT("properties"), NonDefaultProperties(System.GetClass(), &System, System.GetClass()->GetDefaultObject(), [](const FProperty*) { return false; }));
		Root->SetObjectField(TEXT("system"), SystemObject);

		TSharedRef<FJsonObject> UserParameters = MakeShared<FJsonObject>();
		for (const FNiagaraMcpParameterInfo& Parameter : NiagaraMcpExt::DescribeUserParameters(System))
		{
			UserParameters->SetStringField(Parameter.Name, FString::Printf(TEXT("%s = %s"), *Parameter.Type, *Parameter.Value));
		}
		SetIfAny(Root, TEXT("userParameters"), UserParameters);

		TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(System);
		if (ViewModel.IsValid())
		{
			if (UNiagaraStackViewModel* SystemStack = ViewModel->GetSystemStackViewModel())
			{
				SetIfAny(Root, TEXT("systemStacks"), DescribeStacks(*SystemStack, nullptr));
			}
		}

		TSharedRef<FJsonObject> Emitters = MakeShared<FJsonObject>();
		FStructOnScope DefaultEmitterData(FVersionedNiagaraEmitterData::StaticStruct());
		for (FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
		{
			TSharedRef<FJsonObject> EmitterObject = MakeShared<FJsonObject>();
			EmitterObject->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
			FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
			if (EmitterData)
			{
				EmitterObject->SetStringField(TEXT("simTarget"), StaticEnum<ENiagaraSimTarget>()->GetNameStringByValue(static_cast<int64>(EmitterData->SimTarget)));
				SetIfAny(EmitterObject, TEXT("properties"), NonDefaultProperties(FVersionedNiagaraEmitterData::StaticStruct(), EmitterData, DefaultEmitterData.GetStructMemory(),
					[](const FProperty* Property) { return Property->GetFName() == TEXT("SimTarget"); }));
			}
			if (ViewModel.IsValid())
			{
				if (UNiagaraStackViewModel* EmitterStack = Stack::GetStackViewModel(*ViewModel, Handle.GetName()))
				{
					SetIfAny(EmitterObject, TEXT("stacks"), DescribeStacks(*EmitterStack, EmitterData));
				}
			}
			if (EmitterData)
			{
				TSharedRef<FJsonObject> Renderers = MakeShared<FJsonObject>();
				const TArray<UNiagaraRendererProperties*>& RendererList = EmitterData->GetRenderers();
				for (int32 Index = 0; Index < RendererList.Num(); ++Index)
				{
					if (RendererList[Index])
					{
						Renderers->SetObjectField(FString::Printf(TEXT("%d:%s"), Index, *RendererList[Index]->GetClass()->GetName()), DescribeRendererSpec(*RendererList[Index]));
					}
				}
				SetIfAny(EmitterObject, TEXT("renderers"), Renderers);

				TSharedRef<FJsonObject> Stages = MakeShared<FJsonObject>();
				const TArray<UNiagaraSimulationStageBase*>& StageList = EmitterData->GetSimulationStages();
				for (int32 Index = 0; Index < StageList.Num(); ++Index)
				{
					UNiagaraSimulationStageBase* Stage = StageList[Index];
					if (Stage == nullptr)
					{
						continue;
					}
					TSharedRef<FJsonObject> StageObject = MakeShared<FJsonObject>();
					StageObject->SetNumberField(TEXT("index"), Index);
					StageObject->SetStringField(TEXT("class"), Stage->GetClass()->GetName());
					SetIfAny(StageObject, TEXT("properties"), NonDefaultProperties(Stage->GetClass(), Stage, Stage->GetClass()->GetDefaultObject(),
						[](const FProperty* Property) { return Property->GetFName() == TEXT("SimulationStageName"); }));
					Stages->SetObjectField(Stage->SimulationStageName.ToString(), StageObject);
				}
				SetIfAny(EmitterObject, TEXT("simulationStages"), Stages);

				TSharedRef<FJsonObject> Handlers = MakeShared<FJsonObject>();
				const TArray<FNiagaraEventScriptProperties>& HandlerList = EmitterData->GetEventHandlers();
				for (int32 Index = 0; Index < HandlerList.Num(); ++Index)
				{
					const FNiagaraEventScriptProperties& Props = HandlerList[Index];
					TSharedRef<FJsonObject> HandlerObject = MakeShared<FJsonObject>();
					const FNiagaraEmitterHandle* Source = Props.SourceEmitterID.IsValid() ? NiagaraMcpExt::FindHandleById(System, Props.SourceEmitterID) : nullptr;
					HandlerObject->SetStringField(TEXT("sourceEmitter"), Source ? Source->GetName().ToString() : Handle.GetName().ToString());
					HandlerObject->SetStringField(TEXT("sourceEvent"), Props.SourceEventName.ToString());
					HandlerObject->SetStringField(TEXT("executionMode"), StaticEnum<EScriptExecutionMode>()->GetNameStringByValue(static_cast<int64>(Props.ExecutionMode)));
					HandlerObject->SetNumberField(TEXT("spawnNumber"), Props.SpawnNumber);
					HandlerObject->SetNumberField(TEXT("maxEventsPerFrame"), Props.MaxEventsPerFrame);
					Handlers->SetObjectField(FString::FromInt(Index), HandlerObject);
				}
				SetIfAny(EmitterObject, TEXT("eventHandlers"), Handlers);
			}
			Emitters->SetObjectField(Handle.GetName().ToString(), EmitterObject);
		}
		Root->SetObjectField(TEXT("emitters"), Emitters);
		return Root;
	}

	FString JsonValueText(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return FString();
		}
		switch (Value->Type)
		{
		case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Number: return FString::SanitizeFloat(Value->AsNumber(), 0);
		case EJson::String: return Value->AsString();
		default: return FString();
		}
	}

	void Flatten(const TSharedPtr<FJsonObject>& Object, const FString& Prefix, TMap<FString, FString>& Out)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			const FString Path = Prefix.IsEmpty() ? Pair.Key : Prefix + TEXT(".") + Pair.Key;
			if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
			{
				Flatten(Pair.Value->AsObject(), Path, Out);
			}
			else
			{
				Out.Add(Path, JsonValueText(Pair.Value));
			}
		}
	}

	// ---------------------------------------------------------------- Particle readback

	FPreviewScene::ConstructionValues PreviewValues()
	{
		return FPreviewScene::ConstructionValues()
			.SetEditor(true)
			.SetTransactional(false)
			.SetCreatePhysicsScene(false)
			.AllowAudioPlayback(false)
			.SetCreateDefaultLighting(false);
	}

	FString FormatNumber(double Value)
	{
		return FString::Printf(TEXT("%.4g"), Value);
	}

	FString JoinNumbers(const TArray<double>& Values)
	{
		TArray<FString> Parts;
		for (double Value : Values)
		{
			Parts.Add(FormatNumber(Value));
		}
		return FString::Join(Parts, TEXT(", "));
	}

	FString ShortAttributeName(const FName Name)
	{
		FString Text = Name.ToString();
		Text.RemoveFromStart(TEXT("Particles."));
		return Text;
	}

	/** Count / min / max / mean / first samples of an attribute; Value(Instance, Component) returns one component. */
	FNiagaraMcpAttributeStats BuildAttributeStats(const FNiagaraVariableBase& Variable, int32 NumInstances, int32 NumComponents, int32 MaxSamples, TFunctionRef<double(int32 Instance, int32 Component)> Value)
	{
		FNiagaraMcpAttributeStats Stats;
		Stats.Attribute = ShortAttributeName(Variable.GetName());
		Stats.Type = Variable.GetType().GetName();
		Stats.Count = NumInstances;
		if (NumInstances <= 0 || NumComponents <= 0)
		{
			return Stats;
		}
		TArray<double> Min, Max, Sum, Values;
		Min.Init(TNumericLimits<double>::Max(), NumComponents);
		Max.Init(TNumericLimits<double>::Lowest(), NumComponents);
		Sum.Init(0.0, NumComponents);
		Values.SetNumZeroed(NumComponents);
		for (int32 Instance = 0; Instance < NumInstances; ++Instance)
		{
			for (int32 C = 0; C < NumComponents; ++C)
			{
				Values[C] = Value(Instance, C);
				Min[C] = FMath::Min(Min[C], Values[C]);
				Max[C] = FMath::Max(Max[C], Values[C]);
				Sum[C] += Values[C];
			}
			if (Instance < MaxSamples)
			{
				Stats.Samples.Add(JoinNumbers(Values));
			}
		}
		for (double& V : Sum)
		{
			V /= NumInstances;
		}
		Stats.Min = JoinNumbers(Min);
		Stats.Max = JoinNumbers(Max);
		Stats.Mean = JoinNumbers(Sum);
		return Stats;
	}

	FNiagaraMcpAttributeStats ReadAttribute(const FNiagaraDataBuffer& Buffer, const FNiagaraVariableBase& Variable, const FNiagaraVariableLayoutInfo& Layout, int32 MaxSamples)
	{
		const int32 NumInstances = static_cast<int32>(Buffer.GetNumInstances());
		const int32 NumFloat = static_cast<int32>(Layout.GetNumFloatComponents());
		const int32 NumInt = static_cast<int32>(Layout.GetNumInt32Components());
		const int32 NumHalf = static_cast<int32>(Layout.GetNumHalfComponents());
		const bool bBool = Variable.GetType() == FNiagaraTypeDefinition::GetBoolDef();
		return BuildAttributeStats(Variable, NumInstances, NumFloat + NumInt + NumHalf, MaxSamples, [&](int32 Instance, int32 C) -> double
		{
			if (C < NumFloat)
			{
				return *Buffer.GetInstancePtrFloat(Layout.GetFloatComponentStart() + C, Instance);
			}
			C -= NumFloat;
			if (C < NumInt)
			{
				const int32 Raw = *Buffer.GetInstancePtrInt32(Layout.GetInt32ComponentStart() + C, Instance);
				return bBool ? (Raw != 0 ? 1.0 : 0.0) : static_cast<double>(Raw);
			}
			C -= NumInt;
			return Buffer.GetInstancePtrHalf(Layout.GetHalfComponentStart() + C, Instance)->GetFloat();
		});
	}

	/** True when attributes must be read through a sim cache (GPU emitters, lightweight / stateless emitters, no CPU data). */
	bool NeedsSimCacheReadback(FNiagaraEmitterInstance& Emitter)
	{
		if (Emitter.GetSimTarget() == ENiagaraSimTarget::GPUComputeSim || Emitter.AsStateless() != nullptr)
		{
			return true;
		}
		const FNiagaraDataSet& DataSet = Emitter.GetParticleData();
		return !DataSet.IsInitialized() || DataSet.GetCurrentData() == nullptr;
	}

	FString MissingAttributesNote(const TArray<FString>& Missing, TArray<FString> Available)
	{
		Available.Sort();
		return FString::Printf(TEXT("Attributes not in this emitter: %s. Available: %s"), *FString::Join(Missing, TEXT(", ")), *FString::Join(Available, TEXT(", ")));
	}

	/**
	 * Reads attributes of one emitter from a captured sim cache frame. For GPU emitters UNiagaraSimCache::WriteFrame flushes
	 * the pending GPU ticks and reads the particle buffers back; lightweight emitters are captured with CaptureForDebugging.
	 */
	void ReadEmitterFromCache(const UNiagaraSimCache& Cache, int32 FrameIndex, const TArray<FString>& Wanted, int32 MaxSamples, const FString& Issues, FNiagaraMcpEmitterAttributes& Out)
	{
		const int32 EmitterIndex = Cache.GetEmitterIndex(Out.Emitter);
		if (EmitterIndex == INDEX_NONE || FrameIndex < 0 || FrameIndex >= Cache.GetNumFrames())
		{
			Out.Note = TEXT("GPU / lightweight emitter: the readback capture has no frame for this time (the system was not running yet or had completed).") + Issues;
			return;
		}
		Out.Particles = Cache.GetEmitterNumInstances(EmitterIndex, FrameIndex);
		TArray<FNiagaraSimCacheVariable> Variables;
		Cache.ForEachEmitterAttribute(EmitterIndex, [&Variables](const FNiagaraSimCacheVariable& Variable) { Variables.Add(Variable); return true; });
		TArray<FString> Missing;
		for (const FString& WantedName : Wanted)
		{
			const FNiagaraSimCacheVariable* Found = Variables.FindByPredicate([&WantedName](const FNiagaraSimCacheVariable& V) { return ShortAttributeName(V.Variable.GetName()).Equals(WantedName, ESearchCase::IgnoreCase); });
			if (Found == nullptr)
			{
				Missing.Add(WantedName);
				continue;
			}
			TArray<float> Floats;
			TArray<FFloat16> Halfs;
			TArray<int32> Ints;
			Cache.ReadAttribute(Floats, Halfs, Ints, Found->Variable.GetName(), Out.Emitter, FrameIndex);
			const int32 NumFloat = Found->FloatCount;
			const int32 NumHalf = Found->HalfCount;
			const int32 NumInt = Found->Int32Count;
			const int32 NumComponents = NumFloat + NumHalf + NumInt;
			const int32 N = NumComponents > 0 ? (Floats.Num() + Halfs.Num() + Ints.Num()) / NumComponents : 0;
			const bool bBool = Found->Variable.GetType() == FNiagaraTypeDefinition::GetBoolDef();
			// Struct of arrays: component C of instance I is at [C * N + I].
			Out.Attributes.Add(BuildAttributeStats(Found->Variable, N, NumComponents, MaxSamples, [&](int32 Instance, int32 C) -> double
			{
				if (C < NumFloat)
				{
					return Floats[C * N + Instance];
				}
				C -= NumFloat;
				if (C < NumInt)
				{
					const int32 Raw = Ints[C * N + Instance];
					return bBool ? (Raw != 0 ? 1.0 : 0.0) : static_cast<double>(Raw);
				}
				C -= NumInt;
				return Halfs[C * N + Instance].GetFloat();
			}));
		}
		if (Missing.Num() > 0)
		{
			TArray<FString> Available;
			for (const FNiagaraSimCacheVariable& Variable : Variables)
			{
				Available.Add(ShortAttributeName(Variable.Variable.GetName()));
			}
			Out.Note = TEXT("Read back from the GPU through a transient sim cache. ") + MissingAttributesNote(Missing, Available);
		}
		else
		{
			Out.Note = TEXT("Read back from the GPU through a transient sim cache.");
		}
	}

	void ReadEmitter(FNiagaraEmitterInstance& Emitter, const TArray<FString>& Wanted, int32 MaxSamples, FNiagaraMcpEmitterAttributes& Out)
	{
		const FNiagaraDataSet& DataSet = Emitter.GetParticleData();
		const FNiagaraDataBuffer* Buffer = DataSet.IsInitialized() ? DataSet.GetCurrentData() : nullptr;
		if (Buffer == nullptr)
		{
			Out.Note = TEXT("The emitter has no particle data (not simulated yet).");
			return;
		}
		const TArray<FNiagaraVariableBase>& Variables = DataSet.GetVariables();
		const TArray<FNiagaraVariableLayoutInfo>& Layouts = DataSet.GetVariableLayouts();
		TArray<FString> Missing;
		for (const FString& WantedName : Wanted)
		{
			int32 Found = INDEX_NONE;
			for (int32 Index = 0; Index < Variables.Num(); ++Index)
			{
				if (ShortAttributeName(Variables[Index].GetName()).Equals(WantedName, ESearchCase::IgnoreCase))
				{
					Found = Index;
					break;
				}
			}
			if (Found == INDEX_NONE || !Layouts.IsValidIndex(Found))
			{
				Missing.Add(WantedName);
				continue;
			}
			Out.Attributes.Add(ReadAttribute(*Buffer, Variables[Found], Layouts[Found], MaxSamples));
		}
		if (Missing.Num() > 0)
		{
			TArray<FString> Available;
			for (const FNiagaraVariableBase& Variable : Variables)
			{
				Available.Add(ShortAttributeName(Variable.GetName()));
			}
			Out.Note = MissingAttributesNote(Missing, Available);
		}
	}
}

// ------------------------------------------------------------------------------------------------ Function specifiers

FNiagaraMcpFunctionSpecifierInfo UNiagaraMcpEditorTools::SetFunctionSpecifier(UNiagaraScript* Script, const FString& NodeId, FName Specifier, FName Value)
{
	if (Script == nullptr)
	{
		ReportError(TEXT("Script is null or could not be loaded. Pass an asset path such as /Game/FX/NM_Example.NM_Example."));
		return {};
	}
	if (!EnsureNoOpenEditor(Script->GetOutermostObject()))
	{
		return {};
	}
	UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
	UNiagaraGraph* Graph = Source ? Source->NodeGraph.Get() : nullptr;
	if (Graph == nullptr)
	{
		ReportError(FString::Printf(TEXT("Script '%s' has no editable graph."), *Script->GetPathName()));
		return {};
	}
	FGuid Guid;
	const bool bIsGuid = FGuid::Parse(NodeId, Guid);
	UEdGraphNode* Node = nullptr;
	for (UEdGraphNode* Candidate : Graph->Nodes)
	{
		if (Candidate && ((bIsGuid && Candidate->NodeGuid == Guid) || Candidate->GetName() == NodeId))
		{
			Node = Candidate;
			break;
		}
	}
	if (Node == nullptr)
	{
		ReportError(FString::Printf(TEXT("Node '%s' not found. Use GetScriptGraph for node ids."), *NodeId));
		return {};
	}
	UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Node);
	const bool bDataInterfaceCall = Call && Call->FunctionScript == nullptr && Call->Signature.IsValid() && Call->Signature.Inputs.Num() > 0 && Call->Signature.Inputs[0].GetType().IsDataInterface();
	if (!bDataInterfaceCall)
	{
		ReportError(FString::Printf(TEXT("Node '%s' (%s) is not a data interface function call."), *NodeId, *Node->GetClass()->GetName()));
		return {};
	}
	if (Specifier.IsNone() || Value.IsNone())
	{
		ReportError(TEXT("Specifier and Value are required, e.g. Specifier Attribute, Value Age."));
		return {};
	}
	TArray<FString> ValidKeys;
	for (const TPair<FName, FName>& Pair : Call->Signature.FunctionSpecifiers)
	{
		ValidKeys.AddUnique(Pair.Key.ToString());
	}
	for (const TPair<FName, FName>& Pair : Call->FunctionSpecifiers)
	{
		ValidKeys.AddUnique(Pair.Key.ToString());
	}
	const FString* MatchedKey = ValidKeys.FindByPredicate([&Specifier](const FString& Key) { return Key.Equals(Specifier.ToString(), ESearchCase::IgnoreCase); });
	if (MatchedKey == nullptr)
	{
		ReportError(ValidKeys.Num() == 0
			? FString::Printf(TEXT("Function '%s' has no function specifiers."), *Call->Signature.Name.ToString())
			: NiagaraMcpExt::UnknownNameMessage(TEXT("function specifier"), Specifier.ToString(), ValidKeys));
		return {};
	}

	{
		FScopedTransaction Transaction(LOCTEXT("SetFunctionSpecifier", "Niagara MCP: Set Function Specifier"));
		Graph->Modify();
		Call->Modify();
		Call->FunctionSpecifiers.FindOrAdd(FName(**MatchedKey)) = Value;
		(Call->*NiagaraMcpExtB::FExtNiagaraNodeAccess::ReallocatePinsFn())(true);
		Call->MarkNodeRequiresSynchronization(TEXT("NiagaraMcp SetFunctionSpecifier"), true);
	}
	Graph->NotifyGraphChanged();
	return NiagaraMcpExtB::DescribeSpecifiers(*Call);
}

// ------------------------------------------------------------------------------------------------ Dynamic inputs

TArray<FNiagaraMcpDynamicInputInfo> UNiagaraMcpEditorTools::FindDynamicInputs(const FString& Filter, const FString& OutputType, int32 MaxResults)
{
	TArray<FNiagaraMcpDynamicInputInfo> Results;
	FNiagaraTypeDefinition WantedType;
	const bool bTypeFilter = !NiagaraMcpExt::IsUnsetArg(OutputType);
	if (bTypeFilter && !ResolveNiagaraType(OutputType, WantedType))
	{
		return Results;
	}
	const TArray<FString> Words = NiagaraMcpExt::SplitFilter(Filter);
	const int32 Limit = MaxResults > 0 ? MaxResults : 40;

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Assets;
	AssetRegistry.GetAssetsByClass(UNiagaraScript::StaticClass()->GetClassPathName(), Assets);

	struct FCandidate
	{
		FAssetData Asset;
		FString Description;
		FString Category;
		bool bLibrary = false;
	};
	TArray<FCandidate> Candidates;
	for (const FAssetData& Asset : Assets)
	{
		FString UsageTag;
		if (!Asset.GetTagValue(TEXT("Usage"), UsageTag) || !UsageTag.EndsWith(TEXT("DynamicInput")))
		{
			continue;
		}
		FCandidate Candidate;
		Candidate.Asset = Asset;
		Asset.GetTagValue(TEXT("Description"), Candidate.Description);
		Asset.GetTagValue(TEXT("Category"), Candidate.Category);
		FString Keywords;
		Asset.GetTagValue(TEXT("Keywords"), Keywords);
		FString Visibility;
		if (Asset.GetTagValue(TEXT("LibraryVisibility"), Visibility))
		{
			Candidate.bLibrary = Visibility.Contains(TEXT("Library"));
		}
		if (!NiagaraMcpExt::MatchesWords(Asset.AssetName.ToString() + TEXT(" ") + Candidate.Description + TEXT(" ") + Candidate.Category + TEXT(" ") + Keywords + TEXT(" ") + Asset.PackagePath.ToString(), Words))
		{
			continue;
		}
		Candidates.Add(MoveTemp(Candidate));
	}
	Candidates.Sort([](const FCandidate& A, const FCandidate& B)
	{
		if (A.bLibrary != B.bLibrary)
		{
			return A.bLibrary;
		}
		return A.Asset.AssetName.LexicalLess(B.Asset.AssetName);
	});

	auto TypesMatch = [&WantedType](const FNiagaraTypeDefinition& Type)
	{
		if (Type == WantedType)
		{
			return true;
		}
		const bool bWantVector = WantedType == FNiagaraTypeDefinition::GetVec3Def() || WantedType == FNiagaraTypeDefinition::GetPositionDef();
		const bool bIsVector = Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef();
		return bWantVector && bIsVector;
	};

	for (const FCandidate& Candidate : Candidates)
	{
		if (Results.Num() >= Limit)
		{
			break;
		}
		UNiagaraScript* Script = Cast<UNiagaraScript>(Candidate.Asset.GetAsset());
		if (Script == nullptr)
		{
			continue;
		}
		const FNiagaraTypeDefinition Type = NiagaraMcpExtB::DynamicInputOutputType(*Script);
		if (bTypeFilter && (!Type.IsValid() || !TypesMatch(Type)))
		{
			continue;
		}
		FNiagaraMcpDynamicInputInfo& Info = Results.AddDefaulted_GetRef();
		Info.Path = Candidate.Asset.GetObjectPathString();
		Info.Name = Candidate.Asset.AssetName.ToString();
		Info.Description = Candidate.Description;
		Info.Category = Candidate.Category;
		Info.OutputType = Type.IsValid() ? Type.GetName() : FString();
		Info.bLibraryExposed = Candidate.bLibrary;
	}
	return Results;
}

// ------------------------------------------------------------------------------------------------ Specs and diffs

FNiagaraMcpSystemSpec UNiagaraMcpEditorTools::ExportSystemSpec(UNiagaraSystem* System)
{
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	FNiagaraMcpSystemSpec Spec;
	Spec.SystemPath = System->GetPathName();
	const TSharedRef<FJsonObject> Root = NiagaraMcpExtB::BuildSpec(*System);
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Spec.Spec);
	FJsonSerializer::Serialize(Root, Writer);
	return Spec;
}

FNiagaraMcpSpecDiff UNiagaraMcpEditorTools::DiffSystems(UNiagaraSystem* SystemA, UNiagaraSystem* SystemB)
{
	if (SystemA == nullptr || SystemB == nullptr)
	{
		ReportError(TEXT("SystemA and SystemB are required (asset paths such as /Game/FX/NS_A.NS_A)."));
		return {};
	}
	FNiagaraMcpSpecDiff Diff;
	Diff.SystemA = SystemA->GetPathName();
	Diff.SystemB = SystemB->GetPathName();

	TMap<FString, FString> FlatA;
	TMap<FString, FString> FlatB;
	NiagaraMcpExtB::Flatten(NiagaraMcpExtB::BuildSpec(*SystemA), FString(), FlatA);
	NiagaraMcpExtB::Flatten(NiagaraMcpExtB::BuildSpec(*SystemB), FString(), FlatB);
	FlatA.Remove(TEXT("system.path"));
	FlatB.Remove(TEXT("system.path"));

	TArray<FString> Paths;
	FlatA.GetKeys(Paths);
	for (const TPair<FString, FString>& Pair : FlatB)
	{
		Paths.AddUnique(Pair.Key);
	}
	Paths.Sort();
	static const FString Missing(TEXT("<missing>"));
	for (const FString& Path : Paths)
	{
		const FString* A = FlatA.Find(Path);
		const FString* B = FlatB.Find(Path);
		if (A && B && *A == *B)
		{
			continue;
		}
		FNiagaraMcpSpecDifference& Difference = Diff.Differences.AddDefaulted_GetRef();
		Difference.Path = Path;
		Difference.A = A ? *A : Missing;
		Difference.B = B ? *B : Missing;
	}
	Diff.bIdentical = Diff.Differences.Num() == 0;
	return Diff;
}

// ------------------------------------------------------------------------------------------------ Particle attributes

TArray<FNiagaraMcpAttributeSample> UNiagaraMcpEditorTools::GetParticleAttributes(UNiagaraSystem* System, const TArray<float>& InTimes, const TArray<FString>& InAttributes, FName EmitterName, int32 MaxSamples)
{
	TArray<FNiagaraMcpAttributeSample> Samples;
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return Samples;
	}
	TArray<float> Times = InTimes;
	if (Times.Num() == 0)
	{
		Times = { 0.25f, 0.5f, 1.0f, 2.0f };
	}
	if (Times.Num() > 32)
	{
		ReportError(TEXT("At most 32 times per call."));
		return Samples;
	}
	// None / "System" = every emitter, as everywhere else.
	const bool bAllEmitters = Stack::IsSystemName(EmitterName);
	if (!bAllEmitters && Stack::FindEmitter(*System, EmitterName) == nullptr)
	{
		return Samples;
	}
	TArray<FString> Wanted;
	for (const FString& Attribute : InAttributes)
	{
		FString Name = Attribute.TrimStartAndEnd();
		Name.RemoveFromStart(TEXT("Particles."), ESearchCase::IgnoreCase);
		if (!Name.IsEmpty())
		{
			Wanted.AddUnique(Name);
		}
	}
	if (Wanted.Num() == 0)
	{
		Wanted = { TEXT("Position"), TEXT("Velocity"), TEXT("Color"), TEXT("SpriteSize"), TEXT("Age"), TEXT("NormalizedAge") };
	}
	const int32 SampleCount = FMath::Clamp(MaxSamples, 0, 64);

	// Same as GetParticleCounts / CapturePreview: catch up on uncompiled edits first (no-op when in sync).
	System->RequestCompile(false);
	{
		FString WaitError;
		if (!NiagaraMcp::Preview::WaitForSystemCompile(*System, NiagaraMcp::Preview::DefaultMaxWaitSeconds, WaitError))
		{
			ReportError(WaitError);
			return Samples;
		}
	}
	if (!System->IsValid())
	{
		ReportError(FString::Printf(TEXT("'%s' is not valid (compile errors?). Run CompileSystem for details."), *System->GetPathName()));
		return Samples;
	}
	// GPU emitters only simulate once their compute shaders exist.
	if (System->HasAnyGPUEmitters())
	{
		FString WaitError;
		if (!NiagaraMcp::Preview::WaitForShaderJobs(NiagaraMcp::Preview::DefaultMaxWaitSeconds, WaitError))
		{
			ReportError(WaitError);
			return Samples;
		}
	}

	// Same off-screen setup as GetParticleCounts (NiagaraMcp::Preview::SimulateParticleCounts).
	FPreviewScene Scene(NiagaraMcpExtB::PreviewValues());
	TStrongObjectPtr<UNiagaraComponent> Component(NewObject<UNiagaraComponent>(GetTransientPackage()));
	Component->SetAutoActivate(false);
	Component->SetAsset(System);
	Component->SetForceSolo(true);
	Scene.AddComponent(Component.Get(), FTransform::Identity);

	const UEnum* StateEnum = StaticEnum<ENiagaraExecutionState>();
	// GPU and lightweight (stateless) emitters keep their particles on the GPU: capture a transient sim cache during the
	// same continuous run (one frame per sample time) and read the attributes from it afterwards.
	TStrongObjectPtr<UNiagaraSimCache> Cache;
	bool bCacheWriting = false;
	TArray<int32> CacheFrames;
	CacheFrames.Init(INDEX_NONE, Times.Num());
	TArray<TPair<int32, int32>> CacheReads; // (sample index, emitter entry index)
	FNiagaraSimCacheCreateParameters CacheParameters;
	FNiagaraSimCacheFeedbackContext CacheFeedback(false); // collect issues instead of logging them
	CacheParameters.AttributeCaptureMode = ENiagaraSimCacheAttributeCaptureMode::All;
	CacheParameters.bAllowRebasing = false;
	CacheParameters.bAllowDataInterfaceCaching = false;
	CacheParameters.bAllowInterpolation = false;
	// The cache only records lightweight (stateless) emitters in its debug mode.
	CacheParameters.bIncludeDebugData = System->GetEmitterHandles().ContainsByPredicate([](const FNiagaraEmitterHandle& Handle) { return Handle.GetEmitterMode() == ENiagaraEmitterMode::Stateless; });

	// One continuous run through the times (reset once, advance by the differences), samples in the caller's order.
	Samples.AddDefaulted(Times.Num());
	NiagaraMcp::Preview::SimulateInOrder(*Component, Times, 1.0f / 60.0f, [&](int32 SampleIndex, float Seconds)
	{
		FNiagaraMcpAttributeSample& Sample = Samples[SampleIndex];
		Sample.Time = Seconds;
		FNiagaraSystemInstanceControllerPtr Controller = Component->GetSystemInstanceController();
		FNiagaraSystemInstance* Instance = Controller.IsValid() ? Controller->GetSystemInstance_Unsafe() : nullptr;
		if (Instance == nullptr)
		{
			return;
		}
		bool bNeedsCache = false;
		for (const FNiagaraEmitterInstanceRef& Emitter : Instance->GetEmitters())
		{
			const FName Name = Emitter->GetEmitterHandle().GetName();
			if (!bAllEmitters && !Name.ToString().Equals(EmitterName.ToString(), ESearchCase::IgnoreCase))
			{
				continue;
			}
			FNiagaraMcpEmitterAttributes& Info = Sample.Emitters.AddDefaulted_GetRef();
			Info.Emitter = Name;
			Info.SimTarget = StaticEnum<ENiagaraSimTarget>()->GetNameStringByValue(static_cast<int64>(Emitter->GetSimTarget()));
			Info.Particles = Emitter->GetNumParticles();
			Info.State = StateEnum ? StateEnum->GetNameStringByValue(static_cast<int64>(Emitter->GetExecutionState())) : FString();
			if (NiagaraMcpExtB::NeedsSimCacheReadback(Emitter.Get()))
			{
				bNeedsCache = true;
				CacheReads.Emplace(SampleIndex, Sample.Emitters.Num() - 1);
			}
			else
			{
				NiagaraMcpExtB::ReadEmitter(Emitter.Get(), Wanted, SampleCount, Info);
			}
		}
		if (!bNeedsCache)
		{
			return;
		}
		if (!Cache.IsValid())
		{
			Cache.Reset(NewObject<UNiagaraSimCache>(GetTransientPackage(), NAME_None, RF_Transient));
			bCacheWriting = Cache->BeginWrite(CacheParameters, Component.Get(), CacheFeedback);
		}
		if (bCacheWriting)
		{
			// WriteFrame skips a frame when the system was not ticked since the last one (same time twice): reuse it.
			const int32 FramesBefore = Cache->GetNumFrames();
			if (Cache->WriteFrame(Component.Get(), CacheFeedback) && Cache->GetNumFrames() > FramesBefore)
			{
				CacheFrames[SampleIndex] = Cache->GetNumFrames() - 1;
			}
			else if (FramesBefore > 0)
			{
				CacheFrames[SampleIndex] = FramesBefore - 1;
			}
		}
	});
	if (Cache.IsValid())
	{
		if (bCacheWriting)
		{
			Cache->EndWrite();
		}
		const FString CacheIssues = CacheFeedback.Errors.Num() > 0 ? TEXT(" Capture: ") + FString::Join(CacheFeedback.Errors, TEXT(" | ")) : FString();
		for (const TPair<int32, int32>& Read : CacheReads)
		{
			FNiagaraMcpEmitterAttributes& Info = Samples[Read.Key].Emitters[Read.Value];
			if (!bCacheWriting)
			{
				Info.Note = TEXT("GPU / lightweight emitter: the readback capture could not start (no valid simulation); only the particle count is reported.") + CacheIssues;
				continue;
			}
			NiagaraMcpExtB::ReadEmitterFromCache(*Cache, CacheFrames[Read.Key], Wanted, SampleCount, CacheIssues, Info);
		}
		// Transient: release it now instead of waiting for the next garbage collection.
		Cache->MarkAsGarbage();
		Cache.Reset();
	}
	Component->DeactivateImmediate();
	return Samples;
}

#undef LOCTEXT_NAMESPACE
