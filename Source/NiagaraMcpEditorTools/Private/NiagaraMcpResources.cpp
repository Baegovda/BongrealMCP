#include "NiagaraMcpResources.h"

#include "HAL/FileManager.h"
#include "IModelContextProtocolModule.h"
#include "IModelContextProtocolResourceProvider.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ModelContextProtocolResources.h"
#include "NiagaraMcpAgentSkill.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraMcpEditorTools.h"
#include "NiagaraMcpJournal.h"
#include "NiagaraMcpLog.h"
#include "NiagaraSystem.h"
#include "JsonObjectConverter.h"

namespace NiagaraMcp::Resources
{
	namespace
	{
		const TCHAR* Scheme = TEXT("niagara-mcp://");

		FString RecipeDir()
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NiagaraMcpEditorTools"));
			return Plugin.IsValid() ? Plugin->GetBaseDir() / TEXT("Tools/recipes") : FString();
		}

		/** Niagara systems touched by recent journal entries (most recent first, max 10). */
		TArray<FString> RecentSystems()
		{
			TArray<FString> Systems;
			TArray<Journal::FEntry> Entries = Journal::Read(FMath::Max<int64>(0, Journal::GetLatestCursor() - 200), 200);
			for (int32 Index = Entries.Num() - 1; Index >= 0 && Systems.Num() < 10; --Index)
			{
				for (const FString& Package : Entries[Index].Packages)
				{
					const FString ObjectPath = Package + TEXT(".") + FPackageName::GetShortName(Package);
					if (!Systems.Contains(ObjectPath) && FindObject<UNiagaraSystem>(nullptr, *ObjectPath) != nullptr)
					{
						Systems.Add(ObjectPath);
					}
				}
			}
			return Systems;
		}

		class FProvider final : public IModelContextProtocolResourceProvider
		{
		public:
			virtual void ListResources(FModelContextProtocolResourceDescriptorList& Out) const override
			{
				const TSharedRef<const IModelContextProtocolResourceProvider> Self = AsShared();
				Out.Add(FModelContextProtocolResourceDescriptor(FString(Scheme) + FString(TEXT("skill/niagara-vfx-authoring")), FString(TEXT("niagara-vfx-authoring")),
					FString(TEXT("Niagara VFX authoring guide")), FString(TEXT("Workflow, conventions, recipes, error codes and pitfalls for the NiagaraMcpEditorTools toolsets.")), FString(TEXT("text/markdown"))), Self);
				Out.Add(FModelContextProtocolResourceDescriptor(FString(Scheme) + FString(TEXT("log/recent")), FString(TEXT("log-recent")),
					FString(TEXT("Recent output log")), FString(TEXT("Last 300 editor log lines (Niagara, shaders, materials, this plugin).")), FString(TEXT("text/plain"))), Self);
				Out.Add(FModelContextProtocolResourceDescriptor(FString(Scheme) + FString(TEXT("changes/recent")), FString(TEXT("changes-recent")),
					FString(TEXT("Recent changes")), FString(TEXT("Editor transactions, undo/redo and saves, tagged agent or user (same data as GetChangesSince).")), FString(TEXT("application/json"))), Self);
				for (const FString& System : RecentSystems())
				{
					Out.Add(FModelContextProtocolResourceDescriptor(FString(Scheme) + FString(TEXT("system")) + System + FString(TEXT("/spec")), FPackageName::GetShortName(System) + FString(TEXT("-spec")),
						FString::Printf(TEXT("%s spec"), *FPackageName::GetShortName(System)), FString(TEXT("ExportSystemSpec of a recently edited Niagara system.")), FString(TEXT("application/json"))), Self);
				}
				TArray<FString> Recipes;
				IFileManager::Get().FindFiles(Recipes, *(RecipeDir() / TEXT("*.json")), true, false);
				for (const FString& Recipe : Recipes)
				{
					Out.Add(FModelContextProtocolResourceDescriptor(FString(Scheme) + FString(TEXT("recipes/")) + FPaths::GetBaseFilename(Recipe), FPaths::GetBaseFilename(Recipe),
						FString::Printf(TEXT("Recipe: %s"), *FPaths::GetBaseFilename(Recipe)), FString(TEXT("ExecuteBatch operations for a starting effect; replace {{Placeholders}} and run with ExecuteBatch.")), FString(TEXT("application/json"))), Self);
				}
			}

