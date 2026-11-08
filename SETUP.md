# LocalLLM GUI — 環境構築手順 (共通)

OS によらない環境構築の手順と説明をまとめた手順書。パッケージのインストールや llama.cpp のビルドなど、OS ごとに違う部分は次の手順書に書いている。

| OS | 手順書 | パッケージ管理 |
|---|---|---|
| Ubuntu | [SETUP-Ubuntu.md](SETUP-Ubuntu.md) | APT |
| CachyOS / Arch Linux | [SETUP-Arch.md](SETUP-Arch.md) | pacman |
| macOS | [SETUP-macOS.md](SETUP-macOS.md) | Homebrew |
| Windows | [SETUP-Windows.md](SETUP-Windows.md) | vcpkg |

## 1. 概要

C++20 と SDL3 + Dear ImGui を使ったローカル LLM チャットクライアントの開発環境を構築する。

LLM 本体は別プロセスの `llama-server` で実行し、クライアントは OpenAI 互換の HTTP API を通じて通信する。`llama-server` はアプリが起動・停止するので、手で立ち上げる必要はない。

| 項目 | 構成 |
|---|---|
| 言語 | C++20 |
| ビルド | CMake (Linux / macOS: Unix Makefiles、Windows: Visual Studio) |
| ウィンドウ・描画 | SDL3 (SDL_Renderer) |
| GUI・テキスト描画 | Dear ImGui v1.92.9b |
| HTTP 通信 | libcurl |
| JSON | nlohmann/json |
| LLM サーバー | llama.cpp (Linux / Windows: Vulkan、macOS: Metal) |
| 対応モデル | Qwen3 4B 2507 / Gemma 3 4B / Phi-4 mini / gpt-oss 20B |

## 2. 手順の流れ

| 手順 | 書いてある場所 |
|---|---|
| 1. コンパイラとライブラリを入れる | OS ごとの手順書 |
| 2. LocalLLM をビルドする | 「4. ビルド」と OS ごとの手順書 |
| 3. アプリを起動する (`llama-server` はビルドで同梱され、アプリが起動する) | 「6. llama.cpp サーバー」 |
| 4. つながることを確かめる | 「8. 動作確認手順」 |

Python 版 (`python/`) はビルドが要らず、Python 3.10 以上と Tk があれば `python3 python/main.py` で動く (README の「Python 版」と [doc/python-app.md](doc/python-app.md))。`llama-server` が見つからなければ、初回の起動時に公式リリースをユーザーごとのフォルダーへダウンロードする。

## 3. プロジェクト構成

```text
LocalLLM/
├── CMakeLists.txt
├── CMakePresets.json   (OS 別のビルド設定)
├── README.md
├── SETUP.md            (この手順書: 共通)
├── SETUP-Ubuntu.md     (Ubuntu)
├── SETUP-Arch.md       (CachyOS / Arch Linux)
├── SETUP-macOS.md
├── SETUP-Windows.md
├── LICENSE
├── llama-release.json  (同梱する llama-server の公式リリースのタグとハッシュ)
├── config/                 (設定の既定値)
│   ├── ui.json
│   ├── server.json
│   ├── agents.json
│   └── personas.json
├── scripts/
│   ├── clean-model-cache.sh       (ダウンロードしたモデルの削除、Linux / macOS)
│   ├── clean-model-cache.bat
│   ├── clean-model-cache.ps1      (bat から呼ばれる本体)
│   ├── llama-server-linux.sh
│   ├── llama-server-macos.sh
│   ├── llama-server-windows.bat
│   ├── llama-server-windows.ps1   (bat から呼ばれる本体)
│   ├── package-windows.ps1        (Windows の配布用 ZIP を作る)
│   └── package-windows-readme.txt (ZIP に入れる利用者向けの説明)
├── Asset/
│   ├── MPLUSRounded1c-Bold.ttf   (同梱フォント)
│   └── OFL.txt                   (フォントのライセンス)
├── rag/                    (RAG の資料。samples/ と README.md 以外は git で無視)
│   ├── README.md
│   └── samples/
├── doc/
│   ├── architecture.md   (全体の構成)
│   ├── config.md         (設定ファイルと環境変数)
│   ├── privacy.md        (データの行き先)
│   ├── llm-client.md     (LLM との通信)
│   ├── gui-app.md        (GUI アプリ)
│   ├── agent.md          (エージェント)
│   ├── rag.md            (RAG)
│   ├── chatgpt.md        (ChatGPT に聞き直す)
│   ├── chatgpt-plan.md   (ChatGPT 連携の設計メモ)
│   ├── managed-server.md (llama-server の自動起動)
│   ├── managed-server-plan.md (自動起動の設計メモ)
│   └── python-app.md     (Python 版)
├── src/
│   ├── main.cpp
│   ├── chat.h / chat.cpp
│   ├── chat_ui.h / chat_ui.cpp
│   ├── settings_ui.h / settings_ui.cpp
│   ├── agent.h / agent.cpp
│   ├── rag.h / rag.cpp
│   ├── config.h / config.cpp
│   ├── llm_client.h / llm_client.cpp
│   ├── server_manager.h / server_manager.cpp
│   ├── ime.h / ime.cpp
│   ├── font.h / font.cpp
│   └── macos_window.h / macos_window.mm   (macOS のみ)
├── python/                 (Python + Tk 版。ビルド不要)
└── .vscode/
    ├── settings.json
    ├── tasks.json
    ├── launch.json
    └── extensions.json
```

