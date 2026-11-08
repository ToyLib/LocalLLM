# LocalLLM GUI — 環境構築手順 (Windows)

Windows 上で、Visual Studio (MSVC) と vcpkg を使って開発環境を構築する手順。
ビルドには Visual Studio のジェネレーターを使うので、Ninja や開発者用のシェル (Developer PowerShell for VS) は不要。
vcpkg の場所は環境変数 `VCPKG_ROOT` で指定する (この手順では例として `C:\tools\vcpkg` にインストールする)。

OS によらない部分 (プロジェクト構成、フォント、モデル別の起動、動作確認) は [SETUP.md](SETUP.md) にまとめている。

## 1. 概要

| 項目 | 構成 |
|---|---|
| OS | Windows 10 / 11 (x64) |
| コンパイラ | MSVC (Visual Studio 2022 以降。Visual Studio 2026 / MSVC 14.51 で確認) |
| パッケージ管理 | vcpkg (場所は環境変数 `VCPKG_ROOT`。Ubuntu の APT や macOS の Homebrew と同じく、`vcpkg install` で先に入れておく) |
| ビルド | CMake + Visual Studio のジェネレーター (MSBuild。CMake 4.4 で確認) |
| ウィンドウ・描画 | SDL3 (SDL_Renderer、vcpkg) |
| HTTP 通信 | libcurl (vcpkg) |
| JSON | nlohmann/json (vcpkg) |
| LLM サーバー | llama.cpp (公式リリースの Vulkan 版を同梱) |

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

## 3. ライブラリ (vcpkg)

Ubuntu の APT や macOS の Homebrew と同じように、使うライブラリを先にインストールしておく。

| パッケージ | 役割 |
|---|---|
| `sdl3` | ウィンドウ、イベント、2D 描画、IME 入力 |
| `curl` | HTTP 通信 |
| `nlohmann-json` | JSON 処理 |

```powershell
& "$env:VCPKG_ROOT\vcpkg.exe" install sdl3 curl nlohmann-json --triplet x64-windows
```

vcpkg がソースからビルドして `VCPKG_ROOT` の `installed\x64-windows\` に入れる (初回は数分かかる)。一度入れれば、`build\` を消して configure し直してもビルドし直しにはならない。

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
- F5：「実行とデバッグ」パネル (Ctrl+Shift+D) の上のプルダウンで選んだ構成で、ビルドしてから Visual Studio のデバッガーで起動する。
  - 「**Launch LocalLLM (Windows, Debug)**」は `build\Debug\LocalLLM.exe`、「**Launch LocalLLM (Windows, Release)**」は `build\Release\LocalLLM.exe` を起動する。一度選べば次回からはそれが使われる。

### 配布用の ZIP を作る

Visual Studio や vcpkg が無い PC でも動く ZIP を作る。通常の PowerShell で実行する (Developer PowerShell は使わない)。

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\package-windows.ps1
```

Release をビルドし、`dist\LocalLLM-windows-x64-<バージョン>.zip` を作る。バージョンは既定で git のコミット (`-Version 1.0.0` で指定できる)。今の `build\Release` をそのまま使うときは `-SkipBuild` を付ける。

