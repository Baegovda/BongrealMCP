#include "NiagaraMcpEditorTools.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "FileHelpers.h"
#include "JsonObjectConverter.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Internationalization/Regex.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraMcpJournal.h"
#include "MaterialMcpEditorTools.h"
#include "NiagaraSystem.h"
#include "NiagaraMcpEditorToolsInternal.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ToolsetRegistry/ToolsetRegistry.h"
#include "ToolsetRegistry/ToolsetRegistrySubsystem.h"
#include "UObject/Package.h"

using namespace NiagaraMcp;

namespace NiagaraMcpBatch
{
	/** Toolsets searched for short tool names, in order. */
	static const TCHAR* const OwnToolsets[] =
	{
		TEXT("NiagaraMcpEditorTools.NiagaraMcpEditorTools"),
		TEXT("NiagaraMcpEditorTools.MaterialMcpEditorTools"),
		TEXT("NiagaraMcpEditorTools.AssetMcpEditorTools"),
	};

	static FString Normalize(const FString& Name)
	{
		FString Out = Name.ToLower();
		Out.ReplaceInline(TEXT("_"), TEXT(""));
		Out.ReplaceInline(TEXT("-"), TEXT(""));
		return Out;
	}

	struct FToolInfo
	{
		FString FullName;
		TSharedPtr<FJsonObject> InputProperties; // inputSchema.properties
	};

	/** Caches the tool entries of the plugin's toolsets and any toolset referenced by full name. */
	class FToolCatalog
	{
	public:
		explicit FToolCatalog(UE::ToolsetRegistry::FToolsetRegistry& InRegistry) : Registry(InRegistry) {}

		/** Resolves "SetModuleInput", "MaterialMcpEditorTools.AddExpression" or a full tool name. */
		bool Resolve(const FString& InName, FToolInfo& Out, FString& OutError)
		{
			const FString Name = InName.TrimStartAndEnd();
			if (Name.IsEmpty())
			{
				OutError = TEXT("Tool is empty.");
				return false;
			}
			const FString Short = Name.Contains(TEXT(".")) ? Name.RightChop(Name.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd) + 1) : Name;
			if (Short.Equals(TEXT("ExecuteBatch"), ESearchCase::IgnoreCase) || Short.Equals(TEXT("Undo"), ESearchCase::IgnoreCase) || Short.Equals(TEXT("Redo"), ESearchCase::IgnoreCase))
			{
				OutError = FString::Printf(TEXT("%s cannot run inside ExecuteBatch."), *Short);
				return false;
			}

			// Full name: <Toolset>.<Function>, where the toolset name itself contains a dot.
			if (Name.Contains(TEXT(".")))
			{
				const FString ToolsetName = Name.Left(Name.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd));
				TArray<FToolInfo>& Tools = Load(ToolsetName);
				for (const FToolInfo& Tool : Tools)
				{
					if (Tool.FullName.Equals(Name, ESearchCase::IgnoreCase))
					{
						Out = Tool;
						return true;
					}
				}
				// "MaterialMcpEditorTools.AddExpression": match the toolset by its class part.
				for (const TCHAR* Own : OwnToolsets)
				{
					if (FString(Own).EndsWith(TEXT(".") + ToolsetName, ESearchCase::IgnoreCase))
					{
						for (const FToolInfo& Tool : Load(Own))
						{
							if (Tool.FullName.EndsWith(TEXT(".") + Short, ESearchCase::IgnoreCase))
							{
								Out = Tool;
								return true;
							}
						}
					}
				}
			}

