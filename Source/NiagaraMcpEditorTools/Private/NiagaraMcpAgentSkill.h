#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/AgentSkill.h"
#include "NiagaraMcpAgentSkill.generated.h"

/**
 * Agent Skill served by the engine's AgentSkillToolset (ListSkills / GetSkills): how to build and verify Niagara
 * effects and materials with this plugin's toolsets.
 */
UCLASS()
class UNiagaraMcpVfxAuthoringSkill : public UAgentSkill
{
	GENERATED_BODY()

public:
	UNiagaraMcpVfxAuthoringSkill();

	/** The guide text (also served as the MCP resource niagara-mcp://skill/niagara-vfx-authoring). */
	static FString GetGuideText() { return GetDefault<UNiagaraMcpVfxAuthoringSkill>()->Instructions; }
};
