#include "Containers/Ticker.h"
#include "Editor.h"
#include "IModelContextProtocolModule.h"
#include "ModelContextProtocolServer.h"
#include "ModelContextProtocolSettings.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"

#include "ToolsetRegistry/ToolsetRegistrySubsystem.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

#include "AssetMcpEditorTools.h"
#include "MaterialMcpEditorTools.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraMcpEditorTools.h"
#include "NiagaraMcpEditorToolsSettings.h"
#include "NiagaraMcpImageTools.h"
#include "NiagaraMcpJournal.h"
#include "NiagaraMcpResources.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NiagaraMcpLog.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(NiagaraMcpEditorToolsSettings)

UNiagaraMcpEditorToolsSettings::UNiagaraMcpEditorToolsSettings()
{
	SectionName = TEXT("Niagara MCP Editor Tools");
}

/**
 * Registers the toolsets with the engine Toolset Registry. Unreal MCP (ModelContextProtocol) then exposes them as
 * NiagaraMcpEditorTools.NiagaraMcpEditorTools and NiagaraMcpEditorTools.MaterialMcpEditorTools through
 * list_toolsets / describe_toolset / call_tool.
 *
 * The registry subsystem may not exist yet at PostEngineInit, so registration is retried once every module has
 * loaded, and undone before exit.
 */
class FNiagaraMcpEditorToolsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (GLog)
		{
			GLog->AddOutputDevice(&FNiagaraMcpLogBuffer::Get());
		}
		FCoreDelegates::OnAllModuleLoadingPhasesComplete.AddRaw(this, &FNiagaraMcpEditorToolsModule::OnAllModulesLoaded);
		FCoreDelegates::OnPreExit.AddRaw(this, &FNiagaraMcpEditorToolsModule::OnPreExit);
		RegisterToolsets();
	}

	virtual void ShutdownModule() override
	{
		if (GLog)
		{
			GLog->RemoveOutputDevice(&FNiagaraMcpLogBuffer::Get());
		}
		FCoreDelegates::OnAllModuleLoadingPhasesComplete.RemoveAll(this);
		FCoreDelegates::OnPreExit.RemoveAll(this);
		if (AutoStartHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(AutoStartHandle);
			AutoStartHandle.Reset();
		}
		if (HealthHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(HealthHandle);
			HealthHandle.Reset();
		}
		NiagaraMcp::Journal::Shutdown();
		NiagaraMcp::Resources::Unregister();
		NiagaraMcp::ImageTools::Unregister();
		UnregisterToolsets();
	}