			TArray<FToolInfo> Matches;
			TArray<FString> Known;
			for (const TCHAR* Own : OwnToolsets)
			{
				for (const FToolInfo& Tool : Load(Own))
				{
					const FString ToolShort = Tool.FullName.RightChop(Tool.FullName.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd) + 1);
					Known.AddUnique(ToolShort);
					if (ToolShort.Equals(Short, ESearchCase::IgnoreCase))
					{
						Matches.Add(Tool);
					}
				}
			}
			if (Matches.Num() == 1)
			{
				Out = Matches[0];
				return true;
			}
			if (Matches.Num() > 1)
			{
				TArray<FString> Names;
				for (const FToolInfo& Match : Matches)
				{
					Names.Add(Match.FullName);
				}
				OutError = FString::Printf(TEXT("Tool '%s' exists in several toolsets; use the full name: %s"), *Name, *FString::Join(Names, TEXT(", ")));
				return false;
			}
			OutError = FString::Printf(TEXT("[TOOL_NOT_FOUND] Unknown tool '%s'.%s"), *Name, *DidYouMean(Short, Known));
			return false;
		}

	private:
		TArray<FToolInfo>& Load(const FString& ToolsetName)
		{
			if (TArray<FToolInfo>* Cached = Cache.Find(ToolsetName))
			{
				return *Cached;
			}
			TArray<FToolInfo>& Tools = Cache.Add(ToolsetName);
			TSharedPtr<UE::ToolsetRegistry::FToolset> Toolset = Registry.Find(ToolsetName);
			TSharedPtr<FJsonObject> Schema;
			if (!Toolset.IsValid() || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Toolset->GetJsonSchema()), Schema) || !Schema.IsValid())
			{
				return Tools;
			}
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (!Schema->TryGetArrayField(TEXT("tools"), Entries))
			{
				return Tools;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Entries)
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Entry))
				{
					continue;
				}
				FToolInfo& Info = Tools.AddDefaulted_GetRef();
				Info.FullName = (*Entry)->GetStringField(TEXT("name"));
				const TSharedPtr<FJsonObject>* Input = nullptr;
				const TSharedPtr<FJsonObject>* Properties = nullptr;
				if ((*Entry)->TryGetObjectField(TEXT("inputSchema"), Input) && (*Input)->TryGetObjectField(TEXT("properties"), Properties))
				{
					Info.InputProperties = *Properties;
				}
			}
			return Tools;
		}

		UE::ToolsetRegistry::FToolsetRegistry& Registry;
		TMap<FString, TArray<FToolInfo>> Cache;
	};

	/** Follows "a.b.0.c" (keys case-insensitive, numbers index arrays) into a JSON value. */
	static TSharedPtr<FJsonValue> FollowPath(TSharedPtr<FJsonValue> Value, const FString& Path)
	{
		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("."));
		for (const FString& Segment : Segments)
		{
			if (!Value.IsValid())
			{
				return nullptr;
			}
			if (Value->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
				const int32 Index = Segment.IsNumeric() ? FCString::Atoi(*Segment) : INDEX_NONE;
				Value = Array.IsValidIndex(Index) ? Array[Index] : nullptr;
				continue;
			}
			if (Value->Type != EJson::Object)
			{
				return nullptr;
			}
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			TSharedPtr<FJsonValue> Next;
			for (const auto& Pair : Object->Values)
			{
				if (Normalize(FString(Pair.Key)) == Normalize(Segment))
				{
					Next = Pair.Value;
					break;
				}
			}
			Value = Next;
		}
		return Value;
	}

	static FString ValueToText(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return FString();
		}
		FString Text;
		if (Value->TryGetString(Text))
		{
			return Text;
		}
		if (Value->Type == EJson::Number || Value->Type == EJson::Boolean)
		{
			return Value->AsString();
		}
		FString Json;
		FJsonSerializer::Serialize(Value, FString(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
		return Json;
	}

	struct FStepState
	{
		TMap<FString, TSharedPtr<FJsonValue>> Results; // by Id and by index
	};

	/** Looks up "$<ref>.<path>": ref is an Id or a 0-based step index. */
	static bool LookupReference(const FStepState& State, const FString& Reference, TSharedPtr<FJsonValue>& Out, FString& OutError)
	{
		FString Ref = Reference;
		FString Path;
		Reference.Split(TEXT("."), &Ref, &Path);
		const TSharedPtr<FJsonValue>* Found = State.Results.Find(Ref.ToLower());
		if (Found == nullptr)
		{
			OutError = FString::Printf(TEXT("[REFERENCE_NOT_FOUND] Reference '$%s': no earlier step with Id or index '%s'."), *Reference, *Ref);
			return false;
		}
		Out = Path.IsEmpty() ? *Found : FollowPath(*Found, Path);
		if (!Out.IsValid())
		{
			OutError = FString::Printf(TEXT("[REFERENCE_NOT_FOUND] Reference '$%s': the result of step '%s' has no '%s'. Result: %s"), *Reference, *Ref, *Path, *ValueToText(*Found).Left(400));
			return false;
		}
		return true;
	}

	/** Replaces "$ref.path" strings and "${ref.path}" substrings in the arguments. */
	static bool Substitute(TSharedPtr<FJsonValue>& Value, const FStepState& State, FString& OutError)
	{
		if (!Value.IsValid())
		{
			return true;
		}
		switch (Value->Type)
		{
		case EJson::String:
		{
			const FString Text = Value->AsString();
			static const FRegexPattern Whole(TEXT("^\\$([A-Za-z0-9_]+(\\.[A-Za-z0-9_\\[\\]]+)*)$"));
			FRegexMatcher WholeMatch(Whole, Text);
			if (WholeMatch.FindNext())
			{
				TSharedPtr<FJsonValue> Replacement;
				if (!LookupReference(State, WholeMatch.GetCaptureGroup(1), Replacement, OutError))
				{
					return false;
				}
				Value = Replacement;
				return true;
			}
			if (Text.Contains(TEXT("${")))
			{
				FString Out;
				int32 Cursor = 0;
				while (Cursor < Text.Len())
				{
					const int32 Start = Text.Find(TEXT("${"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Cursor);
					if (Start == INDEX_NONE)
					{
						Out += Text.Mid(Cursor);
						break;
					}
					const int32 End = Text.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
					if (End == INDEX_NONE)
					{
						Out += Text.Mid(Cursor);
						break;
					}
					Out += Text.Mid(Cursor, Start - Cursor);
					TSharedPtr<FJsonValue> Replacement;
					if (!LookupReference(State, Text.Mid(Start + 2, End - Start - 2), Replacement, OutError))
					{
						return false;
					}
					Out += ValueToText(Replacement);
					Cursor = End + 1;
				}
				Value = MakeShared<FJsonValueString>(Out);
			}
			return true;
		}
		case EJson::Array:
		{
			TArray<TSharedPtr<FJsonValue>> Items = Value->AsArray();
			for (TSharedPtr<FJsonValue>& Item : Items)
			{
				if (!Substitute(Item, State, OutError))
				{
					return false;
				}
			}
			Value = MakeShared<FJsonValueArray>(Items);
			return true;
		}
		case EJson::Object:
		{
			TSharedPtr<FJsonObject> Object = Value->AsObject();
			for (auto& Pair : Object->Values)
			{
				if (!Substitute(Pair.Value, State, OutError))
				{
					return false;
				}
			}
			return true;
		}
		default:
			return true;
		}
	}

	/**
	 * Maps loosely written argument names onto the schema's names, turns "/Game/X" into "/Game/X.X" for object
	 * parameters and forces bSave=false. Unknown names are an error (with suggestions).
	 */
	static bool ConformArguments(const FToolInfo& Tool, const TSharedPtr<FJsonObject>& In, TSharedPtr<FJsonObject>& Out, FString& OutError)
	{
		Out = MakeShared<FJsonObject>();
		TArray<FString> Valid;
		if (Tool.InputProperties.IsValid())
		{
			for (const auto& Pair : Tool.InputProperties->Values)
			{
				Valid.Add(FString(Pair.Key));
			}
		}
		for (const auto& Pair : In->Values)
		{
			FString Target;
			for (const FString& Name : Valid)
			{
				if (Normalize(Name) == Normalize(FString(Pair.Key)))
				{
					Target = Name;
					break;
				}
			}
			if (Target.IsEmpty())
			{
				const FString Key(Pair.Key);
				OutError = FString::Printf(TEXT("[ARGUMENT_NOT_FOUND] %s has no argument '%s'.%s Arguments: %s"), *Tool.FullName, *Key, *DidYouMean(Key, Valid), *FString::Join(Valid, TEXT(", ")));
				return false;
			}
			TSharedPtr<FJsonValue> Value = Pair.Value;
			const TSharedPtr<FJsonObject>* Property = nullptr;
			FString Text;
			FString Title;
			if (Tool.InputProperties->TryGetObjectField(Target, Property) && (*Property)->TryGetStringField(TEXT("title"), Title)
				&& Title.StartsWith(TEXT("/Script/")) && Value.IsValid() && Value->TryGetString(Text) && Text.StartsWith(TEXT("/")))
			{
				FString Leaf;
				Text.Split(TEXT("/"), nullptr, &Leaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
				if (!Leaf.IsEmpty() && !Leaf.Contains(TEXT(".")) && !Leaf.Contains(TEXT(":")))
				{
					Value = MakeShared<FJsonValueString>(Text + TEXT(".") + Leaf);
				}
			}
			Out->SetField(Target, Value);
		}
		for (const FString& Name : Valid)
		{
			if (Normalize(Name) == TEXT("bsave"))
			{
				Out->SetBoolField(Name, false);
			}
		}
		return true;
	}


	/** "[CODE] message" -> CODE (first bracketed upper-case token), else empty. */
	static FString ExtractErrorCode(const FString& Error)
	{
		int32 Start = INDEX_NONE;
		while ((Start = Error.Find(TEXT("["), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start + 1)) != INDEX_NONE)
		{
			const int32 End = Error.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
			if (End == INDEX_NONE)
			{
				break;
			}
			const FString Token = Error.Mid(Start + 1, End - Start - 1);
			bool bCode = Token.Len() >= 3;
			for (TCHAR Char : Token)
			{
				bCode &= (Char >= 'A' && Char <= 'Z') || Char == '_';
			}
			if (bCode)
			{
				return Token;
			}
		}
		return FString();
	}

	static FString ObjectPathOf(const FString& Path)
	{
		FString Trimmed = Path.TrimStartAndEnd();
		FString Leaf;
		Trimmed.Split(TEXT("/"), nullptr, &Leaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		return Leaf.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + Leaf;
	}

	/** Evaluates one Expect clause. */
	static FNiagaraMcpBatchExpectation Evaluate(const FString& Clause)
	{
		FNiagaraMcpBatchExpectation Out;
		Out.Expectation = Clause;
		FString Kind;
		FString Rest;
		if (!Clause.Split(TEXT(":"), &Kind, &Rest))
		{
			Out.Detail = TEXT("Unknown expectation; use Compiles:, Valid:, Particles: or MaterialCompiles:.");
			return Out;
		}
		Kind.TrimStartAndEndInline();
		Rest.TrimStartAndEndInline();
		if (Kind.Equals(TEXT("Compiles"), ESearchCase::IgnoreCase) || Kind.Equals(TEXT("Valid"), ESearchCase::IgnoreCase))
		{
			UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *ObjectPathOf(Rest));
			if (System == nullptr)
			{
				Out.Detail = FString::Printf(TEXT("System '%s' not found."), *Rest);
				return Out;
			}
			if (Kind.Equals(TEXT("Compiles"), ESearchCase::IgnoreCase))
			{
				const FNiagaraMcpCompileResult Compile = UNiagaraMcpEditorTools::CompileSystem(System, true, false);
				Out.bMet = Compile.bSucceeded;
				const FNiagaraMcpCompileMessage* FirstError = Compile.Messages.FindByPredicate([](const FNiagaraMcpCompileMessage& Message) { return Message.Severity == TEXT("Error"); });
				Out.Detail = Out.bMet ? TEXT("Compiled.") : (FirstError ? FirstError->Message.Left(300) : FString(TEXT("Compile failed or still compiling.")));
			}
			else
			{
				const FNiagaraMcpValidationResult Validation = UNiagaraMcpEditorTools::ValidateSystem(System, true);
				Out.bMet = Validation.ErrorCount == 0;
				const FNiagaraMcpIssue* FirstError = Validation.Issues.FindByPredicate([](const FNiagaraMcpIssue& Issue) { return Issue.Severity == TEXT("Error"); });
				Out.Detail = FString::Printf(TEXT("%d error(s), %d warning(s)%s"), Validation.ErrorCount, Validation.WarningCount, FirstError ? *(TEXT(": ") + FirstError->Message.Left(300)) : TEXT(""));
			}
			return Out;
		}
		if (Kind.Equals(TEXT("MaterialCompiles"), ESearchCase::IgnoreCase))
		{
			UObject* Material = LoadObject<UObject>(nullptr, *ObjectPathOf(Rest));
			if (Material == nullptr)
			{
				Out.Detail = FString::Printf(TEXT("Material '%s' not found."), *Rest);
				return Out;
			}
			const FMaterialMcpCompileResult Compile = UMaterialMcpEditorTools::CompileMaterial(Material, false);
			Out.bMet = Compile.bSucceeded;
			Out.Detail = Out.bMet ? TEXT("Compiled.") : (Compile.Errors.Num() ? Compile.Errors[0].Left(300) : FString(TEXT("Compile failed.")));
			return Out;
		}
		if (Kind.Equals(TEXT("Particles"), ESearchCase::IgnoreCase))
		{
			// /Game/FX/NS_A:Sparks@0.5>=1
			FString Path, Spec, Emitter, Timing, AtLeast;
			if (!Rest.Split(TEXT(":"), &Path, &Spec, ESearchCase::IgnoreCase, ESearchDir::FromEnd) || !Spec.Split(TEXT("@"), &Emitter, &Timing) || !Timing.Split(TEXT(">="), &Timing, &AtLeast))
			{
				Out.Detail = TEXT("Use Particles:<system path>:<emitter or Any>@<seconds>>=<count>, e.g. Particles:/Game/FX/NS_A:Sparks@0.5>=1");
				return Out;
			}
			UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *ObjectPathOf(Path));
			if (System == nullptr)
			{
				Out.Detail = FString::Printf(TEXT("System '%s' not found."), *Path);
				return Out;
			}
			const float Seconds = FCString::Atof(*Timing);
			const int32 Minimum = FCString::Atoi(*AtLeast);
			const TArray<FNiagaraMcpParticleSample> Samples = UNiagaraMcpEditorTools::GetParticleCounts(System, { Seconds });
			int32 Count = 0;
			if (Samples.Num() > 0)
			{
				if (IsUnset(Emitter))
				{
					Count = Samples[0].TotalParticles;
				}
				else
				{
					for (const FNiagaraMcpEmitterCount& EmitterCount : Samples[0].Emitters)
					{
						Count += EmitterCount.Emitter.ToString().Equals(Emitter.TrimStartAndEnd(), ESearchCase::IgnoreCase) ? EmitterCount.Particles : 0;
					}
				}
			}
			Out.bMet = Count >= Minimum;
			Out.Detail = FString::Printf(TEXT("%s: %d particles at %.2f s (need >= %d)"), *Emitter.TrimStartAndEnd(), Count, Seconds, Minimum);
			return Out;
		}
		Out.Detail = FString::Printf(TEXT("Unknown expectation kind '%s'; use Compiles, Valid, Particles or MaterialCompiles."), *Kind);
		return Out;
	}

	static int32 UndoableCount()
	{
		return GEditor && GEditor->Trans ? GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount() : 0;
	}
}

FNiagaraMcpBatchResult UNiagaraMcpEditorTools::ExecuteBatch(const TArray<FNiagaraMcpBatchOp>& Operations, bool bStopOnError, bool bRollbackOnError, bool bDryRun, bool bSave, const FString& Expect)
{
	using namespace NiagaraMcpBatch;
	FNiagaraMcpBatchResult Result;
	Journal::FAgentSaveScope AgentScope; // rollback undos are the agent's
	// Steps must never wait on a modal dialog (it would stall every queued MCP request): message boxes take their default.
	TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
	if (Operations.Num() == 0 || Operations.Num() > 200)
	{
		ReportError(TEXT("[INVALID_VALUE] Operations must contain 1 to 200 steps."));
		return Result;
	}
	auto Subsystem = UToolsetRegistrySubsystem::Get();
	if (Subsystem.HasError())
	{
		ReportError(TEXT("The Toolset Registry is not available."));
		return Result;
	}
	UE::ToolsetRegistry::FToolsetRegistry& Registry = Subsystem.GetValue()->ToolsetRegistry;
	FToolCatalog Catalog(Registry);

	TSet<UPackage*> DirtyBefore;
	{
		TArray<UPackage*> Dirty;
		FEditorFileUtils::GetDirtyContentPackages(Dirty);
		DirtyBefore.Append(Dirty);
	}
	const int32 UndoableBefore = UndoableCount();

	FStepState State;
	for (int32 Index = 0; Index < Operations.Num(); ++Index)
	{
		const FNiagaraMcpBatchOp& Op = Operations[Index];
		FNiagaraMcpBatchStep& Step = Result.Steps.AddDefaulted_GetRef();
		Step.Index = Index;
		Step.Id = Op.Id.TrimStartAndEnd();

		FString Error;
		FToolInfo Tool;
		TSharedPtr<FJsonObject> Arguments;
		const FString ArgumentText = Op.Arguments.TrimStartAndEnd().IsEmpty() ? FString(TEXT("{}")) : Op.Arguments;
		if (!Catalog.Resolve(Op.Tool, Tool, Error))
		{
		}
		else if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ArgumentText), Arguments) || !Arguments.IsValid())
		{
			Error = FString::Printf(TEXT("Arguments is not a JSON object: %s"), *ArgumentText.Left(200));
		}
		Step.Tool = Tool.FullName;

		TSharedPtr<FJsonObject> Conformed;
		if (Error.IsEmpty() && !bDryRun)
		{
			TSharedPtr<FJsonValue> AsValue = MakeShared<FJsonValueObject>(Arguments);
			if (Substitute(AsValue, State, Error))
			{
				Arguments = AsValue->AsObject();
			}
		}
		if (Error.IsEmpty())
		{
			ConformArguments(Tool, Arguments, Conformed, Error);
		}

		if (Error.IsEmpty() && !bDryRun)
		{
			FString Json;
			FJsonSerializer::Serialize(Conformed.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
			TFuture<TValueOrError<FString, FString>> Future = Registry.ExecuteTool(Tool.FullName, Json);
			if (!Future.IsReady())
			{
				Error = TEXT("This tool runs asynchronously and cannot be used inside ExecuteBatch.");
			}
			else
			{
				TValueOrError<FString, FString> Output = Future.Consume();
				if (Output.HasError())
				{
					Error = Output.GetError();
				}
				else
				{
					TSharedPtr<FJsonValue> Parsed;
					if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Output.GetValue()), Parsed) && Parsed.IsValid())
					{
						// Unwrap {"returnValue": ...}.
						const TSharedPtr<FJsonObject>* Object = nullptr;
						if (Parsed->TryGetObject(Object) && (*Object)->Values.Num() == 1 && (*Object)->HasField(TEXT("returnValue")))
						{
							Parsed = (*Object)->TryGetField(TEXT("returnValue"));
						}
					}
					else
					{
						Parsed = MakeShared<FJsonValueString>(Output.GetValue());
					}
					State.Results.Add(FString::FromInt(Index), Parsed);
					if (!Step.Id.IsEmpty())
					{
						State.Results.Add(Step.Id.ToLower(), Parsed);
					}
					Step.Result = ValueToText(Parsed);
					if (Step.Result.Len() > 3000)
					{
						Step.Result = Step.Result.Left(3000) + TEXT(" ...(shortened)");
					}
				}
			}
		}

		Step.bOk = Error.IsEmpty();
		Step.Error = Error;
		Step.ErrorCode = ExtractErrorCode(Error);
		Step.bOk ? ++Result.Succeeded : ++Result.Failed;
		if (!Step.bOk && bStopOnError)
		{
			break;
		}
	}

	Result.bOk = Result.Failed == 0;
	if (bDryRun)
	{
		return Result;
	}

	// Post-conditions, only when every step ran.
	if (Result.bOk && !IsUnset(Expect))
	{
		TArray<FString> Clauses;
		Expect.ParseIntoArray(Clauses, TEXT(";"));
		for (const FString& Clause : Clauses)
		{
			if (!Clause.TrimStartAndEnd().IsEmpty())
			{
				Result.Expectations.Add(Evaluate(Clause.TrimStartAndEnd()));
			}
		}
		if (Result.Expectations.ContainsByPredicate([](const FNiagaraMcpBatchExpectation& Expectation) { return !Expectation.bMet; }))
		{
			Result.bOk = false; // every step ran; an expectation is unmet (reported separately from step failures)
		}
	}

	// A step's error also reaches this tool's own error handler (script exceptions are broadcast), so the registry
	// would answer with the bare step error and drop this result. Append the whole result to the error instead.
	auto ReportFailures = [&Result]()
	{
		if (!Result.bOk)
		{
			const int32 Unmet = Result.Expectations.FilterByPredicate([](const FNiagaraMcpBatchExpectation& Expectation) { return !Expectation.bMet; }).Num();
			FString Json;
			FJsonObjectConverter::UStructToJsonObjectString(Result, Json, 0, 0, 0, nullptr, false);
			ReportError(FString::Printf(TEXT("[%s] ExecuteBatch: %d of %d steps failed, %d expectation(s) unmet%s. Batch result: %s"),
				Result.Failed > 0 ? TEXT("BATCH_STEP_FAILED") : TEXT("EXPECTATION_UNMET"), Result.Failed, Result.Steps.Num(), Unmet,
				Result.bRolledBack ? TEXT(" (rolled back)") : TEXT(""), *Json));
		}
	};

	Result.UndoCount = FMath::Max(0, UndoableCount() - UndoableBefore);
	if (!Result.bOk && bRollbackOnError && Result.UndoCount > 0)
	{
		// Each step is its own transaction (validated before it mutates anything), so undoing the transactions
		// the batch added restores the state from before the batch.
		TArray<UObject*> Touched;
		for (int32 Undo = 0; Undo < Result.UndoCount; ++Undo)
		{
			const int32 TransactionIndex = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount() - 1;
			if (const FTransaction* Transaction = GEditor->Trans->GetTransaction(TransactionIndex))
			{
				TArray<UObject*> Objects;
				Transaction->GetTransactionObjects(Objects);
				Touched.Append(Objects);
			}
			if (!GEditor->UndoTransaction(false))
			{
				break;
			}
		}
		NiagaraMcpTools::RefreshSystemsAfterTransaction(Touched);
		Result.bRolledBack = true;
		Result.UndoCount = 0;
	}
	if (!Result.bOk && bRollbackOnError)
	{
		// Asset creation is not undoable, so delete the never-saved assets the batch created: otherwise their paths
		// stay taken and a corrected re-run fails with "already exists".
		TArray<UPackage*> Dirty;
		FEditorFileUtils::GetDirtyContentPackages(Dirty);
		TArray<UObject*> Created;
		for (UPackage* Package : Dirty)
		{
			if (DirtyBefore.Contains(Package) || FPackageName::DoesPackageExist(Package->GetName()))
			{
				continue;
			}
			if (UObject* Asset = Package->FindAssetInPackage())
			{
				Created.Add(Asset);
				Result.DiscardedAssets.Add(Asset->GetPathName());
			}
		}
		if (Created.Num() > 0)
		{
			ObjectTools::DeleteObjectsUnchecked(Created);
			Result.bRolledBack = true;
		}
		ReportFailures();
		return Result;
	}

	if (bSave && GEditor && GEditor->PlayWorld)
	{
		// Don't turn a successful batch into an error during Play In Editor: report what stays unsaved instead.
		TArray<UPackage*> Dirty;
		FEditorFileUtils::GetDirtyContentPackages(Dirty);
		for (UPackage* Package : Dirty)
		{
			if (!DirtyBefore.Contains(Package))
			{
				Result.UnsavedPackages.Add(Package->GetName());
			}
		}
	}
	else if (bSave)
	{
		// Partial batches (bStopOnError / bRollbackOnError off) still save what they applied; the result says what ran.
		TArray<UPackage*> Dirty;
		FEditorFileUtils::GetDirtyContentPackages(Dirty);
		for (UPackage* Package : Dirty)
		{
			if (DirtyBefore.Contains(Package))
			{
				continue;
			}
			UObject* Asset = Package->FindAssetInPackage();
			if (Asset && SaveAssetSilently(Asset))
			{
				Result.SavedPackages.Add(Package->GetName());
			}
		}
	}
	ReportFailures();
	return Result;
}
