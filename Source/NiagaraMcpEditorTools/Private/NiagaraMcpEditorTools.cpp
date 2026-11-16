#include "NiagaraMcpEditorTools.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraMcpEditorToolsInternal.h"
#include "NiagaraMcpPreview.h"
#include "NiagaraMcpStackUtils.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraShared.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "ScopedTransaction.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(NiagaraMcpEditorTools)

#define LOCTEXT_NAMESPACE "NiagaraMcpEditorTools"

using namespace NiagaraMcp;

namespace NiagaraMcpTools
{
	static const TCHAR* DefaultTemplateEmitter = TEXT("/Niagara/DefaultAssets/Templates/Emitters/Minimal.Minimal");

	bool CheckSystemArg(const UNiagaraSystem* System)
	{
		if (System == nullptr)
		{
			ReportError(TEXT("System is null or could not be loaded. Pass an asset path such as /Game/FX/NS_Example.NS_Example."));
			return false;
		}
		return true;
	}

	FNiagaraMcpRendererInfo DescribeRenderer(UNiagaraRendererProperties* Renderer, int32 Index)
	{
		FNiagaraMcpRendererInfo Info;
		Info.Index = Index;
		if (Renderer)
		{
			Info.Class = Renderer->GetClass()->GetName();
			Info.bEnabled = Renderer->GetIsEnabled();
			Info.Material = GetPropertyText(Renderer->GetClass(), Renderer, TEXT("Material"));
		}
		return Info;
	}

	FNiagaraMcpEmitterInfo DescribeEmitter(const FNiagaraEmitterHandle& Handle)
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
				Info.Renderers.Add(DescribeRenderer(Renderers[Index], Index));
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

