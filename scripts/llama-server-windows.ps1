# --------------------------------------------------
# llama.cpp server launcher (Windows / Vulkan)
# Qwen3 / Gemma3 / Phi-4 mini
#
# Override paths with environment variables:
#   LLAMA_DIR    llama.cpp directory (default: %USERPROFILE%\Repos\llama.cpp)
#   LLAMA_SERVER llama-server.exe (e.g. from the prebuilt release zip)
#
# Run:
#   powershell -ExecutionPolicy Bypass -File .\scripts\llama-server-windows.ps1
# --------------------------------------------------

$ErrorActionPreference = 'Stop'

$LlamaDir = if ($env:LLAMA_DIR) { $env:LLAMA_DIR } else { Join-Path $env:USERPROFILE 'Repos\llama.cpp' }
$Server = if ($env:LLAMA_SERVER) { $env:LLAMA_SERVER } else { Join-Path $LlamaDir 'build\bin\Release\llama-server.exe' }

# $Host is a built-in variable in PowerShell
$HostAddr = '127.0.0.1'
$Port = 8080
$Context = 4096
$GpuLayers = 99

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
Write-Host '----------------------------------'
Write-Host ''

if (Test-Path -LiteralPath $LlamaDir -PathType Container) {
    Set-Location -LiteralPath $LlamaDir
}

& $Server `
    -hf $Model `
    -ngl $GpuLayers `
    -c $Context `
    --alias local `
    --host $HostAddr `
    --port $Port

exit $LASTEXITCODE