各ファイルの役割：

| ファイル | 役割 |
|---|---|
| src/main.cpp | SDL3 + ImGui の初期化、イベント処理、メインループ |
| src/chat.h/.cpp | 会話の状態、ワーカースレッド管理、ChatGPT への聞き直し |
| src/chat_ui.h/.cpp | チャット画面 |
| src/settings_ui.h/.cpp | 設定画面 |
| src/agent.h/.cpp | ルーター・回答役・ChatGPT・書き直しのエージェント |
| src/rag.h/.cpp | RAG のインデックス作成と検索 |
| src/config.h/.cpp | 設定ファイルの読み書き、API キー |
| src/llm_client.h/.cpp | HTTP 通信、SSE ストリーミングの解析、中断処理、埋め込み |
| src/server_manager.h/.cpp | llama-server の起動・監視・停止 ([doc/managed-server.md](doc/managed-server.md)) |
| src/ime.h/.cpp | IME の変換中の文字列の表示 |
| src/font.h/.cpp | 日本語フォントの読み込み |
| src/macos_window.h/.mm | macOS のタイトルバーの設定 |
| config/ | 設定の既定値 (ビルド時に実行ファイルの隣へコピー。[doc/config.md](doc/config.md)) |
| rag/ | RAG の資料 (ビルド時に実行ファイルの隣へコピー。[doc/rag.md](doc/rag.md)) |
| python/ | Python + Tk 版のクライアント ([doc/python-app.md](doc/python-app.md)) |
| CMakePresets.json | ビルド設定のプリセット (Linux / macOS: Unix Makefiles、Windows: Visual Studio) |
| llama-release.json | ビルド時 (Python 版は初回の起動時) に取得する `llama-server` の公式リリース |
| scripts/ | llama-server の起動スクリプト (自分で起動したいとき用) と、ダウンロードしたモデルの削除スクリプト (OS 別) |
| Asset/ | 同梱フォントとそのライセンス (ビルド時に実行ファイルの隣へコピー) |
| .vscode/ | CMake Tools、ビルドタスク、デバッグ (Linux: gdb / macOS: lldb / Windows: Visual Studio) の設定 |

アプリの中の構成 (画面・ワーカースレッド・エージェント・RAG の関係) は [doc/architecture.md](doc/architecture.md) を参照。

## 4. ビルド

### CMakeLists.txt

主な内容 (全体は [CMakeLists.txt](CMakeLists.txt) を参照)：

- `find_package` で `CURL`、`nlohmann_json`、`SDL3`、`Threads` を検出
- `FetchContent` で Dear ImGui v1.92.9b を取得 (初回の configure でネット接続が必要)
- ImGui 本体、SDL3 / SDL_Renderer 用バックエンド、`imgui_stdlib` を静的ライブラリ `imgui` としてビルド
- 実行ファイル `LocalLLM` (`src/` 以下の `.cpp`) にリンク
- ビルド後に `Asset/`、`rag/` (`sync_rag`)、既定値の `config/` (`sync_config`) を実行ファイルの隣へコピー

```cmake
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)
```

描画には SDL3 の 2D Renderer を使用するため、クライアントは OpenGL や Vulkan に直接依存しない。Vulkan (macOS では Metal) は LLM サーバー側で使用する。

テキスト描画は ImGui が行うため、SDL3_ttf は不要。

### プリセット

ビルドの設定は [CMakePresets.json](CMakePresets.json) のプリセットにまとめてある。VS Code のタスクと CMake Tools もこのプリセットを使う。

