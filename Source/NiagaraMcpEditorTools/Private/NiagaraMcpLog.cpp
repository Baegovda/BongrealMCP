#include "NiagaraMcpLog.h"

#include "Misc/ScopeLock.h"

FNiagaraMcpLogBuffer& FNiagaraMcpLogBuffer::Get()
{
	static FNiagaraMcpLogBuffer Instance;
	return Instance;
}

void FNiagaraMcpLogBuffer::Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category)
{
	if (Message == nullptr || *Message == TEXT('\0'))
	{
		return;
	}
	FScopeLock ScopeLock(&Lock);
	FNiagaraMcpLogLine Line;
	Line.Cursor = NextCursor++;
	Line.Category = Category;
	Line.Verbosity = static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
	Line.Text = FString(Message).Left(2000);
	if (Lines.Num() < Capacity)
	{
		Lines.Add(MoveTemp(Line));
	}
	else
	{
		Lines[Head] = MoveTemp(Line);
		Head = (Head + 1) % Capacity;
	}
}

TArray<FNiagaraMcpLogLine> FNiagaraMcpLogBuffer::Read(int64 SinceCursor, const TArray<FString>& Categories, ELogVerbosity::Type MaxVerbosity, const FString& Contains, int32 MaxLines, int64& OutNextCursor) const
{
	FScopeLock ScopeLock(&Lock);
	TArray<FNiagaraMcpLogLine> Result;
	OutNextCursor = NextCursor;
	const int32 Count = Lines.Num();
	for (int32 Offset = 0; Offset < Count; ++Offset)
	{
		const FNiagaraMcpLogLine& Line = Lines[(Head + Offset) % Count];
		if (Line.Cursor <= SinceCursor || Line.Verbosity > MaxVerbosity)
		{
			continue;
		}
		if (Categories.Num() > 0 && !Categories.ContainsByPredicate([&Line](const FString& Category) { return Line.Category.ToString().Equals(Category, ESearchCase::IgnoreCase); }))
		{
			continue;
		}
		if (!Contains.IsEmpty() && !Line.Text.Contains(Contains))
		{
			continue;
		}
		Result.Add(Line);
	}
	// Keep the newest lines when there are more than MaxLines.
	if (MaxLines > 0 && Result.Num() > MaxLines)
	{
		Result.RemoveAt(0, Result.Num() - MaxLines);
	}
	return Result;
}