private:
	void OnAllModulesLoaded()
	{
		RegisterToolsets();
		NiagaraMcp::ImageTools::Register();
		NiagaraMcp::Resources::Register();
		NiagaraMcp::Journal::Startup();
		if (!IsRunningCommandlet())
		{
			// Health file for relays / launchers: <Project>/Saved/NiagaraMcp/health.json, refreshed every 2 s by the game
			// thread (a stale timestamp means the editor is busy or hung).
			HealthHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FNiagaraMcpEditorToolsModule::WriteHealth), 2.0f);
		}

		const UNiagaraMcpEditorToolsSettings* Settings = GetDefault<UNiagaraMcpEditorToolsSettings>();
		if (Settings->bAutoStartMcpServer && !IsRunningCommandlet())
		{
			// One tick later, so the MCP module has finished its own startup and refreshed its tool list.
			AutoStartHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FNiagaraMcpEditorToolsModule::StartMcpServer), 0.5f);
		}
	}

	bool StartMcpServer(float)
	{
		AutoStartHandle.Reset();
		IModelContextProtocolModule* Mcp = IModelContextProtocolModule::Get();
		if (Mcp == nullptr)
		{
			UE_LOG(LogNiagaraMcp, Warning, TEXT("Unreal MCP is not enabled. Enable the Unreal MCP plugin to expose the Niagara MCP toolsets."));
			return false;
		}
		// Unreal MCP has its own auto-start (Editor Preferences > Unreal MCP > Auto Start Server, or
		// -ModelContextProtocolStartServer). Starting again would stop that server and drop its sessions,
		// so leave it alone whenever it is on or a server is already running.
		if (UE::ModelContextProtocol::ShouldAutoStartServer())
		{
			UE_LOG(LogNiagaraMcp, Log, TEXT("Unreal MCP starts its own server (its auto-start is on); not starting it again."));
			return false;
		}
		if (const FModelContextProtocolServer* Server = Mcp->GetServer(); Server && Server->IsServerRunning())
		{
			UE_LOG(LogNiagaraMcp, Log, TEXT("The Unreal MCP server is already running; not restarting it."));
			return false;
		}
		const int32 Port = GetDefault<UNiagaraMcpEditorToolsSettings>()->McpServerPort;
		UE_LOG(LogNiagaraMcp, Log, TEXT("Starting the Unreal MCP server on http://127.0.0.1:%d%s"), Port, UE::ModelContextProtocol::DefaultServerUrlPath);
		Mcp->StartServer(static_cast<uint32>(Port));
		return false; // one shot
	}

	void RegisterToolsets()
	{
		if (bToolsetsRegistered || UToolsetRegistrySubsystem::Get().HasError())
		{
			return;
		}
		bToolsetsRegistered = true;

		const UNiagaraMcpEditorToolsSettings* Settings = GetDefault<UNiagaraMcpEditorToolsSettings>();
		bNiagaraRegistered = Settings->bRegisterNiagaraToolset;
		bMaterialRegistered = Settings->bRegisterMaterialToolset;
		bAssetRegistered = Settings->bRegisterAssetToolset;
		if (bNiagaraRegistered)
		{
			UToolsetRegistry::RegisterToolsetClass(UNiagaraMcpEditorTools::StaticClass());
		}
		if (bMaterialRegistered)
		{
			UToolsetRegistry::RegisterToolsetClass(UMaterialMcpEditorTools::StaticClass());
		}
		if (bAssetRegistered)
		{
			UToolsetRegistry::RegisterToolsetClass(UAssetMcpEditorTools::StaticClass());
		}
		UE_LOG(LogNiagaraMcp, Log, TEXT("Niagara MCP Editor Tools registered (Niagara toolset: %s, Material toolset: %s, Asset toolset: %s)."),
			bNiagaraRegistered ? TEXT("on") : TEXT("off"), bMaterialRegistered ? TEXT("on") : TEXT("off"), bAssetRegistered ? TEXT("on") : TEXT("off"));
	}

	bool WriteHealth(float)
	{
		const IModelContextProtocolModule* Mcp = IModelContextProtocolModule::Get();
		const FModelContextProtocolServer* Server = Mcp ? const_cast<IModelContextProtocolModule*>(Mcp)->GetServer() : nullptr;
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NiagaraMcpEditorTools"));
		const FString Json = FString::Printf(TEXT("{\"pid\": %u, \"utc\": \"%s\", \"mcpServerRunning\": %s, \"pluginVersion\": \"%s\", \"playInEditor\": %s}"),
			FPlatformProcess::GetCurrentProcessId(), *FDateTime::UtcNow().ToIso8601(), Server && Server->IsServerRunning() ? TEXT("true") : TEXT("false"),
			Plugin.IsValid() ? *Plugin->GetDescriptor().VersionName : TEXT(""), GEditor && GEditor->PlayWorld ? TEXT("true") : TEXT("false"));
		FFileHelper::SaveStringToFile(Json, *(FPaths::ProjectSavedDir() / TEXT("NiagaraMcp/health.json")));
		return true;
	}

	void OnPreExit()
	{
		if (HealthHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(HealthHandle);
			HealthHandle.Reset();
		}
		NiagaraMcp::Journal::Shutdown();
		NiagaraMcp::Resources::Unregister();
		NiagaraMcp::ImageTools::Unregister();
		UnregisterToolsets();
	}

	void UnregisterToolsets()
	{
		if (!bToolsetsRegistered)
		{
			return;
		}
		bToolsetsRegistered = false;
		if (bNiagaraRegistered)
		{
			UToolsetRegistry::UnregisterToolsetClass(UNiagaraMcpEditorTools::StaticClass());
		}
		if (bMaterialRegistered)
		{
			UToolsetRegistry::UnregisterToolsetClass(UMaterialMcpEditorTools::StaticClass());
		}
		if (bAssetRegistered)
		{
			UToolsetRegistry::UnregisterToolsetClass(UAssetMcpEditorTools::StaticClass());
		}
	}

	bool bToolsetsRegistered = false;
	bool bNiagaraRegistered = false;
	bool bMaterialRegistered = false;
	bool bAssetRegistered = false;
	FTSTicker::FDelegateHandle AutoStartHandle;
	FTSTicker::FDelegateHandle HealthHandle;
};

IMPLEMENT_MODULE(FNiagaraMcpEditorToolsModule, NiagaraMcpEditorTools)
