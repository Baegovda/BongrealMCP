#include "NiagaraMcpEditorTools.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Misc/ITransaction.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "Engine/World.h"
#include "NiagaraActor.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraLightRendererProperties.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraMcpEditorToolsInternal.h"
#include "NiagaraMcpLog.h"
#include "NiagaraMcpJournal.h"
#include "NiagaraMcpPreview.h"
#include "NiagaraMcpStackUtils.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSystem.h"
#include "ScopedTransaction.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

#define LOCTEXT_NAMESPACE "NiagaraMcpEditorTools"

using namespace NiagaraMcp;

namespace NiagaraMcpExtras
{
	FString SeverityName(EStackIssueSeverity Severity)
	{
		switch (Severity)
		{
		case EStackIssueSeverity::Error: return TEXT("Error");
		case EStackIssueSeverity::Warning: return TEXT("Warning");
		case EStackIssueSeverity::Info: return TEXT("Info");
		default: return TEXT("None");
		}
	}

	void CollectEntries(UNiagaraStackEntry* Entry, TArray<UNiagaraStackEntry*>& Out, int32 Depth)
	{
		if (Entry == nullptr || Depth > 12)
		{
			return;
		}
		Out.Add(Entry);
		TArray<UNiagaraStackEntry*> Children;
		Entry->GetUnfilteredChildren(Children);
		for (UNiagaraStackEntry* Child : Children)
		{
			CollectEntries(Child, Out, Depth + 1);
		}
	}

	/** Every stack issue of one stack (system or emitter), deduplicated by issue id. */
	void CollectStackIssues(UNiagaraStackViewModel& StackViewModel, FName EmitterName, TArray<FNiagaraMcpIssue>& Out, TSet<FString>& Seen)
	{
		UNiagaraStackEntry* Root = StackViewModel.GetRootEntry();
		if (Root == nullptr)
		{
			return;
		}
		Root->RefreshChildren();
		TArray<UNiagaraStackEntry*> Entries;
		CollectEntries(Root, Entries, 0);
		const FString Owner = Stack::IsSystemName(EmitterName) ? FString(TEXT("System")) : EmitterName.ToString();
		for (UNiagaraStackEntry* Entry : Entries)
		{
			for (const UNiagaraStackEntry::FStackIssue& StackIssue : Entry->GetIssues())
			{
				const FString Id = StackIssue.GetUniqueIdentifier();
				if (Seen.Contains(Owner + Id))
				{
					continue;
				}
				Seen.Add(Owner + Id);
				FNiagaraMcpIssue& Issue = Out.AddDefaulted_GetRef();
				Issue.Severity = SeverityName(StackIssue.GetSeverity());
				Issue.Source = FString::Printf(TEXT("Stack: %s / %s"), *Owner, *Entry->GetDisplayName().ToString());
				Issue.EmitterName = Stack::IsSystemName(EmitterName) ? NAME_None : EmitterName;
				Issue.IssueId = Id;
				const FString Short = StackIssue.GetShortDescription().ToString();
				const FString Long = StackIssue.GetLongDescription().ToString();
				Issue.Message = Long.IsEmpty() || Long == Short ? Short : Short + TEXT(": ") + Long;
				for (const UNiagaraStackEntry::FStackIssueFix& Fix : StackIssue.GetFixes())
				{
					Issue.Fixes.Add(Fix.GetDescription().ToString());
				}
			}
		}
	}

	void AddCheck(FNiagaraMcpValidationResult& Result, const TCHAR* Severity, FName Emitter, const FString& Message)
	{
		FNiagaraMcpIssue& Issue = Result.Issues.AddDefaulted_GetRef();
		Issue.Severity = Severity;
		Issue.Source = FString::Printf(TEXT("Check: %s"), Emitter.IsNone() ? TEXT("System") : *Emitter.ToString());
		Issue.EmitterName = Emitter;
		Issue.Message = Message;
	}

