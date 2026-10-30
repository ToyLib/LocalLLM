# LocalLLM

ローカルで動かしている LLM と会話するための、C++20 製のチャットクライアントです。
画面は SDL3 + Dear ImGui で作っています。

OpenAI 互換の Chat Completions API (`/v1/chat/completions`) にストリーミング (SSE) で問い合わせるので、
[llama.cpp](https://github.com/ggml-org/llama.cpp) の `llama-server` などと組み合わせて使えます。

## 主な機能

- 回答をストリーミングで逐次表示
- セッション中の会話履歴を保持し、毎回まとめて送信
- 受信中の中断
- 日本語の表示・入力 (同梱のフリーフォント M PLUS Rounded 1c Bold を使用し、足りない文字は OS の日本語フォントで補完)
- 半透明のチャット画面
- メッセージを右クリックでコピー
- `rag/` フォルダの資料 (.md / .txt) を参照した回答 (RAG、[説明](doc/rag.md))
- 資料を参照するかを質問ごとに判断するルーターと、口調の違う 2 つの回答役 (資料に基づくアシスタント / 時代劇口調の侍) ([説明](doc/agent.md))

## 必要なもの

Linux (Ubuntu)、macOS、Windows でビルドできます。詳しい環境構築手順 (llama.cpp のビルドやモデル別の起動方法を含む) は OS ごとの手順書を参照してください。

| OS | 手順書 | パッケージ管理 | ビルド・実行を確認した環境 |
| --- | --- | --- | --- |
| Ubuntu | [SETUP.md](SETUP.md) | APT | Ubuntu 26.x |
| macOS | [SETUP-macOS.md](SETUP-macOS.md) | Homebrew | macOS 27 |
| Windows | [SETUP-Windows.md](SETUP-Windows.md) | vcpkg (場所は環境変数 `VCPKG_ROOT`) | Windows 11 + Visual Studio 2026 |

Ubuntu の場合は次のパッケージで動作を確認しています。

```bash
sudo apt install build-essential cmake ninja-build \
    libsdl3-dev libcurl4-openssl-dev nlohmann-json3-dev fonts-noto-cjk
```

- C++20 対応のコンパイラ (GCC 15 で確認)
- CMake 3.20 以上
- SDL3
- libcurl
- nlohmann/json
- Dear ImGui (v1.92.9b、ビルド時に CMake の FetchContent で GitHub から自動取得)

## ビルド

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

初回の configure では Dear ImGui を取得するため、ネット接続が必要です。

## 使い方

1. LLM サーバーを `127.0.0.1:8080` で起動します。`llama-server` の例:

   ```bash
   llama-server -m /path/to/model.gguf --port 8080
   ```

   モデルをメニューから選んで起動するサンプルスクリプトもあります (Qwen3 4B / Gemma 3 4B / Phi-4 mini)。

   | OS | スクリプト |
   | --- | --- |
   | Linux | [scripts/llama-server-linux.sh](scripts/llama-server-linux.sh) |
   | macOS | [scripts/llama-server-macos.sh](scripts/llama-server-macos.sh) |
   | Windows | [scripts/llama-server-windows.bat](scripts/llama-server-windows.bat) |

   使い方は OS ごとの手順書を参照してください。

   スクリプトは RAG 用の埋め込みサーバー (8081 番) も一緒に起動します。不要なら `--no-embed` を付けてください (詳しくは [doc/rag.md](doc/rag.md))。

2. アプリを起動します。

   ```bash
   ./build/LocalLLM
   ```

### 操作

| 操作 | 動作 |
| --- | --- |
| Enter | 送信 |
| Shift+Enter | 改行 |
| 中断ボタン | 受信を止める (途中までの回答は残る) |
| クリアボタン | 会話履歴をリセット |
| メッセージを右クリック | コピー |
| 資料 (RAG) | `rag/` の資料を質問に添えるか (自動: ルーターが判断 / 常に / 使わない) |
| 再読込ボタン | `rag/` を読み直してインデックスを更新 |

送信に失敗した場合、その質問は履歴から外されて入力欄に戻ります。

## 設定

| 項目 | 場所 |
| --- | --- |
| サーバーの URL | `src/llm_client.cpp` の `SERVER_URL` |
| システムプロンプト | `src/agent.cpp` の `CreateAssistant` / `CreateSamurai` / `CreateRouter` |
| 最大トークン数 | `src/llm_client.cpp` の `max_tokens` |
| 埋め込みサーバーの URL (RAG) | `src/llm_client.cpp` の `EMBED_URL` |
| RAG の文書フォルダ | 環境変数 `LOCALLLM_RAG_DIR` (既定は実行ファイルの隣の `rag/`) |
| フォントサイズ | `src/font.cpp` の `FONT_SIZE` |
| 背景の不透明度 | `src/main.cpp` の `WINDOW_OPACITY` (既定 0.85) |
| フォントファイル | 環境変数 `LOCALLLM_FONT` (既定は同梱の `Asset/MPLUSRounded1c-Bold.ttf`) |

```bash
LOCALLLM_FONT=/path/to/font.ttf ./build/LocalLLM
```

背景が透けない場合は、デスクトップ環境がウィンドウの透過に対応していない可能性があります。

## VS Code

`.vscode` に設定を入れてあります。C/C++ と CMake Tools の拡張機能を入れて開いてください。

- Ctrl+Shift+B: ビルド
- F5: Debug ビルドして gdb でデバッグ

## ファイル構成

| ファイル | 内容 |
| --- | --- |
| `src/main.cpp` | SDL3 / Dear ImGui の初期化とメインループ |
| `src/chat.cpp` / `src/chat.h` | 会話の状態と、ワーカースレッドでの送受信 |
| `src/agent.cpp` / `src/agent.h` | 役割ごとの LLM の呼び出し (ルーター / アシスタント / 侍) |
| `src/chat_ui.cpp` / `src/chat_ui.h` | チャット画面の組み立て |
| `src/ime.cpp` / `src/ime.h` | IME の変換中の文字列の表示 |
| `src/font.cpp` / `src/font.h` | 日本語フォントの読み込み |
| `src/llm_client.cpp` / `src/llm_client.h` | API へのストリーミング送信と SSE の解析、埋め込みの取得 |
| `src/rag.cpp` / `src/rag.h` | RAG のインデックス作成と検索 |
| `doc/` | コードの解説 ([LLM との通信](doc/llm-client.md) / [GUI アプリ](doc/gui-app.md) / [RAG](doc/rag.md) / [エージェント](doc/agent.md)) |
| `CMakeLists.txt` | ビルド設定 |
| `CMakePresets.json` | ビルド設定のプリセット (Linux / macOS: Ninja、Windows: Visual Studio) |
| `SETUP.md` / `SETUP-macOS.md` / `SETUP-Windows.md` | 環境構築手順 (Ubuntu / macOS / Windows) |
| `vcpkg.json` | vcpkg のマニフェスト (Windows 用) |
| `scripts/` | llama-server の起動スクリプト (チャット用と RAG の埋め込み用をまとめて起動、Linux / macOS / Windows) |
| `rag/` | RAG で参照する文書 (`samples/` は架空のサンプル、ビルド時に実行ファイルの隣へコピー) |
| `Asset/` | 同梱フォントとそのライセンス (ビルド時に実行ファイルの隣へコピー) |

## ライセンス

[MIT License](LICENSE)

同梱フォント `Asset/MPLUSRounded1c-Bold.ttf` ([M PLUS Rounded 1c](https://fonts.google.com/specimen/M+PLUS+Rounded+1c)、Copyright 2016 The Rounded M+ Project Authors.) は本プロジェクトの MIT License の対象外で、[SIL Open Font License 1.1](Asset/OFL.txt) に従います。

Dear ImGui は MIT License、SDL3 は zlib License、nlohmann/json は MIT License、libcurl は curl License です。
