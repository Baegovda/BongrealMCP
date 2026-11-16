#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "NiagaraMcpEditorToolsSettings.generated.h"

/**
 * Editor Preferences > Plugins > Niagara MCP Editor Tools.
 * Stored per project and user (Config/DefaultEditorPerProjectUserSettings.ini for the project default):
 *
 *   [/Script/NiagaraMcpEditorTools.NiagaraMcpEditorToolsSettings]
 *   bAutoStartMcpServer=True
 */
UCLASS(config = EditorPerProjectUserSettings, defaultconfig, meta = (DisplayName = "Niagara MCP Editor Tools"))
class UNiagaraMcpEditorToolsSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UNiagaraMcpEditorToolsSettings();

	/** Start the Unreal MCP server when the editor opens this project. Skipped when Unreal MCP's own Auto Start Server is on or a server is already running (starting twice would drop connected sessions). */
	UPROPERTY(config, EditAnywhere, Category = "MCP Server")
	bool bAutoStartMcpServer = false;

	/** Port passed to ModelContextProtocol.StartServer. Clients connect to http://127.0.0.1:<Port>/mcp. */
	UPROPERTY(config, EditAnywhere, Category = "MCP Server", meta = (ClampMin = "1", ClampMax = "65535", EditCondition = "bAutoStartMcpServer"))
	int32 McpServerPort = 8000;

	/** Register the Niagara toolset (NiagaraMcpEditorTools.NiagaraMcpEditorTools). Restart the editor after changing. */
	UPROPERTY(config, EditAnywhere, Category = "Toolsets")
	bool bRegisterNiagaraToolset = true;

	/** Register the Material toolset (NiagaraMcpEditorTools.MaterialMcpEditorTools). Restart the editor after changing. */
	UPROPERTY(config, EditAnywhere, Category = "Toolsets")
	bool bRegisterMaterialToolset = true;

	/** Register the Asset toolset for VFX support assets (NiagaraMcpEditorTools.AssetMcpEditorTools). Restart the editor after changing. */
	UPROPERTY(config, EditAnywhere, Category = "Toolsets")
	bool bRegisterAssetToolset = true;

	virtual FName GetContainerName() const override { return TEXT("Editor"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
};
