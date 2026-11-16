#pragma once

#include "CoreMinimal.h"

/**
 * MCP resources (resources/list, resources/read) served through Unreal MCP's resource provider API:
 *   niagara-mcp://skill/niagara-vfx-authoring   the authoring guide (same text as the Agent Skill)
 *   niagara-mcp://log/recent                    last 300 log lines
 *   niagara-mcp://changes/recent                change journal (agent + user)
 *   niagara-mcp://system/<object path>/spec     ExportSystemSpec of recently edited systems
 *   niagara-mcp://recipes/<name>                ExecuteBatch recipes shipped in Tools/recipes
 */
namespace NiagaraMcp::Resources
{
	void Register();
	void Unregister();
}
