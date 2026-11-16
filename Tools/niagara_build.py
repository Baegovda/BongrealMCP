"""
Builds Niagara assets from a JSON spec through the Niagara MCP toolset (materials via material_rebuild).

Spec (see examples/chain_strike.json for a full one):
{
  "materials": [ <material_rebuild spec>, ... ],
  "module_scripts": [{
    "path": "/Game/FX/NM_Motion", "description": "...", "stacks": ["ParticleSpawn", "ParticleUpdate"],
    "nodes":  [{"id": "hlsl", "type": "CustomHlsl", "pos": [0, 200]}],
    "pins":   [{"node": "@get", "name": "Module.Length", "type": "float", "default": "305"},
               {"node": "@set", "name": "Particles.Position", "type": "position"}],
    "hlsl":   [{"node": "hlsl", "code": ["line", "line"], "inputs": [["Length", "float"]], "outputs": [["Position", "position"]]}],
    "links":  [["@get", "Module.Length", "hlsl", "Length"], ["hlsl", "Position", "@set", "Particles.Position"]]
  }],
  "system": {
    "path": "/Game/FX/NS_Effect",
    "rebuild": true,                       # existing system: remove the listed emitters first and rebuild them
    "user_parameters": [{"name": "Tint", "type": "color", "value": "1,0.2,0.1,1"}],
    "emitter_defaults": { ...same keys as an emitter, merged into every emitter... },
    "emitters": [{
      "name": "Chain", "template": "",     # "" = Minimal template
      "properties": {"bLocalSpace": "true"},
      "remove_modules": ["Spawn Rate"],
      "add_modules": [{"stack": "EmitterUpdate", "script": "/Niagara/Modules/Beams/BeamEmitterSetup.BeamEmitterSetup"}],
      "inputs": [{"module": "Spawn Burst Instantaneous", "input": "Spawn Count", "value": "40"}],
      "notes":  [{"module": "NM_Motion", "header": "AI", "message": "What this does"}],
      "remove_renderers": [0],
      "renderers": [{"type": "Ribbon", "material": "/Game/FX/M_Chain", "properties": {"UV0Settings.TilingLength": "100"}}]
    }]
  }
}

Graph node references: "@input" (Input Map), "@output" (Output Module), "@set" (template Map Set), "@get" (Map Get,
created and linked to the input map when the template has none) or ids from "nodes". "<map>" as a pin name means the
node's parameter map pin.

    python niagara_build.py spec.json [--url http://127.0.0.1:8000/mcp]
"""

from __future__ import annotations

import argparse
import copy
import json
import sys
from typing import Any

from material_rebuild import build_material
from mcp_client import MCP_URL, McpClient, McpError

MAP_TYPE = "NiagaraParameterMap"


def _get(data: dict, key: str, default: Any = None) -> Any:
    """Field access that tolerates camelCase / PascalCase result keys."""
    for candidate in (key, key[:1].lower() + key[1:], key[:1].upper() + key[1:]):
        if candidate in data:
            return data[candidate]
    return default


def _code(value: Any) -> str:
    return "\n".join(value) if isinstance(value, list) else str(value or "")


def _merge(defaults: dict, emitter: dict) -> dict:
    merged = copy.deepcopy(defaults)
    for key, value in emitter.items():
        if isinstance(value, list) and isinstance(merged.get(key), list):
            merged[key] = merged[key] + value
        elif isinstance(value, dict) and isinstance(merged.get(key), dict):
            merged[key] = {**merged[key], **value}
        else:
            merged[key] = value
    return merged


