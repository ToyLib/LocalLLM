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
- 資料を参照するかを質問ごとに判断するルーターと、2 つの回答役 (資料を参照する質問用 / 資料の要らない質問用。既定はどちらもアシスタント) ([説明](doc/agent.md))
- 物足りない回答は「ChatGPT に聞き直す」ボタンで ChatGPT (OpenAI API) に聞き直し、回答役の口調で言い直して表示 ([説明](doc/chatgpt.md))
- 設定でオンにすると、聞き直した後に Web 検索でも聞き直して、検索の有無による回答の違いを比べられる ([説明](doc/chatgpt.md#web-でも調べて比べる))
- 「クイズ」タブで、分野 (プログラミング・IT パスポート・一般常識・`rag/` の資料) を選んで 10 問の 4 択に挑戦し、点数とねぎらいのメッセージを見られる (Python 版のみ、[説明](doc/quiz.md))
- 回答役の人格をテンプレート (侍・悟空風・バカボンのパパ風・大阪のおばちゃん・執事・猫など) から選び、設定画面で好みに直せる

## 必要なもの

Linux (Ubuntu / CachyOS などの Arch 系)、macOS、Windows でビルドできます。詳しい環境構築手順は、OS によらない部分を [SETUP.md](SETUP.md) (プロジェクト構成、ビルドのプリセット、フォント、モデル別の起動、動作確認)、OS ごとの部分 (パッケージのインストールや llama.cpp のビルド) を次の手順書にまとめています。

| OS | 手順書 | パッケージ管理 | ビルド・実行を確認した環境 |
| --- | --- | --- | --- |
| Ubuntu | [SETUP-Ubuntu.md](SETUP-Ubuntu.md) | APT | Ubuntu 26.x |
| CachyOS / Arch Linux | [SETUP-Arch.md](SETUP-Arch.md) | pacman | CachyOS |
| macOS | [SETUP-macOS.md](SETUP-macOS.md) | Homebrew | macOS 27 |
| Windows | [SETUP-Windows.md](SETUP-Windows.md) | vcpkg (場所は環境変数 `VCPKG_ROOT`) | Windows 11 + Visual Studio 2026 |

Ubuntu の場合は次のパッケージで動作を確認しています。

```bash
sudo apt install build-essential cmake \
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
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

初回の configure では Dear ImGui を取得するため、ネット接続が必要です。

## 使い方

1. アプリを起動します。

   ```bash
   ./build/LocalLLM
   ```

   `llama-server` はビルドで同梱され、アプリが起動・停止します ([説明](doc/managed-server.md))。初回はモデルをダウンロードするので時間がかかります。モデルは設定画面で選べます (Qwen3 4B 2507 / Gemma 3 4B / Phi-4 mini / gpt-oss 20B)。

   自分でサーバーを起動しておくこともできます。`127.0.0.1:8080` で応答するサーバーがあれば、アプリはそれを使います。起動用のサンプルスクリプトもあります。

   | OS | スクリプト |
   | --- | --- |
   | Linux | [scripts/llama-server-linux.sh](scripts/llama-server-linux.sh) |
   | macOS | [scripts/llama-server-macos.sh](scripts/llama-server-macos.sh) |
   | Windows | [scripts/llama-server-windows.bat](scripts/llama-server-windows.bat) |

   使い方は OS ごとの手順書を参照してください。

   スクリプトは RAG 用の埋め込みサーバー (8081 番) も一緒に起動します。不要なら `--no-embed` を付けてください (詳しくは [doc/rag.md](doc/rag.md))。

### 操作

| 操作 | 動作 |
| --- | --- |
| Enter | 送信 |
| Shift+Enter | 改行 |
| 中断ボタン | 受信を止める (途中までの回答は残る) |
| クリアボタン | 会話履歴をリセット |
| メッセージを右クリック | コピー |
| ChatGPT に聞き直すボタン | 最新の質問を ChatGPT に聞き直す (API キーが必要。[説明](doc/chatgpt.md)) |
| 資料 (RAG) | `rag/` の資料を質問に添えるか (自動: ルーターが判断 / 常に / 使わない) |
| 再読込ボタン | `rag/` を読み直してインデックスを更新 |
| 設定ボタン | 設定画面を開く (画面・システムプロンプト・サーバーの URL を変えて保存) |

送信に失敗した場合、その質問は履歴から外されて入力欄に戻ります。

## 設定

サーバーの URL、system プロンプト、画面の設定は `config/` の JSON ファイルに書きます。
画面右上の「設定」ボタンから開く設定画面でも変えられます (タブは 画面 / システムプロンプト / サーバー / ChatGPT。Python 版は 画面 / システムプロンプト / サーバー / llama-server)。

- 背景の不透明度・文字の大きさ・色は、設定画面で動かすとすぐに反映されます (キャンセルで元に戻ります)。フォントファイルは再起動後に反映されます。
- system プロンプトは 1 行が `system_prompt` の配列の 1 要素です (送るときは行をそのままつなげます)。
- 回答役のタブでは、`config/personas.json` の人格のテンプレートを選んで「このテンプレートにする」で名前とプロンプトを読み込めます。そこから直して保存します (テンプレート自体は変わりません)。
- 保存すると、読み込んでいるフォルダーの JSON を書き換えます。設定画面に出ない項目はそのまま残ります。

リポジトリの `config/` は既定値です。実際に読み書きするのは、ユーザーごとのフォルダーの `config/` です (C++ 版と Python 版で共有。API キーの `secrets.json` と同じ `LocalLLM` フォルダーの中)。

| OS | 設定のフォルダー |
| --- | --- |
| macOS | `~/Library/Application Support/LocalLLM/config/` |
| Linux | `$XDG_CONFIG_HOME/LocalLLM/config/` (無ければ `~/.config/LocalLLM/config/`) |
| Windows | `%APPDATA%\LocalLLM\config\` |

- 無いファイルだけを、起動時に既定値からコピーします。設定画面で保存してもリポジトリの `config/` は変わらず、ビルドしても消えません。
- **リポジトリの `config/` を書き換えても、すでにコピーされたファイルには反映されません。** 取り込むには、設定画面の「すべて既定に戻す」を押すか、アプリを閉じてから設定のフォルダーのファイルを消します (C++ 版は先にビルドして、新しい既定値を実行ファイルの隣にコピーしておきます)。
`server.json` と `agents.json` は送信のたびに読み直すので、書き換えると再起動しなくても次の質問から反映されます。`ui.json` は起動時に読みます。
環境変数 `LOCALLLM_CONFIG_DIR` で別のフォルダを指定できます。
全項目と環境変数の一覧は [doc/config.md](doc/config.md) にまとめています。

```json
{
  "chat_url": "http://127.0.0.1:8080/v1/chat/completions",
  "embed_url": "http://127.0.0.1:8081/v1/embeddings",
  "cloud": {
    "name": "ChatGPT",
    "chat_url": "https://api.openai.com/v1/chat/completions",
    "model": "gpt-5.6-terra",
    "api_key_name": "OPENAI_API_KEY",
    "auth": "bearer"
  }
}
```

| 項目 | 場所 |
| --- | --- |
| サーバーの URL | `config/server.json` の `chat_url` |
| 埋め込みサーバーの URL (RAG) | `config/server.json` の `embed_url` |
| システムプロンプト | `config/agents.json` (`router` / `assistant` / `general` / `cloud` / `rewriter` / `comparer`、[説明](doc/agent.md)) |
| ChatGPT の接続先・モデル | `config/server.json` の `cloud` ([説明](doc/chatgpt.md)) |
| ChatGPT の API キー | 環境変数か、ユーザーごとの秘密ファイル (設定ファイルには書かない。[説明](doc/chatgpt.md#api-キーの登録)。何が送られるかは [doc/privacy.md](doc/privacy.md)) |
| 人格のテンプレート | `config/personas.json` (設定画面で選ぶ候補。[説明](doc/agent.md)) |
| 最大トークン数 | `src/llm_client.cpp` の `max_tokens` |
| RAG の文書フォルダ | 環境変数 `LOCALLLM_RAG_DIR` (既定は実行ファイルの隣の `rag/`) |
| 背景の不透明度 | `config/ui.json` の `window_opacity` (0〜1、既定 0.85) |
| フォントファイル | `config/ui.json` の `font_file` (相対パスは実行ファイルのフォルダーから。既定は同梱の `Asset/MPLUSRounded1c-Bold.ttf`)。環境変数 `LOCALLLM_FONT` があればそちらを優先 |
| フォントサイズ | `config/ui.json` の `font_size` (ピクセル、既定 18) |
| フォントサイズ (Python 版) | `config/ui.json` の `python_font_size` (ポイント、既定 14) |
| 色 | `config/ui.json` の `colors` (下の表のキーに `"#RRGGBB"`)。書かなかった色は既定値 |
| 色 (Python 版) | `config/ui.json` の `python_colors` (キーは `colors` と同じ。背景と文字の既定は OS のテーマの色) |

| `colors` のキー | 色を付ける所 | C++ 版の既定 |
| --- | --- | --- |
| `background` | チャット画面の背景 (Python 版は会話欄と入力欄。透け具合は `window_opacity`) | `#0F0F0F` |
| `text` | 文字 | `#FFFFFF` |
| `sub_text` | 補足の文字 (考え中・参照した資料・RAG の状態) | `#808080` |
| `user` | 送信者名「You」 | `#73BFFF` |
| `ai` | 回答したエージェントの名前 | `#8CE68C` |
| `error` | エラー | `#FF6666` |

C++ 版は暗い背景、Python 版は OS の明るいテーマを前提に既定の色を決めているので、`colors` と `python_colors` を分けています。

```json
{
  "window_opacity": 0.85,
  "font_file": "Asset/MPLUSRounded1c-Bold.ttf",
  "font_size": 18,
  "python_font_size": 14,
  "colors": {
    "user": "#73BFFF",
    "ai": "#8CE68C"
  }
}
```

`ui.json` が無い、または値が正しくない項目は、ログに書いて既定値で起動します。

背景が透けない場合は、デスクトップ環境がウィンドウの透過に対応していない可能性があります。

## Python 版

`python/` に、同じ機能を Python + Tk で作ったクライアントがあります (標準ライブラリだけで動き、ビルドは不要)。
Python 3.10 以上と、Tk が使える Python が必要です (macOS の Homebrew なら `brew install python-tk`、Ubuntu なら `sudo apt install python3-tk`)。

```bash
python3 python/main.py
```

- 操作と設定は C++ 版と同じです。フォントサイズと色は `ui.json` の `python_font_size` と `python_colors` です。
- 設定は C++ 版と同じ、ユーザーごとのフォルダーの `config/` を読み書きします ([設定](#設定) の表)。環境変数 `LOCALLLM_CONFIG_DIR` を指定したときは、コピーせずにそのフォルダーを直接読み書きします。
- RAG の文書はリポジトリの `rag/` をそのまま読みます (環境変数 `LOCALLLM_RAG_DIR` で変更可)。インデックスは `python/rag-index.json` に保存します (形式は C++ 版と同じ)。
- フォントは OS の標準フォントを使います。背景の半透明には対応していません。

C++ 版との違いと、Tk で作るときの工夫は [doc/python-app.md](doc/python-app.md) にまとめています。

| ファイル | 内容 (対応する C++ のファイル) |
| --- | --- |
| `python/main.py` | 起動 (`main.cpp`) |
| `python/chat_ui.py` | Tk のチャット画面 (`chat_ui.cpp`) |
| `python/quiz_ui.py` / `python/quiz_mode.py` | クイズタブ ([説明](doc/quiz.md)) |
| `python/chat.py` | 会話の状態と、ワーカースレッドでの送受信 (`chat.cpp`) |
| `python/agent.py` | ルーター / アシスタント / 汎用の回答役 (`agent.cpp`) |
| `python/config.py` | `config/` の設定ファイルの読み込み (`config.cpp`) |
| `python/llm_client.py` | ストリーミング送信と SSE の解析、埋め込みの取得 (`llm_client.cpp`) |
| `python/rag.py` | RAG のインデックス作成と検索 (`rag.cpp`) |

## VS Code

`.vscode` に設定を入れてあります。C/C++ と CMake Tools の拡張機能を入れて開いてください。

- Ctrl+Shift+B: ビルド
- F5: Debug ビルドして gdb でデバッグ

## ファイル構成

| ファイル | 内容 |
| --- | --- |
| `src/main.cpp` | SDL3 / Dear ImGui の初期化とメインループ |
| `src/chat.cpp` / `src/chat.h` | 会話の状態と、ワーカースレッドでの送受信 |
| `src/agent.cpp` / `src/agent.h` | 役割ごとの LLM の呼び出し (ルーター / 回答役) |
| `src/chat_ui.cpp` / `src/chat_ui.h` | チャット画面の組み立て |
| `src/ime.cpp` / `src/ime.h` | IME の変換中の文字列の表示 |
| `src/font.cpp` / `src/font.h` | 日本語フォントの読み込み |
| `src/llm_client.cpp` / `src/llm_client.h` | API へのストリーミング送信と SSE の解析、埋め込みの取得 |
| `src/rag.cpp` / `src/rag.h` | RAG のインデックス作成と検索 |
| `src/server_manager.cpp` / `src/server_manager.h` | `llama-server` の起動・監視・停止 |
| `doc/` | コードの解説 ([全体の構成](doc/architecture.md) / [設定ファイル](doc/config.md) / [データの行き先](doc/privacy.md) / [LLM との通信](doc/llm-client.md) / [GUI アプリ](doc/gui-app.md) / [RAG](doc/rag.md) / [エージェント](doc/agent.md) / [ChatGPT に聞き直す](doc/chatgpt.md) / [Python 版](doc/python-app.md) / [クイズ](doc/quiz.md) / [llama-server の自動起動](doc/managed-server.md)) |
| `CMakeLists.txt` | ビルド設定 |
| `llama-release.json` | 同梱する `llama-server` の公式リリース (タグとハッシュ) |
| `CMakePresets.json` | ビルド設定のプリセット (Linux / macOS: Unix Makefiles、Windows: Visual Studio) |
| `SETUP.md` | 環境構築手順 (OS によらない部分) |
| `SETUP-Ubuntu.md` / `SETUP-Arch.md` / `SETUP-macOS.md` / `SETUP-Windows.md` | 環境構築手順 (Ubuntu / Arch 系 / macOS / Windows) |
| `scripts/` | llama-server を自分で起動するときのスクリプト (チャット用と RAG の埋め込み用をまとめて起動、Linux / macOS / Windows) と、ダウンロードしたモデルの削除スクリプト、Windows の配布用 ZIP を作るスクリプト |
| `rag/` | RAG で参照する文書 (`samples/` は架空のサンプル、ビルド時に実行ファイルの隣へコピー) |
| `Asset/` | 同梱フォントとそのライセンス (ビルド時に実行ファイルの隣へコピー) |

## ライセンス

[MIT License](LICENSE)

同梱フォント `Asset/MPLUSRounded1c-Bold.ttf` ([M PLUS Rounded 1c](https://fonts.google.com/specimen/M+PLUS+Rounded+1c)、Copyright 2016 The Rounded M+ Project Authors.) は本プロジェクトの MIT License の対象外で、[SIL Open Font License 1.1](Asset/OFL.txt) に従います。

Dear ImGui は MIT License、SDL3 は zlib License、nlohmann/json は MIT License、libcurl は curl License です。
