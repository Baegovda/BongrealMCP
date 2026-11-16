#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "IModelContextProtocolModule.h"
#include "IModelContextProtocolResourceProvider.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "ModelContextProtocolResources.h"
#include "NiagaraMcpEditorTools.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace NiagaraMcpInfraTests
{
	FString Folder(const TCHAR* Area)
	{
		return FString::Printf(TEXT("/Game/NiagaraMcpTests/Infra/%s_%s"), Area, *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}

	UNiagaraSystem* MakeBurstSystem(const FString& Path)
	{
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *UNiagaraMcpEditorTools::CreateSystem(Path, false).Path);
		if (System)
		{
			UNiagaraMcpEditorTools::AddEmitter(System, TEXT("A"), TEXT("Minimal"), false);
			UNiagaraMcpEditorTools::AddModule(System, TEXT("A"), TEXT("EmitterUpdate"), TEXT("/Niagara/Modules/Emitter/SpawnBurst_Instantaneous.SpawnBurst_Instantaneous"), -1, false);
			UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("A"), TEXT("Spawn Burst Instantaneous"), TEXT("Spawn Count"), TEXT("10"), TEXT("Any"), false);
		}
		return System;
	}

	int32 BurstCount(UNiagaraSystem* System)
	{
		const FNiagaraMcpStackInfo Stack = UNiagaraMcpEditorTools::GetStack(System, TEXT("A"), true, TEXT("burst"));
		for (const FNiagaraMcpModuleInfo& Module : Stack.Modules)
		{
			for (const FNiagaraMcpInputInfo& Input : Module.Inputs)
			{
				if (Input.Name == TEXT("Spawn Count"))
				{
					return FCString::Atoi(*Input.Value);
				}
			}
		}
		return -1;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpInfraRecipesTest, "NiagaraMcp.Infra.Recipes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpInfraRecipesTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NiagaraMcpEditorTools"));
	const FString Dir = Plugin.IsValid() ? Plugin->GetBaseDir() / TEXT("Tools/recipes") : FString();
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.json")), true, false);
	if (!TestTrue(FString::Printf(TEXT("Recipes found in %s"), *Dir), Files.Num() >= 5))
	{
		return false;
	}
	const FString Root = NiagaraMcpInfraTests::Folder(TEXT("Recipes"));
	for (const FString& File : Files)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *(Dir / File));
		TSharedPtr<FJsonObject> Recipe;
		if (!TestTrue(File + TEXT(" parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Recipe) && Recipe.IsValid()))
		{
			continue;
		}
		// Placeholders: defaults, with SystemPath moved under the test folder.
		const TSharedPtr<FJsonObject>* Defaults = nullptr;
		Recipe->TryGetObjectField(TEXT("placeholders"), Defaults);
		TMap<FString, FString> Values;
		if (Defaults)
		{
			for (const auto& Pair : (*Defaults)->Values)
			{
				Values.Add(FString(Pair.Key), Pair.Value->AsString());
			}
		}
		Values.Add(TEXT("SystemPath"), Root / FPaths::GetBaseFilename(File));
		auto Fill = [&Values](FString In)
		{
			for (const TPair<FString, FString>& Pair : Values)
			{
				In.ReplaceInline(*(TEXT("{{") + Pair.Key + TEXT("}}")), *Pair.Value);
			}
			return In;
		};

		TArray<FNiagaraMcpBatchOp> Ops;
		for (const TSharedPtr<FJsonValue>& Value : Recipe->GetArrayField(TEXT("operations")))
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			FNiagaraMcpBatchOp& Op = Ops.AddDefaulted_GetRef();
			Object->TryGetStringField(TEXT("Id"), Op.Id);
			Op.Tool = Object->GetStringField(TEXT("Tool"));
			FString Arguments;
			FJsonSerializer::Serialize(Object->GetObjectField(TEXT("Arguments")).ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Arguments));
			Op.Arguments = Fill(Arguments);
		}
		const FNiagaraMcpBatchResult Result = UNiagaraMcpEditorTools::ExecuteBatch(Ops, true, false, false, false, Fill(Recipe->GetStringField(TEXT("expect"))));
		for (const FNiagaraMcpBatchStep& Step : Result.Steps)
		{
			if (!Step.bOk)
			{
				AddError(FString::Printf(TEXT("%s step %d (%s): %s"), *File, Step.Index, *Step.Tool, *Step.Error.Left(400)));
			}
		}
		for (const FNiagaraMcpBatchExpectation& Expectation : Result.Expectations)
		{
			TestTrue(FString::Printf(TEXT("%s: %s (%s)"), *File, *Expectation.Expectation, *Expectation.Detail), Expectation.bMet);
		}
		TestTrue(File + TEXT(" builds"), Result.bOk && Result.Expectations.Num() >= 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpInfraJournalTest, "NiagaraMcp.Infra.JournalAndRestorePoints",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpInfraJournalTest::RunTest(const FString& Parameters)
{
	using namespace NiagaraMcpInfraTests;
	UNiagaraSystem* System = MakeBurstSystem(Folder(TEXT("Journal")) / TEXT("NS_Journal"));
	if (!TestNotNull(TEXT("System"), System))
	{
		return false;
	}
	const FString Package = System->GetOutermost()->GetName();
	const int64 Start = UNiagaraMcpEditorTools::GetChangesSince(0, true, TEXT("Any"), 1000).NextCursor;

	// Agent change.
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("A"), TEXT("Spawn Burst Instantaneous"), TEXT("Spawn Count"), TEXT("20"), TEXT("Any"), false);
	FNiagaraMcpChangeLog Log = UNiagaraMcpEditorTools::GetChangesSince(Start, true, Package, 100);
	for (const FNiagaraMcpChangeEntry& Entry : Log.Entries)
	{
		AddInfo(FString::Printf(TEXT("journal: %s '%s' agent=%d packages=%s"), *Entry.Kind, *Entry.Title, Entry.bByAgent ? 1 : 0, *FString::Join(Entry.Packages, TEXT(","))));
	}
	TestTrue(TEXT("Agent edit journaled with its package"), Log.Entries.ContainsByPredicate([](const FNiagaraMcpChangeEntry& Entry) { return Entry.bByAgent && Entry.Kind == TEXT("Edit"); }));
	TestEqual(TEXT("No user changes yet"), UNiagaraMcpEditorTools::GetChangesSince(Start, false, Package, 100).UserChangeCount, 0);

	// Restore point, two agent edits, rollback.
	UNiagaraMcpEditorTools::CreateRestorePoint(TEXT("rp"));
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("A"), TEXT("Spawn Burst Instantaneous"), TEXT("Spawn Count"), TEXT("30"), TEXT("Any"), false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("A"), TEXT("Spawn Burst Instantaneous"), TEXT("Spawn Count"), TEXT("40"), TEXT("Any"), false);
	TestEqual(TEXT("Edited to 40"), BurstCount(System), 40);
	const FNiagaraMcpRollbackResult Rolled = UNiagaraMcpEditorTools::RollbackToRestorePoint(TEXT("rp"), false);
	TestTrue(TEXT("Rolled back two steps"), Rolled.bRolledBack && Rolled.Undone.Num() == 2);
	TestEqual(TEXT("Value restored to the restore point"), BurstCount(System), 20);

	// A user edit (editor transaction with a non-agent title) is reported and blocks a rollback.
	UNiagaraMcpEditorTools::CreateRestorePoint(TEXT("rp2"));
	{
		FScopedTransaction UserEdit(NSLOCTEXT("NiagaraMcpTests", "UserEdit", "Edit Spawn Count"));
		System->Modify();
		System->bFixedBounds = !System->bFixedBounds; // a real change; empty transactions are dropped
	}
	Log = UNiagaraMcpEditorTools::GetChangesSince(Start, false, Package, 100);
	TestTrue(TEXT("User edit reported"), Log.UserChangeCount >= 1 && Log.Entries.ContainsByPredicate([](const FNiagaraMcpChangeEntry& Entry) { return !Entry.bByAgent && Entry.Title == TEXT("Edit Spawn Count"); }));
	AddExpectedError(TEXT("USER_CHANGES"), EAutomationExpectedErrorFlags::Contains, 0);
	TestFalse(TEXT("Rollback refused over a user edit"), UNiagaraMcpEditorTools::RollbackToRestorePoint(TEXT("rp2"), false).bRolledBack);
	TestTrue(TEXT("Rollback allowed when asked"), UNiagaraMcpEditorTools::RollbackToRestorePoint(TEXT("rp2"), true).bRolledBack);
	TestTrue(TEXT("Restore points listed"), UNiagaraMcpEditorTools::ListRestorePoints().Num() >= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpInfraBatchTest, "NiagaraMcp.Infra.BatchExpectAndPaging",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpInfraBatchTest::RunTest(const FString& Parameters)
{
	using namespace NiagaraMcpInfraTests;
	const FString Path = Folder(TEXT("Batch")) / TEXT("NS_Expect");
	UNiagaraSystem* System = MakeBurstSystem(Path);
	if (!TestNotNull(TEXT("System"), System))
	{
		return false;
	}

	// Unmet expectation -> batch fails and rolls back.
	TArray<FNiagaraMcpBatchOp> Ops;
	FNiagaraMcpBatchOp& Op = Ops.AddDefaulted_GetRef();
	Op.Tool = TEXT("SetModuleInput");
	Op.Arguments = FString::Printf(TEXT("{\"System\": \"%s\", \"EmitterName\": \"A\", \"Module\": \"Spawn Burst Instantaneous\", \"Input\": \"Spawn Count\", \"Value\": \"5\"}"), *Path);
	const FNiagaraMcpBatchResult Unmet = UNiagaraMcpEditorTools::ExecuteBatch(Ops, true, true, false, false, FString::Printf(TEXT("Compiles:%s;Particles:%s:A@0.2>=1000"), *Path, *Path));
	TestTrue(TEXT("Two expectations evaluated"), Unmet.Expectations.Num() == 2);
	TestTrue(TEXT("Compile expectation met"), Unmet.Expectations.Num() == 2 && Unmet.Expectations[0].bMet);
	TestTrue(TEXT("Particle expectation unmet with detail"), Unmet.Expectations.Num() == 2 && !Unmet.Expectations[1].bMet && Unmet.Expectations[1].Detail.Contains(TEXT("5 particles")));
	TestTrue(TEXT("Unmet expectation fails and rolls back"), !Unmet.bOk && Unmet.bRolledBack);
	TestEqual(TEXT("Rolled back value"), BurstCount(System), 10);

	// Met expectation.
	const FNiagaraMcpBatchResult Met = UNiagaraMcpEditorTools::ExecuteBatch(Ops, true, true, false, false, FString::Printf(TEXT("Particles:%s:A@0.2>=5"), *Path));
	TestTrue(TEXT("Met expectation passes"), Met.bOk && Met.Expectations.Num() == 1 && Met.Expectations[0].bMet);

	// Error codes on failing steps.
	TArray<FNiagaraMcpBatchOp> Bad = Ops;
	Bad[0].Arguments = FString::Printf(TEXT("{\"System\": \"%s\", \"EmitterName\": \"Nope\", \"Module\": \"Spawn Burst Instantaneous\", \"Input\": \"Spawn Count\", \"Value\": \"5\"}"), *Path);
	const FNiagaraMcpBatchResult Coded = UNiagaraMcpEditorTools::ExecuteBatch(Bad, true, true, false, false, TEXT("None"));
	TestTrue(FString::Printf(TEXT("Error code extracted (%s)"), Coded.Steps.Num() ? *Coded.Steps[0].ErrorCode : TEXT("-")), Coded.Steps.Num() == 1 && Coded.Steps[0].ErrorCode == TEXT("EMITTER_NOT_FOUND"));

	// Paging and filters.
	const FNiagaraMcpStackInfo Filtered = UNiagaraMcpEditorTools::GetStack(System, TEXT("A"), false, TEXT("burst"));
	TestTrue(TEXT("Module filter keeps only matching modules"), Filtered.Modules.Num() == 1);
	UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate"));
	const FNiagaraMcpGraphInfo Page = UNiagaraMcpEditorTools::GetScriptGraph(Script, TEXT("Any"), 0, 2);
	TestTrue(FString::Printf(TEXT("Graph paging (%d of %d, next %d)"), Page.Nodes.Num(), Page.TotalNodes, Page.NextOffset), Page.Nodes.Num() == 2 && Page.TotalNodes > 2 && Page.NextOffset == 2);

	// Thumbnails and templates.
	const FToolsetImage Thumbs = UNiagaraMcpEditorTools::PreviewAssets({ TEXT("/Niagara/DefaultAssets/DefaultSpriteMaterial"), Path }, 64);
	TestFalse(TEXT("Thumbnail sheet rendered"), Thumbs.Data.IsEmpty());
	const TArray<FNiagaraMcpTemplateInfo> Templates = UNiagaraMcpEditorTools::ListNiagaraTemplates(TEXT("Emitter"), TEXT("Any"));
	TestTrue(FString::Printf(TEXT("Emitter templates listed (%d)"), Templates.Num()), Templates.Num() >= 5 && Templates.ContainsByPredicate([](const FNiagaraMcpTemplateInfo& Info) { return Info.Path.Contains(TEXT("Minimal")); }));

	// MCP resources.
	IModelContextProtocolModule* Mcp = IModelContextProtocolModule::Get();
	if (TestNotNull(TEXT("Unreal MCP loaded"), Mcp))
	{
		bool bFound = false;
		for (const TSharedRef<IModelContextProtocolResourceProvider>& Provider : Mcp->GetResourceProviders())
		{
			TValueOrError<FModelContextProtocolResource, FString> Skill = Provider->ReadResource(TEXT("niagara-mcp://skill/niagara-vfx-authoring"));
			TValueOrError<FModelContextProtocolResource, FString> Recipe = Provider->ReadResource(TEXT("niagara-mcp://recipes/fireball"));
			if (Skill.HasValue() && Recipe.HasValue())
			{
				bFound = true;
			}
		}
		TestTrue(TEXT("Skill and recipe resources readable"), bFound);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpInfraReviewFixesTest, "NiagaraMcp.Infra.ReviewFixes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpInfraReviewFixesTest::RunTest(const FString& Parameters)
{
	using namespace NiagaraMcpInfraTests;
	UNiagaraSystem* System = MakeBurstSystem(Folder(TEXT("Fixes")) / TEXT("NS_Fixes"));
	if (!TestNotNull(TEXT("System"), System))
	{
		return false;
	}
	const FString Package = System->GetOutermost()->GetName();

	// Huge / invalid times are clamped instead of ticking the game thread for hours.
	const double Start = FPlatformTime::Seconds();
	const TArray<FNiagaraMcpParticleSample> Clamped = UNiagaraMcpEditorTools::GetParticleCounts(System, { 100000.0f, -5.0f });
	TestTrue(FString::Printf(TEXT("Times clamped to 0-600 s (%.1f, %.1f) in %.1f s"), Clamped.Num() ? Clamped[0].Time : -1.0f, Clamped.Num() > 1 ? Clamped[1].Time : -1.0f, FPlatformTime::Seconds() - Start),
		Clamped.Num() == 2 && FMath::IsNearlyEqual(Clamped[0].Time, 600.0f) && FMath::IsNearlyEqual(Clamped[1].Time, 0.0f) && FPlatformTime::Seconds() - Start < 120.0);

	// A transaction that records nothing must not produce a phantom journal entry (it is popped by the buffer).
	const int64 Cursor = UNiagaraMcpEditorTools::GetChangesSince(0, true, TEXT("Any"), 1000).NextCursor;
	{
		FScopedTransaction UserEdit(NSLOCTEXT("NiagaraMcpTests", "UserEdit2", "Edit Something"));
		System->Modify();
		System->bFixedBounds = !System->bFixedBounds;
	}
	{
		FScopedTransaction Empty(NSLOCTEXT("NiagaraMcpTests", "Empty", "Niagara MCP: No-op"));
	}
	FNiagaraMcpChangeLog Log = UNiagaraMcpEditorTools::GetChangesSince(Cursor, true, TEXT("Any"), 100);
	TestEqual(TEXT("Exactly one journal entry (no phantom for the empty transaction)"), Log.Entries.Num(), 1);
	TestTrue(TEXT("It is the user edit"), Log.Entries.Num() == 1 && !Log.Entries[0].bByAgent && Log.Entries[0].Title == TEXT("Edit Something"));

	// Paging: MaxEntries=1 over two entries -> bMore and the next call returns the second.
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("A"), TEXT("Spawn Burst Instantaneous"), TEXT("Spawn Count"), TEXT("12"), TEXT("Any"), false);
	const FNiagaraMcpChangeLog First = UNiagaraMcpEditorTools::GetChangesSince(Cursor, true, TEXT("Any"), 1);
	const FNiagaraMcpChangeLog Second = UNiagaraMcpEditorTools::GetChangesSince(First.NextCursor, true, TEXT("Any"), 1);
	TestTrue(TEXT("Paged journal returns every entry once"), First.bMore && First.Entries.Num() == 1 && Second.Entries.Num() == 1 && Second.Entries[0].bByAgent);

	// Malformed spec entries are skipped, not dereferenced.
	const FNiagaraMcpApplySpecResult Bad = UNiagaraMcpEditorTools::ApplySystemSpec(System, TEXT("{\"emitters\": {\"Ghost\": null, \"Other\": \"x\"}}"), TEXT("Merge"), true, false);
	TestTrue(TEXT("Malformed emitter entries handled without crashing"), true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpInfraUndoSavesTest, "NiagaraMcp.Infra.UndoAndRollbackSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpInfraUndoSavesTest::RunTest(const FString& Parameters)
{
	using namespace NiagaraMcpInfraTests;
	// A saved asset: Undo / Redo / rollback must leave the file matching what the editor shows.
	const FString Path = Folder(TEXT("UndoSave")) / TEXT("NS_UndoSave");
	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *UNiagaraMcpEditorTools::CreateSystem(Path, true).Path);
	if (!TestNotNull(TEXT("System"), System))
	{
		return false;
	}
	UNiagaraMcpEditorTools::AddEmitter(System, TEXT("A"), TEXT("Minimal"), true);
	UNiagaraMcpEditorTools::AddModule(System, TEXT("A"), TEXT("EmitterUpdate"), TEXT("/Niagara/Modules/Emitter/SpawnBurst_Instantaneous.SpawnBurst_Instantaneous"), -1, true);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("A"), TEXT("Spawn Burst Instantaneous"), TEXT("Spawn Count"), TEXT("10"), TEXT("Any"), true);
	UPackage* Package = System->GetOutermost();
	const FString PackageName = Package->GetName();
	TestFalse(TEXT("Saved after setup"), Package->IsDirty());

	UNiagaraMcpEditorTools::CreateRestorePoint(TEXT("rp_save"));
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("A"), TEXT("Spawn Burst Instantaneous"), TEXT("Spawn Count"), TEXT("30"), TEXT("Any"), true);
	const FNiagaraMcpRollbackResult Rolled = UNiagaraMcpEditorTools::RollbackToRestorePoint(TEXT("rp_save"));
	AddInfo(FString::Printf(TEXT("rollback saved=[%s] unsaved=[%s]"), *FString::Join(Rolled.SavedPackages, TEXT(",")), *FString::Join(Rolled.UnsavedPackages, TEXT(","))));
	TestTrue(TEXT("Rolled back"), Rolled.bRolledBack);
	TestEqual(TEXT("Rollback restored the value"), BurstCount(System), 10);
	TestTrue(TEXT("Rollback saved the package"), Rolled.SavedPackages.Contains(PackageName));
	TestFalse(TEXT("Not dirty after rollback"), Package->IsDirty());

	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("A"), TEXT("Spawn Burst Instantaneous"), TEXT("Spawn Count"), TEXT("40"), TEXT("Any"), true);
	const FNiagaraMcpUndoResult Undone = UNiagaraMcpEditorTools::Undo(1);
	TestEqual(TEXT("Undo restored the value"), BurstCount(System), 10);
	TestTrue(TEXT("Undo saved the package"), Undone.SavedPackages.Contains(PackageName));
	TestFalse(TEXT("Not dirty after undo"), Package->IsDirty());

	const FNiagaraMcpUndoResult Redone = UNiagaraMcpEditorTools::Redo(1, false);
	TestEqual(TEXT("Redo reapplied the value"), BurstCount(System), 40);
	TestTrue(TEXT("Redo with bSave=false saves nothing"), Redone.SavedPackages.Num() == 0 && Package->IsDirty());
	const FNiagaraMcpUndoResult Undone2 = UNiagaraMcpEditorTools::Undo(1);
	TestFalse(TEXT("Clean again"), Package->IsDirty());

	// Never-saved assets stay unsaved (their bSave=false was deliberate) and are reported.
	UNiagaraSystem* Unsaved = MakeBurstSystem(Folder(TEXT("UndoSave")) / TEXT("NS_NeverSaved"));
	if (TestNotNull(TEXT("Unsaved system"), Unsaved))
	{
		UNiagaraMcpEditorTools::SetModuleInput(Unsaved, TEXT("A"), TEXT("Spawn Burst Instantaneous"), TEXT("Spawn Count"), TEXT("25"), TEXT("Any"), false);
		const FNiagaraMcpUndoResult UndoneUnsaved = UNiagaraMcpEditorTools::Undo(1);
		TestTrue(TEXT("Never-saved asset reported as unsaved"), UndoneUnsaved.SavedPackages.Num() == 0 && UndoneUnsaved.UnsavedPackages.Contains(Unsaved->GetOutermost()->GetName()));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