class GraphBuilder:
    def __init__(self, mcp: McpClient, script: str, graph: dict, log=print):
        self.mcp, self.script, self.log = mcp, script, log
        self.nodes: dict[str, dict] = {}
        self._index(graph)

    def _index(self, graph: dict) -> None:
        by_class = {"NiagaraNodeInput": "@input", "NiagaraNodeOutput": "@output",
                    "NiagaraNodeParameterMapSet": "@set", "NiagaraNodeParameterMapGet": "@get"}
        for node in _get(graph, "Nodes", []):
            ref = by_class.get(_get(node, "Class"))
            if ref and ref not in self.nodes:
                self.nodes[ref] = node

    def node(self, ref: str) -> dict:
        if ref == "@get" and ref not in self.nodes:
            created = self.mcp.niagara("AddGraphNode", Script=self.script, NodeType="MapGet", PosX=-400, PosY=250)
            self.nodes["@get"] = created
            self.link("@input", "<map>", "@get", "<map>")
        if ref not in self.nodes:
            raise McpError(f"Unknown graph node reference '{ref}' in {self.script}")
        return self.nodes[ref]

    def node_id(self, ref: str) -> str:
        return _get(self.node(ref), "NodeId")

    def refresh(self, ref: str, info: dict) -> None:
        if info and _get(info, "NodeId"):
            self.nodes[ref] = info

    def pin(self, ref: str, name: str, direction: str) -> str:
        if name != "<map>":
            return name
        for pin in _get(self.node(ref), "Pins", []):
            if _get(pin, "Direction") == direction and _get(pin, "Type") == MAP_TYPE:
                return _get(pin, "Name")
        raise McpError(f"Node {ref} has no {direction.lower()} parameter map pin")

    def link(self, src: str, src_pin: str, dst: str, dst_pin: str) -> None:
        info = self.mcp.niagara("ConnectPins", Script=self.script,
                                FromNodeId=self.node_id(src), FromPin=self.pin(src, src_pin, "Output"),
                                ToNodeId=self.node_id(dst), ToPin=self.pin(dst, dst_pin, "Input"))
        self.refresh(dst, info)


def build_module_script(mcp: McpClient, spec: dict, log=print) -> str:
    path = spec["path"]
    try:
        graph = mcp.niagara("GetScriptGraph", Script=path)
        log(f"[module] {path} already exists; using it as is (delete it to rebuild).")
        return _get(graph, "ScriptPath", path)
    except McpError:
        pass

    graph = mcp.niagara("CreateModuleScript", AssetPath=path, Description=spec.get("description", ""),
                        AllowedStacks=spec.get("stacks", []), bSave=False)
    script = _get(graph, "ScriptPath")
    log(f"[module] created {script}")
    builder = GraphBuilder(mcp, script, graph, log)

    for node in spec.get("nodes", []):
        x, y = (node.get("pos") or [0, 0])[:2]
        builder.nodes[node["id"]] = mcp.niagara("AddGraphNode", Script=script, NodeType=node["type"], PosX=int(x), PosY=int(y))
    for pin in spec.get("pins", []):
        info = mcp.niagara("AddParameterPin", Script=script, NodeId=builder.node_id(pin["node"]), ParameterName=pin["name"], Type=pin["type"])
        builder.refresh(pin["node"], info)
        if "default" in pin:
            info = mcp.niagara("SetPinDefaultValue", Script=script, NodeId=builder.node_id(pin["node"]), PinName=pin["name"], Value=str(pin["default"]))
            builder.refresh(pin["node"], info)
    for hlsl in spec.get("hlsl", []):
        info = mcp.niagara("SetCustomHlsl", Script=script, NodeId=builder.node_id(hlsl["node"]), Hlsl=_code(hlsl["code"]),
                           Inputs=[{"Name": n, "Type": t} for n, t in hlsl.get("inputs", [])],
                           Outputs=[{"Name": n, "Type": t} for n, t in hlsl.get("outputs", [])])
        builder.refresh(hlsl["node"], info)
    for src, src_pin, dst, dst_pin in spec.get("links", []):
        builder.link(src, src_pin, dst, dst_pin)

    mcp.niagara("ApplyScriptChanges", Script=script, bSave=True)
    return script


