$ErrorActionPreference = "Stop"
$ToolDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$VenvDir = Join-Path $ToolDir ".venv"
$VenvPython = Join-Path $VenvDir "Scripts/python.exe"

if (-not (Test-Path $VenvPython)) {
    if (Get-Command py -ErrorAction SilentlyContinue) {
        & py -3 -m venv $VenvDir
    } else {
        & python -m venv $VenvDir
    }
    if ($LASTEXITCODE -ne 0) { throw "Could not create the code-index Python environment." }
}

& $VenvPython -m pip install --disable-pip-version-check -r (Join-Path $ToolDir "requirements.txt")
if ($LASTEXITCODE -ne 0) { throw "Could not install code-index dependencies." }
& $VenvPython -m pip check
if ($LASTEXITCODE -ne 0) { throw "Code-index dependency check failed." }

& $VenvPython (Join-Path $ToolDir "check_env.py")
if ($LASTEXITCODE -ne 0) { throw "Code-index parser smoke check failed; installation incomplete." }
