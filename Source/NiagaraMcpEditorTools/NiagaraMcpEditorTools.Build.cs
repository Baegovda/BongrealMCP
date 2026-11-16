using UnrealBuildTool;

public class NiagaraMcpEditorTools : ModuleRules
{
	public NiagaraMcpEditorTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"RHI",
			"RenderCore",
			"Slate",
			"SlateCore",
			"InputCore",
			"UnrealEd",
			"EditorFramework",
			"DeveloperSettings",
			"AssetRegistry",
			"AssetTools",
			"Json",
			"JsonUtilities",

			// Engine Toolset Registry (UToolsetDefinition / UToolsetRegistry). Unreal MCP exposes every
			// registered toolset through list_toolsets / describe_toolset / call_tool.
			"ToolsetRegistry",
			"ImageWrapper",
			"ImageCore",
			"Projects",

			// Niagara runtime + editor.
			"Niagara",
			"NiagaraCore",
			"NiagaraShader",
			"NiagaraEditor",

			// Private dependencies of NiagaraEditor whose headers leak through the system / stack view model headers.
			"GraphEditor",
			"PropertyEditor",
			"BlueprintGraph",
			"CurveEditor",
			"MovieScene",
			"SequencerCore",
			"Sequencer",
			"TimeManagement",
			"ToolMenus",
			"VectorVM",

			// UMaterialEditingLibrary for the Material toolset.
			"MaterialEditor",

			// Silent checkout before saving when source control is enabled.
			"SourceControl",

			// Unreal MCP: server start (defers to its own auto-start setting) and the top-level image tools
			// (niagara_capture_preview / material_capture_preview) that return real MCP image content.
			"ModelContextProtocol",
			"ModelContextProtocolEngine",
		});
	}
}
