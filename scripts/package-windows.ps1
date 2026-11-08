# --------------------------------------------------
# Windows distribution package
#
# Builds the Release configuration and creates a ZIP that runs on a PC
# without Visual Studio or vcpkg:
#   dist\LocalLLM-windows-x64-<version>.zip
#
# Only an explicit list of files goes into the ZIP. rag\ and config\ are
# taken from the repository, not from build\Release, so documents you added
# to rag\ and the generated rag-index.json are never distributed.
#
# Run it from a normal PowerShell, not the Developer PowerShell for VS
# (CMakeLists.txt explains why):
#   powershell -ExecutionPolicy Bypass -File .\scripts\package-windows.ps1
#   (add -SkipBuild to package the existing build\Release as it is,
#    -Version 1.0.0 to replace the git commit in the file name)
# --------------------------------------------------

param(
    [string]$Version,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

$Repo = Split-Path -Parent $PSScriptRoot
$Release = Join-Path $Repo 'build\Release'
$Dist = Join-Path $Repo 'dist'

# The C++ runtime is copied next to both executables, because Windows looks
# for DLLs in the folder of the executable that loads them
$CrtDlls = @(
    'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll'
)

function Invoke-Checked {
    param([string]$Command, [string[]]$Arguments)

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Failed ($LASTEXITCODE): $Command $Arguments"
    }
}

function Find-CrtFolder {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "vswhere.exe not found. Install Visual Studio: $vswhere"
    }

    $found = & $vswhere -latest -prerelease -find 'VC\Redist\MSVC\*\x64\*.CRT' |
        Where-Object { $_ -notmatch 'onecore|debug' } |
        Select-Object -Last 1

    if (-not $found) {
        throw 'The Visual C++ redistributable files were not found in Visual Studio.'
    }

    return $found
}

function Copy-Required {
    param([string]$Source, [string]$Destination)

    if (-not (Test-Path -LiteralPath $Source)) {
        throw "Required file not found: $Source"
    }

    Copy-Item -LiteralPath $Source -Destination $Destination -Recurse
}

function New-Zip {
    param([string]$Folder, [string]$RootName, [string]$ZipPath)

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    # Written entry by entry: Compress-Archive in Windows PowerShell 5.1 can
    # store backslashes in the entry names, which other tools read as one
    # long file name
    $zip = [IO.Compression.ZipFile]::Open($ZipPath, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $Folder -Recurse -File) {
            $relative = $file.FullName.Substring($Folder.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, $file.FullName, "$RootName/$relative",
                [IO.Compression.CompressionLevel]::Optimal)
        }
    }
    finally {
        $zip.Dispose()
    }
}

if (-not $SkipBuild) {
    Invoke-Checked 'cmake' @('--preset', 'windows')
    Invoke-Checked 'cmake' @('--build', '--preset', 'windows-release')
}

if (-not (Test-Path -LiteralPath (Join-Path $Release 'LocalLLM.exe'))) {
    throw "LocalLLM.exe not found in $Release. Run without -SkipBuild."
}

if (-not $Version) {
    $Version = (& git -C $Repo describe --tags --always --dirty).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not get the version from git. Pass -Version.'
    }
}

$Name = "LocalLLM-windows-x64-$Version"
$Stage = Join-Path $Dist $Name
$ZipPath = Join-Path $Dist "$Name.zip"

if (Test-Path -LiteralPath $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
if (Test-Path -LiteralPath $ZipPath) { Remove-Item -LiteralPath $ZipPath -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $Stage 'rag'), (Join-Path $Stage 'licenses') | Out-Null

try {
    # Application
    Copy-Required (Join-Path $Release 'LocalLLM.exe') $Stage
    Get-ChildItem -LiteralPath $Release -Filter '*.dll' | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $Stage
    }
    Copy-Required (Join-Path $Release 'llama') $Stage

    # Data, from the repository
    Copy-Required (Join-Path $Repo 'Asset') $Stage
    Copy-Required (Join-Path $Repo 'config') $Stage
    Copy-Required (Join-Path $Repo 'rag\README.md') (Join-Path $Stage 'rag')
    Copy-Required (Join-Path $Repo 'rag\samples') (Join-Path $Stage 'rag')

    # C++ runtime
    $crtFolder = Find-CrtFolder
    foreach ($dll in $CrtDlls) {
        Copy-Required (Join-Path $crtFolder $dll) $Stage
        Copy-Required (Join-Path $crtFolder $dll) (Join-Path $Stage 'llama')
    }

    # Documents and licenses
    Copy-Required (Join-Path $PSScriptRoot 'package-windows-readme.txt') (Join-Path $Stage 'README.txt')
    Copy-Required (Join-Path $Repo 'LICENSE') (Join-Path $Stage 'LICENSE.txt')

    $vcpkgShare = Join-Path $env:VCPKG_ROOT 'installed\x64-windows\share'
    Copy-Required (Join-Path $vcpkgShare 'sdl3\copyright') (Join-Path $Stage 'licenses\SDL3.txt')
    Copy-Required (Join-Path $vcpkgShare 'curl\copyright') (Join-Path $Stage 'licenses\curl.txt')
    Copy-Required (Join-Path $vcpkgShare 'zlib\copyright') (Join-Path $Stage 'licenses\zlib.txt')
    Copy-Required (Join-Path $vcpkgShare 'nlohmann-json\copyright') (Join-Path $Stage 'licenses\nlohmann-json.txt')
    Copy-Required (Join-Path $Repo 'build\_deps\imgui-src\LICENSE.txt') (Join-Path $Stage 'licenses\imgui.txt')

    # The official llama.cpp release does not contain its own license
    $tag = (Get-Content -LiteralPath (Join-Path $Repo 'llama-release.json') -Raw -Encoding UTF8 | ConvertFrom-Json).tag
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing -OutFile (Join-Path $Stage 'licenses\llama.cpp.txt') `
        -Uri "https://raw.githubusercontent.com/ggml-org/llama.cpp/$tag/LICENSE"

    New-Zip -Folder $Stage -RootName $Name -ZipPath $ZipPath
}
finally {
    if (Test-Path -LiteralPath $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
}

$sizeMb = [math]::Round((Get-Item -LiteralPath $ZipPath).Length / 1MB, 1)
Write-Host "Created $ZipPath ($sizeMb MB)"
