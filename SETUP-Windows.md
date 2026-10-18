# LocalLLM GUI — 環境構築手順 (Windows)

Windows 上で、Visual Studio (MSVC) と vcpkg を使って開発環境を構築する手順。
vcpkg は `C:\tools\vcpkg` にインストールすることを前提とする。

Ubuntu の手順は [SETUP.md](SETUP.md)、macOS の手順は [SETUP-macOS.md](SETUP-macOS.md) を参照。

アプリの構成 (GUI とワーカースレッドの関係など) は OS によらず同じなので、[SETUP.md](SETUP.md) の「9. GUI クライアントの構成」を参照。

## 1. 概要

| 項目 | 構成 |
|---|---|
| OS | Windows 10 / 11 (x64) |
| コンパイラ | MSVC (Visual Studio 2022 以降) |
| パッケージ管理 | vcpkg (`C:\tools\vcpkg`。アプリはマニフェストモード、llama.cpp 用は `vcpkg install`) |
| ビルド | CMake + Ninja |
| ウィンドウ・描画 | SDL3 (SDL_Renderer) |
| GUI・テキスト描画 | Dear ImGui v1.92.9b |
| HTTP 通信 | libcurl (vcpkg) |
| JSON | nlohmann/json (vcpkg) |
| LLM サーバー | llama.cpp (Vulkan。Vulkan のヘッダー・ローダーと glslc は vcpkg) |

## 2. 必要なツール

### Visual Studio

Visual Studio 2022 以降 (Community で可) または Build Tools for Visual Studio をインストールし、ワークロード「**C++ によるデスクトップ開発**」を選ぶ。

このワークロードには MSVC、Windows SDK、CMake、Ninja が含まれる。

### Git