	FString ParameterValueToString(const FNiagaraParameterStore& Store, const FNiagaraVariable& Variable)
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
		return Data ? ExportStructValue(Type.GetScriptStruct(), Data, Type.GetEnum()) : FString();
	}

	FNiagaraMcpSystemSummary DescribeSystem(UNiagaraSystem& System)
	{
		FNiagaraMcpSystemSummary Summary;
		Summary.SystemPath = System.GetPathName();
		for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
		{
			Summary.Emitters.Add(DescribeEmitter(Handle));
		}

		TArray<FNiagaraVariable> UserParameters;
		System.GetExposedParameters().GetUserParameters(UserParameters);
		for (const FNiagaraVariable& Parameter : UserParameters)
		{
			FNiagaraMcpParameterInfo& Info = Summary.UserParameters.AddDefaulted_GetRef();
			// GetUserParameters strips the namespace; report the full name ("User.Color"), the form links and the
			// user-parameter tools take, so a name can be copied into "@User.Color" as is.
			Info.Name = Parameter.GetName().ToString();
			if (!Info.Name.StartsWith(TEXT("User.")))
			{
				Info.Name = TEXT("User.") + Info.Name;
			}
			Info.Type = Parameter.GetType().GetName();
			Info.Value = ParameterValueToString(System.GetExposedParameters(), Parameter);
		}

		Summary.bHasOutstandingCompile = System.HasOutstandingCompilationRequests();
		Summary.bOpenInEditor = IsAssetEditorOpen(&System);
		return Summary;
	}

	FNiagaraEmitterHandle* FindEmitterById(UNiagaraSystem& System, const FGuid& Id)
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

	UNiagaraRendererProperties* FindRenderer(FNiagaraEmitterHandle& Handle, int32 RendererIndex)
	{
		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		const TArray<UNiagaraRendererProperties*>* Renderers = EmitterData ? &EmitterData->GetRenderers() : nullptr;
		if (Renderers == nullptr || !Renderers->IsValidIndex(RendererIndex) || (*Renderers)[RendererIndex] == nullptr)
		{
			ReportError(FString::Printf(TEXT("Emitter '%s' has no renderer at index %d (it has %d)."), *Handle.GetName().ToString(), RendererIndex, Renderers ? Renderers->Num() : 0));
			return nullptr;
		}
		return (*Renderers)[RendererIndex];
	}

	UClass* ResolveRendererClass(const FString& InType)
	{
		const FString Type = InType.TrimStartAndEnd();
		UClass* Class = nullptr;
		if (Type.StartsWith(TEXT("/")))
		{
			Class = LoadObject<UClass>(nullptr, *Type);
		}
		else
		{
			const FString Candidates[] =
			{
				FString::Printf(TEXT("Niagara%sRendererProperties"), *Type),
				Type,
				FString::Printf(TEXT("%sRendererProperties"), *Type),
			};
			for (const FString& Candidate : Candidates)
			{
				Class = FindFirstObject<UClass>(*Candidate, EFindFirstObjectOptions::NativeFirst);
				if (Class)
				{
					break;
				}
			}
		}
		if (Class == nullptr || !Class->IsChildOf(UNiagaraRendererProperties::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
		{
			TArray<FString> Names;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->IsChildOf(UNiagaraRendererProperties::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
				{
					Names.Add(It->GetName());
				}
			}
			ReportError(FString::Printf(TEXT("Unknown renderer type '%s'. Renderer classes: %s"), *InType, *FString::Join(Names, TEXT(", "))));
			return nullptr;
		}
		return Class;
	}

	FString SeverityToString(FNiagaraCompileEventSeverity Severity)
	{
		return StaticEnum<FNiagaraCompileEventSeverity>()->GetNameStringByValue(static_cast<int64>(Severity));
	}

	void CollectCompileInfo(UNiagaraScript* Script, const FString& Owner, FNiagaraMcpCompileResult& Result, bool& bAnyError)
	{
		if (Script == nullptr)
		{
			return;
		}
		const FString ScriptLabel = FString::Printf(TEXT("%s/%s"), *Owner, *UsageToString(Script->GetUsage()));
		const ENiagaraScriptCompileStatus Status = Script->GetLastCompileStatus();
		Result.ScriptStatus.Add(FString::Printf(TEXT("%s: %s"), *ScriptLabel, *StaticEnum<ENiagaraScriptCompileStatus>()->GetNameStringByValue(static_cast<int64>(Status))));
		if (Status == ENiagaraScriptCompileStatus::NCS_Error)
		{
			bAnyError = true;
		}

		for (const FNiagaraCompileEvent& Event : Script->GetVMExecutableData().LastCompileEvents)
		{
			if (Event.Severity == FNiagaraCompileEventSeverity::Error)
			{
				bAnyError = true;
			}
			if (Event.Severity == FNiagaraCompileEventSeverity::Error || Event.Severity == FNiagaraCompileEventSeverity::Warning)
			{
				FNiagaraMcpCompileMessage& Message = Result.Messages.AddDefaulted_GetRef();
				Message.Script = ScriptLabel;
				Message.Severity = SeverityToString(Event.Severity);
				Message.Message = Event.Message;
			}
		}
	}
}

using namespace NiagaraMcpTools;

// ------------------------------------------------------------------------------------------------ Inspection

FNiagaraMcpSystemSummary UNiagaraMcpEditorTools::GetSystemSummary(UNiagaraSystem* System)
{
	if (!CheckSystemArg(System))
	{
		return {};
	}
	return DescribeSystem(*System);
}

FNiagaraMcpStackInfo UNiagaraMcpEditorTools::GetStack(UNiagaraSystem* System, FName EmitterName, bool bIncludeInputs, const FString& ModuleFilter)
{
	if (!CheckSystemArg(System))
	{
		return {};
	}
	TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(*System);
	UNiagaraStackViewModel* StackViewModel = ViewModel.IsValid() ? Stack::GetStackViewModel(*ViewModel, EmitterName) : nullptr;
	if (StackViewModel == nullptr)
	{
		return {};
	}
	FNiagaraMcpStackInfo Info = DescribeStack(*StackViewModel, Stack::IsSystemName(EmitterName) ? FString(TEXT("System")) : EmitterName.ToString(), bIncludeInputs);
	if (!IsUnset(ModuleFilter))
	{
		TArray<FString> Words;
		ModuleFilter.ToLower().ParseIntoArrayWS(Words);
		Info.Modules.RemoveAll([&Words](const FNiagaraMcpModuleInfo& Module)
		{
			const FString Haystack = (Module.DisplayName + TEXT(" ") + Module.FunctionName + TEXT(" ") + Module.ScriptPath).ToLower();
			return Words.ContainsByPredicate([&Haystack](const FString& Word) { return !Haystack.Contains(Word); });
		});
	}
	return Info;
}

TArray<FNiagaraMcpModuleScriptInfo> UNiagaraMcpEditorTools::FindModuleScripts(const FString& Filter, const FString& StackName, int32 MaxResults)
{
	TArray<FNiagaraMcpModuleScriptInfo> Results;

	ENiagaraScriptUsage StackUsage = ENiagaraScriptUsage::Module;
	const bool bStackFilter = !IsUnset(StackName);
	if (bStackFilter && !ParseUsage(StackName, StackUsage))
	{
		return Results;
	}

	TArray<FString> Words;
	if (!IsUnset(Filter))
	{
		Filter.ToLower().ParseIntoArrayWS(Words);
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Assets;
	AssetRegistry.GetAssetsByClass(UNiagaraScript::StaticClass()->GetClassPathName(), Assets);

	for (const FAssetData& Asset : Assets)
	{
		FString UsageTag;
		if (Asset.GetTagValue(TEXT("Usage"), UsageTag) && !UsageTag.EndsWith(TEXT("Module")))
		{
			continue;
		}

		FString Description;
		Asset.GetTagValue(TEXT("Description"), Description);
		FString Category;
		Asset.GetTagValue(TEXT("Category"), Category);

		const FString Haystack = (Asset.AssetName.ToString() + TEXT(" ") + Description + TEXT(" ") + Category + TEXT(" ") + Asset.PackagePath.ToString()).ToLower();
		bool bAllWords = true;
		for (const FString& Word : Words)
		{
			if (!Haystack.Contains(Word))
			{
				bAllWords = false;
				break;
			}
		}
		if (!bAllWords)
		{
			continue;
		}

		FNiagaraMcpModuleScriptInfo Info;
		Info.Path = Asset.GetObjectPathString();
		Info.Name = Asset.AssetName.ToString();
		Info.Description = Description;
		Info.Category = Category;

		FString BitmaskTag;
		if (Asset.GetTagValue(TEXT("ModuleUsageBitmask"), BitmaskTag) && BitmaskTag.IsNumeric())
		{
			const int32 Bitmask = FCString::Atoi(*BitmaskTag);
			if (bStackFilter && (Bitmask & (1 << static_cast<int32>(StackUsage))) == 0)
			{
				continue;
			}
			for (ENiagaraScriptUsage Usage : UNiagaraScript::GetSupportedUsageContextsForBitmask(Bitmask))
			{
				Info.AllowedStacks.Add(UsageToString(Usage));
			}
		}

		FString VisibilityTag;
		if (Asset.GetTagValue(TEXT("LibraryVisibility"), VisibilityTag))
		{
			Info.bLibraryExposed = VisibilityTag.Contains(TEXT("Library"));
		}

		Results.Add(MoveTemp(Info));
	}

	Results.Sort([](const FNiagaraMcpModuleScriptInfo& A, const FNiagaraMcpModuleScriptInfo& B)
	{
		if (A.bLibraryExposed != B.bLibraryExposed)
		{
			return A.bLibraryExposed;
		}
		return A.Name < B.Name;
	});
	if (MaxResults > 0 && Results.Num() > MaxResults)
	{
		Results.SetNum(MaxResults);
	}
	return Results;
}

FNiagaraMcpCompileResult UNiagaraMcpEditorTools::CompileSystem(UNiagaraSystem* System, bool bWaitForCompletion, bool bSave)
{
	FNiagaraMcpCompileResult Result;
	if (!CheckSystemArg(System))
	{
		return Result;
	}
	System->RequestCompile(false);
	if (bWaitForCompletion)
	{
		System->WaitForCompilationComplete(false, false);
	}
	Result = NiagaraMcpTools::ReadCompileResult(*System);
	if (bSave)
	{
		Result.bSaved = SaveAssetSilently(System);
	}
	return Result;
}

FNiagaraMcpCompileResult NiagaraMcpTools::ReadCompileResult(UNiagaraSystem& SystemRef)
{
	FNiagaraMcpCompileResult Result;
	UNiagaraSystem* System = &SystemRef;
	Result.SystemPath = System->GetPathName();
	bool bAnyError = false;
	CollectCompileInfo(System->GetSystemSpawnScript(), TEXT("System"), Result, bAnyError);
	CollectCompileInfo(System->GetSystemUpdateScript(), TEXT("System"), Result, bAnyError);
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		if (EmitterData == nullptr || !Handle.GetIsEnabled())
		{
			continue;
		}
		TArray<UNiagaraScript*> Scripts;
		EmitterData->GetScripts(Scripts, false);
		for (UNiagaraScript* Script : Scripts)
		{
			CollectCompileInfo(Script, Handle.GetName().ToString(), Result, bAnyError);
		}
	}

	Result.bStillCompiling = System->HasOutstandingCompilationRequests();
	Result.bSucceeded = !bAnyError && !Result.bStillCompiling;
	return Result;
}

FNiagaraMcpPreviewResult UNiagaraMcpEditorTools::CapturePreview(UNiagaraSystem* System, const TArray<float>& Times, int32 FrameSize, float CameraYaw, float CameraPitch, float CameraDistance, const FString& Background, const FString& CameraYaws, const FString& QualityLevel, const FString& ReferenceImage)
{
	if (!CheckSystemArg(System))
	{
		return {};
	}
	// Catch up on edits that were not compiled yet (e.g. after Undo); a no-op when the scripts are in sync.
	System->RequestCompile(false);
	Vision::FPreviewArgs Args;
	Args.Times = Times;
	Args.FrameSize = FrameSize;
	Args.Camera.Yaw = CameraYaw;
	Args.Camera.Pitch = CameraPitch;
	Args.Camera.Distance = CameraDistance;
	Args.Camera.Background = IsUnset(Background) ? FString(TEXT("Black")) : Background;
	Args.CameraYaws = CameraYaws;
	Args.QualityLevel = QualityLevel;
	Args.ReferenceImage = ReferenceImage;
	FNiagaraMcpPreviewResult Result;
	FString PreviewError;
	if (!Vision::CapturePreview(*System, Args, Result, PreviewError))
	{
		ReportError(PreviewError);
		return {};
	}
	return Result;
}

// ------------------------------------------------------------------------------------------------ Systems and emitters

FNiagaraMcpAssetResult UNiagaraMcpEditorTools::CreateSystem(const FString& AssetPath, bool bSave)
{
	FString AssetName;
	UPackage* Package = CreateNewAssetPackage(AssetPath, AssetName);
	if (Package == nullptr)
	{
		return {};
	}

	// No undo transaction: asset creation is not undoable in the editor (factories don't record it). Undoing a
	// recorded NewObject would leave an empty, broken asset in memory that crashes the next tool using it.
	UNiagaraSystem* System = NewObject<UNiagaraSystem>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	UNiagaraSystemFactoryNew::InitializeSystem(System, true);
	FAssetRegistryModule::AssetCreated(System);
	Package->MarkPackageDirty();

	FNiagaraMcpAssetResult Result;
	Result.Path = System->GetPathName();
	Result.Class = System->GetClass()->GetName();
	Result.bSaved = bSave && SaveAssetSilently(System);
	return Result;
}

FNiagaraMcpEmitterInfo UNiagaraMcpEditorTools::AddEmitter(UNiagaraSystem* System, FName EmitterName, const FString& TemplateEmitter, bool bSave)
{
	if (!CheckSystemArg(System))
	{
		return {};
	}
	const FString TemplatePath = IsUnset(TemplateEmitter) || TemplateEmitter.TrimStartAndEnd().Equals(TEXT("Minimal"), ESearchCase::IgnoreCase) ? FString(DefaultTemplateEmitter) : TemplateEmitter;
	UNiagaraEmitter* Template = LoadAssetByPath<UNiagaraEmitter>(TemplatePath, TEXT("TemplateEmitter"));
	if (Template == nullptr)
	{
		return {};
	}
	const FGuid TemplateVersion = Template->GetExposedVersion().VersionGuid;

	FScopedTransaction Transaction(LOCTEXT("AddEmitter", "Niagara MCP: Add Emitter"));
	System->Modify();

	FGuid NewHandleId;
	TSharedPtr<FNiagaraSystemViewModel> EditorViewModel;
	const Stack::EEditorLookup Lookup = Stack::FindEditorViewModel(*System, EditorViewModel);
	if (Lookup == Stack::EEditorLookup::Unreachable)
	{
		ReportError(FString::Printf(TEXT("The Niagara editor for '%s' is open but its view model could not be found. Close and reopen that editor, then retry."), *System->GetPathName()));
		Transaction.Cancel();
		return {};
	}
	if (Lookup == Stack::EEditorLookup::Found)
	{
		TSharedPtr<FNiagaraEmitterHandleViewModel> HandleViewModel = EditorViewModel->AddEmitter(*Template, TemplateVersion);
		FNiagaraEmitterHandle* NewHandle = HandleViewModel.IsValid() ? HandleViewModel->GetEmitterHandle() : nullptr;
		NewHandleId = NewHandle ? NewHandle->GetId() : FGuid();
	}
	else
	{
		NewHandleId = FNiagaraEditorUtilities::AddEmitterToSystem(*System, *Template, TemplateVersion, true);
	}

	FNiagaraEmitterHandle* Handle = NewHandleId.IsValid() ? FindEmitterById(*System, NewHandleId) : nullptr;
	if (Handle == nullptr)
	{
		ReportError(FString::Printf(TEXT("Adding emitter '%s' from '%s' failed."), *EmitterName.ToString(), *TemplatePath));
		Transaction.Cancel();
		return {};
	}
	if (!EmitterName.IsNone())
	{
		Handle->SetName(EmitterName, *System);
	}

	Stack::FinishSystemEdit(*System, bSave);
	Handle = FindEmitterById(*System, NewHandleId);
	return Handle ? DescribeEmitter(*Handle) : FNiagaraMcpEmitterInfo();
}

FNiagaraMcpSystemSummary UNiagaraMcpEditorTools::RemoveEmitter(UNiagaraSystem* System, FName EmitterName, bool bSave)
{
	if (!CheckSystemArg(System))
	{
		return {};
	}
	FNiagaraEmitterHandle* Handle = Stack::FindEmitter(*System, EmitterName);
	if (Handle == nullptr)
	{
		return {};
	}
	const FGuid HandleId = Handle->GetId();

	// Always go through a system view model: DeleteEmitters kills running instances, rebuilds the system
	// scripts' emitter nodes and syncs the overview graph. The bare UNiagaraSystem::RemoveEmitterHandlesById
	// leaves stale emitter nodes and live instances pointing at the removed emitter data.
	TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(*System);
	if (!ViewModel.IsValid())
	{
		return {};
	}
	FScopedTransaction Transaction(LOCTEXT("RemoveEmitter", "Niagara MCP: Remove Emitter"));
	System->Modify();
	ViewModel->DeleteEmitters({ HandleId });

	Stack::FinishSystemEdit(*System, bSave);
	return DescribeSystem(*System);
}

FNiagaraMcpEmitterInfo UNiagaraMcpEditorTools::RenameEmitter(UNiagaraSystem* System, FName EmitterName, FName NewName, bool bSave)
{
	if (!CheckSystemArg(System))
	{
		return {};
	}
	FNiagaraEmitterHandle* Handle = Stack::FindEmitter(*System, EmitterName);
	if (Handle == nullptr)
	{
		return {};
	}
	if (NewName.IsNone())
	{
		ReportError(TEXT("NewName is required."));
		return {};
	}
	const FGuid HandleId = Handle->GetId();

	FScopedTransaction Transaction(LOCTEXT("RenameEmitter", "Niagara MCP: Rename Emitter"));
	System->Modify();
	Handle->SetName(NewName, *System);

	Stack::FinishSystemEdit(*System, bSave);
	Handle = FindEmitterById(*System, HandleId);
	return Handle ? DescribeEmitter(*Handle) : FNiagaraMcpEmitterInfo();
}

FNiagaraMcpPropertyResult UNiagaraMcpEditorTools::SetEmitterProperty(UNiagaraSystem* System, FName EmitterName, const FString& PropertyName, const FString& Value, bool bSave)
{
	if (!CheckSystemArg(System))
	{
		return {};
	}
	FNiagaraEmitterHandle* Handle = Stack::FindEmitter(*System, EmitterName);
	FVersionedNiagaraEmitterData* EmitterData = Handle ? Handle->GetEmitterData() : nullptr;
	if (EmitterData == nullptr)
	{
		return {};
	}
	const FVersionedNiagaraEmitter VersionedEmitter = Handle->GetInstance();

	FScopedTransaction Transaction(LOCTEXT("SetEmitterProperty", "Niagara MCP: Set Emitter Property"));
	VersionedEmitter.Emitter->Modify();

	FString PropertyError;
	FProperty* Changed = SetPropertyFromText(FVersionedNiagaraEmitterData::StaticStruct(), EmitterData, VersionedEmitter.Emitter, PropertyName, Value, PropertyError);
	if (Changed == nullptr)
	{
		ReportError(PropertyError);
		Transaction.Cancel();
		return {};
	}

	FPropertyChangedEvent ChangedEvent(Changed, EPropertyChangeType::ValueSet);
	VersionedEmitter.Emitter->PostEditChangeVersionedProperty(ChangedEvent, VersionedEmitter.Version);
	Stack::FinishSystemEdit(*System, false);

	FNiagaraMcpPropertyResult Result;
	Result.Property = PropertyName;
	Result.Value = GetPropertyText(FVersionedNiagaraEmitterData::StaticStruct(), Handle->GetEmitterData(), PropertyName);
	Result.bSaved = bSave && SaveAssetSilently(System);
	return Result;
}

FNiagaraMcpRendererInfo UNiagaraMcpEditorTools::AddRenderer(UNiagaraSystem* System, FName EmitterName, const FString& RendererType, const FString& Material, bool bSave)
{
	if (!CheckSystemArg(System))
	{
		return {};
	}
	FNiagaraEmitterHandle* Handle = Stack::FindEmitter(*System, EmitterName);
	if (Handle == nullptr || Handle->GetEmitterData() == nullptr)
	{
		return {};
	}
	UClass* RendererClass = ResolveRendererClass(RendererType);
	if (RendererClass == nullptr)
	{
		return {};
	}
	const FVersionedNiagaraEmitter VersionedEmitter = Handle->GetInstance();

	FScopedTransaction Transaction(LOCTEXT("AddRenderer", "Niagara MCP: Add Renderer"));
	VersionedEmitter.Emitter->Modify();
	UNiagaraRendererProperties* Renderer = NewObject<UNiagaraRendererProperties>(VersionedEmitter.Emitter, RendererClass, NAME_None, RF_Transactional);
	VersionedEmitter.Emitter->AddRenderer(Renderer, VersionedEmitter.Version);

	if (!IsUnset(Material))
	{
		FString PropertyError;
		FProperty* Changed = SetPropertyFromText(Renderer->GetClass(), Renderer, Renderer, TEXT("Material"), Material, PropertyError);
		if (Changed == nullptr)
		{
			ReportError(PropertyError);
			Transaction.Cancel();
			VersionedEmitter.Emitter->RemoveRenderer(Renderer, VersionedEmitter.Version);
			return {};
		}
		FPropertyChangedEvent ChangedEvent(Changed, EPropertyChangeType::ValueSet);
		Renderer->PostEditChangeProperty(ChangedEvent);
	}

	Stack::FinishSystemEdit(*System, bSave);
	const TArray<UNiagaraRendererProperties*>& Renderers = Handle->GetEmitterData()->GetRenderers();
	return DescribeRenderer(Renderer, Renderers.IndexOfByKey(Renderer));
}

FNiagaraMcpPropertyResult UNiagaraMcpEditorTools::SetRendererProperty(UNiagaraSystem* System, FName EmitterName, int32 RendererIndex, const FString& PropertyName, const FString& Value, bool bSave)
{
	if (!CheckSystemArg(System))
	{
		return {};
	}
	FNiagaraEmitterHandle* Handle = Stack::FindEmitter(*System, EmitterName);
	UNiagaraRendererProperties* Renderer = Handle ? FindRenderer(*Handle, RendererIndex) : nullptr;
	if (Renderer == nullptr)
	{
		return {};
	}

	FScopedTransaction Transaction(LOCTEXT("SetRendererProperty", "Niagara MCP: Set Renderer Property"));
	Renderer->Modify();
	FString PropertyError;
	FProperty* Changed = SetPropertyFromText(Renderer->GetClass(), Renderer, Renderer, PropertyName, Value, PropertyError);
	if (Changed == nullptr)
	{
		ReportError(PropertyError);
		Transaction.Cancel();
		return {};
	}
	FPropertyChangedEvent ChangedEvent(Changed, EPropertyChangeType::ValueSet);
	Renderer->PostEditChangeProperty(ChangedEvent);
	Stack::FinishSystemEdit(*System, false);

	FNiagaraMcpPropertyResult Result;
	Result.Property = PropertyName;
	Result.Value = GetPropertyText(Renderer->GetClass(), Renderer, PropertyName);
	Result.bSaved = bSave && SaveAssetSilently(System);
	return Result;
}

FNiagaraMcpEmitterInfo UNiagaraMcpEditorTools::RemoveRenderer(UNiagaraSystem* System, FName EmitterName, int32 RendererIndex, bool bSave)
{
	if (!CheckSystemArg(System))
	{
		return {};
	}
	FNiagaraEmitterHandle* Handle = Stack::FindEmitter(*System, EmitterName);
	UNiagaraRendererProperties* Renderer = Handle ? FindRenderer(*Handle, RendererIndex) : nullptr;
	if (Renderer == nullptr)
	{
		return {};
	}
	const FVersionedNiagaraEmitter VersionedEmitter = Handle->GetInstance();

	FScopedTransaction Transaction(LOCTEXT("RemoveRenderer", "Niagara MCP: Remove Renderer"));
	VersionedEmitter.Emitter->Modify();
	VersionedEmitter.Emitter->RemoveRenderer(Renderer, VersionedEmitter.Version);

	Stack::FinishSystemEdit(*System, bSave);
	return DescribeEmitter(*Handle);
}

FNiagaraMcpParameterInfo UNiagaraMcpEditorTools::SetUserParameter(UNiagaraSystem* System, FName ParameterName, const FString& Type, const FString& DefaultValue, bool bSave)
{
	if (!CheckSystemArg(System))
	{
		return {};
	}
	FNiagaraTypeDefinition TypeDef;
	if (!ResolveNiagaraType(Type, TypeDef))
	{
		return {};
	}
	FString Name = ParameterName.ToString();
	if (Name.IsEmpty() || ParameterName.IsNone())
	{
		ReportError(TEXT("ParameterName is required."));
		return {};
	}
	if (!Name.StartsWith(TEXT("User.")))
	{
		Name = TEXT("User.") + Name;
	}
	const FNiagaraVariable Variable(TypeDef, FName(*Name));

	// Parse first so a bad value changes nothing.
	TOptional<FStructOnScope> Value;
	if (!TypeDef.IsDataInterface() && !TypeDef.IsUObject() && !DefaultValue.TrimStartAndEnd().IsEmpty())
	{
		Value.Emplace(TypeDef.GetScriptStruct());
		FString ValueError;
		if (!ImportStructValue(TypeDef.GetScriptStruct(), Value->GetStructMemory(), DefaultValue, TypeDef.GetEnum(), ValueError))
		{
			ReportError(ValueError);
			return {};
		}
	}

	FScopedTransaction Transaction(LOCTEXT("SetUserParameter", "Niagara MCP: Set User Parameter"));
	System->Modify();
	FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
	Store.AddParameter(Variable, true, true);
	if (Value.IsSet())
	{
		Store.SetParameterData(Value->GetStructMemory(), Variable, true);
	}

	Stack::FinishSystemEdit(*System, bSave);

	FNiagaraMcpParameterInfo Info;
	Info.Name = Variable.GetName().ToString();
	Info.Type = TypeDef.GetName();
	Info.Value = ParameterValueToString(Store, Variable);
	return Info;
}

#undef LOCTEXT_NAMESPACE
