"""
Installs Niagara MCP Editor Tools into an Unreal project (what the demo's "check and install it" step did).

  1. Enables NiagaraMcpEditorTools and ModelContextProtocol (Unreal MCP) in <Project>.uproject.
  2. Makes the MCP server start when the project opens:
       Config/DefaultEditorPerProjectUserSettings.ini
       [/Script/NiagaraMcpEditorTools.NiagaraMcpEditorToolsSettings]
       bAutoStartMcpServer=True
  3. Writes MCP client configs in the project root, server name "unreal-mcp":
       .codex/config.toml (Codex), .mcp.json (Claude Code), .cursor/mcp.json (Cursor)
     Existing files are merged, never replaced; an existing unreal-mcp entry in config.toml is left alone.

Usage (run from anywhere):
    python setup_project.py <path to .uproject or project folder> [--port 8000] [--no-autostart] [--no-client-configs]
With no path, the project containing this plugin (../../..) is used.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

PLUGIN_NAME = "NiagaraMcpEditorTools"
REQUIRED_PLUGINS = [PLUGIN_NAME, "ModelContextProtocol"]
# Epic stock toolsets that complement this plugin (viewport capture, PIE, material instances, Niagara components, ...).
EPIC_TOOLSETS = ["EditorToolset", "NiagaraToolsets"]
SETTINGS_SECTION = "[/Script/NiagaraMcpEditorTools.NiagaraMcpEditorToolsSettings]"
SERVER_NAME = "unreal-mcp"


def find_uproject(arg: str | None) -> Path:
    if arg:
        path = Path(arg).resolve()
    else:
        path = Path(__file__).resolve().parents[3]  # <Project>/Plugins/NiagaraMcpEditorTools/Tools/setup_project.py
    if path.is_file() and path.suffix == ".uproject":
        return path
    candidates = sorted(path.glob("*.uproject")) if path.is_dir() else []
    if len(candidates) != 1:
        sys.exit(f"Could not find a single .uproject in '{path}'. Pass the .uproject path.")
    return candidates[0]


def enable_plugins(uproject: Path, extra: list[str] | None = None) -> list[str]:
    text = uproject.read_text(encoding="utf-8-sig")
    data = json.loads(text)
    plugins = data.setdefault("Plugins", [])
    changed = []
    for name in REQUIRED_PLUGINS + (extra or []):
        entry = next((p for p in plugins if p.get("Name") == name), None)
        if entry is None:
            plugins.append({"Name": name, "Enabled": True})
            changed.append(f"enabled {name}")
        elif not entry.get("Enabled", False):
            entry["Enabled"] = True
            changed.append(f"re-enabled {name}")
    if changed:
        uproject.write_text(json.dumps(data, indent="\t") + "\n", encoding="utf-8")
    return changed


def set_autostart(project_dir: Path, port: int, enabled: bool) -> str:
    ini = project_dir / "Config" / "DefaultEditorPerProjectUserSettings.ini"
    ini.parent.mkdir(parents=True, exist_ok=True)
    lines = ini.read_text(encoding="utf-8-sig").splitlines() if ini.exists() else []
    wanted = {"bAutoStartMcpServer": "True" if enabled else "False"}
    if port != 8000:
        wanted["McpServerPort"] = str(port)

    if SETTINGS_SECTION in lines:
        start = lines.index(SETTINGS_SECTION) + 1
        end = start
        while end < len(lines) and not lines[end].startswith("["):
            end += 1
        body = [l for l in lines[start:end] if l.split("=", 1)[0].strip() not in wanted]
        body = [f"{k}={v}" for k, v in wanted.items()] + [l for l in body if l.strip()]
        lines = lines[:start] + body + [""] + lines[end:]
    else:
        if lines and lines[-1].strip():
            lines.append("")
        lines += [SETTINGS_SECTION] + [f"{k}={v}" for k, v in wanted.items()]
    ini.write_text("\n".join(lines).rstrip() + "\n", encoding="utf-8")
    return str(ini)


def write_client_configs(project_dir: Path, port: int) -> list[str]:
    url = f"http://127.0.0.1:{port}/mcp"
    written = []

    # Codex: .codex/config.toml (streamable HTTP server).
    codex = project_dir / ".codex" / "config.toml"
    codex.parent.mkdir(parents=True, exist_ok=True)
    existing = codex.read_text(encoding="utf-8") if codex.exists() else ""
    if re.search(rf'^\[mcp_servers\.("?){re.escape(SERVER_NAME)}\1\]', existing, re.MULTILINE):
        written.append(f"{codex} (kept existing [mcp_servers.{SERVER_NAME}])")
    else:
        block = f'[mcp_servers.{SERVER_NAME}]\nurl = "{url}"\n'
        codex.write_text((existing.rstrip() + "\n\n" if existing.strip() else "") + block, encoding="utf-8")
        written.append(str(codex))

    # Claude Code: .mcp.json; Cursor: .cursor/mcp.json.
    for path, entry in (
        (project_dir / ".mcp.json", {"type": "http", "url": url}),
        (project_dir / ".cursor" / "mcp.json", {"url": url}),
    ):
        path.parent.mkdir(parents=True, exist_ok=True)
        data = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
        data.setdefault("mcpServers", {})[SERVER_NAME] = entry
        path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        written.append(str(path))
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("project", nargs="?", help=".uproject file or project folder")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--no-autostart", action="store_true", help="do not start the MCP server with the editor")
    parser.add_argument("--no-client-configs", action="store_true", help="do not write Codex / Claude Code / Cursor configs")
    parser.add_argument("--with-epic-toolsets", action="store_true",
                        help="also enable Epic's EditorToolset and NiagaraToolsets (viewport capture, PIE, components, ...)")
    args = parser.parse_args()

    uproject = find_uproject(args.project)
    project_dir = uproject.parent
    if not (project_dir / "Plugins" / PLUGIN_NAME / f"{PLUGIN_NAME}.uplugin").exists():
        print(f"warning: {project_dir / 'Plugins' / PLUGIN_NAME} not found. Copy the plugin folder into the project's Plugins folder.")

    changes = enable_plugins(uproject, EPIC_TOOLSETS if args.with_epic_toolsets else None)
    print(f"{uproject.name}: {', '.join(changes) if changes else 'plugins already enabled'}")
    print(f"MCP auto start {'off' if args.no_autostart else 'on'}: {set_autostart(project_dir, args.port, not args.no_autostart)}")
    if not args.no_client_configs:
        for path in write_client_configs(project_dir, args.port):
            print(f"client config: {path}")
    print(f"\nRestart the editor, then connect your AI client to http://127.0.0.1:{args.port}/mcp (server name '{SERVER_NAME}').")
    print("Check: python Tools/mcp_client.py toolsets  -> should list NiagaraMcpEditorTools.NiagaraMcpEditorTools")
    return 0


if __name__ == "__main__":
    sys.exit(main())
