# LocalLLM GUI — 環境構築手順 (Windows)

Windows 上で、Visual Studio (MSVC) と vcpkg を使って開発環境を構築する手順。
ビルドには Visual Studio のジェネレーターを使うので、Ninja や開発者用のシェル (Developer PowerShell for VS) は不要。
vcpkg の場所は環境変数 `VCPKG_ROOT` で指定する (この手順では例として `C:\tools\vcpkg` にインストールする)。

Ubuntu の手順は [SETUP.md](SETUP.md)、macOS の手順は [SETUP-macOS.md](SETUP-macOS.md) を参照。

アプリの構成 (GUI とワーカースレッドの関係など) は OS によらず同じなので、[SETUP.md](SETUP.md) の「9. GUI クライアントの構成」を参照。

## 1. 概要

| 項目 | 構成 |
|---|---|
| OS | Windows 10 / 11 (x64) |
| コンパイラ | MSVC (Visual Studio 2022 以降。Visual Studio 2026 / MSVC 14.51 で確認) |
| パッケージ管理 | vcpkg (場所は環境変数 `VCPKG_ROOT`。アプリはマニフェストモード、llama.cpp 用は `vcpkg install`) |
| ビルド | CMake + Visual Studio のジェネレーター (MSBuild。CMake 4.4 で確認) |
| ウィンドウ・描画 | SDL3 (SDL_Renderer) |
| GUI・テキスト描画 | Dear ImGui v1.92.9b |
| HTTP 通信 | libcurl (vcpkg) |
| JSON | nlohmann/json (vcpkg) |
| LLM サーバー | llama.cpp (Vulkan。Vulkan のヘッダー・ローダーと glslc は vcpkg) |

## 2. 必要なツール

### Visual Studio

Visual Studio 2022 以降 (Community で可) または Build Tools for Visual Studio をインストールし、ワークロード「**C++ によるデスクトップ開発**」を選ぶ。

このワークロードには MSVC、Windows SDK、MSBuild が含まれる。

### CMake

通常の PowerShell から `cmake` を使うため、CMake を単体でインストールして PATH に入れる (Visual Studio に付属の CMake は、開発者用のシェルからしか使えない)。

```powershell
winget install Kitware.CMake
```

Visual Studio 2026 を使う場合は、そのバージョンに対応した新しい CMake (`cmake --help` の Generators に `Visual Studio 18 2026` が出るもの) が必要。

### Git