	/** Plugin checks for common authoring mistakes the stack does not flag. */
	void RunChecks(UNiagaraSystem& System, FNiagaraSystemViewModel& ViewModel, FNiagaraMcpValidationResult& Result)
	{
		if (System.GetEmitterHandles().Num() == 0)
		{
			AddCheck(Result, TEXT("Warning"), NAME_None, TEXT("The system has no emitters."));
		}
		for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
		{
			const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
			if (EmitterData == nullptr || !Handle.GetIsEnabled())
			{
				continue;
			}
			const FName Name = Handle.GetName();
			const bool bGpu = EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim;
			if (bGpu && EmitterData->CalculateBoundsMode == ENiagaraEmitterCalculateBoundMode::Dynamic)
			{
				AddCheck(Result, TEXT("Warning"), Name, TEXT("GPU emitter with dynamic bounds: GPU particles cannot report bounds, so the effect may be culled. Set CalculateBoundsMode to Fixed and FixedBounds."));
			}
			if (EmitterData->GetRenderers().Num() == 0)
			{
				AddCheck(Result, TEXT("Warning"), Name, TEXT("Emitter has no renderer, so nothing is drawn."));
			}
			for (int32 Index = 0; Index < EmitterData->GetRenderers().Num(); ++Index)
			{
				const UNiagaraRendererProperties* Renderer = EmitterData->GetRenderers()[Index];
				if (Renderer == nullptr || !Renderer->GetIsEnabled())
				{
					continue;
				}
				if (bGpu && Renderer->IsA<UNiagaraLightRendererProperties>())
				{
					AddCheck(Result, TEXT("Warning"), Name, FString::Printf(TEXT("Renderer %d: light renderers do not work on GPU emitters."), Index));
				}
				if (FindFProperty<FObjectPropertyBase>(Renderer->GetClass(), TEXT("Material")))
				{
					const FString Material = GetPropertyText(Renderer->GetClass(), Renderer, TEXT("Material"));
					if (Material.IsEmpty() || Material.Equals(TEXT("None"), ESearchCase::IgnoreCase))
					{
						AddCheck(Result, TEXT("Warning"), Name, FString::Printf(TEXT("Renderer %d (%s) has no material; the default material is used."), Index, *Renderer->GetClass()->GetName()));
					}
				}
			}

			// An emitter without any spawn module never produces particles.
			if (UNiagaraStackViewModel* StackViewModel = Stack::GetStackViewModel(ViewModel, Name))
			{
				TArray<UNiagaraStackModuleItem*> Items;
				Stack::CollectModuleItems(*StackViewModel, Items);
				const bool bHasSpawn = Items.ContainsByPredicate([](const UNiagaraStackModuleItem* Item)
				{
					const ENiagaraScriptUsage Usage = Stack::GetModuleUsage(*Item);
					return (Usage == ENiagaraScriptUsage::EmitterSpawnScript || Usage == ENiagaraScriptUsage::EmitterUpdateScript)
						&& Item->GetIsEnabled() && Item->GetModuleNode().GetFunctionName().Contains(TEXT("Spawn"));
				});
				if (!bHasSpawn)
				{
					AddCheck(Result, TEXT("Info"), Name, TEXT("No Spawn Rate / Spawn Burst / Spawn Per Unit module in the emitter stacks; the emitter may never spawn particles."));
				}
			}
		}
	}

