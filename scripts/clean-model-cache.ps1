# --------------------------------------------------
# Model cache cleaner (Windows)
#
# Lists the GGUF models that llama-server downloaded with -hf, with their
# sizes, and deletes the ones you pick. Deleted models are downloaded
# again the next time the server starts with them.
#
# Where models are searched:
#   - Hugging Face cache (new llama.cpp):
#       HF_HUB_CACHE, HF_HOME\hub, %USERPROFILE%\.cache\huggingface\hub
#   - llama.cpp cache (old llama.cpp):
#       LLAMA_CACHE, %LOCALAPPDATA%\llama.cpp
#
# The Hugging Face cache is shared with other tools, so only folders that
# contain .gguf files are listed. The old llama.cpp cache keeps all models
# in one folder, so it is listed (and deleted) as a whole.
#
# Run:
#   scripts\clean-model-cache.bat  (calls this script with the policy bypassed)
#   or: powershell -ExecutionPolicy Bypass -File .\scripts\clean-model-cache.ps1
#   (add -List to only list the models)
# --------------------------------------------------

param(
    [switch]$List
)

$ErrorActionPreference = 'Stop'

# Candidate folders (duplicates are skipped below)
$Candidates = @()
if ($env:HF_HUB_CACHE) { $Candidates += $env:HF_HUB_CACHE }
if ($env:HF_HOME) { $Candidates += (Join-Path $env:HF_HOME 'hub') }
$Candidates += (Join-Path $env:USERPROFILE '.cache\huggingface\hub')
if ($env:LLAMA_CACHE) { $Candidates += $env:LLAMA_CACHE }
if ($env:LOCALAPPDATA) { $Candidates += (Join-Path $env:LOCALAPPDATA 'llama.cpp') }

# Models used by the launcher script (marked in the list)
$UsedModels = @()
$Launcher = Join-Path $PSScriptRoot 'llama-server-windows.ps1'
if (Test-Path -LiteralPath $Launcher) {
    $UsedModels = Select-String -LiteralPath $Launcher -Pattern "'([^']*-GGUF[^']*)'" -AllMatches |
        ForEach-Object { $_.Matches } |
        ForEach-Object { ($_.Groups[1].Value -split ':')[0] } |
        Sort-Object -Unique
}

function Get-FolderSize([string]$Path) {
    $bytes = (Get-ChildItem -LiteralPath $Path -Recurse -File -Force -ErrorAction SilentlyContinue |
        Where-Object { -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) } |
        Measure-Object -Property Length -Sum).Sum
    if (-not $bytes) { $bytes = 0 }
    if ($bytes -ge 1GB) { return '{0:N1}G' -f ($bytes / 1GB) }
    return '{0:N0}M' -f ($bytes / 1MB)
}

$Items = @()
$Seen = @{}

foreach ($dir in $Candidates) {
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) { continue }

    $real = (Resolve-Path -LiteralPath $dir).ProviderPath.TrimEnd('\')
    if ($Seen.ContainsKey($real.ToLower())) { continue }
    $Seen[$real.ToLower()] = $true

    # Hugging Face layout: one models--<owner>--<repo> folder per repository
    foreach ($model in Get-ChildItem -LiteralPath $real -Directory -Filter 'models--*' -ErrorAction SilentlyContinue) {
        $gguf = Get-ChildItem -LiteralPath $model.FullName -Recurse -Filter '*.gguf' -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if (-not $gguf) { continue }

        # models--<owner>--<repo> -> <owner>/<repo> (only the first -- is the separator)
        $repo = (($model.Name -replace '^models--', '') -split '--', 2) -join '/'
        $label = $repo
        if ($UsedModels -contains $repo) { $label += '  [launcher]' }

        $Items += [pscustomobject]@{ Path = $model.FullName; Label = $label }
    }

    # Old llama.cpp layout: .gguf files directly in the folder
    $flat = Get-ChildItem -LiteralPath $real -File -Filter '*.gguf' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($flat) {
        $Items += [pscustomobject]@{ Path = $real; Label = '(old llama.cpp cache, all models)' }
    }
}

if ($Items.Count -eq 0) {
    Write-Host 'No downloaded GGUF models found.'
    exit 0
}

Write-Host '=================================='
Write-Host ' Model Cache Cleaner'
Write-Host '=================================='
Write-Host ''

for ($i = 0; $i -lt $Items.Count; $i++) {
    $size = Get-FolderSize $Items[$i].Path
    Write-Host ('  {0,2}) {1,6}  {2}' -f ($i + 1), $size, $Items[$i].Label)
    Write-Host ('               {0}' -f $Items[$i].Path)
}

Write-Host ''
Write-Host '  [launcher] = offered by llama-server-windows.ps1 (bge-m3 is the RAG embedding model)'
Write-Host ''

if ($List) { exit 0 }

$answer = Read-Host 'Models to delete (e.g. 1 3, a = all, q = quit)'

$Selected = @()

if ($answer -eq '' -or $answer -eq 'q') {
    # -eq is case-insensitive, so 'Q' also matches
    exit 0
}
elseif ($answer -eq 'a') {
    $Selected = 0..($Items.Count - 1)
}
else {
    foreach ($n in ($answer -split '[,\s]+' | Where-Object { $_ })) {
        $num = 0
        if (-not [int]::TryParse($n, [ref]$num) -or $num -lt 1 -or $num -gt $Items.Count) {
            Write-Host "Invalid selection: $n"
            exit 1
        }
        $Selected += ($num - 1)
    }
    $Selected = $Selected | Sort-Object -Unique
}

Write-Host ''
Write-Host 'These will be deleted (they cannot be restored, only downloaded again):'
foreach ($i in $Selected) {
    Write-Host "  $($Items[$i].Label)"
    Write-Host "    $($Items[$i].Path)"
}
Write-Host ''

if (Get-Process -Name 'llama-server' -ErrorAction SilentlyContinue) {
    Write-Host 'Warning: llama-server is running. Stop it first; Windows cannot delete files that are in use.'
    Write-Host ''
}

$confirm = Read-Host 'Delete? [y/N]'
if ($confirm -ne 'y') {
    Write-Host 'Cancelled.'
    exit 0
}

foreach ($i in $Selected) {
    $path = $Items[$i].Path

    # Only delete what this script listed
    if (Test-Path -LiteralPath $path -PathType Container) {
        Remove-Item -LiteralPath $path -Recurse -Force
        Write-Host "Deleted: $($Items[$i].Label)"
    }
}