[Git for Windows](https://git-scm.com/download/win) をインストールする。vcpkg の取得と、Dear ImGui の自動取得に使う。

### vcpkg

vcpkg はどこにインストールしてもよい。この手順では例として `C:\tools\vcpkg` に取得してセットアップする。別の場所にする場合は、以降の `C:\tools\vcpkg` を読み替える。

```powershell
New-Item -ItemType Directory -Force C:\tools

git clone https://github.com/microsoft/vcpkg.git C:\tools\vcpkg

C:\tools\vcpkg\bootstrap-vcpkg.bat -disableMetrics
```

環境変数 `VCPKG_ROOT` に、インストールした vcpkg のフォルダーを設定する。`setx` はユーザー環境変数に保存するので、設定後は新しいターミナルを開く (VS Code も開き直す)。

```powershell
setx VCPKG_ROOT C:\tools\vcpkg
```

新しいターミナルで、設定されたことを確認する。

```powershell
$env:VCPKG_ROOT
```

このプロジェクトは `VCPKG_ROOT` の vcpkg を使う。`CMakeLists.txt` が `VCPKG_ROOT` からツールチェーン (`scripts\buildsystems\vcpkg.cmake`) を見つけるので、プリセットを使っても使わなくても同じ vcpkg が使われる。`VCPKG_ROOT` が未設定のときや、そこに vcpkg が無いときは、configure がエラーで止まる。

Visual Studio 2022 以降には vcpkg が付属しており、開発者用のシェル (Developer PowerShell for VS など) は `VCPKG_ROOT` を付属の vcpkg に書き換える。付属の vcpkg はこのプロジェクトでは使えないので、configure は通常の PowerShell で行う (開発者用のシェルで実行すると、`CMakeLists.txt` がエラーで止める)。

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

プロジェクトのルートディレクトリで、通常の PowerShell から実行する。設定は [CMakePresets.json](CMakePresets.json) のプリセット `windows` にまとめてある (最新の Visual Studio のジェネレーター、x64)。

configure：

```powershell
cmake --preset windows
```

Debug ビルドと実行：

```powershell
cmake --build --preset windows-debug

.\build\Debug\LocalLLM.exe
```

Release ビルドと実行：

```powershell
cmake --build --preset windows-release

.\build\Release\LocalLLM.exe
```

Visual Studio のジェネレーターは Debug と Release を同じ `build\` で扱い、ビルド時に選ぶ (configure は1回でよい)。実行ファイルは `build\Debug\` または `build\Release\` に作られ、`Asset\` と vcpkg の DLL も同じフォルダーにコピーされる。

`build\LocalLLM.sln` を Visual Studio で開いて、ビルドやデバッグをすることもできる。

ソースには日本語の文字列リテラルが含まれるため、MSVC では `/utf-8` オプションを付けてビルドする (`CMakeLists.txt` で設定済み)。

### VS Code

C/C++ と CMake Tools の拡張機能を入れる。VS Code は通常どおり起動すればよい。

- CMake Tools：`CMakePresets.json` を使う設定にしてあり、Windows では configure のプリセット「Windows (Visual Studio, x64)」だけが表示される。
- タスク (Ctrl+Shift+B)：Windows では自動でプリセット `windows` / `windows-debug` を使う。
- F5：構成「Debug LocalLLM (Windows)」を選ぶと、Debug ビルドしてから Visual Studio のデバッガーで `build\Debug\LocalLLM.exe` をデバッグする。
  - 「実行とデバッグ」パネル (Ctrl+Shift+D) の上のプルダウンで、最初に「**Debug LocalLLM (Windows)**」を選んでおく。既定で選ばれる先頭の「Debug LocalLLM」は Linux / macOS 用で、Windows では起動に失敗する。一度選べば次回からはそれが使われる。

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

AMD Radeon などの GPU では Vulkan 版を使う。Vulkan SDK は使わず、ビルドに必要なものは vcpkg (`VCPKG_ROOT`) で入れる。

### 必要なライブラリ (vcpkg)

| パッケージ | 役割 |
|---|---|
| `vulkan` | Vulkan のヘッダーとローダー (`vulkan-headers`、`vulkan-loader` をまとめたもの) |
| `shaderc` | シェーダーコンパイラー `glslc` (llama.cpp のビルド時に GPU 用のシェーダーをコンパイルする) |
| `spirv-headers` | SPIR-V のヘッダー |
| `openssl` | HTTPS 通信 (`-hf` で Hugging Face からモデルをダウンロードするのに必要) |

```powershell
& "$env:VCPKG_ROOT\vcpkg.exe" install vulkan shaderc spirv-headers openssl --triplet x64-windows
```

Vulkan を実際に動かすのは GPU ドライバー (AMD Software など) なので、GPU ドライバーは最新にしておく。

### ビルド

通常の PowerShell で実行する。

```powershell
cd $env:USERPROFILE\Repos
git clone https://github.com/ggml-org/llama.cpp.git
cd llama.cpp

cmake -S . -B build -A x64 `
    -DGGML_VULKAN=ON `
    -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows `
    -DVulkan_GLSLC_EXECUTABLE="$env:VCPKG_ROOT/installed/x64-windows/tools/shaderc/glslc.exe"

cmake --build build --config Release
```

- `-A x64`：`-G` を省くと最新の Visual Studio のジェネレーターが使われる。`-A x64` で 64ビット向けにする。
- `--config Release`：Visual Studio のジェネレーターでは、Debug / Release をビルド時に選ぶ (`CMAKE_BUILD_TYPE` は使われない)。
- `Vulkan_GLSLC_EXECUTABLE`：CMake は glslc を Vulkan SDK のフォルダーで探すため、vcpkg の glslc は明示的に指定する。
- configure の出力に `OpenSSL found` が出ることを確認する。`OpenSSL not found, HTTPS support disabled` と出た場合は `-hf` が使えない。
- configure の出力に `... not supported by glslc` が出る場合は、glslc が対応していない GPU の機能が無効になる。ビルドは通るが、速度が落ちることがある。

実行ファイルは `build\bin\Release\` に作られる。vcpkg の DLL (`vulkan-1.dll`、`libssl-3-x64.dll` など) も同じフォルダーにコピーされる。

GPU 認識確認：

```powershell
cd build\bin\Release
.\llama-cli.exe --list-devices
```

AMD Radeon などの GPU が Vulkan デバイスとして表示されることを確認する。

### ビルド済みのバイナリを使う場合

ソースからビルドしない場合は、[llama.cpp の Releases](https://github.com/ggml-org/llama.cpp/releases) から Windows の Vulkan 版 (`llama-<バージョン>-bin-win-vulkan-x64.zip`) をダウンロードして展開しても使える。この場合、vcpkg のライブラリは不要。

### 起動スクリプト

モデルをメニューから選んで起動するサンプルスクリプトを [scripts/llama-server-windows.bat](scripts/llama-server-windows.bat) に置いてある。エクスプローラーからダブルクリックするか、コマンドプロンプト / PowerShell で実行する。

```powershell
.\scripts\llama-server-windows.bat
```

中身は [scripts/llama-server-windows.ps1](scripts/llama-server-windows.ps1) を、そのときだけ PowerShell の実行ポリシーを外して呼び出している (システムの設定は変わらない)。

メニューで番号を選ぶと、次の「モデル別の起動」と同じ設定 (`-ngl 99`、`-c 4096`、`--alias local`、`127.0.0.1:8080`) でサーバーを起動する。`q` で何もせずに終了する。

あわせて RAG 用の埋め込みサーバー (BGE-M3、`127.0.0.1:8081`、初回は約 600 MB をダウンロード) を裏で起動し、チャット用のサーバーを止めると一緒に止める。RAG を使わないときは `--no-embed` を付ける (`-NoEmbed` でもよい)。詳しくは [doc/rag.md](doc/rag.md) を参照。

llama-server.exe は既定で `%USERPROFILE%\Repos\llama.cpp\build\bin\Release\llama-server.exe` を使う。場所が違う場合 (ビルド済みのバイナリを使う場合など) は環境変数で指定する。

```powershell
$env:LLAMA_SERVER = "C:\path\to\llama-server.exe"
.\scripts\llama-server-windows.bat
```

### モデル別の起動

`build\bin\Release\` (ビルド済みのバイナリの場合は展開したフォルダー) で実行する。PowerShell では、行の継続にバッククォート (`` ` ``) を使う。

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
- configure が `VCPKG_ROOT is not set` や `vcpkg not found in VCPKG_ROOT` で止まる場合は、`$env:VCPKG_ROOT` が vcpkg のフォルダー (`scripts\buildsystems\vcpkg.cmake` があるところ) を指しているか確認する。`setx` の後は新しいターミナルを開く必要がある。
- `VCPKG_ROOT points to the vcpkg bundled with Visual Studio` というエラーや、`this vcpkg instance requires a manifest with a specified baseline` というエラーが出た場合は、Visual Studio 付属の vcpkg が使われている。開発者用のシェルではなく通常の PowerShell で configure する。
- vcpkg の場所はツールチェーンとして `build\` のキャッシュに記録される。`VCPKG_ROOT` を変えたときは、`build\` を消して configure し直す。
- 以前に Ninja など別のジェネレーターで configure した `build\` があると、`Does not match the generator used previously` というエラーになる。configure が途中で失敗した場合も含め、`Remove-Item -Recurse -Force build` で消してから configure し直す。vcpkg のライブラリはキャッシュ (`%LOCALAPPDATA%\vcpkg\archives`) から戻るので、2回目以降はすぐ終わる。
- `--host 127.0.0.1` で起動すると外部には公開されないため、通常は Windows ファイアウォールの許可は不要。
- llama.cpp のビルドで vcpkg の Vulkan と glslc を使う方法は、llama.cpp の公式手順 (Vulkan SDK を使う) とは異なる。うまくいかない場合は Vulkan SDK を入れるか、ビルド済みのバイナリを使う。
- この手順は Windows 11 + Visual Studio 2026 の実機で、環境構築から LocalLLM のビルド・実行まで確認済み。