	FNiagaraMcpValidationResult Validate(UNiagaraSystem& System, bool bCompile)
	{
		FNiagaraMcpValidationResult Result;
		Result.SystemPath = System.GetPathName();

		// bCompile=false only reads the current state: no compile request, no side effects.
		const FNiagaraMcpCompileResult Compile = bCompile ? UNiagaraMcpEditorTools::CompileSystem(&System, true, false) : NiagaraMcpTools::ReadCompileResult(System);
		for (const FNiagaraMcpCompileMessage& Message : Compile.Messages)
		{
			FNiagaraMcpIssue& Issue = Result.Issues.AddDefaulted_GetRef();
			Issue.Severity = Message.Severity;
			Issue.Source = FString::Printf(TEXT("Compile: %s"), *Message.Script);
			Issue.Message = Message.Message;
		}
		for (const FString& Status : Compile.ScriptStatus)
		{
			if (Status.EndsWith(TEXT("NCS_Error")) && !Result.Issues.ContainsByPredicate([](const FNiagaraMcpIssue& Issue) { return Issue.Severity == TEXT("Error"); }))
			{
				FNiagaraMcpIssue& Issue = Result.Issues.AddDefaulted_GetRef();
				Issue.Severity = TEXT("Error");
				Issue.Source = TEXT("Compile");
				Issue.Message = Status;
			}
		}

		TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(System);
		if (ViewModel.IsValid())
		{
			TSet<FString> Seen;
			if (UNiagaraStackViewModel* SystemStack = ViewModel->GetSystemStackViewModel())
			{
				CollectStackIssues(*SystemStack, NAME_None, Result.Issues, Seen);
			}
			for (const TSharedRef<FNiagaraEmitterHandleViewModel>& HandleViewModel : ViewModel->GetEmitterHandleViewModels())
			{
				if (UNiagaraStackViewModel* EmitterStack = HandleViewModel->GetEmitterStackViewModel())
				{
					CollectStackIssues(*EmitterStack, HandleViewModel->GetName(), Result.Issues, Seen);
				}
			}
			RunChecks(System, *ViewModel, Result);
		}

		for (const FNiagaraMcpIssue& Issue : Result.Issues)
		{
			Result.ErrorCount += Issue.Severity == TEXT("Error") ? 1 : 0;
			Result.WarningCount += Issue.Severity == TEXT("Warning") ? 1 : 0;
		}
		Result.bOk = Result.ErrorCount == 0;
		return Result;
	}
}

using namespace NiagaraMcpExtras;

FNiagaraMcpValidationResult UNiagaraMcpEditorTools::ValidateSystem(UNiagaraSystem* System, bool bCompile)
{
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	return Validate(*System, bCompile);
}

FNiagaraMcpValidationResult UNiagaraMcpEditorTools::ApplyStackIssueFix(UNiagaraSystem* System, FName EmitterName, const FString& IssueId, int32 FixIndex, bool bSave)
{
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	TSharedPtr<FNiagaraSystemViewModel> ViewModel = Stack::AcquireViewModel(*System);
	UNiagaraStackViewModel* StackViewModel = ViewModel.IsValid() ? Stack::GetStackViewModel(*ViewModel, EmitterName) : nullptr;
	UNiagaraStackEntry* Root = StackViewModel ? StackViewModel->GetRootEntry() : nullptr;
	if (Root == nullptr)
	{
		return {};
	}
	Root->RefreshChildren();
	TArray<UNiagaraStackEntry*> Entries;
	CollectEntries(Root, Entries, 0);

	for (UNiagaraStackEntry* Entry : Entries)
	{
		for (const UNiagaraStackEntry::FStackIssue& Issue : Entry->GetIssues())
		{
			if (Issue.GetUniqueIdentifier() != IssueId)
			{
				continue;
			}
			const TArray<UNiagaraStackEntry::FStackIssueFix>& Fixes = Issue.GetFixes();
			if (!Fixes.IsValidIndex(FixIndex) || !Fixes[FixIndex].GetFixDelegate().IsBound())
			{
				ReportError(FString::Printf(TEXT("Issue '%s' has no fix at index %d (it has %d)."), *IssueId, FixIndex, Fixes.Num()));
				return {};
			}
			const UNiagaraStackEntry::FStackIssueFixDelegate FixDelegate = Fixes[FixIndex].GetFixDelegate();
			{
				FScopedTransaction Transaction(LOCTEXT("ApplyStackIssueFix", "Niagara MCP: Apply Stack Issue Fix"));
				System->Modify();
				FixDelegate.ExecuteIfBound();
			}
			Stack::FinishSystemEdit(*System, bSave);
			return Validate(*System, true);
		}
	}
	ReportError(FString::Printf(TEXT("Issue '%s' not found on %s. Run ValidateSystem for current issue ids."), *IssueId, Stack::IsSystemName(EmitterName) ? TEXT("the system") : *EmitterName.ToString()));
	return {};
}

