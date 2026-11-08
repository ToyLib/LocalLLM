# LocalLLM GUI — 環境構築手順 (Ubuntu)

Ubuntu で、APT を使って開発環境を構築する手順。
OS によらない部分 (プロジェクト構成、ビルドのプリセット、フォント、モデル別の起動、動作確認) は [SETUP.md](SETUP.md) にまとめている。

## 1. 概要

| 項目 | 構成 |
|---|---|
| OS | Ubuntu 26.10 |
| ハードウェア | GMKtec M6 Ultra |
| GPU | AMD Radeon 760M |
| コンパイラ | GCC (G++) |
| ビルド | CMake + Make (Unix Makefiles) |
| LLM サーバー | llama.cpp (Vulkan) |

ライブラリの構成は [SETUP.md](SETUP.md) の「1. 概要」を参照。

## 2. 必要なパッケージ

APT で開発用パッケージをインストールする。

```bash
sudo apt update

sudo apt install -y \
    build-essential \
    cmake \
    pkg-config \
    git \
    curl \
    libsdl3-dev \
    libcurl4-openssl-dev \
    nlohmann-json3-dev \
    fonts-noto-cjk
```

パッケージの役割：

- `build-essential`：GCC、G++ などのコンパイラと make
- `cmake`：ビルドシステム
- `pkg-config`：ライブラリの確認
- `git`：ソース管理、Dear ImGui の取得
- `curl`：サーバーの動作確認用コマンド
- `libsdl3-dev`：ウィンドウ、イベント、2D 描画、IME 入力
- `libcurl4-openssl-dev`：HTTP 通信
- `nlohmann-json3-dev`：JSON 処理
- `fonts-noto-cjk`：日本語表示用フォント

Dear ImGui は APT ではなく、初回の CMake configure 時に FetchContent で GitHub から自動取得する (ネット接続が必要)。
テキスト描画は ImGui が行うため、`libsdl3-ttf-dev` は不要。

デバッグに gdb を使う場合は `gdb` も入れる。

インストール確認：

```bash
pkg-config --modversion sdl3
curl-config --version
cmake --version
make --version
g++ --version
```

## 3. ビルド

プロジェクトのルートディレクトリで実行する。

```bash
cmake --preset debug
cmake --build --preset debug
./build/LocalLLM
```

プリセットを使わない場合：

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build build -j$(nproc)
```

Release やプリセットの一覧、`build/` を作り直すときは [SETUP.md](SETUP.md) の「4. ビルド」を参照。

VS Code では Ctrl+Shift+B でビルド、F5 で Debug ビルドして gdb でデバッグできる (構成「Debug LocalLLM」)。

## 4. 日本語フォント

フォントの探し方は [SETUP.md](SETUP.md) の「5. 日本語フォントと入力」を参照。

Ubuntu では `fonts-noto-cjk` のフォント (`/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc`) を、同梱フォントに無い文字の補完に使う。

OS のフォントの確認：

```bash
fc-match -f '%{file}\n' "Noto Sans CJK JP"
```

## 5. llama.cpp サーバー

配置場所：

```text
~/Repos/llama.cpp/
```

ソースの取得：

```bash
mkdir -p ~/Repos
cd ~/Repos
git clone https://github.com/ggml-org/llama.cpp.git
```

更新する場合は `cd ~/Repos/llama.cpp && git pull` してからビルドし直す。

### ビルドに必要なパッケージ

Vulkan バックエンドのビルドには、Vulkan の開発パッケージに加えて SPIR-V のヘッダー (`spirv-headers`) が必要。無いと CMake の configure またはシェーダーのビルドで失敗する。

```bash
sudo apt install -y \
    libvulkan-dev \
    vulkan-tools \
    glslc \
    spirv-headers \
    libssl-dev
```

パッケージの役割：

- `libvulkan-dev`：Vulkan のヘッダーとローダー
- `vulkan-tools`：`vulkaninfo` などの確認用コマンド
- `glslc`：シェーダーコンパイラー (ビルド時に GPU 用のシェーダーを SPIR-V にコンパイルする)
- `spirv-headers`：SPIR-V のヘッダー (`spirv/unified1/spirv.hpp` など)
- `libssl-dev`：`-DLLAMA_OPENSSL=ON` (HTTPS での `-hf` によるモデル取得) 用

インストール確認：

```bash
glslc --version
ls /usr/include/spirv/unified1/spirv.hpp
vulkaninfo --summary
```

### Vulkan バックエンドでビルド

```bash
cd ~/Repos/llama.cpp

cmake -S . -B build \
    -DGGML_VULKAN=ON \
    -DLLAMA_OPENSSL=ON \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build -j$(nproc)
```

パッケージを後から入れた場合は、`build/` を削除してから configure し直す。

GPU 認識確認：

```bash
./build/bin/llama-cli --list-devices
```

AMD Radeon 760M が Vulkan デバイスとして認識されることを確認する。

### 起動

```bash
./scripts/llama-server-linux.sh
```

メニューや埋め込みサーバー、`--no-embed` については [SETUP.md](SETUP.md) の「6. llama.cpp サーバー」を参照。

llama-server は次の順に探す。

1. 環境変数 `LLAMA_SERVER` で指定したファイル
2. `$LLAMA_DIR/build/bin/llama-server` (ソースからビルドしたもの。`LLAMA_DIR` の既定は `~/Repos/llama.cpp`)
3. PATH 上の `llama-server` (APT や pacman で入れた `/usr/bin/llama-server` など)

ソースからビルドしたものがあればそれを優先し、無い場合はパッケージで入れたものを使う。どれを使うかは起動時に `Server` の行に表示される。

ビルドした場所が違う場合や、特定のファイルを使いたい場合は環境変数で指定する。

```bash
LLAMA_DIR=/path/to/llama.cpp ./scripts/llama-server-linux.sh
```

```bash
LLAMA_SERVER=/path/to/llama-server ./scripts/llama-server-linux.sh
```

スクリプトを使わずに起動する場合 (Qwen3 4B 2507。ほかのモデルの `-hf` の値は [SETUP.md](SETUP.md) の「モデル別の起動」)：

```bash
cd ~/Repos/llama.cpp

./build/bin/llama-server \
    -hf unsloth/Qwen3-4B-Instruct-2507-GGUF:Q4_K_M \
    -ngl 99 \
    -c 4096 \
    --alias local \
    --host 127.0.0.1 \
    --port 8080
```

Hugging Face から取得したモデルは、通常 `~/.cache/huggingface/hub/` 以下にキャッシュされる。

## 6. クライアントとサーバーの接続

[SETUP.md](SETUP.md) の「7. クライアントとサーバーの接続」と「8. 動作確認手順」を参照。

## 7. 注意事項

- この手順は Ubuntu 26.x の実機で、環境構築から LocalLLM のビルド・実行まで確認済み。
- IME の変換確定で押した Enter が送信されないことも、実機で確認済み。
