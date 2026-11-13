# --------------------------------------------------
# Uninstaller (Windows)
#
# Removes what LocalLLM leaves outside its own folder. Two groups are
# listed with their sizes, and each one is confirmed separately:
#
#   1. Settings and data of LocalLLM (the whole folder, including the
#      API key, the logs and the downloaded llama-server):
#        %APPDATA%\LocalLLM
#   2. Models that llama-server downloaded:
#        Hugging Face cache (new llama.cpp):
#          HF_HUB_CACHE, HF_HOME\hub, %USERPROFILE%\.cache\huggingface\hub
#        llama.cpp cache (old llama.cpp):
#          %LOCALAPPDATA%\llama.cpp, LLAMA_CACHE
#
# The Hugging Face cache is shared with other tools, so only the model
# folders that contain .gguf files are removed. The old llama.cpp cache
# belongs to llama.cpp, so it is removed as a whole.
#
# The application itself (the repository or the extracted folder) and a
# folder set with LOCALLLM_CONFIG_DIR are not touched.
#
# Run:
#   scripts\uninstall.bat  (calls this script with the policy bypassed)
#   or: powershell -ExecutionPolicy Bypass -File .\scripts\uninstall.ps1
#   (add -List to only list what would be deleted)
# --------------------------------------------------

param(
    [switch]$List
)

$ErrorActionPreference = 'Stop'

if (-not $env:APPDATA -or -not $env:USERPROFILE) {
    Write-Host 'APPDATA or USERPROFILE is not set.'
    exit 1
}

# Same place as UserDataDirectory (src/common/config.cpp) and user_data_directory (python/common/config.py)
$DataDir = Join-Path $env:APPDATA 'LocalLLM'

# Whole: never  = only the GGUF model folders inside (shared with other tools)
#        always = the folder itself (it belongs to llama.cpp)
#        gguf   = the folder itself if it has .gguf files directly in it
# The Hugging Face folders come first: a folder listed twice must be treated as shared
$Candidates = @()
if ($env:HF_HUB_CACHE) { $Candidates += [pscustomobject]@{ Path = $env:HF_HUB_CACHE; Whole = 'never' } }
if ($env:HF_HOME) { $Candidates += [pscustomobject]@{ Path = (Join-Path $env:HF_HOME 'hub'); Whole = 'never' } }
$Candidates += [pscustomobject]@{ Path = (Join-Path $env:USERPROFILE '.cache\huggingface\hub'); Whole = 'never' }
if ($env:LOCALAPPDATA) { $Candidates += [pscustomobject]@{ Path = (Join-Path $env:LOCALAPPDATA 'llama.cpp'); Whole = 'always' } }
if ($env:LLAMA_CACHE) { $Candidates += [pscustomobject]@{ Path = $env:LLAMA_CACHE; Whole = 'gguf' } }

