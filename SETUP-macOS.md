# LocalLLM GUI — 環境構築手順 (macOS)

macOS 上で、Homebrew を使って開発環境を構築する手順。
OS によらない部分 (プロジェクト構成、ビルドのプリセット、フォント、モデル別の起動、動作確認) は [SETUP.md](SETUP.md) にまとめている。

## 1. 概要

| 項目 | 構成 |
|---|---|
| OS | macOS (Apple Silicon / Intel) |
| パッケージ管理 | Homebrew |
| コンパイラ | Apple Clang (Xcode Command Line Tools) |
| ビルド | CMake + Make (Unix Makefiles) |
| ウィンドウ・描画 | SDL3 (SDL_Renderer、Metal) |
| HTTP 通信 | libcurl (macOS 標準) |
| LLM サーバー | llama.cpp (Metal) |

ほかのライブラリの構成は [SETUP.md](SETUP.md) の「1. 概要」を参照。

## 2. 必要なツールとパッケージ

### Xcode Command Line Tools

コンパイラ (Apple Clang)、make、git が入る。

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
    pkgconf \
    sdl3 \
    nlohmann-json
```

パッケージの役割：

- `cmake`：ビルドシステム
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
make --version
clang++ --version
```

## 3. ビルド

プロジェクトのルートディレクトリで実行する。

```bash
cmake --preset debug
cmake --build --preset debug
./build/LocalLLM
```

プリセットを使わない場合は `-j$(sysctl -n hw.ncpu)` で CPU の数だけ並列にビルドできる。Release やプリセットの一覧、`build/` を作り直すときは [SETUP.md](SETUP.md) の「4. ビルド」を参照。

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build build -j$(sysctl -n hw.ncpu)
```

CMake が SDL3 や nlohmann/json を見つけられない場合は、Homebrew のインストール先を指定する。

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH="$(brew --prefix)"
```

### VS Code

C/C++ と CMake Tools の拡張機能を入れて開く。

- Ctrl+Shift+B (⇧⌘B)：ビルド
- F5：Debug ビルドして lldb でデバッグ (構成「Debug LocalLLM」)

## 4. 日本語フォント

フォントの探し方は [SETUP.md](SETUP.md) の「5. 日本語フォントと入力」を参照。

macOS では、同梱フォントに無い文字をヒラギノ (`/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc`) で補う。追加のインストールは要らない。

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

### 起動

```bash
./scripts/llama-server-macos.sh
```

メニューや埋め込みサーバー、`--no-embed` については [SETUP.md](SETUP.md) の「6. llama.cpp サーバー」を参照。macOS 標準の bash (3.2) で動く。

llama-server は次の順に探す。

1. 環境変数 `LLAMA_SERVER` で指定したファイル
2. `$LLAMA_DIR/build/bin/llama-server` (ソースからビルドしたもの。`LLAMA_DIR` の既定は `~/Repos/llama.cpp`)
3. PATH 上の `llama-server` (Homebrew で入れたもの)

スクリプトを使わずに起動する場合 (Qwen3 4B 2507。ほかのモデルの `-hf` の値は [SETUP.md](SETUP.md) の「モデル別の起動」)：

```bash
llama-server \
    -hf unsloth/Qwen3-4B-Instruct-2507-GGUF:Q4_K_M \
    -ngl 99 \
    -c 4096 \
    --alias local \
    --host 127.0.0.1 \
    --port 8080
```

ソースからビルドした場合は `~/Repos/llama.cpp/build/bin/llama-server` を使う。

Hugging Face から取得したモデルは、通常 `~/Library/Caches/llama.cpp/` または `~/.cache/huggingface/hub/` 以下にキャッシュされる (llama.cpp のバージョンによる)。

## 6. クライアントとサーバーの接続

[SETUP.md](SETUP.md) の「7. クライアントとサーバーの接続」と「8. 動作確認手順」を参照。

## 7. 注意事項

- 本文をタイトルバーの下まで広げ、タイトルバーも本文と同じ半透明にする (`src/macos_window.mm`)。
- Retina ディスプレイでは SDL3 の高解像度対応 (`SDL_WINDOW_HIGH_PIXEL_DENSITY`) により、文字がくっきり表示される。
- この手順は macOS 27 の実機で、環境構築から LocalLLM のビルド・実行まで確認済み。
