# --------------------------------------------------
# llama.cpp embedding server launcher for RAG (Windows / Vulkan)
# BGE-M3 (multilingual)
#
# Override paths with environment variables:
#   LLAMA_DIR    llama.cpp directory (default: %USERPROFILE%\Repos\llama.cpp)
#   LLAMA_SERVER llama-server.exe (e.g. from the prebuilt release zip)
#
# Run this alongside llama-server-windows.ps1 (port 8080):
#   powershell -ExecutionPolicy Bypass -File .\scripts\embed-server-windows.ps1
# --------------------------------------------------

$ErrorActionPreference = 'Stop'

$LlamaDir = if ($env:LLAMA_DIR) { $env:LLAMA_DIR } else { Join-Path $env:USERPROFILE 'Repos\llama.cpp' }
$Server = if ($env:LLAMA_SERVER) { $env:LLAMA_SERVER } else { Join-Path $LlamaDir 'build\bin\Release\llama-server.exe' }

# $Host is a built-in variable in PowerShell
$HostAddr = '127.0.0.1'
$Port = 8081
$Model = 'ggml-org/bge-m3-Q8_0-GGUF'
# Each chunk must fit in one batch, so batch = context
$Context = 8192
$GpuLayers = 99

if (-not (Test-Path -LiteralPath $Server -PathType Leaf)) {
    Write-Host "Error: llama-server not found: $Server"
    exit 1
}

Write-Host '----------------------------------'
Write-Host ' Embedding  : BGE-M3'
Write-Host ' Backend    : Vulkan'
Write-Host " Endpoint   : http://${HostAddr}:$Port"
Write-Host ' Alias      : embed'
Write-Host '----------------------------------'
Write-Host ''

if (Test-Path -LiteralPath $LlamaDir -PathType Container) {
    Set-Location -LiteralPath $LlamaDir
}

& $Server `
    -hf $Model `
    --embeddings `
    -ngl $GpuLayers `
    -c $Context `
    -b $Context `
    -ub $Context `
    --alias embed `
    --host $HostAddr `
    --port $Port

exit $LASTEXITCODE