TArray<FNiagaraMcpParticleSample> UNiagaraMcpEditorTools::GetParticleCounts(UNiagaraSystem* System, const TArray<float>& Times, const FString& QualityLevel)
{
	TArray<FNiagaraMcpParticleSample> Samples;
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return Samples;
	}
	int32 Quality = INDEX_NONE;
	FString CountError;
	if (!Preview::ParseQualityLevel(QualityLevel, Quality, CountError))
	{
		ReportError(CountError);
		return Samples;
	}
	System->RequestCompile(false); // catch up on uncompiled edits (no-op when in sync)
	if (!Preview::SimulateParticleCounts(*System, Times, Samples, CountError, Quality))
	{
		ReportError(CountError);
	}
	return Samples;
}

FNiagaraMcpAssetPage UNiagaraMcpEditorTools::FindAssets(const FString& ClassName, const FString& NameFilter, const FString& Path, int32 MaxResults, int32 Offset)
{
	FNiagaraMcpAssetPage Page;
	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;
	FString Root = IsUnset(Path) ? FString() : Path.TrimStartAndEnd();
	Root.RemoveFromEnd(TEXT("/"));
	Filter.PackagePaths.Add(FName(Root.IsEmpty() ? TEXT("/Game") : *Root));

	const FString Class = IsUnset(ClassName) ? FString() : ClassName.TrimStartAndEnd();
	if (!Class.IsEmpty())
	{
		UClass* AssetClass = Class.StartsWith(TEXT("/")) ? LoadObject<UClass>(nullptr, *Class) : FindFirstObject<UClass>(*Class, EFindFirstObjectOptions::NativeFirst);
		if (AssetClass == nullptr)
		{
			ReportError(FString::Printf(TEXT("Unknown asset class '%s'. Examples: Texture2D, Material, MaterialInstanceConstant, NiagaraSystem, NiagaraScript, StaticMesh."), *ClassName));
			return Page;
		}
		Filter.ClassPaths.Add(AssetClass->GetClassPathName());
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	const FString Needle = IsUnset(NameFilter) ? FString() : NameFilter.TrimStartAndEnd();
	Assets.RemoveAll([&Needle](const FAssetData& Asset) { return !Needle.IsEmpty() && !Asset.AssetName.ToString().Contains(Needle); });
	Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.GetObjectPathString() < B.GetObjectPathString(); });

	Page.TotalMatched = Assets.Num();
	const int32 First = FMath::Max(0, Offset);
	const int32 Count = FMath::Clamp(MaxResults, 1, 500);
	for (int32 Index = First; Index < Assets.Num() && Index < First + Count; ++Index)
	{
		FNiagaraMcpAssetInfo& Info = Page.Assets.AddDefaulted_GetRef();
		Info.Path = Assets[Index].GetObjectPathString();
		Info.Class = Assets[Index].AssetClassPath.GetAssetName().ToString();
	}
	Page.NextOffset = First + Count < Assets.Num() ? First + Count : -1;
	return Page;
}

FNiagaraMcpActorInfo UNiagaraMcpEditorTools::PlaceInLevel(UNiagaraSystem* System, const FString& Location, const FString& Label)
{
	FNiagaraMcpActorInfo Info;
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return Info;
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (World == nullptr)
	{
		ReportError(TEXT("No editor world is open."));
		return Info;
	}
	FVector SpawnLocation = FVector::ZeroVector;
	FString LocationError;
	if (!IsUnset(Location) && !ImportStructValue(TBaseStructure<FVector>::Get(), &SpawnLocation, Location, nullptr, LocationError))
	{
		ReportError(FString::Printf(TEXT("Location: %s"), *LocationError));
		return Info;
	}

	FScopedTransaction Transaction(LOCTEXT("PlaceInLevel", "Niagara MCP: Place System In Level"));
	ANiagaraActor* Actor = World->SpawnActor<ANiagaraActor>(SpawnLocation, FRotator::ZeroRotator);
	if (Actor == nullptr)
	{
		ReportError(TEXT("Spawning the Niagara actor failed."));
		Transaction.Cancel();
		return Info;
	}
	Actor->GetNiagaraComponent()->SetAsset(System);
	Actor->SetActorLabel(IsUnset(Label) ? System->GetName() : Label);
	Actor->GetNiagaraComponent()->Activate(true);
	GEditor->SelectNone(false, true);
	GEditor->SelectActor(Actor, true, true);
	GEditor->MoveViewportCamerasToActor(*Actor, false);

	Info.ActorName = Actor->GetName();
	Info.ActorLabel = Actor->GetActorLabel();
	Info.Location = Actor->GetActorLocation().ToString();
	return Info;
}

