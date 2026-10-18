# LocalLLM GUI — 環境構築手順 (macOS)

macOS 上で、Homebrew を使って開発環境を構築する手順。
Ubuntu の手順は [SETUP.md](SETUP.md)、Windows の手順は [SETUP-Windows.md](SETUP-Windows.md) を参照。

アプリの構成 (GUI とワーカースレッドの関係など) は OS によらず同じなので、[SETUP.md](SETUP.md) の「9. GUI クライアントの構成」を参照。

## 1. 概要

| 項目 | 構成 |
|---|---|
| OS | macOS (Apple Silicon / Intel) |
| パッケージ管理 | Homebrew |
| コンパイラ | Apple Clang (Xcode Command Line Tools) |
| ビルド | CMake + Ninja |
| ウィンドウ・描画 | SDL3 (SDL_Renderer、Metal) |
| GUI・テキスト描画 | Dear ImGui v1.92.9b |
| HTTP 通信 | libcurl (macOS 標準) |
| JSON | nlohmann/json |
| LLM サーバー | llama.cpp (Metal) |

## 2. 必要なツールとパッケージ

### Xcode Command Line Tools

コンパイラ (Apple Clang) と git が入る。

```bash
xcode-select --install
```

### Homebrew

未導入の場合は [brew.sh](https://brew.sh/) の手順でインストールする。

### パッケージ

```bash
brew update

brew install \
    cmake \
    ninja \
    pkgconf \
    sdl3 \
    nlohmann-json
```

パッケージの役割：

- `cmake`：ビルドシステム
- `ninja`：CMake のジェネレーター (VS Code の設定でも使用)
- `pkgconf`：ライブラリの確認 (`pkg-config` コマンド)
- `sdl3`：ウィンドウ、イベント、2D 描画、IME 入力
- `nlohmann-json`：JSON 処理

libcurl は macOS に標準で入っているものを使うため、Homebrew で入れる必要はない。
Dear ImGui は初回の CMake configure 時に FetchContent で GitHub から自動取得する (ネット接続が必要)。

インストール確認：

```bash
pkg-config --modversion sdl3
curl-config --version
cmake --version
ninja --version
clang++ --version
```

## 3. ビルド

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

CMake が SDL3 や nlohmann/json を見つけられない場合は、Homebrew のインストール先を指定する。

```bash
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH="$(brew --prefix)"
```

### VS Code

C/C++ と CMake Tools の拡張機能を入れて開く。

- Ctrl+Shift+B (⇧⌘B)：ビルド
- F5：Debug ビルドして lldb でデバッグ (構成「Debug LocalLLM」)

## 4. 日本語フォントと入力

日本語の表示には、プロジェクトに同梱したフリーフォント **M PLUS Rounded 1c Bold** (`Asset/MPLUSRounded1c-Bold.ttf`) を使う。配布元は [Google Fonts](https://fonts.google.com/specimen/M+PLUS+Rounded+1c) で、ライセンスは SIL Open Font License 1.1 (`Asset/OFL.txt`)。

ビルドすると `Asset/` が実行ファイルの隣 (`build/Asset/`) にコピーされ、アプリは実行ファイルの場所を基準にフォントを読み込む。そのため、どのフォルダーから起動しても同じフォントが使われる。

アプリは次の順にフォントを探す。

1. 環境変数 `LOCALLLM_FONT` で指定したファイル
2. 実行ファイルの隣の `Asset/MPLUSRounded1c-Bold.ttf`
3. OS の日本語フォント (下記)

M PLUS Rounded 1c に含まれない文字 (JIS 第3・第4水準の漢字など) は、OS の日本語フォントで補う (ImGui のフォント合成)。OS の日本語フォントが見つからない場合は、M PLUS Rounded 1c に無い文字は表示されない。

macOS で補完に使う OS のフォント：

- `/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc`
- `/System/Library/Fonts/Hiragino Sans GB.ttc`

別のフォントを使う場合：

```bash
LOCALLLM_FONT=/path/to/font.ttf ./build/LocalLLM
```

日本語入力 (IME) は ImGui の SDL3 バックエンドが処理する。

## 5. llama.cpp サーバー

macOS では llama.cpp が Metal (Apple Silicon の GPU) を使う。

### Homebrew で入れる場合 (簡単)

```bash
brew install llama.cpp
```

`llama-server` や `llama-cli` がそのまま使える。

### ソースからビルドする場合

macOS では Metal が既定で有効になる。

```bash
git clone https://github.com/ggml-org/llama.cpp.git
cd llama.cpp

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
```

この場合、コマンドは `./build/bin/llama-server` になる。

GPU 認識確認：

```bash
llama-cli --list-devices
```

Metal のデバイスが表示されることを確認する。

### 起動スクリプト

モデルをメニューから選んで起動するサンプルスクリプトを [scripts/llama-server-macos.sh](scripts/llama-server-macos.sh) に置いてある。

```bash
./scripts/llama-server-macos.sh
```

メニューで番号を選ぶと、次の「モデル別の起動」と同じ設定 (`-ngl 99`、`-c 4096`、`--alias local`、`127.0.0.1:8080`) でサーバーを起動する。`q` で何もせずに終了する。

llama-server は次の順に探す。

1. 環境変数 `LLAMA_SERVER` で指定したファイル
2. `$LLAMA_DIR/build/bin/llama-server` (ソースからビルドしたもの。`LLAMA_DIR` の既定は `~/Repos/llama.cpp`)
3. PATH 上の `llama-server` (Homebrew で入れたもの)

macOS 標準の bash (3.2) で動く。

### モデル別の起動

Qwen3 4B：

```bash
llama-server \
    -hf Qwen/Qwen3-4B-GGUF:Q4_K_M \
    -ngl 99 \
    -c 4096 \
    --alias local \
    --host 127.0.0.1 \
    --port 8080
```

Gemma 3 4B：

```bash
llama-server \
    -hf ggml-org/gemma-3-4b-it-GGUF \
    -ngl 99 \
    -c 4096 \
    --alias local \
    --host 127.0.0.1 \
    --port 8080
```

Phi-4 mini：

```bash
llama-server \
    -hf bartowski/microsoft_Phi-4-mini-instruct-GGUF:Q4_K_M \
    -ngl 99 \
    -c 4096 \
    --alias local \
    --host 127.0.0.1 \
    --port 8080
```

モデルを切り替える場合は、現在のサーバーを `Ctrl+C` で終了してから、別のモデルで起動する。

Hugging Face から取得したモデルは、通常 `~/Library/Caches/llama.cpp/` または `~/.cache/huggingface/hub/` 以下にキャッシュされる (llama.cpp のバージョンによる)。

## 6. クライアントとサーバーの接続

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

## 7. 注意事項

- Retina ディスプレイでは SDL3 の高解像度対応 (`SDL_WINDOW_HIGH_PIXEL_DENSITY`) により、文字がくっきり表示される。
- 実機 (macOS) でのビルド・動作はまだ確認していない。
