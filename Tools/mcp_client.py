"""
Minimal MCP client (Streamable HTTP, standard library only) for the Unreal MCP server.

The Unreal MCP plugin (ModelContextProtocol) serves http://127.0.0.1:8000/mcp and exposes every toolset of the
engine Toolset Registry through gateway tools (list_toolsets, describe_toolset(s), call_tool). This module wraps
that so scripts can drive the Niagara MCP Editor Tools directly:

    from mcp_client import McpClient
    mcp = McpClient()
    print(mcp.niagara("GetSystemSummary", System="/Game/TestSample/NS_ChainStrike"))

Argument names are matched against the tool schema loosely (AssetPath, asset_path and assetPath all work).

CLI:
    python mcp_client.py toolsets
    python mcp_client.py describe NiagaraMcpEditorTools.NiagaraMcpEditorTools
    python mcp_client.py call NiagaraMcpEditorTools.NiagaraMcpEditorTools GetSystemSummary "{\"System\": \"/Game/FX/NS_A\"}"

Set UNREAL_MCP_URL to use another port, e.g. http://127.0.0.1:8001/mcp.
"""

from __future__ import annotations

import itertools
import json
import os
import sys
import urllib.error
import urllib.request
from typing import Any

MCP_URL = os.environ.get("UNREAL_MCP_URL", "http://127.0.0.1:8000/mcp")
NIAGARA_TOOLSET = "NiagaraMcpEditorTools.NiagaraMcpEditorTools"
MATERIAL_TOOLSET = "NiagaraMcpEditorTools.MaterialMcpEditorTools"
ASSET_TOOLSET = "NiagaraMcpEditorTools.AssetMcpEditorTools"
PROTOCOL_VERSION = "2025-06-18"


class McpError(RuntimeError):
    """A transport, protocol or tool error."""


def _normalize(name: str) -> str:
    return name.replace("_", "").replace("-", "").lower()


# Tool parameters typed as UObject* (UNiagaraSystem*, UMaterial*, UNiagaraScript*) must be full object paths.
OBJECT_ARGUMENTS = {"system", "material", "script", "parent", "instance", "asset", "texture", "rendertarget", "collection", "source", "systema", "systemb", "frames"}


def _image_part(image: Any) -> dict:
    """Finds the {"data", "mimeType"} part of an image result.

    Accepts a plain image ({"data": base64, "mimeType": ...} / {"Data": ..., "MimeType": ...}), a list of them (first
    wins), a result with a nested image ({"image": {...}, "caption": ..., "frames": [...]}, e.g. the registry
    CapturePreview / ComparePreview / CaptureCostView results) and the {"returnValue": ...} wrapper."""
    if isinstance(image, list):
        return _image_part(image[0]) if image else {}
    if not isinstance(image, dict):
        return {}
    image = _strip_wrapper(image)
    if isinstance(image, dict) and not (image.get("data") or image.get("Data")):
        for key in ("image", "Image"):
            if isinstance(image.get(key), dict):
                return _image_part(image[key])
    return image if isinstance(image, dict) else {}


def save_image(image: Any, path: str) -> str:
    """Writes an image result to disk: a plain image ({"data": base64, "mimeType": ...}), an image tool result with a
    caption ({"data", "mimeType", "caption"}) or a registry result with a nested image ({"image": {...}, ...})."""
    import base64
    part = _image_part(image)
    data = part.get("data") or part.get("Data") or ""
    if not data:
        raise McpError("save_image: the result contains no image data")
    with open(path, "wb") as handle:
        handle.write(base64.b64decode(data))
    return path


def image_caption(image: Any) -> str:
    """The text caption of an image result (top-level image tools add it as text content; registry results carry it
    as "caption"); empty when there is none."""
    if isinstance(image, dict):
        image = _strip_wrapper(image)
        if isinstance(image, dict):
            return str(image.get("caption") or image.get("Caption") or "")
    return ""


def object_path(path: str) -> str:
    """"/Game/FX/NS_A" -> "/Game/FX/NS_A.NS_A" (object paths are required for asset parameters)."""
    path = path.strip()
    if path.startswith("/") and "." not in path.rsplit("/", 1)[-1]:
        return f"{path}.{path.rsplit('/', 1)[-1]}"
    return path


