# --------------------------------------------------
# llama.cpp server launcher (Windows / Vulkan)
# Qwen3 / Gemma3 / Phi-4 mini
#
# Also starts the embedding server for RAG (BGE-M3, port 8081) in the
# background and stops it when the chat server exits.
# Its log goes to logs\embed-server.log (stderr: logs\embed-server.err.log).
#
# Override paths with environment variables:
#   LLAMA_DIR    llama.cpp directory (default: %USERPROFILE%\Repos\llama.cpp)
#   LLAMA_SERVER llama-server.exe (e.g. from the prebuilt release zip)
#
# Run:
#   scripts\llama-server-windows.bat  (calls this script with the policy bypassed)
#   or: powershell -ExecutionPolicy Bypass -File .\scripts\llama-server-windows.ps1
#   (add -NoEmbed to start the chat server only, without RAG)
# --------------------------------------------------

param(
    [switch]$NoEmbed
)

$ErrorActionPreference = 'Stop'

$LlamaDir = if ($env:LLAMA_DIR) { $env:LLAMA_DIR } else { Join-Path $env:USERPROFILE 'Repos\llama.cpp' }
$Server = if ($env:LLAMA_SERVER) { $env:LLAMA_SERVER } else { Join-Path $LlamaDir 'build\bin\Release\llama-server.exe' }

# $Host is a built-in variable in PowerShell
$HostAddr = '127.0.0.1'
$Port = 8080
$Context = 4096
$GpuLayers = 99

# Embedding server for RAG
# Each chunk must fit in one batch, so batch = context
$EmbedPort = 8081
$EmbedModel = 'ggml-org/bge-m3-Q8_0-GGUF'
$EmbedContext = 8192

$LogDir = Join-Path (Split-Path $PSScriptRoot -Parent) 'logs'
# Start-Process cannot send stdout and stderr to the same file
$LogFile = Join-Path $LogDir 'embed-server.log'
$ErrLogFile = Join-Path $LogDir 'embed-server.err.log'

if (-not (Test-Path -LiteralPath $Server -PathType Leaf)) {
    Write-Host "Error: llama-server not found: $Server"
    exit 1
}

Write-Host '=================================='
Write-Host ' Local LLM Server Launcher'
Write-Host '=================================='
Write-Host ''
Write-Host '  1) Qwen3 4B'
Write-Host '  2) Gemma 3 4B'
Write-Host '  3) Phi-4 mini'
Write-Host '  q) Quit'
Write-Host ''

$choice = Read-Host 'Select model [1-3]'

switch ($choice) {
    '1' {
        $Name = 'Qwen3 4B'
        $Model = 'Qwen/Qwen3-4B-GGUF:Q4_K_M'
    }
    '2' {
        $Name = 'Gemma 3 4B'
        $Model = 'ggml-org/gemma-3-4b-it-GGUF'
    }
    '3' {
        $Name = 'Phi-4 mini'
        $Model = 'bartowski/microsoft_Phi-4-mini-instruct-GGUF:Q4_K_M'
    }
    'q' {
        # switch is case-insensitive, so 'Q' also matches
        exit 0
    }
    default {
        Write-Host 'Invalid selection.'
        exit 1
    }
}

Write-Host ''
Write-Host '----------------------------------'
Write-Host " Model      : $Name"
Write-Host ' Backend    : Vulkan'
Write-Host " GPU layers : $GpuLayers"
Write-Host " Context    : $Context"
Write-Host " Endpoint   : http://${HostAddr}:$Port"
Write-Host ' Alias      : local'
if ($NoEmbed) {
    Write-Host ' Embedding  : off'
} else {
    Write-Host " Embedding  : BGE-M3 (http://${HostAddr}:$EmbedPort, alias embed)"
}
Write-Host '----------------------------------'
Write-Host ''

$Embed = $null

if (-not $NoEmbed) {
    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

    $Embed = Start-Process -FilePath $Server `
        -ArgumentList '-hf', $EmbedModel, '--embeddings',
            '-ngl', $GpuLayers, '-c', $EmbedContext,
            '-b', $EmbedContext, '-ub', $EmbedContext,
            '--alias', 'embed', '--host', $HostAddr, '--port', $EmbedPort `
        -RedirectStandardOutput $LogFile `
        -RedirectStandardError $ErrLogFile `
        -NoNewWindow -PassThru

    # Fail early if it could not start (e.g. port in use)
    Start-Sleep -Seconds 1
    if ($Embed.HasExited) {
        Write-Host "Error: embedding server exited. Log: $ErrLogFile"
        Get-Content -LiteralPath $ErrLogFile -Tail 20 -ErrorAction SilentlyContinue
        exit 1
    }

    Write-Host "Embedding server is running in the background (PID $($Embed.Id))"
    Write-Host "  Log: $LogFile"
    Write-Host ''
}

if (Test-Path -LiteralPath $LlamaDir -PathType Container) {
    Set-Location -LiteralPath $LlamaDir
}

# Stop the embedding server when the chat server exits (Ctrl+C included)
try {
    & $Server `
        -hf $Model `
        -ngl $GpuLayers `
        -c $Context `
        --alias local `
        --host $HostAddr `
        --port $Port

    $ExitCode = $LASTEXITCODE
}
finally {
    if ($Embed -and -not $Embed.HasExited) {
        Write-Host "Stopping embedding server (PID $($Embed.Id))..."
        Stop-Process -Id $Embed.Id -Force
    }
}

exit $ExitCode
