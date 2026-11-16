#include "NiagaraMcpEditorTools.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Editor/Transactor.h"
#include "Misc/ObjectThumbnail.h"
#include "NiagaraEmitter.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraMcpEditorToolsInternal.h"
#include "NiagaraMcpJournal.h"
#include "NiagaraSystem.h"
#include "ObjectTools.h"

using namespace NiagaraMcp;

namespace NiagaraMcpInfra
{
	struct FRestorePoint
	{
		/** Top undoable transaction when the point was made (invalid = empty history). */
		FGuid TopTransaction;
		int32 UndoableCount = 0;
		int64 Cursor = 0;
		FDateTime Created;
	};

	TMap<FName, FRestorePoint>& RestorePoints()
	{
		static TMap<FName, FRestorePoint> Points;
		return Points;
	}

	FNiagaraMcpChangeEntry ToStruct(const Journal::FEntry& Entry)
	{
		FNiagaraMcpChangeEntry Out;
		Out.Cursor = Entry.Cursor;
		Out.Time = Entry.Time.ToIso8601();
		Out.Kind = Entry.Kind;
		Out.Title = Entry.Title;
		Out.Packages = Entry.Packages;
		Out.bByAgent = Entry.bByAgent;
		return Out;
	}
}

using namespace NiagaraMcpInfra;

FNiagaraMcpChangeLog UNiagaraMcpEditorTools::GetChangesSince(int64 SinceCursor, bool bIncludeAgentChanges, const FString& Asset, int32 MaxEntries)
{
	FNiagaraMcpChangeLog Log;
	const FString AssetFilter = IsUnset(Asset) ? FString() : FPackageName::ObjectPathToPackageName(Asset.TrimStartAndEnd());
	int64 Last = SinceCursor;
	Log.bHistoryTruncated = SinceCursor > 0 && SinceCursor < Journal::GetOldestCursor() - 1;
	const TArray<Journal::FEntry> Entries = Journal::Read(SinceCursor, 1000);
	for (const Journal::FEntry& Entry : Entries)
	{
		const bool bMatches = (bIncludeAgentChanges || !Entry.bByAgent)
			&& (AssetFilter.IsEmpty() || Entry.Packages.ContainsByPredicate([&AssetFilter](const FString& Package) { return Package.Equals(AssetFilter, ESearchCase::IgnoreCase); }));
		if (bMatches && Log.Entries.Num() >= FMath::Clamp(MaxEntries, 1, 1000))
		{
			Log.bMore = true; // NextCursor stops before this entry so the next call returns it
			break;
		}
		Last = Entry.Cursor;
		if (bMatches)
		{
			Log.UserChangeCount += Entry.bByAgent ? 0 : 1;
			Log.Entries.Add(ToStruct(Entry));
		}
	}
	Log.NextCursor = Log.bMore ? Last : FMath::Max(Last, Journal::GetLatestCursor());
	return Log;
}

FNiagaraMcpRestorePointInfo UNiagaraMcpEditorTools::CreateRestorePoint(FName Name)
{
	if (Name.IsNone())
	{
		ReportError(TEXT("[MISSING_ARGUMENT] Name is required, e.g. \"before_color_pass\"."));
		return {};
	}
	FRestorePoint Point;
	Point.TopTransaction = Journal::GetTopTransactionId();
	Point.UndoableCount = Journal::GetUndoableCount();
	Point.Cursor = Journal::GetLatestCursor();
	Point.Created = FDateTime::UtcNow();
	RestorePoints().Add(Name, Point);

	FNiagaraMcpRestorePointInfo Info;
	Info.Name = Name;
	Info.UndoableCount = Point.UndoableCount;
	Info.Created = Point.Created.ToIso8601();
	return Info;
}

TArray<FNiagaraMcpRestorePointInfo> UNiagaraMcpEditorTools::ListRestorePoints()
{
	TArray<FNiagaraMcpRestorePointInfo> Result;
	const int32 Current = Journal::GetUndoableCount();
	for (const TPair<FName, FRestorePoint>& Pair : RestorePoints())
	{
		FNiagaraMcpRestorePointInfo& Info = Result.AddDefaulted_GetRef();
		Info.Name = Pair.Key;
		Info.UndoableCount = Pair.Value.UndoableCount;
		Info.Created = Pair.Value.Created.ToIso8601();
		Info.StepsSince = Current - Pair.Value.UndoableCount;
	}
	return Result;
}