def _strip_wrapper(data: Any) -> Any:
    # Toolset Registry returns {"returnValue": <value>}; Unreal MCP may wrap plain values as {"result": value}.
    while isinstance(data, dict) and len(data) == 1 and next(iter(data)) in ("returnValue", "result"):
        data = next(iter(data.values()))
    return data


def _unwrap_result(result: dict | None, tool: str) -> Any:
    """Turns an MCP tools/call result into Python data (structured content first, then JSON text)."""
    if result is None:
        return None
    texts = [c.get("text", "") for c in result.get("content", []) if c.get("type") == "text"]
    if result.get("isError"):
        raise McpError(f"{tool}: {' '.join(texts).strip() or 'tool error'}")
    images = [c for c in result.get("content", []) if c.get("type") == "image"]
    if images:
        # Image tools (niagara_capture_preview, texture_view, ...) return MCP image content {"mimeType", "data"},
        # optionally followed by a text caption (per-tile numbers); the caption is returned as "caption".
        caption = "\n".join(t for t in texts if t).strip()
        if len(images) == 1:
            single = dict(images[0])
            if caption:
                single["caption"] = caption
            return single
        return images
    if "structuredContent" in result and result["structuredContent"] is not None:
        return _strip_wrapper(result["structuredContent"])
    joined = "\n".join(texts).strip()
    if not joined:
        return None
    try:
        return _strip_wrapper(json.loads(joined))
    except json.JSONDecodeError:
        return joined


def _find_tool_entries(node: Any, out: list[dict]) -> None:
    """Collects every dict that looks like a tool description (has a name and some schema)."""
    if isinstance(node, dict):
        if "name" in node and any(k in node for k in ("inputSchema", "input_schema", "parameters", "inputs", "input")):
            out.append(node)
        for value in node.values():
            _find_tool_entries(value, out)
    elif isinstance(node, list):
        for value in node:
            _find_tool_entries(value, out)


def _schema_properties(entry: dict) -> list[str]:
    for key in ("inputSchema", "input_schema", "parameters", "input"):
        schema = entry.get(key)
        if isinstance(schema, dict):
            props = schema.get("properties", schema)
            if isinstance(props, dict):
                return list(props.keys())
    inputs = entry.get("inputs")
    if isinstance(inputs, list):
        return [i.get("name") for i in inputs if isinstance(i, dict) and i.get("name")]
    return []