namespace NiagaraMcpExtras
{
	/** Objects recorded by the transaction that the next Undo (bUndo) or Redo would apply. */
	void GetPendingTransactionObjects(bool bUndo, TArray<UObject*>& OutObjects)
	{
		UTransBuffer* Buffer = GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
		if (Buffer == nullptr)
		{
			return;
		}
		const int32 Index = Buffer->GetQueueLength() - Buffer->GetUndoCount() - (bUndo ? 1 : 0);
		if (const FTransaction* Transaction = Buffer->GetTransaction(Index))
		{
			Transaction->GetTransactionObjects(OutObjects);
		}
	}
}

void NiagaraMcpTools::RefreshSystemsAfterTransaction(const TArray<UObject*>& Objects)
{
	// With the Niagara editor closed no live view model handles PostUndo (headless ones only exist during a tool
	// call), so nothing would recompile the system after Undo/Redo and previews would show the old state.
	// Recompile every touched system here (with the editor open this is a harmless second request).
	TSet<UNiagaraSystem*> Systems;
	for (UObject* Object : Objects)
	{
		for (UObject* Outer = Object; Outer; Outer = Outer->GetOuter())
		{
			if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Outer))
			{
				Systems.Add(System);
				break;
			}
		}
	}
	for (UNiagaraSystem* System : Systems)
	{
		if (!IsValid(System))
		{
			continue;
		}
		// Same as FNiagaraSystemViewModel::PostUndo. No PostEditChange here: right after an undo the system's
		// runtime data can still reference undone emitters, and a full property-change refresh asserts on it.
		System->KillAllActiveCompilations();
		System->RequestCompile(false);
	}
}

void NiagaraMcpTools::SaveTouchedPackages(const TArray<UObject*>& Objects, TArray<FString>& OutSaved, TArray<FString>& OutUnsaved)
{
	// Undo/Redo only change the objects in memory: without this the files keep the undone state and the editor shows
	// the asset as modified (the next editor restart would bring the undone edit back).
	TSet<UPackage*> Packages;
	for (UObject* Object : Objects)
	{
		UPackage* Package = IsValid(Object) ? Object->GetOutermost() : nullptr;
		if (Package && Package != GetTransientPackage() && Package->IsDirty())
		{
			Packages.Add(Package);
		}
	}
	const bool bPlaying = GEditor && GEditor->PlayWorld;
	for (UPackage* Package : Packages)
	{
		const FString Name = Package->GetName();
		if (!FPackageName::IsValidLongPackageName(Name) || Name.StartsWith(TEXT("/Temp/")) || Name.StartsWith(TEXT("/Script/")))
		{
			continue;
		}
		// Levels are the user's to save; never-saved assets were created unsaved on purpose (bSave = false).
		UObject* Asset = Package->FindAssetInPackage();
		if (bPlaying || Package->ContainsMap() || Asset == nullptr || !FPackageName::DoesPackageExist(Name))
		{
			OutUnsaved.Add(Name);
			continue;
		}
		if (SaveAssetSilently(Asset))
		{
			OutSaved.Add(Name);
		}
		else
		{
			OutUnsaved.Add(Name);
		}
	}
	OutSaved.Sort();
	OutUnsaved.Sort();
}