FNiagaraMcpRollbackResult UNiagaraMcpEditorTools::RollbackToRestorePoint(FName Name, bool bAllowUserChanges, bool bSave)
{
	FNiagaraMcpRollbackResult Result;
	Result.Name = Name;
	const FRestorePoint* Point = RestorePoints().Find(Name);
	if (Point == nullptr)
	{
		TArray<FString> Names;
		for (const TPair<FName, FRestorePoint>& Pair : RestorePoints())
		{
			Names.Add(Pair.Key.ToString());
		}
		ReportError(FString::Printf(TEXT("[RESTORE_POINT_NOT_FOUND] No restore point '%s'.%s Restore points: %s"), *Name.ToString(), *DidYouMean(Name.ToString(), Names), Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("(none; call CreateRestorePoint first)")));
		return Result;
	}
	if (GEditor && GEditor->PlayWorld)
	{
		ReportError(TEXT("[PIE_ACTIVE] Stop Play In Editor before rolling back."));
		return Result;
	}
	// The point is identified by its top transaction, not by a count (counts drift when old transactions are trimmed
	// for memory, or after undo + new edits). It must still be in the undoable history.
	if (Point->TopTransaction.IsValid() && !Journal::IsUndoable(Point->TopTransaction))
	{
		ReportError(FString::Printf(TEXT("[HISTORY_CHANGED] The undo history no longer contains the state of '%s' (something was undone past it or the history was trimmed/cleared). Inspect GetChangesSince instead."), *Name.ToString()));
		return Result;
	}
	if (Point->Cursor < Journal::GetOldestCursor() - 1)
	{
		ReportError(FString::Printf(TEXT("[HISTORY_CHANGED] Too many changes since '%s' to verify that none of them is the user's. Revert manually with Undo after checking GetChangesSince."), *Name.ToString()));
		return Result;
	}
	for (const Journal::FEntry& Entry : Journal::Read(Point->Cursor, 1000))
	{
		if (!Entry.bByAgent && (Entry.Kind == TEXT("Edit") || Entry.Kind == TEXT("Undo") || Entry.Kind == TEXT("Redo")))
		{
			Result.UserChanges.Add(FString::Printf(TEXT("%s (%s)"), *Entry.Title, *FString::Join(Entry.Packages, TEXT(", "))));
		}
	}
	if (Result.UserChanges.Num() > 0 && !bAllowUserChanges)
	{
		ReportError(FString::Printf(TEXT("[USER_CHANGES] Rolling back to '%s' would also undo %d edit(s) made in the editor: %s. Pass bAllowUserChanges=true to undo them too, or keep them."),
			*Name.ToString(), Result.UserChanges.Num(), *FString::Join(Result.UserChanges, TEXT("; "))));
		return Result;
	}

	TArray<UObject*> Touched;
	int32 Steps = 0;
	{
		Journal::FAgentSaveScope AgentScope;
		// Undo until the point's transaction is on top again (or the history is empty for a point made on an empty one).
		while (GEditor && GEditor->Trans && Journal::GetUndoableCount() > 0 && Journal::GetTopTransactionId() != Point->TopTransaction && Steps < 1000)
		{
			++Steps;
			const FTransactionContext Context = GEditor->Trans->GetUndoContext(true);
			const int32 Index = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount() - 1;
			if (const FTransaction* Transaction = GEditor->Trans->GetTransaction(Index))
			{
				TArray<UObject*> Objects;
				Transaction->GetTransactionObjects(Objects);
				Touched.Append(Objects);
			}
			if (!GEditor->UndoTransaction())
			{
				break;
			}
			Result.Undone.Add(Context.Title.ToString());
		}
	}
	NiagaraMcpTools::RefreshSystemsAfterTransaction(Touched);
	Result.bRolledBack = Journal::GetTopTransactionId() == Point->TopTransaction || (!Point->TopTransaction.IsValid() && Journal::GetUndoableCount() == 0);
	if (bSave)
	{
		NiagaraMcpTools::SaveTouchedPackages(Touched, Result.SavedPackages, Result.UnsavedPackages);
	}
	return Result;
}