class McpClient:
    def __init__(self, url: str = MCP_URL, timeout: float = 900.0):
        self.url = url
        self.timeout = timeout
        self.session_id: str | None = None
        self._ids = itertools.count(1)
        self._initialized = False
        self._tools: dict[str, dict] | None = None
        self._arg_maps: dict[tuple[str, str], dict[str, str]] = {}

    # ------------------------------------------------------------------ transport
    def _post(self, payload: dict, expect_response: bool = True) -> Any:
        request = urllib.request.Request(
            self.url,
            data=json.dumps(payload).encode("utf-8"),
            method="POST",
            headers={"Content-Type": "application/json", "Accept": "application/json, text/event-stream",
                     "MCP-Protocol-Version": PROTOCOL_VERSION},
        )
        if self.session_id:
            request.add_header("Mcp-Session-Id", self.session_id)
        try:
            response = urllib.request.urlopen(request, timeout=self.timeout)
        except urllib.error.HTTPError as error:
            raise McpError(f"HTTP {error.code} from {self.url}: {error.read().decode('utf-8', 'replace')[:800]}") from None
        except urllib.error.URLError as error:
            raise McpError(f"Cannot reach {self.url} ({error.reason}). Is the editor open with the Unreal MCP server started? "
                           f"(console: ModelContextProtocol.StartServer)") from None
        with response:
            session = response.headers.get("Mcp-Session-Id")
            if session:
                self.session_id = session
            content_type = response.headers.get("Content-Type", "")
            body = response.read().decode("utf-8", "replace")

        if not expect_response:
            return None
        messages: list[dict] = []
        if "text/event-stream" in content_type:
            for block in body.replace("\r\n", "\n").split("\n\n"):
                data = [line[5:].lstrip() for line in block.split("\n") if line.startswith("data:")]
                if data:
                    try:
                        messages.append(json.loads("\n".join(data)))
                    except json.JSONDecodeError:
                        pass
        elif body.strip():
            parsed = json.loads(body)
            messages = parsed if isinstance(parsed, list) else [parsed]
        for message in messages:
            if message.get("id") == payload.get("id"):
                if "error" in message:
                    error = message["error"]
                    raise McpError(error.get("message", json.dumps(error)))
                return message.get("result")
        raise McpError(f"No response to {payload.get('method')}: {body[:500]}")

    def request(self, method: str, params: dict | None = None) -> Any:
        return self._post({"jsonrpc": "2.0", "id": next(self._ids), "method": method, "params": params or {}})

    def notify(self, method: str, params: dict | None = None) -> None:
        self._post({"jsonrpc": "2.0", "method": method, "params": params or {}}, expect_response=False)

    def connect(self) -> "McpClient":
        if not self._initialized:
            self.request("initialize", {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": {"name": "niagara-mcp-editor-tools", "version": "0.2.0"},
            })
            self.notify("notifications/initialized")
            self._initialized = True
        return self

    # ------------------------------------------------------------------ MCP tools
    def list_tools(self) -> dict[str, dict]:
        self.connect()
        if self._tools is None:
            tools: list[dict] = []
            cursor = None
            while True:
                result = self.request("tools/list", {"cursor": cursor} if cursor else {})
                tools += result.get("tools", [])
                cursor = result.get("nextCursor")
                if not cursor:
                    break
            self._tools = {tool["name"]: tool for tool in tools}
        return self._tools

    def call(self, name: str, arguments: dict | None = None) -> Any:
        self.connect()
        return _unwrap_result(self.request("tools/call", {"name": name, "arguments": arguments or {}}), name)

    # ------------------------------------------------------------------ Toolset Registry gateway
    def list_toolsets(self) -> Any:
        return self.call("list_toolsets")

    def describe_toolset(self, toolset: str) -> Any:
        tools = self.list_tools()
        if "describe_toolset" in tools:
            key = self._gateway_key("describe_toolset", "toolset", "toolset_name")
            return self.call("describe_toolset", {key: toolset})
        if "describe_toolsets" in tools:
            key = self._gateway_key("describe_toolsets", "toolset", "toolset_names")
            return self.call("describe_toolsets", {key: [toolset], "include_schemas": True})
        # Tool search disabled: every tool is listed as "<Toolset>.<Tool>".
        return [tool for name, tool in tools.items() if name.startswith(toolset + ".")]

    def _gateway_key(self, gateway: str, contains: str, default: str, exclude: str | None = None) -> str:
        schema = (self.list_tools().get(gateway) or {}).get("inputSchema", {})
        for name in schema.get("properties", {}):
            lowered = name.lower()
            if contains in lowered and (exclude is None or exclude not in lowered):
                return name
        return default

    def _arg_map(self, toolset: str, tool: str) -> dict[str, str]:
        key = (toolset, tool)
        if key not in self._arg_maps:
            mapping: dict[str, str] = {}
            try:
                entries: list[dict] = []
                _find_tool_entries(self.describe_toolset(toolset), entries)
                for entry in entries:
                    name = str(entry.get("name", ""))
                    if name == tool or name.endswith("." + tool) or _normalize(name) == _normalize(tool):
                        for prop in _schema_properties(entry):
                            mapping[_normalize(prop)] = prop
                            if prop[:1] == "b" and prop[1:2].isupper():
                                mapping[_normalize(prop[1:])] = prop  # bSave <- save
                        break
            except McpError:
                pass
            self._arg_maps[key] = mapping
        return self._arg_maps[key]

    def call_tool(self, toolset: str, tool: str, **arguments: Any) -> Any:
        mapping = self._arg_map(toolset, tool)
        adapted = {}
        for name, value in arguments.items():
            normalized = _normalize(name)
            target = mapping.get(normalized)
            if target is None and normalized.startswith("b"):
                target = mapping.get(normalized[1:])
            if normalized in OBJECT_ARGUMENTS and isinstance(value, str):
                value = object_path(value)
            adapted[target or name] = value

        tools = self.list_tools()
        if "call_tool" in tools:
            toolset_key = self._gateway_key("call_tool", "toolset", "toolset_name")
            tool_key = self._gateway_key("call_tool", "tool", "tool_name", exclude="toolset")
            args_key = self._gateway_key("call_tool", "arg", "arguments")
            return self.call("call_tool", {toolset_key: toolset, tool_key: tool, args_key: adapted})
        direct = f"{toolset}.{tool}"
        if direct in tools:
            return self.call(direct, adapted)
        raise McpError(f"Neither call_tool nor {direct} is available. Is the Niagara MCP Editor Tools plugin enabled?")

    def niagara(self, tool: str, **arguments: Any) -> Any:
        return self.call_tool(NIAGARA_TOOLSET, tool, **arguments)

    def material(self, tool: str, **arguments: Any) -> Any:
        return self.call_tool(MATERIAL_TOOLSET, tool, **arguments)

    def assets(self, tool: str, **arguments: Any) -> Any:
        return self.call_tool(ASSET_TOOLSET, tool, **arguments)

    def batch(self, operations: list[dict], **options: Any) -> Any:
        """Runs ExecuteBatch. Each operation: {"Tool": "SetModuleInput", "Arguments": {...} or JSON text, "Id": "x"}.

        Arguments may reference earlier results as "$<Id>.<field>" (see the ExecuteBatch tool description).
        """
        ops = []
        for op in operations:
            args = op.get("Arguments", op.get("arguments", {}))
            if not isinstance(args, str):
                args = {k: (object_path(v) if _normalize(k) in OBJECT_ARGUMENTS and isinstance(v, str) and not v.startswith("$") else v)
                        for k, v in args.items()}
                args = json.dumps(args, ensure_ascii=False)
            ops.append({"Id": op.get("Id", op.get("id", "")), "Tool": op.get("Tool", op.get("tool", "")), "Arguments": args})
        return self.niagara("ExecuteBatch", Operations=ops, **options)

    def recipe(self, name: str, run: bool = True, **placeholders: Any) -> Any:
        """Loads Tools/recipes/<name>.json, fills {{Placeholders}} (defaults from the recipe) and runs it with ExecuteBatch.

        Example: McpClient().recipe("spark_burst", SystemPath="/Game/FX/NS_Sparks")
        """
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "recipes", f"{name}.json")
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        values = dict(json.loads(text).get("placeholders", {}))
        values.update({k: str(v) for k, v in placeholders.items()})
        for key, value in values.items():
            text = text.replace("{{" + key + "}}", value)
        recipe = json.loads(text)
        if not run:
            return recipe
        return self.batch(recipe["operations"], Expect=recipe.get("expect", "None"))

    def capture_image(self, kind: str, **arguments: Any) -> Any:
        """Calls the top-level image tool (niagara_capture_preview / material_capture_preview): returns {"data", "mimeType"}
        plus "caption" (per-tile numbers) for niagara; the older-plugin fallback returns the registry result. Either
        way save_image() / image_caption() accept it."""
        name = f"{kind}_capture_preview"
        if name not in self.list_tools():
            # Older plugin: fall back to the registry tool (base64 inside JSON).
            return self.call_tool(NIAGARA_TOOLSET if kind == "niagara" else MATERIAL_TOOLSET, "CapturePreview", **arguments)
        schema = (self.list_tools()[name].get("inputSchema") or {}).get("properties", {})
        mapping = {_normalize(prop): prop for prop in schema}
        adapted = {}
        for key, value in arguments.items():
            if _normalize(key) in OBJECT_ARGUMENTS and isinstance(value, str):
                value = object_path(value)
            adapted[mapping.get(_normalize(key), key)] = value
        return self.call(name, adapted)


def _main(argv: list[str]) -> int:
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    mcp = McpClient()
    command = argv[0]
    if command == "tools":
        result: Any = sorted(mcp.list_tools())
    elif command == "toolsets":
        result = mcp.list_toolsets()
    elif command == "describe":
        result = mcp.describe_toolset(argv[1] if len(argv) > 1 else NIAGARA_TOOLSET)
    elif command == "call":
        result = mcp.call_tool(argv[1], argv[2], **(json.loads(argv[3]) if len(argv) > 3 else {}))
    else:
        print(f"Unknown command '{command}'. Use tools, toolsets, describe or call.", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2, ensure_ascii=False) if not isinstance(result, str) else result)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(_main(sys.argv[1:]))
    except McpError as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
