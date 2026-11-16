#include "NiagaraMcpJournal.h"

#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Editor/Transactor.h"
#include "Misc/ITransaction.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"

namespace NiagaraMcp::Journal
{
	namespace
	{
		constexpr int32 MaxEntries = 1000;
		FCriticalSection Lock;
		TArray<FEntry> Entries;
		// Seeded with the session start time (ms) so cursors keep increasing across editor restarts.
		int64 NextCursor = FDateTime::UtcNow().ToUnixTimestamp() * 1000;
		int32 AgentSaveDepth = 0;
		FDelegateHandle StateHandle;
		FDelegateHandle UndoHandle;
		FDelegateHandle RedoHandle;
		FDelegateHandle SaveHandle;

		bool IsAgentTitle(const FString& Title)
		{
			return Title.StartsWith(TEXT("Niagara MCP:")) || Title.StartsWith(TEXT("Material MCP:")) || Title.StartsWith(TEXT("Asset MCP:"));
		}

		UTransBuffer* GetBuffer()
		{
			return GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
		}

		/** Content packages touched by the transaction at QueueIndex. */
		TArray<FString> PackagesOf(int32 QueueIndex)
		{
			TArray<FString> Packages;
			UTransBuffer* Buffer = GetBuffer();
			const FTransaction* Transaction = Buffer ? Buffer->GetTransaction(QueueIndex) : nullptr;
			if (Transaction == nullptr)
			{
				return Packages;
			}
			TArray<UObject*> Objects;
			Transaction->GetTransactionObjects(Objects);
			for (UObject* Object : Objects)
			{
				if (Object && Object->GetOutermost() && Object->GetOutermost() != GetTransientPackage())
				{
					const FString Name = Object->GetOutermost()->GetName();
					if (Name.StartsWith(TEXT("/")) && !Name.StartsWith(TEXT("/Temp/")) && !Name.StartsWith(TEXT("/Engine/Transient")))
					{
						Packages.AddUnique(Name);
					}
				}
			}
			return Packages;
		}

		void Add(FEntry&& Entry)
		{
			FScopeLock ScopeLock(&Lock);
			Entry.Cursor = NextCursor++;
			Entry.Time = FDateTime::UtcNow();
			Entries.Add(MoveTemp(Entry));
			if (Entries.Num() > MaxEntries)
			{
				Entries.RemoveAt(0, Entries.Num() - MaxEntries);
			}
		}

		void OnTransactionState(const FTransactionContext& Context, ETransactionStateEventType State)
		{
			if (State != ETransactionStateEventType::PostTransactionFinalized)
			{
				return;
			}
			FEntry Entry;
			Entry.Kind = TEXT("Edit");
			Entry.UndoableCount = GetUndoableCount();
			// Transient transactions (nothing recorded, or PIE objects only) are popped again but still raise this event;
			// only log when the top undoable transaction IS the finalized one, and read its title from there.
			UTransBuffer* Buffer = GetBuffer();
			const FTransaction* Transaction = Buffer ? Buffer->GetTransaction(Entry.UndoableCount - 1) : nullptr;
			if (Transaction == nullptr || Transaction->GetContext().TransactionId != Context.TransactionId)
			{
				return;
			}
			Entry.Title = Transaction->GetContext().Title.ToString();
			Entry.bByAgent = IsAgentTitle(Entry.Title);
			Entry.Packages = PackagesOf(Entry.UndoableCount - 1);
			Add(MoveTemp(Entry));
		}

		void OnUndoRedo(const FTransactionContext& Context, bool bSucceeded, const TCHAR* Kind)
		{
			if (!bSucceeded)
			{
				return;
			}
			FEntry Entry;
			Entry.Kind = Kind;
			Entry.Title = Context.Title.ToString();
			// Undo/Redo issued through this plugin's tools runs inside an agent call; the editor's Ctrl+Z does not.
			Entry.bByAgent = AgentSaveDepth > 0;
			Entry.UndoableCount = GetUndoableCount();
			Add(MoveTemp(Entry));
		}

