@echo off
rem Runs Python with this Tools folder importable (mcp_client, material_rebuild, niagara_build).
rem   Plugins\NiagaraMcpEditorTools\Tools\mcppy.cmd Saved\chain_build.py
rem   Plugins\NiagaraMcpEditorTools\Tools\mcppy.cmd -m mcp_client toolsets
setlocal
set "PYTHONPATH=%~dp0;%PYTHONPATH%"
set "PYTHONIOENCODING=utf-8"
where py >nul 2>nul
if %ERRORLEVEL%==0 (
    py -3 %*
) else (
    python %*
)
exit /b %ERRORLEVEL%
