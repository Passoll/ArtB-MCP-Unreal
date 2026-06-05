@echo off
setlocal EnableExtensions

set "MCP_DIR=%~dp0"
set "SERVER_SCRIPT=%MCP_DIR%toolplay_mcp_server.py"
set "REQUIREMENTS=%MCP_DIR%requirements.txt"
set "VALIDATE_CATALOGS=%MCP_DIR%scripts\validate_catalogs.py"
set "MCP_DIR_PY=%MCP_DIR:\=\\%"
set "PYTHON_CMD=python"
set "CHECK_ONLY=0"
set "HAS_ERROR=0"

if /I "%~1"=="--check-only" set "CHECK_ONLY=1"
if /I "%~1"=="/check" set "CHECK_ONLY=1"
if not "%TOOLPLAY_PYTHON%"=="" set "PYTHON_CMD=%TOOLPLAY_PYTHON%"

echo.
echo === ToolPlayMCP Setup ===
echo MCP server: %SERVER_SCRIPT%
echo Python: %PYTHON_CMD%
echo.

%PYTHON_CMD% --version
if errorlevel 1 (
    echo.
    echo ERROR: Python was not found. Install Python 3.10+ or set TOOLPLAY_PYTHON to a python.exe path.
    exit /b 1
)

if "%CHECK_ONLY%"=="0" (
    echo.
    echo === Installing Python requirements ===
    %PYTHON_CMD% -m pip install -r "%REQUIREMENTS%"
    if errorlevel 1 (
        echo.
        echo ERROR: Failed to install requirements.
        exit /b 1
    )
) else (
    echo.
    echo === Skipping pip install because --check-only was provided ===
)

echo.
echo === Checking MCP Python dependency ===
%PYTHON_CMD% -c "import importlib.util, sys; ok=importlib.util.find_spec('mcp') is not None; print('mcp import ok' if ok else 'mcp import missing'); sys.exit(0 if ok else 1)"
if errorlevel 1 (
    echo.
    echo ERROR: Missing Python package 'mcp'. Run this bat without --check-only or install requirements manually.
    set "HAS_ERROR=1"
)

echo.
echo === Validating local catalogs ===
if exist "%VALIDATE_CATALOGS%" (
    %PYTHON_CMD% "%VALIDATE_CATALOGS%"
    if errorlevel 1 (
        echo.
        echo ERROR: Catalog validation failed.
        set "HAS_ERROR=1"
    )
) else (
    echo No catalog validator found. Skipping.
)

echo.
echo === Checking Unreal Editor bridge ===
%PYTHON_CMD% -B -c "import sys,json; sys.path.insert(0, '%MCP_DIR_PY%'); from toolplay_bridge import call_unreal_bridge; r=call_unreal_bridge('list_tools',{}); names=[t.get('name') for t in r.get('tools',[])]; print(json.dumps({'bridge':'ok','tool_count':len(names),'has_diagnostics':'get_niagara_diagnostics' in names}, indent=2))"
if errorlevel 1 (
    echo.
    echo WARNING: Could not reach the Unreal bridge at 127.0.0.1:55557.
    echo Open Unreal Editor, enable ToolPlayMCP, rebuild if needed, then run this bat again.
    set "HAS_ERROR=1"
) else (
    echo Unreal bridge check completed.
)

echo.
echo === Codex MCP config snippet ===
echo Add this to %%USERPROFILE%%\.codex\config.toml, then restart Codex:
echo.
echo [mcp_servers.toolplay-mcp]
echo command = 'python'
echo args = ['%SERVER_SCRIPT%']
echo.
echo For other MCP clients, use the same server script as a stdio MCP server.
echo Do not configure the UE TCP bridge port as an MCP server.
echo.
if "%HAS_ERROR%"=="0" (
    echo Setup complete.
) else (
    echo Setup completed with warnings/errors. Read the messages above.
    exit /b 1
)

endlocal