| OS | configure | ビルド | 出力先 |
|---|---|---|---|
| Linux / macOS | `cmake --preset debug` / `release` | `cmake --build --preset debug` / `release` (8 並列) | `build/` / `build-release/` |
| Windows | `cmake --preset windows` | `cmake --build --preset windows-debug` / `windows-release` | `build\Debug\` / `build\Release\` |

Linux / macOS でプリセットを使わずに書く場合：

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build build -j8
```

- ジェネレーターは CMake の標準 (Unix Makefiles) なので、`-G` の指定は不要。make は並列数を指定しないと 1 ファイルずつビルドするので、`-j` を付ける (Linux は `-j$(nproc)`、macOS は `-j$(sysctl -n hw.ncpu)` で CPU の数にできる)。
- Release は `-B build-release -DCMAKE_BUILD_TYPE=Release`。

実行：

```bash
./build/LocalLLM
```

### VS Code

C/C++ と CMake Tools の拡張機能を入れて開く。Ctrl+Shift+B (macOS は ⇧⌘B) でビルド、F5 で Debug ビルドしてデバッグする (デバッガーは OS ごとの手順書を参照)。

### build/ を作り直すとき

次の場合は configure が失敗するので、`build/` (や `build-release/`) を削除して作り直す。

- `build/` を別の場所からコピーした・移動した (CMake のキャッシュがパスと合わない)
- 以前に Ninja など別のジェネレーターで configure した (`Does not match the generator used previously`)
- ライブラリやツールを後から入れた・入れ替えた

`build/` を消すと、RAG のインデックス (`build/rag-index.json`) も消える (作り直すだけ)。設定画面で保存した設定はユーザーごとのフォルダーにあるので消えない。

## 5. 日本語フォントと入力