			virtual TValueOrError<FModelContextProtocolResource, FString> ReadResource(const FString& Uri) const override
			{
				if (!Uri.StartsWith(Scheme))
				{
					return MakeError(FString::Printf(TEXT("Unknown resource '%s'."), *Uri));
				}
				const FString Path = Uri.RightChop(FCString::Strlen(Scheme));
				if (Path == TEXT("skill/niagara-vfx-authoring"))
				{
					FString Text = UNiagaraMcpVfxAuthoringSkill::GetGuideText();
					return MakeValue(FModelContextProtocolResource(Uri, MoveTemp(Text), {}, {}, FString(TEXT("text/markdown"))));
				}
				if (Path == TEXT("log/recent"))
				{
					int64 Next = 0;
					TArray<FString> Lines;
					for (const FNiagaraMcpLogLine& Line : FNiagaraMcpLogBuffer::Get().Read(0, {}, ELogVerbosity::Log, FString(), 100000, Next))
					{
						Lines.Add(Line.Text);
					}
					if (Lines.Num() > 300)
					{
						Lines.RemoveAt(0, Lines.Num() - 300);
					}
					return MakeValue(FModelContextProtocolResource(Uri, FString::Join(Lines, TEXT("\n")), {}, {}, FString(TEXT("text/plain"))));
				}
				if (Path == TEXT("changes/recent"))
				{
					const FNiagaraMcpChangeLog Log = UNiagaraMcpEditorTools::GetChangesSince(FMath::Max<int64>(0, Journal::GetLatestCursor() - 200), true, TEXT("Any"), 200);
					FString Json;
					FJsonObjectConverter::UStructToJsonObjectString(Log, Json);
					return MakeValue(FModelContextProtocolResource(Uri, MoveTemp(Json), {}, {}, FString(TEXT("application/json"))));
				}
				if (Path.StartsWith(TEXT("system/")) && Path.EndsWith(TEXT("/spec")))
				{
					const FString ObjectPath = Path.Mid(6, Path.Len() - 6 - 5);
					UNiagaraSystem* System = FindObject<UNiagaraSystem>(nullptr, *ObjectPath);
					if (System == nullptr)
					{
						return MakeError(FString::Printf(TEXT("System '%s' is not loaded."), *ObjectPath));
					}
					FString Json;
					FJsonObjectConverter::UStructToJsonObjectString(UNiagaraMcpEditorTools::ExportSystemSpec(System), Json);
					return MakeValue(FModelContextProtocolResource(Uri, MoveTemp(Json), {}, {}, FString(TEXT("application/json"))));
				}
				if (Path.StartsWith(TEXT("recipes/")))
				{
					FString Text;
					const FString File = RecipeDir() / Path.RightChop(8) + TEXT(".json");
					if (Path.Contains(TEXT("..")) || !FFileHelper::LoadFileToString(Text, *File))
					{
						return MakeError(FString::Printf(TEXT("Recipe '%s' not found."), *Path.RightChop(8)));
					}
					return MakeValue(FModelContextProtocolResource(Uri, MoveTemp(Text), {}, {}, FString(TEXT("application/json"))));
				}
				return MakeError(FString::Printf(TEXT("Unknown resource '%s'."), *Uri));
			}
		};

		TSharedPtr<FProvider> GProvider;
	}

	void Register()
	{
		IModelContextProtocolModule* Module = IModelContextProtocolModule::Get();
		if (Module == nullptr || GProvider.IsValid())
		{
			return;
		}
		GProvider = MakeShared<FProvider>();
		Module->AddResourceProvider(GProvider.ToSharedRef());
	}

	void Unregister()
	{
		if (IModelContextProtocolModule* Module = IModelContextProtocolModule::Get(); Module && GProvider.IsValid())
		{
			Module->RemoveResourceProvider(GProvider.ToSharedRef());
		}
		GProvider.Reset();
	}
}
