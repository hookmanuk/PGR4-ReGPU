# Stages the installer payload from the current build and compiles dist\PGR4-Setup-<version>.exe.
param([string]$Version = "0.1.0")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$payload = Join-Path $PSScriptRoot "payload"
Remove-Item -Recurse -Force $payload -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$payload\build\rel", "$payload\scripts", "$payload\py" | Out-Null

# The port: pgr4.exe and the DLLs beside it (not the test builds, symbols or build files)
Copy-Item "$root\build\rel\pgr4.exe", "$root\build\rel\*.dll" "$payload\build\rel" -Force
# The launcher (launcher\PGR4Launcher): framework-dependent, needs the .NET 10 Desktop Runtime (the installer gets it)
dotnet publish "$root\launcher\PGR4Launcher\PGR4Launcher.csproj" -c Release -o "$payload\launcher" --nologo -v q
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed with $LASTEXITCODE" }
# pgr4.exe has no version resource: the launcher reads version.txt
Set-Content -Path "$payload\scripts\version.txt" -Value $Version -Encoding ascii -NoNewline
Copy-Item "$PSScriptRoot\setup_game.py" "$payload\py" -Force
Copy-Item "$root\tools\xiso.py", "$root\tools\stfs.py" "$payload\py" -Force
# The embeddable Python (python.org's python-3.12 embed zip, unpacked to out\pyembed\py)
if (-not (Test-Path "$root\out\pyembed\py\python.exe")) { throw "out\pyembed\py\python.exe missing: unpack python-3.12-embed-amd64.zip there" }
Copy-Item "$root\out\pyembed\py\*" "$payload\py" -Recurse -Force

$iscc = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
& $iscc "/DAppVersion=$Version" (Join-Path $PSScriptRoot "pgr4.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with $LASTEXITCODE" }
Write-Host "Built: $PSScriptRoot\dist\PGR4-Setup-$Version.exe"