def build_system(mcp: McpClient, spec: dict, log=print) -> dict:
    path = spec["path"]
    emitters = [_merge(spec.get("emitter_defaults", {}), emitter) for emitter in spec.get("emitters", [])]
    try:
        summary = mcp.niagara("GetSystemSummary", System=path)
        if not spec.get("rebuild", True):
            raise McpError(f"{path} already exists (set \"rebuild\": true to rebuild its emitters).")
        existing = {str(_get(e, "Name")) for e in _get(summary, "Emitters", [])}
        for emitter in emitters:
            if emitter["name"] in existing:
                mcp.niagara("RemoveEmitter", System=path, EmitterName=emitter["name"], bSave=False)
        log(f"[system] rebuilding emitters of {path}")
    except McpError as error:
        if "already exists" in str(error):
            raise
        result = mcp.niagara("CreateSystem", AssetPath=path, bSave=False)
        path = _get(result, "Path", path)
        log(f"[system] created {path}")

    for parameter in spec.get("user_parameters", []):
        mcp.niagara("SetUserParameter", System=path, ParameterName=parameter["name"], Type=parameter["type"],
                    DefaultValue=str(parameter.get("value", "")), bSave=False)

    for emitter in emitters:
        name = emitter["name"]
        mcp.niagara("AddEmitter", System=path, EmitterName=name, TemplateEmitter=emitter.get("template", ""), bSave=False)
        for prop, value in emitter.get("properties", {}).items():
            mcp.niagara("SetEmitterProperty", System=path, EmitterName=name, PropertyName=prop, Value=str(value), bSave=False)
        for module in emitter.get("remove_modules", []):
            try:
                mcp.niagara("RemoveModule", System=path, EmitterName=name, Module=module, StackName="", bSave=False)
            except McpError as error:
                log(f"    (skip remove '{module}': {error})")
        for module in emitter.get("add_modules", []):
            mcp.niagara("AddModule", System=path, EmitterName=name, StackName=module["stack"], ModuleScript=module["script"],
                        TargetIndex=int(module.get("index", -1)), bSave=False)
        for item in emitter.get("inputs", []):
            mcp.niagara("SetModuleInput", System=path, EmitterName=name, Module=item["module"], Input=item["input"],
                        Value=str(item["value"]), StackName=item.get("stack", ""), bSave=False)
        for note in emitter.get("notes", []):
            mcp.niagara("SetModuleNote", System=path, EmitterName=name, Module=note["module"], Header=note.get("header", ""),
                        Message=note.get("message", ""), bInline=bool(note.get("inline", False)), StackName=note.get("stack", ""), bSave=False)
        for index in sorted(emitter.get("remove_renderers", []), reverse=True):
            mcp.niagara("RemoveRenderer", System=path, EmitterName=name, RendererIndex=int(index), bSave=False)
        for renderer in emitter.get("renderers", []):
            info = mcp.niagara("AddRenderer", System=path, EmitterName=name, RendererType=renderer["type"],
                               Material=renderer.get("material", ""), bSave=False)
            for prop, value in renderer.get("properties", {}).items():
                mcp.niagara("SetRendererProperty", System=path, EmitterName=name, RendererIndex=int(_get(info, "Index")),
                            PropertyName=prop, Value=str(value), bSave=False)
        log(f"[system] emitter {name} done")

    result = mcp.niagara("CompileSystem", System=path, bWaitForCompletion=True, bSave=True)
    messages = _get(result, "Messages", [])
    errors = [m for m in messages if _get(m, "Severity") == "Error"]
    log(f"[system] {path}: {'compiled OK' if _get(result, 'bSucceeded', not errors) else f'{len(errors)} compile error(s)'}")
    for message in messages:
        log(f"    {_get(message, 'Severity')} {_get(message, 'Script')}: {_get(message, 'Message')}")
    return result


def build(spec: dict, mcp: McpClient, log=print) -> bool:
    ok = True
    for material in spec.get("materials", []):
        result = build_material(mcp, material, log)
        ok &= bool(_get(result, "bSucceeded", True))
    for module in spec.get("module_scripts", []):
        build_module_script(mcp, module, log)
    if spec.get("system"):
        result = build_system(mcp, spec["system"], log)
        ok &= bool(_get(result, "bSucceeded", True))
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description="Build Niagara assets from a JSON spec over Unreal MCP.")
    parser.add_argument("spec")
    parser.add_argument("--url", default=MCP_URL)
    args = parser.parse_args()
    with open(args.spec, encoding="utf-8") as handle:
        spec = json.load(handle)
    return 0 if build(spec, McpClient(args.url)) else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except McpError as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