		void OnPackageSaved(const FString& Filename, UPackage* Package, FObjectPostSaveContext SaveContext)
		{
			if (Package == nullptr || SaveContext.IsProceduralSave() || !Package->GetName().StartsWith(TEXT("/Game")))
			{
				return;
			}
			FEntry Entry;
			Entry.Kind = TEXT("Save");
			Entry.Title = TEXT("Save");
			Entry.Packages.Add(Package->GetName());
			Entry.bByAgent = AgentSaveDepth > 0;
			Entry.UndoableCount = GetUndoableCount();
			Add(MoveTemp(Entry));
		}
	}

	int32 GetUndoableCount()
	{
		UTransBuffer* Buffer = GetBuffer();
		return Buffer ? Buffer->GetQueueLength() - Buffer->GetUndoCount() : 0;
	}

	FAgentSaveScope::FAgentSaveScope()
	{
		++AgentSaveDepth;
	}

	FAgentSaveScope::~FAgentSaveScope()
	{
		--AgentSaveDepth;
	}

	void Startup()
	{
		if (UTransBuffer* Buffer = GetBuffer(); Buffer && !StateHandle.IsValid())
		{
			StateHandle = Buffer->OnTransactionStateChanged().AddStatic(&OnTransactionState);
			UndoHandle = Buffer->OnUndo().AddLambda([](const FTransactionContext& Context, bool bSucceeded) { OnUndoRedo(Context, bSucceeded, TEXT("Undo")); });
			RedoHandle = Buffer->OnRedo().AddLambda([](const FTransactionContext& Context, bool bSucceeded) { OnUndoRedo(Context, bSucceeded, TEXT("Redo")); });
		}
		if (!SaveHandle.IsValid())
		{
			SaveHandle = UPackage::PackageSavedWithContextEvent.AddStatic(&OnPackageSaved);
		}
	}

	void Shutdown()
	{
		if (UTransBuffer* Buffer = GetBuffer())
		{
			Buffer->OnTransactionStateChanged().Remove(StateHandle);
			Buffer->OnUndo().Remove(UndoHandle);
			Buffer->OnRedo().Remove(RedoHandle);
		}
		UPackage::PackageSavedWithContextEvent.Remove(SaveHandle);
		StateHandle.Reset();
		UndoHandle.Reset();
		RedoHandle.Reset();
		SaveHandle.Reset();
	}

	TArray<FEntry> Read(int64 SinceCursor, int32 InMax)
	{
		FScopeLock ScopeLock(&Lock);
		TArray<FEntry> Result;
		const int32 Max = FMath::Clamp(InMax, 1, MaxEntries);
		for (const FEntry& Entry : Entries)
		{
			if (Entry.Cursor > SinceCursor)
			{
				Result.Add(Entry);
				if (Result.Num() >= Max)
				{
					break;
				}
			}
		}
		return Result;
	}

	int64 GetLatestCursor()
	{
		FScopeLock ScopeLock(&Lock);
		return NextCursor - 1;
	}

	int64 GetOldestCursor()
	{
		FScopeLock ScopeLock(&Lock);
		return Entries.Num() ? Entries[0].Cursor : NextCursor;
	}

	FGuid GetTopTransactionId()
	{
		UTransBuffer* Buffer = GetBuffer();
		const FTransaction* Transaction = Buffer ? Buffer->GetTransaction(GetUndoableCount() - 1) : nullptr;
		return Transaction ? Transaction->GetContext().TransactionId : FGuid();
	}

	bool IsUndoable(const FGuid& TransactionId)
	{
		UTransBuffer* Buffer = GetBuffer();
		const int32 Undoable = GetUndoableCount();
		for (int32 Index = 0; Buffer && Index < Undoable; ++Index)
		{
			if (const FTransaction* Transaction = Buffer->GetTransaction(Index); Transaction && Transaction->GetContext().TransactionId == TransactionId)
			{
				return true;
			}
		}
		return false;
	}
}
