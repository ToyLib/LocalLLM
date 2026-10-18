# LocalLLM GUI — 環境構築手順 (Ubuntu)

macOS の手順は [SETUP-macOS.md](SETUP-macOS.md)、Windows の手順は [SETUP-Windows.md](SETUP-Windows.md) を参照。

## 1. 概要

Ubuntu 上で、C++20 と SDL3 + Dear ImGui を使ったローカル LLM チャットクライアントの開発環境を構築する。

LLM 本体は別プロセスの `llama-server` で実行し、クライアントは OpenAI 互換の HTTP API を通じて通信する。

### 開発環境

| 項目 | 構成 |
|---|---|
| OS | Ubuntu 26.10 |
| ハードウェア | GMKtec M6 Ultra |
| GPU | AMD Radeon 760M |
| 言語 | C++20 |
| ビルド | CMake + Ninja |
| ウィンドウ・描画 | SDL3 (SDL_Renderer) |
| GUI・テキスト描画 | Dear ImGui v1.92.9b |
| HTTP 通信 | libcurl |
| JSON | nlohmann/json |
| LLM サーバー | llama.cpp (Vulkan) |
| 対応モデル | Qwen3 4B / Gemma 3 4B / Phi-4 mini |

## 2. 必要なパッケージ

APT で開発用パッケージをインストールする。

```bash
sudo apt update

sudo apt install -y \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    git \
    curl \
    libsdl3-dev \
    libcurl4-openssl-dev \
    nlohmann-json3-dev \
    fonts-noto-cjk
```

パッケージの役割：

- `build-essential`：GCC、G++ などのコンパイラ
- `cmake`：ビルドシステム
- `ninja-build`：CMake のジェネレーター (VS Code の設定でも使用)
- `pkg-config`：ライブラリの確認
- `git`：ソース管理、Dear ImGui の取得
- `curl`：サーバーの動作確認用コマンド
- `libsdl3-dev`：ウィンドウ、イベント、2D 描画、IME 入力
- `libcurl4-openssl-dev`：HTTP 通信
- `nlohmann-json3-dev`：JSON 処理
- `fonts-noto-cjk`：日本語表示用フォント

Dear ImGui は APT ではなく、初回の CMake configure 時に FetchContent で GitHub から自動取得する (ネット接続が必要)。
テキスト描画は ImGui が行うため、`libsdl3-ttf-dev` は不要。

インストール確認：

```bash
pkg-config --modversion sdl3
curl-config --version
cmake --version
ninja --version
g++ --version
```

## 3. プロジェクト構成

```text
LocalLLM/
├── CMakeLists.txt
├── CMakePresets.json   (OS 別のビルド設定)
├── README.md
├── SETUP.md            (Ubuntu)
├── SETUP-macOS.md
├── SETUP-Windows.md
├── LICENSE
├── vcpkg.json          (Windows 用の依存ライブラリ)
├── Asset/
│   ├── MPLUSRounded1c-Bold.ttf   (同梱フォント)
│   └── OFL.txt                   (フォントのライセンス)
├── main.cpp
├── llm_client.h
├── llm_client.cpp
└── .vscode/
    ├── settings.json
    ├── tasks.json
    ├── launch.json
    └── extensions.json
```

各ファイルの役割：

| ファイル | 役割 |
|---|---|
| main.cpp | SDL3 + ImGui の初期化、イベント処理、チャット画面、ワーカースレッド管理 |
| llm_client.h/.cpp | HTTP 通信、SSE ストリーミングの解析、中断処理 |
| CMakePresets.json | ビルド設定のプリセット (Linux / macOS: Ninja、Windows: Visual Studio) |
| vcpkg.json | vcpkg のマニフェスト (Windows でのみ使用) |
| Asset/ | 同梱フォントとそのライセンス (ビルド時に実行ファイルの隣へコピー) |
| .vscode/ | CMake Tools、ビルドタスク、デバッグ (Linux: gdb / macOS: lldb / Windows: Visual Studio) の設定 |

GUI と LLM 通信は別のファイルに分けている。会話履歴は GUI 側 (`main.cpp`) で保持し、送信のたびに `llm_client` へ渡す。

## 4. CMakeLists.txt

主な内容 (全体は [CMakeLists.txt](CMakeLists.txt) を参照)：

