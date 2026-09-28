[CmdletBinding()]
param(
    [string]$MsysRoot,
    [ValidateRange(1, 32)]
    [int]$Jobs = 4
)

# Uses an existing MSYS2 MINGW64 toolchain. This script does not download or
# install packages, change the global environment, or remove existing files.
# Required packages: gcc, make, pkgconf, python, pybind11, qt6-base, qt6-charts,
# qt6-serialport, qt6-serialbus and qt6-svg (all mingw-w64-x86_64-*).
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $MsysRoot) { $MsysRoot = Join-Path $projectRoot 'build/toolchain/msys64' }
$mingwRoot = Join-Path ([IO.Path]::GetFullPath($MsysRoot)) 'mingw64'
$mingwBin = Join-Path $mingwRoot 'bin'
$bundleDir = Join-Path $projectRoot 'bin/CANgaroo-Windows'
$bundleExe = Join-Path $bundleDir 'cangaroo.exe'

function Invoke-Checked {
    param([string]$Tool, [string[]]$ToolArguments)
    & $Tool @ToolArguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Tool failed with exit code $LASTEXITCODE."
    }
}

function Copy-RuntimeDependencies {
    param([string]$Directory, [string]$RuntimeBin, [string]$Objdump)

    $pending = [Collections.Generic.Queue[string]]::new()
    $visited = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $copied = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in Get-ChildItem -LiteralPath $Directory -File -Recurse) {
        if ($file.Extension -in '.exe', '.dll', '.pyd') {
            $pending.Enqueue($file.FullName)
        }
    }

    $systemDir = [Environment]::GetFolderPath('System')
    while ($pending.Count -gt 0) {
        $binaryPath = $pending.Dequeue()
        if (-not $visited.Add($binaryPath)) { continue }
        $imports = & $Objdump -p $binaryPath
        if ($LASTEXITCODE -ne 0) { throw "Cannot inspect DLL imports: $binaryPath" }
        foreach ($line in $imports) {
            if ($line -notmatch '^\s*DLL Name:\s*(\S+)\s*$') { continue }
            $dll = $Matches[1]
            if ($dll -match '^(api-ms-win-|ext-ms-win-)') { continue }
            $destination = Join-Path $Directory $dll
            $source = Join-Path $RuntimeBin $dll
            if (Test-Path -LiteralPath $source -PathType Leaf) {
                # Refresh imported runtimes on repeated builds as well, so an
                # upgraded compiler/Qt cannot retain an older DLL in the bundle.
                if ($copied.Add($destination)) {
                    Copy-Item -LiteralPath $source -Destination $destination -Force
                }
                $pending.Enqueue($destination)
            } elseif (Test-Path -LiteralPath $destination -PathType Leaf) {
                $pending.Enqueue($destination)
            } elseif (-not (Test-Path -LiteralPath (Join-Path $systemDir $dll) -PathType Leaf)) {
                throw "Missing runtime dependency '$dll', required by '$binaryPath'."
            }
        }
    }
}

$qmake = Join-Path $mingwBin 'qmake6.exe'
$make = Join-Path $mingwBin 'mingw32-make.exe'
$deploy = Join-Path $mingwBin 'windeployqt6.exe'
$python = Join-Path $mingwBin 'python3.exe'
$objdump = Join-Path $mingwBin 'objdump.exe'
foreach ($tool in @($qmake, $make, $deploy, $python, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required tool not found: $tool. Supply -MsysRoot with an installed MINGW64 toolchain."
    }
}

$savedEnvironment = @{}
foreach ($name in @('PATH', 'PYTHONHOME', 'PYTHONPATH', 'PKG_CONFIG_PATH', 'PKG_CONFIG_LIBDIR', 'QT_PLUGIN_PATH')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

try {
    # Process-scoped changes ensure qmake, Python and Qt use this same toolchain.
    $env:PATH = $mingwBin + ';' + $savedEnvironment['PATH']
    [Environment]::SetEnvironmentVariable('PYTHONHOME', $null, 'Process')
    [Environment]::SetEnvironmentVariable('PYTHONPATH', $null, 'Process')
    $env:PKG_CONFIG_PATH = (Join-Path $mingwRoot 'lib/pkgconfig') + ';' + (Join-Path $mingwRoot 'share/pkgconfig')
    $env:PKG_CONFIG_LIBDIR = $env:PKG_CONFIG_PATH
    $env:QT_PLUGIN_PATH = Join-Path $mingwRoot 'share/qt6/plugins'

    Write-Host 'Building CANgaroo...'
    Push-Location (Join-Path $projectRoot 'src')
    try {
        Invoke-Checked $qmake @('src.pro', 'CONFIG+=release', 'CONFIG+=c++20', 'CONFIG-=debug', 'CONFIG-=debug_and_release')
        Invoke-Checked $make @('-j', "$Jobs")
    } finally {
        Pop-Location
    }

    Write-Host "Preparing portable application: $bundleDir"
    New-Item -ItemType Directory -Path $bundleDir -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $projectRoot 'bin/cangaroo.exe') -Destination $bundleExe -Force
    Invoke-Checked $deploy @('--release', '--no-system-d3d-compiler', '--dir', $bundleDir, $bundleExe)

    $pythonVersion = & $python -c 'import sysconfig; print(sysconfig.get_python_version())'
    if ($LASTEXITCODE -ne 0 -or $pythonVersion -notmatch '^\d+\.\d+$') {
        throw 'Cannot determine the toolchain Python version.'
    }
    $pythonSource = Join-Path $mingwRoot "lib/python$pythonVersion"
    if (-not (Test-Path -LiteralPath (Join-Path $pythonSource 'os.py'))) {
        throw "Python standard library not found: $pythonSource"
    }
    $pythonDestination = Join-Path $bundleDir "lib/python$pythonVersion"
    # Copy only; robocopy does not mirror or delete anything. Exit codes 0..7
    # represent successful copies, including files already present.
    & robocopy.exe $pythonSource $pythonDestination /E /NFL /NDL /NJH /NJS /NP `
        /XD test tests __pycache__ site-packages idlelib turtledemo ensurepip `
        /XF '*.pyc' '*.pyo'
    if ($LASTEXITCODE -ge 8) { throw "Python standard library copy failed: $LASTEXITCODE" }

    $exampleDir = Join-Path $bundleDir 'examples'
    New-Item -ItemType Directory -Path $exampleDir -Force | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $projectRoot 'examples') -Filter '*.py' -File |
        Copy-Item -Destination $exampleDir -Force
    Get-ChildItem -LiteralPath $projectRoot -Filter 'LICENSE*' -File |
        Copy-Item -Destination $bundleDir -Force

    Write-Host 'Resolving runtime DLLs, including Python extension dependencies...'
    Copy-RuntimeDependencies $bundleDir $mingwBin $objdump
    Write-Host "Built application: $bundleExe"
    Write-Host 'Start cangaroo.exe from this directory; keep its DLLs, plugins and lib directory together.'
} finally {
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
}
