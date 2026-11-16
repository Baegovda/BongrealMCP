#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

/** One captured output log line. */
struct FNiagaraMcpLogLine
{
	int64 Cursor = 0;
	FName Category;
	ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
	FString Text;
};

/**
 * Ring buffer of recent output log lines, so ReadOutputLog can return what the editor logged (shader compile
 * errors, Niagara warnings, ...) since a cursor. Registered with GLog by the module.
 */
class FNiagaraMcpLogBuffer : public FOutputDevice
{
public:
	static FNiagaraMcpLogBuffer& Get();

	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
	virtual bool IsMemoryOnly() const override { return true; }

	/** Lines after SinceCursor that pass the filters, oldest first. OutNextCursor is the cursor to pass next time. */
	TArray<FNiagaraMcpLogLine> Read(int64 SinceCursor, const TArray<FString>& Categories, ELogVerbosity::Type MaxVerbosity, const FString& Contains, int32 MaxLines, int64& OutNextCursor) const;

private:
	static constexpr int32 Capacity = 4000;
	mutable FCriticalSection Lock;
	TArray<FNiagaraMcpLogLine> Lines;
	int32 Head = 0;
	int64 NextCursor = 1;
};