- `find_package` で `CURL`、`nlohmann_json`、`SDL3`、`Threads` を検出
- `FetchContent` で Dear ImGui v1.92.9b を取得
- ImGui 本体、SDL3 / SDL_Renderer 用バックエンド、`imgui_stdlib` を静的ライブラリ `imgui` としてビルド
- 実行ファイル `LocalLLM` (`main.cpp`、`llm_client.cpp`) にリンク

```cmake
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)
```

描画には SDL3 の 2D Renderer を使用するため、クライアントは OpenGL や Vulkan に直接依存しない。

Vulkan は LLM サーバー側で使用する。

## 5. ビルド

プロジェクトのルートディレクトリで実行する。

Debug ビルド：

```bash
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build build
```

実行：

```bash
./build/LocalLLM
```

Release ビルド：

```bash
cmake -S . -B build-release -G Ninja \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build-release
```

上と同じ設定は [CMakePresets.json](CMakePresets.json) のプリセットにもまとめてあり、次のように短く書ける。VS Code のタスクと CMake Tools もこのプリセットを使う。

```bash
cmake --preset debug
cmake --build --preset debug
```

Release は `release` プリセット (`build-release/`)。

VS Code では Ctrl+Shift+B でビルド、F5 で Debug ビルドして gdb でデバッグできる。

`build/` を別の場所からコピーした場合や移動した場合は、CMake のキャッシュがパスと合わずに configure が失敗する。そのときは `build/` を削除して作り直す。

## 6. 日本語フォントと入力