- Visual C++ ランタイムを `LocalLLM.exe` の隣と `llama\` の両方に同梱するので、受け取る側のインストールは要らない。
- `rag\` は `README.md` と `samples\` だけを同梱する。自分で追加した文書と、生成された `rag-index.json` は入らない。
- 同梱ライブラリのライセンスは `licenses\` に入る。llama.cpp のライセンスは公式リリースに含まれないので、実行時に GitHub から取得する。
- 利用者向けの説明は [scripts/package-windows-readme.txt](scripts/package-windows-readme.txt) で、ZIP の `README.txt` になる。

## 5. 日本語フォント

フォントの探し方は [SETUP.md](SETUP.md) の「5. 日本語フォントと入力」を参照。

Windows では、同梱フォントに無い文字を游ゴシック (`C:\Windows\Fonts\YuGothM.ttc`)、メイリオ、MS ゴシックの順で補う。追加のインストールは要らない。

別のフォントを環境変数で指定する場合：

```powershell
$env:LOCALLLM_FONT = "C:\path\to\font.ttf"
.\build\Debug\LocalLLM.exe
```

## 6. llama.cpp サーバー

ビルドすると、公式リリースの Vulkan 版 (`llama-<タグ>-bin-win-vulkan-x64.zip`) が取得され、`LocalLLM.exe` の隣の `llama\` に同梱される。アプリが起動・停止するので、llama.cpp のビルドも起動も要らない。使い方は [SETUP.md](SETUP.md) の「6. llama.cpp サーバー」を参照。

Vulkan を実際に動かすのは GPU ドライバー (AMD Software など) なので、GPU ドライバーは最新にしておく。GPU が見つからないときは、設定画面の「サーバー」のタブに注意が出る。

自分で用意した `llama-server.exe` を使うときは、設定画面の「llama-server の場所」か環境変数 `LLAMA_SERVER` で指定する。

### 自分で起動するとき

```powershell
.\scripts\llama-server-windows.bat
```

エクスプローラーからダブルクリックしてもよい。中身は [scripts/llama-server-windows.ps1](scripts/llama-server-windows.ps1) を、そのときだけ PowerShell の実行ポリシーを外して呼び出している (システムの設定は変わらない)。

メニューや埋め込みサーバー、`--no-embed` (`-NoEmbed` でもよい) については [SETUP.md](SETUP.md) の「6. llama.cpp サーバー」を参照。

llama-server.exe は既定で `%USERPROFILE%\Repos\llama.cpp\build\bin\Release\llama-server.exe` を使う。場所が違う場合 (ビルド済みのバイナリを使う場合など) は環境変数で指定する。

```powershell
$env:LLAMA_SERVER = "C:\path\to\llama-server.exe"
.\scripts\llama-server-windows.bat
```

スクリプトを使わずに起動する場合は、`build\bin\Release\` (ビルド済みのバイナリの場合は展開したフォルダー) で実行する。PowerShell では、行の継続にバッククォート (`` ` ``) を使う (Qwen3 4B 2507。ほかのモデルの `-hf` の値は [SETUP.md](SETUP.md) の「モデル別の起動」)。

```powershell
.\llama-server.exe `
    -hf unsloth/Qwen3-4B-Instruct-2507-GGUF:Q4_K_M `
    -ngl 99 `
    -c 4096 `
    --alias local `
    --host 127.0.0.1 `
    --port 8080
```

ダウンロードしたモデルの保存場所と削除のしかたは [SETUP.md](SETUP.md) の「ダウンロードしたモデルの保存場所と削除」を参照。

## 7. クライアントとサーバーの接続

接続先や動作確認の手順は [SETUP.md](SETUP.md) の「7. クライアントとサーバーの接続」と「8. 動作確認手順」を参照。ここでは PowerShell での書き方だけを示す。

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

- `build\` は Visual Studio のジェネレーターで作るため、Linux や macOS の `build/` とは共有できない。
- configure が `Could not find a package configuration file provided by "SDL3"` などで止まる場合は、「3. ライブラリ」の `vcpkg install` が済んでいるか確認する。
- 以前のマニフェストモード (`vcpkg.json`) で configure した `build\` が残っている場合は、`build\` を消してから configure し直す。
- configure が `VCPKG_ROOT is not set` や `vcpkg not found in VCPKG_ROOT` で止まる場合は、`$env:VCPKG_ROOT` が vcpkg のフォルダー (`scripts\buildsystems\vcpkg.cmake` があるところ) を指しているか確認する。`setx` の後は新しいターミナルを開く必要がある。
- `VCPKG_ROOT points to the vcpkg bundled with Visual Studio` というエラーや、`This vcpkg distribution does not have a classic mode instance` というエラーが出た場合は、Visual Studio 付属の vcpkg が使われている。開発者用のシェルではなく通常の PowerShell で configure する。
- vcpkg の場所はツールチェーンとして `build\` のキャッシュに記録される。`VCPKG_ROOT` を変えたときは、`build\` を消して configure し直す。
- 以前に Ninja など別のジェネレーターで configure した `build\` があると、`Does not match the generator used previously` というエラーになる。configure が途中で失敗した場合も含め、`Remove-Item -Recurse -Force build` で消してから configure し直す。vcpkg のライブラリは `VCPKG_ROOT` 側にあるので、消してもビルドし直しにはならない。
- `--host 127.0.0.1` で起動すると外部には公開されないため、通常は Windows ファイアウォールの許可は不要。
- llama.cpp のビルドで vcpkg の Vulkan と glslc を使う方法は、llama.cpp の公式手順 (Vulkan SDK を使う) とは異なる。うまくいかない場合は Vulkan SDK を入れるか、ビルド済みのバイナリを使う。
- この手順は Windows 11 + Visual Studio 2026 の実機で、環境構築から LocalLLM のビルド・実行まで確認済み。
