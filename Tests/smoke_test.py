"""
Live smoke test over MCP: the editor must be open with the Unreal MCP server running.

  1. list_toolsets contains the Niagara and Material toolsets
  2. describe_toolset returns the Niagara tools
  3. builds examples/chain_strike.json under /Game/NiagaraMcpTests/Smoke_<time> and checks that
     everything compiles, the custom module has its inputs and the stack note is there

    python Tests/smoke_test.py [--url http://127.0.0.1:8000/mcp] [--keep-path /Game/NiagaraMcpTests]

The in-editor C++ automation tests (no MCP client needed) run with:
    UnrealEditor-Cmd.exe <Project>.uproject -ExecCmds="Automation RunTests NiagaraMcp;Quit" -unattended -nopause
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1] / "Tools"
sys.path.insert(0, str(TOOLS_DIR))

from mcp_client import MATERIAL_TOOLSET, MCP_URL, NIAGARA_TOOLSET, McpClient, McpError  # noqa: E402
from niagara_build import _get, build  # noqa: E402


def check(condition: bool, message: str) -> None:
    print(("  ok    " if condition else "  FAIL  ") + message)
    if not condition:
        raise SystemExit(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default=MCP_URL)
    parser.add_argument("--root", default="/Game/NiagaraMcpTests")
    args = parser.parse_args()
    mcp = McpClient(args.url)

    print("1. toolsets")
    toolsets = json.dumps(mcp.list_toolsets())
    check(NIAGARA_TOOLSET in toolsets, f"{NIAGARA_TOOLSET} is registered")
    check(MATERIAL_TOOLSET in toolsets, f"{MATERIAL_TOOLSET} is registered")

    print("2. describe")
    description = json.dumps(mcp.describe_toolset(NIAGARA_TOOLSET))
    for tool in ("GetSystemSummary", "AddModule", "SetModuleInput", "SetModuleNote", "SetCustomHlsl", "CompileSystem"):
        check(tool in description, f"tool {tool}")

    print("3. build the chain strike example")
    folder = f"{args.root}/Smoke_{time.strftime('%Y%m%d_%H%M%S')}"
    spec_text = (TOOLS_DIR / "examples" / "chain_strike.json").read_text(encoding="utf-8").replace("/Game/TestSample", folder)
    ok = build(json.loads(spec_text), mcp, log=lambda line: print("    " + line))
    check(ok, "material and system compiled without errors")

    system = f"{folder}/NS_ChainStrike"
    summary = mcp.niagara("GetSystemSummary", System=system)
    check(len(_get(summary, "Emitters", [])) == 3, "three emitters")
    stack = mcp.niagara("GetStack", System=system, EmitterName="Chain", bIncludeInputs=True)
    modules = _get(stack, "Modules", [])
    motion = next((m for m in modules if "NM_ChainStrikeMotion" in str(_get(m, "ScriptPath", ""))), None)
    check(motion is not None, "custom module is in the stack")
    inputs = {str(_get(i, "Name")): str(_get(i, "Value")) for i in _get(motion, "Inputs", [])}
    check(abs(float(inputs.get("Length", "0")) - 355.0) < 0.01, f"Length input = 355 (got {inputs.get('Length')})")
    check(bool(_get(motion, "bHasNote", False)), "module stack note")
    print(f"\nAll checks passed. Assets are under {folder}.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except McpError as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