日本語の表示には、プロジェクトに同梱したフリーフォント **M PLUS Rounded 1c Bold** (`Asset/MPLUSRounded1c-Bold.ttf`) を使う。配布元は [Google Fonts](https://fonts.google.com/specimen/M+PLUS+Rounded+1c) で、ライセンスは SIL Open Font License 1.1 (`Asset/OFL.txt`)。

ビルドすると `Asset/` が実行ファイルの隣 (`build/Asset/`) にコピーされ、アプリは実行ファイルの場所を基準にフォントを読み込む。そのため、どのフォルダーから起動しても同じフォントが使われる。

アプリは次の順にフォントを探す。

1. 環境変数 `LOCALLLM_FONT` で指定したファイル
2. 実行ファイルの隣の `Asset/MPLUSRounded1c-Bold.ttf`
3. OS の日本語フォント (下記)

M PLUS Rounded 1c に含まれない文字 (JIS 第3・第4水準の漢字など) は、OS の日本語フォントで補う (ImGui のフォント合成)。OS の日本語フォントが見つからない場合は、M PLUS Rounded 1c に無い文字は表示されない。

Ubuntu で補完に使う OS のフォント (`fonts-noto-cjk`)：

- `/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc`
- `/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc`

OS のフォントの確認：

```bash
fc-match -f '%{file}\n' "Noto Sans CJK JP"
```

別のフォントを使う場合は環境変数 `LOCALLLM_FONT` で指定する。

```bash
LOCALLLM_FONT=/path/to/font.ttf ./build/LocalLLM
```

ImGui 1.92 はフォントの文字を必要なときに読み込むため、日本語用の文字範囲 (glyph ranges) を指定する必要はない。

日本語入力 (IME) は ImGui の SDL3 バックエンドが処理する。入力欄にフォーカスが当たるとテキスト入力を開始し、IME の候補ウィンドウの位置も入力欄に合わせて設定される。

## 7. llama.cpp サーバー

llama.cpp はクライアントとは独立して起動する。

配置場所：

```text
~/Repos/llama.cpp/
```

Vulkan バックエンドでビルドする場合：

```bash
cd ~/Repos/llama.cpp

cmake -S . -B build \
    -DGGML_VULKAN=ON \
    -DLLAMA_OPENSSL=ON \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build -j$(nproc)
```

Vulkan の開発パッケージが不足する場合：

```bash
sudo apt install -y \
    libvulkan-dev \
    vulkan-tools \
    glslc
```

GPU 認識確認：

```bash
./build/bin/llama-cli --list-devices
```

AMD Radeon 760M が Vulkan デバイスとして認識されることを確認する。

### モデル別の起動

Qwen3 4B：

```bash
./build/bin/llama-server \
    -hf Qwen/Qwen3-4B-GGUF:Q4_K_M \
    -ngl 99 \
    -c 4096 \
    --alias local \
    --host 127.0.0.1 \
    --port 8080
```

Gemma 3 4B：

```bash
./build/bin/llama-server \
    -hf ggml-org/gemma-3-4b-it-GGUF \
    -ngl 99 \
    -c 4096 \
    --alias local \
    --host 127.0.0.1 \
    --port 8080
```

Phi-4 mini：

```bash
./build/bin/llama-server \
    -hf bartowski/microsoft_Phi-4-mini-instruct-GGUF:Q4_K_M \
    -ngl 99 \
    -c 4096 \
    --alias local \
    --host 127.0.0.1 \
    --port 8080
```

モデルを切り替える場合は、現在のサーバーを `Ctrl+C` で終了してから、別のモデルで起動する。

Hugging Face から取得したモデルは、通常 `~/.cache/huggingface/hub/` 以下にキャッシュされる。

## 8. クライアントとサーバーの接続

接続先 (`llm_client.cpp` の `SERVER_URL`)：

```text
http://127.0.0.1:8080/v1/chat/completions
```

使用モデル名：

```text
local
```

すべてのモデルを `--alias local` で起動するため、クライアント側のモデル指定を変更する必要はない。

サーバーの稼働確認 (`{"status":"ok"}` が返れば正常)：

```bash
curl http://127.0.0.1:8080/health
```

API 動作確認：

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

## 9. GUI クライアントの構成

メインスレッドは SDL3 のイベント処理と ImGui の描画を担当する。

LLM 通信は送信のたびに起動するワーカースレッドで実行する。

```text
SDL3 Main Thread
    |
    +-- Window / SDL_Renderer
    +-- Keyboard / IME (ImGui SDL3 backend)
    +-- Chat Rendering (Dear ImGui)
    |
    +-- ChatState (std::mutex で保護)
    |       ^
    |       |  受信テキストを追記
    |       |
    +-- SDL ユーザーイベントで描画を再開
            ^
            |
     Worker Thread (std::thread)
            |
            +-- libcurl
            +-- SSE Streaming
            +-- llama-server
```

- 会話履歴は `ChatState` の `std::vector<Message>` で保持し、送信時に system プロンプトと合わせて `nlohmann::json` の配列に変換する。
- 受信したテキストはワーカースレッドが `std::mutex` で保護しながら最後の回答に追記し、`SDL_PushEvent` でメインスレッドを起こす。
- SDL と ImGui の API はメインスレッドからだけ呼び出す。
- 中断は `std::atomic<bool>` のフラグを libcurl の進捗コールバック (`CURLOPT_XFERINFOFUNCTION`) で確認して転送を止める。
- 何も起きていないときは `SDL_WaitEventTimeout` でイベントを待ち、CPU 使用率を抑える。

## 10. 動作確認手順

1. `llama-server` を起動する。
2. `/health` でサーバーの応答を確認する。
3. `LocalLLM` を起動する。
4. 日本語で質問を入力する。
5. Enter キーで送信する (Shift+Enter で改行)。
6. 回答がストリーミング表示されることを確認する。
7. 続けて質問し、会話履歴が保持されることを確認する。
8. 受信中に「中断」を押し、途中で止まることを確認する。
9. サーバーを別のモデルで起動し直して比較する。

## 11. 注意事項

- `-c 4096` はコンテキスト長の設定。会話履歴が長くなると上限に達する。クライアントは履歴を削らないので、長くなったら「クリア」で履歴をリセットする。
- 履歴はクライアントのメモリ上に保持する。アプリを閉じると消え、現段階では永続化しない。
- GUI をブロックしないよう、HTTP 通信はワーカースレッドで実行する。
- サーバーが起動していなくても GUI は起動する。送信するとエラーが表示され、質問は入力欄に戻る。
- `127.0.0.1` にバインドし、外部ネットワークには公開しない。
- Qwen、Gemma、Phi の切り替えはサーバー側で管理する。
- IME の変換確定で押した Enter は IME が受け取るため送信されない想定だが、実機ではまだ確認していない。
- 背景の半透明表示は `SDL_WINDOW_TRANSPARENT` を使う。デスクトップ環境が透過に対応していない場合は透けない。