FToolsetImage UNiagaraMcpEditorTools::PreviewAssets(const TArray<FString>& AssetPaths, int32 Size)
{
	FToolsetImage Image;
	if (AssetPaths.Num() == 0 || AssetPaths.Num() > 16)
	{
		ReportError(TEXT("[MISSING_ARGUMENT] Pass 1 to 16 asset paths, e.g. from FindAssets."));
		return Image;
	}
	const int32 Tile = FMath::Clamp(Size, 32, 512);
	const int32 Columns = FMath::Min(AssetPaths.Num(), 4);
	const int32 Rows = FMath::DivideAndRoundUp(AssetPaths.Num(), Columns);
	TArray<FColor> Sheet;
	Sheet.Init(FColor(24, 24, 24, 255), Columns * Tile * Rows * Tile);
	for (int32 Index = 0; Index < AssetPaths.Num(); ++Index)
	{
		UObject* Asset = LoadObjectByPath(AssetPaths[Index], nullptr, TEXT("Asset"));
		if (Asset == nullptr)
		{
			return FToolsetImage();
		}
		FObjectThumbnail Thumbnail;
		ThumbnailTools::RenderThumbnail(Asset, Tile, Tile, ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush, nullptr, &Thumbnail);
		const TArray<uint8>& Data = Thumbnail.GetUncompressedImageData();
		if (Thumbnail.GetImageWidth() != Tile || Thumbnail.GetImageHeight() != Tile || Data.Num() < Tile * Tile * 4)
		{
			continue; // no renderer for this class: leave the tile dark
		}
		const int32 OffsetX = (Index % Columns) * Tile;
		const int32 OffsetY = (Index / Columns) * Tile;
		for (int32 Y = 0; Y < Tile; ++Y)
		{
			for (int32 X = 0; X < Tile; ++X)
			{
				const uint8* Pixel = &Data[(Y * Tile + X) * 4]; // BGRA
				Sheet[(OffsetY + Y) * Columns * Tile + OffsetX + X] = FColor(Pixel[2], Pixel[1], Pixel[0], 255);
			}
		}
	}
	if (!Image.SetFromBitmap(Sheet, FIntPoint(Columns * Tile, Rows * Tile)))
	{
		ReportError(TEXT("Encoding the thumbnail sheet failed."));
	}
	return Image;
}

TArray<FNiagaraMcpTemplateInfo> UNiagaraMcpEditorTools::ListNiagaraTemplates(const FString& Kind, const FString& Filter)
{
	TArray<FNiagaraMcpTemplateInfo> Result;
	const bool bEmitters = IsUnset(Kind) || Kind.Equals(TEXT("Emitter"), ESearchCase::IgnoreCase) || Kind.Equals(TEXT("Emitters"), ESearchCase::IgnoreCase);
	const bool bSystems = IsUnset(Kind) || Kind.Equals(TEXT("System"), ESearchCase::IgnoreCase) || Kind.Equals(TEXT("Systems"), ESearchCase::IgnoreCase);
	if (!bEmitters && !bSystems)
	{
		ReportError(FString::Printf(TEXT("[INVALID_VALUE] Kind '%s': use Emitter, System or Any."), *Kind));
		return Result;
	}
	TArray<FString> Words;
	if (!IsUnset(Filter))
	{
		Filter.ToLower().ParseIntoArrayWS(Words);
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter AssetFilter;
	AssetFilter.bRecursivePaths = true;
	AssetFilter.PackagePaths.Add(TEXT("/Niagara/DefaultAssets/Templates"));
	if (bEmitters)
	{
		AssetFilter.ClassPaths.Add(UNiagaraEmitter::StaticClass()->GetClassPathName());
	}
	if (bSystems)
	{
		AssetFilter.ClassPaths.Add(UNiagaraSystem::StaticClass()->GetClassPathName());
	}
	TArray<FAssetData> Assets;
	Registry.GetAssets(AssetFilter, Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.GetObjectPathString() < B.GetObjectPathString(); });
	for (const FAssetData& Asset : Assets)
	{
		FString Description;
		if (!Asset.GetTagValue(TEXT("TemplateAssetDescription"), Description) || Description.IsEmpty())
		{
			if (const UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Asset.GetAsset()))
			{
				Description = Emitter->TemplateAssetDescription.ToString();
			}
			else if (const UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset.GetAsset()))
			{
				Description = System->TemplateAssetDescription.ToString();
			}
		}
		const FString Haystack = (Asset.AssetName.ToString() + TEXT(" ") + Asset.PackagePath.ToString() + TEXT(" ") + Description).ToLower();
		if (Words.ContainsByPredicate([&Haystack](const FString& Word) { return !Haystack.Contains(Word); }))
		{
			continue;
		}
		FNiagaraMcpTemplateInfo& Info = Result.AddDefaulted_GetRef();
		Info.Path = Asset.GetObjectPathString();
		Info.Kind = Asset.AssetClassPath.GetAssetName() == UNiagaraEmitter::StaticClass()->GetFName() ? TEXT("Emitter") : TEXT("System");
		Info.Category = Asset.PackagePath.ToString().RightChop(FString(TEXT("/Niagara/DefaultAssets/Templates/")).Len());
		Info.Description = Description.Left(300);
	}
	return Result;
}