# A cache folder set by the environment must not take a drive or the profile with it
function Test-ProtectedFolder([string]$Path) {
    $folder = $Path.TrimEnd('\') + '\'
    $root = [IO.Path]::GetPathRoot($Path).TrimEnd('\') + '\'
    $userFolder = $env:USERPROFILE.TrimEnd('\') + '\'

    if ($folder -eq $root) { return $true }
    return $userFolder.StartsWith($folder, [StringComparison]::OrdinalIgnoreCase)
}

function Get-FolderSize([string]$Path) {
    $bytes = (Get-ChildItem -LiteralPath $Path -Recurse -File -Force -ErrorAction SilentlyContinue |
        Where-Object { -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) } |
        Measure-Object -Property Length -Sum).Sum
    if (-not $bytes) { $bytes = 0 }
    if ($bytes -ge 1GB) { return '{0:N1}G' -f ($bytes / 1GB) }
    return '{0:N0}M' -f ($bytes / 1MB)
}

$Models = @()
$Seen = @{}

foreach ($candidate in $Candidates) {
    if (-not (Test-Path -LiteralPath $candidate.Path -PathType Container)) { continue }

    $real = (Resolve-Path -LiteralPath $candidate.Path).ProviderPath
    $key = $real.TrimEnd('\').ToLower()
    if ($Seen.ContainsKey($key)) { continue }
    $Seen[$key] = $true

    $whole = $candidate.Whole -eq 'always'
    if ($candidate.Whole -eq 'gguf') {
        $flat = Get-ChildItem -LiteralPath $real -File -Filter '*.gguf' -ErrorAction SilentlyContinue |
            Select-Object -First 1
        $whole = [bool]$flat
    }

    if ($whole) {
        if (Test-ProtectedFolder $real) {
            Write-Host "Skipped (contains your profile folder): $real"
        }
        else {
            $Models += [pscustomobject]@{ Path = $real; Label = '(old llama.cpp cache, all models)'; Lock = '' }
            continue
        }
    }

    # Hugging Face layout: one models--<owner>--<repo> folder per repository
    foreach ($model in Get-ChildItem -LiteralPath $real -Directory -Filter 'models--*' -ErrorAction SilentlyContinue) {
        $gguf = Get-ChildItem -LiteralPath $model.FullName -Recurse -Filter '*.gguf' -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if (-not $gguf) { continue }

        # models--<owner>--<repo> -> <owner>/<repo> (only the first -- is the separator)
        $repo = (($model.Name -replace '^models--', '') -split '--', 2) -join '/'

        $Models += [pscustomobject]@{
            Path  = $model.FullName
            Label = $repo
            Lock  = (Join-Path (Join-Path $real '.locks') $model.Name)
        }
    }
}

$HasData = Test-Path -LiteralPath $DataDir

if (-not $HasData -and $Models.Count -eq 0) {
    Write-Host 'Nothing to remove.'
    exit 0
}

Write-Host '=================================='
Write-Host ' LocalLLM Uninstaller'
Write-Host '=================================='
Write-Host ''

if ($HasData) {
    Write-Host '[1] Settings and data (settings, API key, logs, downloaded llama-server)'
    Write-Host ('  {0,6}  {1}' -f (Get-FolderSize $DataDir), $DataDir)
}
else {
    Write-Host '[1] Settings and data: none'
}
Write-Host ''

if ($Models.Count -gt 0) {
    Write-Host '[2] Downloaded models (bge-m3 is the RAG embedding model)'
    foreach ($model in $Models) {
        Write-Host ('  {0,6}  {1}' -f (Get-FolderSize $model.Path), $model.Label)
        Write-Host ('          {0}' -f $model.Path)
    }
}
else {
    Write-Host '[2] Downloaded models: none'
}
Write-Host ''

if ($env:LOCALLLM_CONFIG_DIR) {
    Write-Host "Not touched: LOCALLLM_CONFIG_DIR ($env:LOCALLLM_CONFIG_DIR)"
    Write-Host ''
}

if ($List) { exit 0 }

if (Get-Process -Name 'llama-server', 'LocalLLM' -ErrorAction SilentlyContinue) {
    Write-Host 'Warning: LocalLLM or llama-server is running. Quit it first; Windows cannot delete files that are in use.'
    Write-Host ''
}

# Everything is asked first, so that nothing is deleted before the last answer
$DeleteData = $false
$DeleteModels = $false

if ($HasData) {
    $DeleteData = (Read-Host 'Delete [1] settings and data? The API key is deleted too and cannot be restored. [y/N]') -eq 'y'
}

if ($Models.Count -gt 0) {
    $DeleteModels = (Read-Host 'Delete [2] all downloaded models listed above? Other tools that use them will need them downloaded again. [y/N]') -eq 'y'
}

if (-not $DeleteData -and -not $DeleteModels) {
    Write-Host 'Nothing was deleted.'
    exit 0
}

Write-Host ''

function Remove-Target([string]$Path, [string]$Label) {
    try {
        if (Test-Path -LiteralPath $Path) {
            Remove-Item -LiteralPath $Path -Recurse -Force
        }
        Write-Host "Deleted: $Label"
        return $true
    }
    catch {
        Write-Host "Failed: $Label ($Path): $($_.Exception.Message)"
        return $false
    }
}

$Failed = $false

if ($DeleteData) {
    if (-not (Remove-Target $DataDir 'settings and data')) { $Failed = $true }
}

if ($DeleteModels) {
    foreach ($model in $Models) {
        if (-not (Remove-Target $model.Path $model.Label)) { $Failed = $true }

        if ($model.Lock -and (Test-Path -LiteralPath $model.Lock)) {
            Remove-Item -LiteralPath $model.Lock -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

Write-Host ''
Write-Host 'The application folder itself was not removed. Delete it yourself if you no longer need it.'

if ($Failed) { exit 1 }