[Git for Windows](https://git-scm.com/download/win) をインストールする。vcpkg の取得と、Dear ImGui の自動取得に使う。

### vcpkg

PowerShell で `C:\tools\vcpkg` に取得してセットアップする。

```powershell
New-Item -ItemType Directory -Force C:\tools

git clone https://github.com/microsoft/vcpkg.git C:\tools\vcpkg

C:\tools\vcpkg\bootstrap-vcpkg.bat -disableMetrics
```

環境変数 `VCPKG_ROOT` を設定しておく (設定後は新しいターミナルを開く)。

```powershell
setx VCPKG_ROOT C:\tools\vcpkg
```

`CMakeLists.txt` は Windows では `VCPKG_ROOT` の vcpkg を自動で使う。`VCPKG_ROOT` が未設定の場合は `C:\tools\vcpkg` を使う。

## 3. ライブラリ (vcpkg マニフェスト)

使うライブラリはリポジトリの [vcpkg.json](vcpkg.json) に書いてある。

| パッケージ | 役割 |
|---|---|
| `sdl3` | ウィンドウ、イベント、2D 描画、IME 入力 |
| `curl` | HTTP 通信 |
| `nlohmann-json` | JSON 処理 |

マニフェストモードなので、`vcpkg install` を手動で実行する必要はない。初回の CMake configure 時に vcpkg が自動でビルドして `build\vcpkg_installed\` に入れる (初回は数分かかる)。

Dear ImGui は vcpkg ではなく、CMake の FetchContent で GitHub から自動取得する。

ビルドしたライブラリの DLL (`SDL3.dll`、`libcurl.dll` など) は、vcpkg が実行ファイルと同じフォルダーへ自動でコピーする。

## 4. ビルド

MSVC の環境変数が必要なので、スタートメニューの「**Developer PowerShell for VS**」で、プロジェクトのルートディレクトリに移動して実行する。

Debug ビルド：

```powershell
cmake -S . -B build -G Ninja `
    -DCMAKE_BUILD_TYPE=Debug `
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build build
```

実行：

```powershell
.\build\LocalLLM.exe
```

Release ビルド：

```powershell
cmake -S . -B build-release -G Ninja `
    -DCMAKE_BUILD_TYPE=Release

cmake --build build-release
```

ソースには日本語の文字列リテラルが含まれるため、MSVC では `/utf-8` オプションを付けてビルドする (`CMakeLists.txt` で設定済み)。

### VS Code

C/C++ と CMake Tools の拡張機能を入れる。

- CMake Tools を使う場合：ステータスバーでキットに「Visual Studio ... - amd64」を選ぶ。CMake Tools が MSVC の環境を自動で設定する。
- タスク (Ctrl+Shift+B) を使う場合：Developer PowerShell for VS から `code .` で VS Code を起動する。通常のターミナルから起動すると `cl.exe` が見つからずビルドできない。
- F5：構成「Debug LocalLLM (Windows)」を選ぶと、Visual Studio のデバッガーでデバッグできる。

## 5. 日本語フォントと入力

日本語の表示には、プロジェクトに同梱したフリーフォント **M PLUS Rounded 1c Bold** (`Asset/MPLUSRounded1c-Bold.ttf`) を使う。配布元は [Google Fonts](https://fonts.google.com/specimen/M+PLUS+Rounded+1c) で、ライセンスは SIL Open Font License 1.1 (`Asset/OFL.txt`)。

ビルドすると `Asset/` が実行ファイルの隣 (`build\Asset\`) にコピーされ、アプリは実行ファイルの場所を基準にフォントを読み込む。そのため、どのフォルダーから起動しても同じフォントが使われる。

アプリは次の順にフォントを探す。

1. 環境変数 `LOCALLLM_FONT` で指定したファイル
2. 実行ファイルの隣の `Asset/MPLUSRounded1c-Bold.ttf`
3. OS の日本語フォント (下記)

M PLUS Rounded 1c に含まれない文字 (JIS 第3・第4水準の漢字など) は、OS の日本語フォントで補う (ImGui のフォント合成)。OS の日本語フォントが見つからない場合は、M PLUS Rounded 1c に無い文字は表示されない。

Windows で補完に使う OS のフォント：

- `C:\Windows\Fonts\YuGothM.ttc` (游ゴシック Medium)
- `C:\Windows\Fonts\meiryo.ttc` (メイリオ)
- `C:\Windows\Fonts\msgothic.ttc` (MS ゴシック)

別のフォントを使う場合：

```powershell
$env:LOCALLLM_FONT = "C:\path\to\font.ttf"
.\build\LocalLLM.exe
```

日本語入力 (IME) は ImGui の SDL3 バックエンドが処理する。

## 6. llama.cpp サーバー

AMD Radeon などの GPU では Vulkan 版を使う。Vulkan SDK は使わず、ビルドに必要なものは vcpkg (`C:\tools\vcpkg`) で入れる。

### 必要なライブラリ (vcpkg)

| パッケージ | 役割 |
|---|---|
| `vulkan` | Vulkan のヘッダーとローダー (`vulkan-headers`、`vulkan-loader` をまとめたもの) |
| `shaderc` | シェーダーコンパイラー `glslc` (llama.cpp のビルド時に GPU 用のシェーダーをコンパイルする) |
| `spirv-headers` | SPIR-V のヘッダー |
| `openssl` | HTTPS 通信 (`-hf` で Hugging Face からモデルをダウンロードするのに必要) |

```powershell
C:\tools\vcpkg\vcpkg.exe install vulkan shaderc spirv-headers openssl --triplet x64-windows
```

Vulkan を実際に動かすのは GPU ドライバー (AMD Software など) なので、GPU ドライバーは最新にしておく。

### ビルド

Developer PowerShell for VS で実行する。

```powershell
cd $env:USERPROFILE\Repos
git clone https://github.com/ggml-org/llama.cpp.git
cd llama.cpp

cmake -S . -B build -G Ninja `
    -DGGML_VULKAN=ON `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_TOOLCHAIN_FILE=C:/tools/vcpkg/scripts/buildsystems/vcpkg.cmake `
    -DVCPKG_TARGET_TRIPLET=x64-windows `
    -DVulkan_GLSLC_EXECUTABLE=C:/tools/vcpkg/installed/x64-windows/tools/shaderc/glslc.exe

cmake --build build
```

- `Vulkan_GLSLC_EXECUTABLE`：CMake は glslc を Vulkan SDK のフォルダーで探すため、vcpkg の glslc は明示的に指定する。
- configure の出力に `OpenSSL found` が出ることを確認する。`OpenSSL not found, HTTPS support disabled` と出た場合は `-hf` が使えない。
- configure の出力に `... not supported by glslc` が出る場合は、glslc が対応していない GPU の機能が無効になる。ビルドは通るが、速度が落ちることがある。

実行ファイルは `build\bin\` に作られる。vcpkg の DLL (`vulkan-1.dll`、`libssl-3-x64.dll` など) も同じフォルダーにコピーされる。

GPU 認識確認：

```powershell
cd build\bin
.\llama-cli.exe --list-devices
```

AMD Radeon などの GPU が Vulkan デバイスとして表示されることを確認する。

### ビルド済みのバイナリを使う場合

ソースからビルドしない場合は、[llama.cpp の Releases](https://github.com/ggml-org/llama.cpp/releases) から Windows の Vulkan 版 (`llama-<バージョン>-bin-win-vulkan-x64.zip`) をダウンロードして展開しても使える。この場合、vcpkg のライブラリは不要。

### モデル別の起動

`build\bin\` (ビルド済みのバイナリの場合は展開したフォルダー) で実行する。PowerShell では、行の継続にバッククォート (`` ` ``) を使う。

Qwen3 4B：

```powershell
.\llama-server.exe `
    -hf Qwen/Qwen3-4B-GGUF:Q4_K_M `
    -ngl 99 `
    -c 4096 `
    --alias local `
    --host 127.0.0.1 `
    --port 8080
```

Gemma 3 4B：

```powershell
.\llama-server.exe `
    -hf ggml-org/gemma-3-4b-it-GGUF `
    -ngl 99 `
    -c 4096 `
    --alias local `
    --host 127.0.0.1 `
    --port 8080
```

Phi-4 mini：

```powershell
.\llama-server.exe `
    -hf bartowski/microsoft_Phi-4-mini-instruct-GGUF:Q4_K_M `
    -ngl 99 `
    -c 4096 `
    --alias local `
    --host 127.0.0.1 `
    --port 8080
```

モデルを切り替える場合は、現在のサーバーを `Ctrl+C` で終了してから、別のモデルで起動する。

Hugging Face から取得したモデルは、通常 `%LOCALAPPDATA%\llama.cpp\` または `%USERPROFILE%\.cache\huggingface\hub\` 以下にキャッシュされる (llama.cpp のバージョンによる)。

## 7. クライアントとサーバーの接続

サーバーの稼働確認 (`{"status":"ok"}` が返れば正常)。
Windows PowerShell 5.1 では `curl` が `Invoke-WebRequest` の別名になっているため、`curl.exe` と書く。

```powershell
curl.exe http://127.0.0.1:8080/health
```

API 動作確認 (日本語を正しく送るため、本文を UTF-8 のバイト列にする)：

```powershell
$body = @{
    model    = "local"
    messages = @(@{ role = "user"; content = "こんにちは" })
    stream   = $false
} | ConvertTo-Json -Depth 5

Invoke-RestMethod http://127.0.0.1:8080/v1/chat/completions `
    -Method Post `
    -ContentType "application/json; charset=utf-8" `
    -Body ([Text.Encoding]::UTF8.GetBytes($body))
```

## 8. 注意事項

- `build\` は vcpkg のライブラリも含むため、Linux や macOS の `build/` とは共有できない。
- `--host 127.0.0.1` で起動すると外部には公開されないため、通常は Windows ファイアウォールの許可は不要。
- llama.cpp のビルドで vcpkg の Vulkan と glslc を使う方法は、llama.cpp の公式手順 (Vulkan SDK を使う) とは異なる。うまくいかない場合は Vulkan SDK を入れるか、ビルド済みのバイナリを使う。
- 実機 (Windows) でのビルド・動作はまだ確認していない。