FNiagaraMcpUndoResult UNiagaraMcpEditorTools::Undo(int32 Count, bool bSave)
{
	Journal::FAgentSaveScope AgentScope; // undo/redo issued by the agent, not the editor's Ctrl+Z
	FNiagaraMcpUndoResult Result;
	TArray<UObject*> Touched;
	for (int32 Index = 0; GEditor && GEditor->Trans && Index < FMath::Clamp(Count, 1, 50); ++Index)
	{
		const FTransactionContext Context = GEditor->Trans->GetUndoContext(true);
		TArray<UObject*> Objects;
		GetPendingTransactionObjects(true, Objects);
		if (!GEditor->UndoTransaction())
		{
			break;
		}
		Touched.Append(Objects);
		Result.Transactions.Add(Context.Title.ToString());
	}
	if (Result.Transactions.Num() == 0)
	{
		ReportError(TEXT("Nothing to undo."));
		return Result;
	}
	NiagaraMcpTools::RefreshSystemsAfterTransaction(Touched);
	if (bSave)
	{
		NiagaraMcpTools::SaveTouchedPackages(Touched, Result.SavedPackages, Result.UnsavedPackages);
	}
	return Result;
}

FNiagaraMcpUndoResult UNiagaraMcpEditorTools::Redo(int32 Count, bool bSave)
{
	Journal::FAgentSaveScope AgentScope; // undo/redo issued by the agent, not the editor's Ctrl+Z
	FNiagaraMcpUndoResult Result;
	TArray<UObject*> Touched;
	for (int32 Index = 0; GEditor && GEditor->Trans && Index < FMath::Clamp(Count, 1, 50); ++Index)
	{
		const FTransactionContext Context = GEditor->Trans->GetRedoContext();
		TArray<UObject*> Objects;
		GetPendingTransactionObjects(false, Objects);
		if (!GEditor->RedoTransaction())
		{
			break;
		}
		Touched.Append(Objects);
		Result.Transactions.Add(Context.Title.ToString());
	}
	if (Result.Transactions.Num() == 0)
	{
		ReportError(TEXT("Nothing to redo."));
		return Result;
	}
	NiagaraMcpTools::RefreshSystemsAfterTransaction(Touched);
	if (bSave)
	{
		NiagaraMcpTools::SaveTouchedPackages(Touched, Result.SavedPackages, Result.UnsavedPackages);
	}
	return Result;
}

FNiagaraMcpLogPage UNiagaraMcpEditorTools::ReadOutputLog(const TArray<FString>& Categories, const FString& MinVerbosity, const FString& Contains, int64 SinceCursor, int32 MaxLines)
{
	FNiagaraMcpLogPage Page;
	ELogVerbosity::Type MaxVerbosity = ELogVerbosity::Log;
	const FString Level = MinVerbosity.TrimStartAndEnd();
	if (Level.Equals(TEXT("Error"), ESearchCase::IgnoreCase))
	{
		MaxVerbosity = ELogVerbosity::Error;
	}
	else if (Level.Equals(TEXT("Warning"), ESearchCase::IgnoreCase))
	{
		MaxVerbosity = ELogVerbosity::Warning;
	}
	else if (Level.Equals(TEXT("Display"), ESearchCase::IgnoreCase))
	{
		MaxVerbosity = ELogVerbosity::Display;
	}
	else if (Level.Equals(TEXT("Verbose"), ESearchCase::IgnoreCase))
	{
		MaxVerbosity = ELogVerbosity::Verbose;
	}
	int64 NextCursor = 0;
	for (const FNiagaraMcpLogLine& Line : FNiagaraMcpLogBuffer::Get().Read(SinceCursor, Categories, MaxVerbosity, IsUnset(Contains) ? FString() : Contains.TrimStartAndEnd(), FMath::Clamp(MaxLines, 1, 2000), NextCursor))
	{
		Page.Lines.Add(FString::Printf(TEXT("[%s] %s: %s"), *Line.Category.ToString(), ToString(Line.Verbosity), *Line.Text));
	}
	Page.NextCursor = NextCursor;
	return Page;
}

#undef LOCTEXT_NAMESPACE
