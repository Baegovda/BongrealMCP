"""
Builds (or rebuilds) a material from a JSON spec through the Material MCP toolset.

An existing material is cleared (all expressions removed) and rebuilt in place, so references to it stay valid.
The material's editor must be closed.

Spec:
{
  "path": "/Game/TestSample/M_ChainRibbon",
  "properties": {"BlendMode": "Masked", "ShadingModel": "Unlit", "TwoSided": "true"},
  "expressions": [
    {"id": "UV",   "class": "TextureCoordinate", "pos": [-900, 0]},
    {"id": "Rate", "class": "ScalarParameter", "pos": [-900, 200],
     "properties": {"ParameterName": "Gaze Changes Per Second", "DefaultValue": "2.3"}},
    {"id": "Rings", "class": "Custom", "pos": [-500, 0],
     "custom": {"description": "Rings", "inputs": ["UV"], "output": "Float1", "code": ["line 1", "line 2"]}}
  ],
  "links":   [["UV", "", "Rings", "UV"]],            # from, from output ("" = first), to, to input
  "outputs": [["Rings", "", "OpacityMask"]],         # from, from output, material output
  "layout": false                                     # true: auto-arrange instead of using "pos"
}

    python material_rebuild.py spec.json [--url http://127.0.0.1:8000/mcp]
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import Any

from mcp_client import MCP_URL, McpClient, McpError


def _code(value: Any) -> str:
    return "\n".join(value) if isinstance(value, list) else str(value or "")


def material_exists(mcp: McpClient, path: str) -> bool:
    try:
        mcp.material("GetMaterialGraph", Material=path)
        return True
    except McpError:
        return False


def build_material(mcp: McpClient, spec: dict, log=print) -> dict:
    path = spec["path"]
    if material_exists(mcp, path):
        graph = mcp.material("GetMaterialGraph", Material=path)
        for expression in graph.get("Expressions", graph.get("expressions", [])):
            name = expression.get("Name") or expression.get("name")
            mcp.material("RemoveExpression", Material=path, Expression=name)
        log(f"[material] cleared {path}")
    else:
        mcp.material("CreateMaterial", AssetPath=path, bSave=False)
        log(f"[material] created {path}")

    for prop, value in spec.get("properties", {}).items():
        mcp.material("SetMaterialProperty", Material=path, PropertyName=prop, Value=str(value), bSave=False)

    names: dict[str, str] = {}
    for expr in spec.get("expressions", []):
        x, y = (expr.get("pos") or [0, 0])[:2]
        info = mcp.material("AddExpression", Material=path, ExpressionClass=expr["class"], PosX=int(x), PosY=int(y))
        names[expr["id"]] = info.get("Name") or info.get("name")
        for prop, value in expr.get("properties", {}).items():
            mcp.material("SetExpressionProperty", Material=path, Expression=names[expr["id"]], PropertyName=prop, Value=str(value))
        custom = expr.get("custom")
        if custom:
            mcp.material("SetCustomExpression", Material=path, Expression=names[expr["id"]], Code=_code(custom.get("code")),
                         Description=custom.get("description", ""), InputNames=custom.get("inputs", []),
                         OutputType=custom.get("output", "Float3"))

    def resolve(ref: str) -> str:
        return names.get(ref, ref)

    for src, src_out, dst, dst_in in spec.get("links", []):
        mcp.material("ConnectExpressions", Material=path, FromExpression=resolve(src), FromOutput=src_out,
                     ToExpression=resolve(dst), ToInput=dst_in)
    for src, src_out, prop in spec.get("outputs", []):
        mcp.material("ConnectToMaterialProperty", Material=path, FromExpression=resolve(src), FromOutput=src_out, MaterialProperty=prop)
    if spec.get("layout"):
        mcp.material("LayoutMaterial", Material=path)

    result = mcp.material("CompileMaterial", Material=path, bSave=True)
    errors = result.get("Errors", result.get("errors", []))
    log(f"[material] {path}: {'compiled OK' if not errors else f'{len(errors)} compile error(s)'}")
    for error in errors:
        log(f"    {error}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Rebuild a material from a JSON spec over Unreal MCP.")
    parser.add_argument("spec")
    parser.add_argument("--url", default=MCP_URL)
    args = parser.parse_args()
    with open(args.spec, encoding="utf-8") as handle:
        spec = json.load(handle)
    mcp = McpClient(args.url)
    specs = spec if isinstance(spec, list) else spec.get("materials", [spec])
    failed = 0
    for material in specs:
        result = build_material(mcp, material)
        failed += 0 if result.get("bSucceeded", result.get("succeeded", True)) else 1
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except McpError as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