日本語の表示には、プロジェクトに同梱したフリーフォント **M PLUS Rounded 1c Bold** (`Asset/MPLUSRounded1c-Bold.ttf`) を使う。配布元は [Google Fonts](https://fonts.google.com/specimen/M+PLUS+Rounded+1c) で、ライセンスは SIL Open Font License 1.1 (`Asset/OFL.txt`)。

ビルドすると `Asset/` が実行ファイルの隣にコピーされ、アプリは実行ファイルの場所を基準にフォントを読み込む。そのため、どのフォルダーから起動しても同じフォントが使われる。

アプリは次の順にフォントを探す。

1. 環境変数 `LOCALLLM_FONT` で指定したファイル
2. `config/ui.json` の `font_file` (既定は実行ファイルの隣の `Asset/MPLUSRounded1c-Bold.ttf`)
3. OS の日本語フォント (下の表)

フォントの大きさは `config/ui.json` の `font_size` (既定 18) で変えられる。

M PLUS Rounded 1c に含まれない文字 (JIS 第3・第4水準の漢字など) は、OS の日本語フォントで補う (ImGui のフォント合成)。OS の日本語フォントが見つからない場合は、M PLUS Rounded 1c に無い文字は表示されない。

| OS | 補完に使う OS のフォント (`src/font.cpp` の `SYSTEM_FONT_PATHS`) |
|---|---|
| Linux | `/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc` (Ubuntu)、`/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc` (Arch 系) |
| macOS | `/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc`、`/System/Library/Fonts/Hiragino Sans GB.ttc` |
| Windows | `C:\Windows\Fonts\YuGothM.ttc` (游ゴシック Medium)、`meiryo.ttc` (メイリオ)、`msgothic.ttc` (MS ゴシック) |

別のフォントを使う場合は `config/ui.json` の `font_file` を書き換えるか、環境変数 `LOCALLLM_FONT` で指定する。

```bash
LOCALLLM_FONT=/path/to/font.ttf ./build/LocalLLM
```

ImGui 1.92 はフォントの文字を必要なときに読み込むため、日本語用の文字範囲 (glyph ranges) を指定する必要はない。

日本語入力 (IME) は ImGui の SDL3 バックエンドが処理する。入力欄にフォーカスが当たるとテキスト入力を開始し、IME の候補ウィンドウの位置も入力欄に合わせて設定される。変換中の文字列はアプリが描く ([doc/gui-app.md](doc/gui-app.md#ime-の変換中の文字列-imecpp))。

## 6. llama.cpp サーバー

ビルドすると [llama-release.json](llama-release.json) の公式リリースが取得され、実行ファイルの隣の `llama/` に同梱される。アプリを起動すると、チャット用と埋め込み用の `llama-server` を起動し、終了するときに止める。自分で llama.cpp をビルドしたり起動したりする必要はない。

- 使うモデルは設定画面の「サーバー」のタブ (Python 版は「llama-server」のタブ) で選ぶ。保存すると起動し直す。既定は Qwen3 4B 2507。
- 初回はモデルをダウンロードするので時間がかかる (下の「ダウンロードしたモデルの保存場所と削除」)。その間は画面の上部に状態が出て、送信ボタンは押せない。
- サーバーのログは、ユーザーごとのフォルダーの `logs/` に出る (場所は設定画面に表示される)。
- 動きの詳細は [doc/managed-server.md](doc/managed-server.md) を参照。

### 自分で用意した llama-server を使う場合

次のどちらかで、同梱のものの代わりに使える。

- 設定画面の「llama-server の場所」で指定するか、環境変数 `LLAMA_SERVER` で指定する。
- 自分で起動しておく。`/health` に応答するサーバーがあれば、アプリは新しく起動せず、それを使う (止めることもしない)。

ビルド時に公式リリースを取得したくないときは、CMake に `-DLOCALLLM_BUNDLE_LLAMA=OFF` を付ける。

### 起動スクリプト (自分で起動するとき)

モデルをメニューから選んで起動するサンプルスクリプトを `scripts/` に置いてある。

| OS | スクリプト |
|---|---|
| Linux | [scripts/llama-server-linux.sh](scripts/llama-server-linux.sh) |
| macOS | [scripts/llama-server-macos.sh](scripts/llama-server-macos.sh) |
| Windows | [scripts/llama-server-windows.bat](scripts/llama-server-windows.bat) |

- メニューで番号を選ぶと、下の「モデル別の起動」と同じ設定でサーバーを起動する。`q` で何もせずに終了する。
- あわせて RAG 用の埋め込みサーバー (BGE-M3、`127.0.0.1:8081`、初回は約 600 MB をダウンロード) を裏で起動し、チャット用のサーバーを止めると一緒に止める。RAG を使わないときは `--no-embed` を付ける。詳しくは [doc/rag.md](doc/rag.md) を参照。
- どの llama-server を使うか (環境変数 `LLAMA_SERVER` / `LLAMA_DIR` など) は OS ごとの手順書を参照。

### モデル別の起動

どのモデルも、次の共通の設定で起動する。

| オプション | 値 | 意味 |
|---|---|---|
| `-hf` | (下の表) | Hugging Face から取得するモデル |
| `-ngl` | `99` | すべての層を GPU に載せる |
| `-c` | `4096` | コンテキスト長 |
| `--alias` | `local` | モデル名。クライアントは常に `local` を指定するので、モデルを変えてもクライアントの設定は変えなくてよい |
| `--host` / `--port` | `127.0.0.1` / `8080` | 自分のマシンからだけ接続できるようにする |

| モデル | `-hf` の値 |
|---|---|
| Qwen3 4B 2507 | `unsloth/Qwen3-4B-Instruct-2507-GGUF:Q4_K_M` |
| Gemma 3 4B | `ggml-org/gemma-3-4b-it-GGUF` |
| Phi-4 mini | `bartowski/microsoft_Phi-4-mini-instruct-GGUF:Q4_K_M` |
| gpt-oss 20B | `ggml-org/gpt-oss-20b-GGUF:MXFP4` |

例 (Qwen3 4B 2507。`llama-server` の場所と行の継続の書き方は OS ごとの手順書を参照)：

```bash
llama-server \
    -hf unsloth/Qwen3-4B-Instruct-2507-GGUF:Q4_K_M \
    -ngl 99 \
    -c 4096 \
    --alias local \
    --host 127.0.0.1 \
    --port 8080
```

モデルを切り替える場合は、現在のサーバーを `Ctrl+C` で終了してから、別のモデルで起動する。

### ダウンロードしたモデルの保存場所と削除

`-hf` で指定したモデルは、初回の起動時に Hugging Face からダウンロードされ、次からはそれが使われる。保存場所は OS ではなく llama.cpp のバージョンで決まる。

| OS | 新しい llama.cpp | 古い llama.cpp |
|---|---|---|
| macOS | `~/.cache/huggingface/hub/` | `~/Library/Caches/llama.cpp/` |
| Linux | `~/.cache/huggingface/hub/` | `~/.cache/llama.cpp/` |
| Windows | `%USERPROFILE%\.cache\huggingface\hub\` | `%LOCALAPPDATA%\llama.cpp\` |

- 新しい llama.cpp は Hugging Face の公式ツールと同じ場所に保存するので、それらとモデルを共有できる (macOS の Homebrew 版 build 12110 で確認)。
- 環境変数 `HF_HOME` や `LLAMA_CACHE` を設定しているときは、そちらが優先される。
- 保存済みのモデルの一覧は `llama-server -cl` で確認できる (保存場所は表示されない)。

llama.cpp にはモデルを削除するコマンドが無いので、サーバーを止めてから、モデルごとのフォルダーを削除する。削除したモデルは、次に起動したときにまたダウンロードされる。

削除用のスクリプトを用意している。上の表の場所 (環境変数の指定も含む) からダウンロード済みのモデルを探して、サイズと一緒に一覧にし、番号で選んだものを確認のうえ削除する。

| OS | スクリプト |
|---|---|
| macOS / Linux | [scripts/clean-model-cache.sh](scripts/clean-model-cache.sh) |
| Windows | [scripts/clean-model-cache.bat](scripts/clean-model-cache.bat) (中身は [clean-model-cache.ps1](scripts/clean-model-cache.ps1)) |

- `--list` (PowerShell では `-List`) を付けると、一覧を出すだけで削除しない。
- 起動スクリプトのメニューにあるモデルには `[launcher]` が付く。`ggml-org/bge-m3-Q8_0-GGUF` は RAG 用の埋め込みモデル。
- Hugging Face の保存場所はほかのツールと共有なので、`.gguf` を含むフォルダーだけを一覧に出す。
- 古い llama.cpp の保存場所はすべてのモデルが 1 つのフォルダーに入っているので、フォルダーごとまとめて削除する。

手で削除する場合は、モデルごとのフォルダーを消す。新しい llama.cpp では、フォルダー名は `models--<作者>--<リポジトリ名>` になる。

```bash
# 例: Qwen3 4B 2507 を削除する (macOS / Linux)
rm -rf ~/.cache/huggingface/hub/models--unsloth--Qwen3-4B-Instruct-2507-GGUF
```

```powershell
# 例: Qwen3 4B 2507 を削除する (Windows / PowerShell)
Remove-Item -Recurse -Force "$env:USERPROFILE\.cache\huggingface\hub\models--unsloth--Qwen3-4B-Instruct-2507-GGUF"
```

RAG 用の埋め込みモデル (`models--ggml-org--bge-m3-Q8_0-GGUF`) もここにあるので、まとめて削除するときは注意する。

## 7. クライアントとサーバーの接続

| 項目 | 値 |
|---|---|
| 接続先 (`config/server.json` の `chat_url`) | `http://127.0.0.1:8080/v1/chat/completions` |
| 埋め込み (`embed_url`、RAG 用) | `http://127.0.0.1:8081/v1/embeddings` |
| モデル名 | `local` (`--alias local` と合わせる) |

サーバーの稼働確認 (`{"status":"ok"}` が返れば正常。Windows の PowerShell では `curl.exe` と書く)：

```bash
curl http://127.0.0.1:8080/health
```

API 動作確認 (Linux / macOS。Windows の書き方は [SETUP-Windows.md](SETUP-Windows.md) を参照)：

```bash
curl http://127.0.0.1:8080/v1/chat/completions \
    -H "Content-Type: application/json" \
    -d '{
        "model": "local",
        "messages": [
            {
                "role": "user",
                "content": "こんにちは"
            }
        ],
        "stream": false
    }'
```

## 8. 動作確認手順

1. `LocalLLM` を起動し、画面上部の「読み込み中」の表示が消えるまで待つ (初回はモデルのダウンロードがある)。
2. 日本語で質問を入力する。
3. Enter キーで送信する (Shift+Enter で改行)。
4. 回答がストリーミング表示されることを確認する。
5. 続けて質問し、会話履歴が保持されることを確認する。
6. 受信中に「中断」を押し、途中で止まることを確認する。
7. 画面上部に `6 ファイル / 54 チャンク` のように RAG のインデックスの状態が出ることを確認し、「ミケのおやつは 1 日何本まで？」のように資料についての質問をする ([doc/rag.md](doc/rag.md))。
8. 設定画面で別のモデルを選んで保存し、比較する。

## 9. 注意事項

- `-c 4096` はコンテキスト長の設定。会話履歴が長くなると上限に達する。クライアントは履歴を削らないので、長くなったら「クリア」で履歴をリセットする。
- 履歴はクライアントのメモリ上に保持する。アプリを閉じると消え、保存はしない。
- サーバーが起動していなくても GUI は起動する。送信するとエラーが表示され、質問は入力欄に戻る。
- llama-server は `127.0.0.1` にバインドし、外部ネットワークには公開しない。
- IME の変換確定で押した Enter は IME が受け取るため送信されない。
- 背景の半透明表示は `SDL_WINDOW_TRANSPARENT` を使う。デスクトップ環境が透過に対応していない場合は透けない。
- 何がどこへ送られるか (ChatGPT に聞き直す場合を含む) は [doc/privacy.md](doc/privacy.md) を参照。
