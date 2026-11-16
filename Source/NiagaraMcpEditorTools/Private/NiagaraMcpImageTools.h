#pragma once

#include "CoreMinimal.h"

/**
 * Top-level MCP tools that return real MCP image content.
 *
 * Unreal MCP wraps every Toolset Registry result as text, so a FToolsetImage returned through call_tool reaches
 * the AI as a long base64 string (expensive and not viewable). These tools proxy the registry's CapturePreview
 * tools and convert their result to an MCP image result, so the client shows the picture to the model:
 *
 *   niagara_capture_preview  -> NiagaraMcpEditorTools.NiagaraMcpEditorTools.CapturePreview
 *   niagara_compare_preview  -> NiagaraMcpEditorTools.NiagaraMcpEditorTools.ComparePreview
 *   niagara_cost_view        -> NiagaraMcpEditorTools.NiagaraMcpEditorTools.CaptureCostView
 *   material_capture_preview -> NiagaraMcpEditorTools.MaterialMcpEditorTools.CapturePreview
 *   texture_view             -> NiagaraMcpEditorTools.AssetMcpEditorTools.ViewTexture
 *   asset_thumbnails         -> NiagaraMcpEditorTools.NiagaraMcpEditorTools.PreviewAssets
 *
 * A registry result that is an FToolsetImage becomes one image content item. A struct result with an "image"
 * field becomes the image followed by a text content item with its "caption" (per-tile numbers and hints).
 *
 * Input schemas are copied from the registry tools, so both stay in sync automatically.
 */
namespace NiagaraMcp::ImageTools
{
	/** Adds the tools to the MCP module (no-op when Unreal MCP is not loaded). Safe to call repeatedly. */
	void Register();

	/** Removes the tools from the MCP module. */
	void Unregister();
}
