# LocalLLM GUI — 環境構築手順 (CachyOS / Arch Linux)

CachyOS などの Arch Linux 系で、pacman を使って開発環境を構築する手順。
Ubuntu の手順は [SETUP.md](SETUP.md)、macOS の手順は [SETUP-macOS.md](SETUP-macOS.md)、Windows の手順は [SETUP-Windows.md](SETUP-Windows.md) を参照。

Ubuntu との違いは主にパッケージ名とインストールのコマンドで、ビルドや llama-server の使い方は同じ。アプリの構成 (GUI とワーカースレッドの関係など) は OS によらず同じなので、[SETUP.md](SETUP.md) の「9. GUI クライアントの構成」を参照。

## 1. 概要

| 項目 | 構成 |
|---|---|
| OS | CachyOS (Arch Linux 系) |
| GPU | AMD Radeon (Vulkan) |
| 言語 | C++20 |
| ビルド | CMake + Make (Unix Makefiles) |
| ウィンドウ・描画 | SDL3 (SDL_Renderer) |
| GUI・テキスト描画 | Dear ImGui v1.92.9b |
| HTTP 通信 | libcurl |
| JSON | nlohmann/json |
| LLM サーバー | llama.cpp (Vulkan) |

## 2. 必要なパッケージ

pacman で開発用パッケージをインストールする。

```bash
sudo pacman -Syu --needed \
    base-devel \
    cmake \
    pkgconf \
    git \
    curl \
    sdl3 \
    nlohmann-json \
    noto-fonts-cjk
```

パッケージの役割：

- `base-devel`：GCC、G++ などのコンパイラと make
- `cmake`：ビルドシステム
- `pkgconf`：ライブラリの確認 (`pkg-config` コマンド)
- `git`：ソース管理、Dear ImGui の取得
- `curl`：libcurl 本体とヘッダー、サーバーの動作確認用コマンド
- `sdl3`：ウィンドウ、イベント、2D 描画、IME 入力 (ヘッダーも含む)
- `nlohmann-json`：JSON 処理
- `noto-fonts-cjk`：日本語表示用フォント

Arch 系ではライブラリのパッケージにヘッダーも含まれるため、Ubuntu の `-dev` にあたるパッケージは不要。

デバッグに gdb を使う場合は `gdb` も入れる。

Dear ImGui は pacman ではなく、初回の CMake configure 時に FetchContent で GitHub から自動取得する (ネット接続が必要)。

インストール確認：

```bash
pkg-config --modversion sdl3
curl-config --version
cmake --version
make --version
g++ --version
```

## 3. ビルド

プロジェクトのルートディレクトリで実行する。手順は Ubuntu と同じ。

```bash
cmake --preset debug
cmake --build --preset debug
./build/LocalLLM
```

Release は `release` プリセット (`build-release/`)。プリセットを使わない書き方や VS Code での使い方は [SETUP.md](SETUP.md) の「5. ビルド」を参照。

## 4. 日本語フォント

同梱フォント (`Asset/MPLUSRounded1c-Bold.ttf`) を使い、含まれない文字は OS の日本語フォントで補う。仕組みは [SETUP.md](SETUP.md) の「6. 日本語フォントと入力」を参照。

Arch 系の `noto-fonts-cjk` は次の場所に入り、アプリはこれを自動で使う。

- `/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc`

OS のフォントの確認：

```bash
fc-match -f '%{file}\n' "Noto Sans CJK JP"
```

## 5. llama.cpp サーバー

llama.cpp はクライアントとは独立して起動する。

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
sudo pacman -S --needed \
    vulkan-headers \
    vulkan-icd-loader \
    vulkan-tools \
    vulkan-radeon \
    shaderc \
    spirv-headers \
    openssl
```

パッケージの役割：

- `vulkan-headers`：Vulkan のヘッダー
- `vulkan-icd-loader`：Vulkan のローダー (`libvulkan.so`)
- `vulkan-tools`：`vulkaninfo` などの確認用コマンド
- `vulkan-radeon`：AMD GPU 用の Vulkan ドライバー (Mesa RADV)。CachyOS では通常インストール済み
- `shaderc`：シェーダーコンパイラー `glslc` (ビルド時に GPU 用のシェーダーを SPIR-V にコンパイルする)
- `spirv-headers`：SPIR-V のヘッダー (`spirv/unified1/spirv.hpp` など)
- `openssl`：`-DLLAMA_OPENSSL=ON` (HTTPS での `-hf` によるモデル取得) 用

Ubuntu の `glslc` パッケージにあたるものは、Arch 系では `shaderc` に含まれる。

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

AMD Radeon が Vulkan デバイスとして認識されることを確認する。

### 起動

起動スクリプトは Ubuntu と同じ [scripts/llama-server-linux.sh](scripts/llama-server-linux.sh) を使う。

```bash
./scripts/llama-server-linux.sh
```

スクリプトのオプション、環境変数 (`LLAMA_DIR`、`LLAMA_SERVER`)、モデル別の起動コマンドは [SETUP.md](SETUP.md) の「7. llama.cpp サーバー」を参照。

## 6. クライアントとサーバーの接続

Ubuntu と同じ。[SETUP.md](SETUP.md) の「8. クライアントとサーバーの接続」と「10. 動作確認手順」を参照。

サーバーの稼働確認 (`{"status":"ok"}` が返れば正常)：

```bash
curl http://127.0.0.1:8080/health
```

## 7. 注意事項

- Arch 系はローリングリリースのため、`sudo pacman -Syu` でシステムを更新した後は、ライブラリのバージョンが変わって LocalLLM や llama.cpp が起動しなくなることがある。そのときは `build/` を削除してビルドし直す。
- pacman の `llama-cpp` や AUR の `llama.cpp-vulkan` などでも llama-server は入る。起動スクリプトは `~/Repos/llama.cpp` にビルドしたものを優先し、無い場合は PATH 上の `llama-server` (`/usr/bin/llama-server` など) を使う。パッケージ版が Vulkan に対応しているかはパッケージによるので、GPU を使うにはこの手順のようにソースからビルドするのが確実。
- 背景の半透明表示は `SDL_WINDOW_TRANSPARENT` を使う。デスクトップ環境 (KDE Plasma など) が透過に対応していない場合は透けない。
