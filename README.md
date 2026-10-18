# LocalLLM

ローカルで動かしている LLM と会話するための、C++20 製のチャットクライアントです。
画面は SDL3 + Dear ImGui で作っています。

OpenAI 互換の Chat Completions API (`/v1/chat/completions`) にストリーミング (SSE) で問い合わせるので、
[llama.cpp](https://github.com/ggml-org/llama.cpp) の `llama-server` などと組み合わせて使えます。

## 主な機能

- 回答をストリーミングで逐次表示
- セッション中の会話履歴を保持し、毎回まとめて送信
- 受信中の中断
- 日本語の表示・入力 (Noto Sans CJK を使用)
- 半透明のチャット画面
- メッセージを右クリックでコピー

## 必要なもの

詳しい環境構築手順 (llama.cpp の Vulkan ビルドやモデル別の起動方法を含む) は [SETUP.md](SETUP.md) を参照してください。

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

送信に失敗した場合、その質問は履歴から外されて入力欄に戻ります。

## 設定

| 項目 | 場所 |
| --- | --- |
| サーバーの URL | `llm_client.cpp` の `SERVER_URL` |
| システムプロンプト | `llm_client.cpp` の `SYSTEM_PROMPT` |
| 最大トークン数 | `llm_client.cpp` の `max_tokens` |
| フォントサイズ | `main.cpp` の `FONT_SIZE` |
| 背景の不透明度 | `main.cpp` の `WINDOW_OPACITY` (既定 0.85) |
| フォントファイル | 環境変数 `LOCALLLM_FONT` (既定は Noto Sans CJK) |

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
| `main.cpp` | SDL3 + Dear ImGui のチャット画面 |
| `llm_client.cpp` / `llm_client.h` | API へのストリーミング送信と SSE の解析 |
| `CMakeLists.txt` | ビルド設定 |
| `SETUP.md` | 環境構築手順 |

## ライセンス

[MIT License](LICENSE)

Dear ImGui は MIT License、SDL3 は zlib License、nlohmann/json は MIT License、libcurl は curl License です。
