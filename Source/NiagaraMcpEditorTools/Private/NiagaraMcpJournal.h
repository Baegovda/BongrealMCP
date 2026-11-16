#pragma once

#include "CoreMinimal.h"

/**
 * Change journal: every finalized editor transaction (with the packages it touched), undo / redo, and package saves,
 * tagged as made by the agent (this plugin's "Niagara MCP: / Material MCP: / Asset MCP:" transactions and saves) or by
 * someone else (the user in the editor). Lets an agent see what the artist changed between its calls instead of
 * silently overwriting it, and backs the restore points.
 */
namespace NiagaraMcp::Journal
{
	struct FEntry
	{
		int64 Cursor = 0;
		FDateTime Time;
		/** Edit, Undo, Redo or Save. */
		FString Kind;
		FString Title;
		/** Packages touched, e.g. /Game/FX/NS_Fire. */
		TArray<FString> Packages;
		bool bByAgent = false;
		/** Undoable transaction count right after this entry (Edit/Undo/Redo). */
		int32 UndoableCount = 0;
	};

	void Startup();
	void Shutdown();

	/** Entries with Cursor > SinceCursor (oldest first, capped). */
	TArray<FEntry> Read(int64 SinceCursor, int32 MaxEntries);
	int64 GetLatestCursor();

	/** Cursor of the oldest entry still kept (older ones were evicted). */
	int64 GetOldestCursor();

	/** Id of the top undoable transaction (invalid when there is none). */
	FGuid GetTopTransactionId();

	/** True when the transaction is still in the undoable part of the history. */
	bool IsUndoable(const FGuid& TransactionId);

	/** RAII: saves performed in this scope are attributed to the agent. */
	struct FAgentSaveScope
	{
		FAgentSaveScope();
		~FAgentSaveScope();
	};

	/** Transactions currently undoable (queue length - undo count). */
	int32 GetUndoableCount();
}
