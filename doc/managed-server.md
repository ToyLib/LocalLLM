# llama-server の自動起動 (server_manager)

アプリが `llama-server` (チャット用と埋め込み用) を子プロセスとして起動・監視・停止する部分の説明です。
対象のファイルは [src/common/server_manager.h](../src/common/server_manager.h)、[src/common/server_manager.cpp](../src/common/server_manager.cpp)、[python/common/server_manager.py](../python/common/server_manager.py) です。
設計の経緯は [managed-server-plan.md](managed-server-plan.md) にあります。

通信はこれまでどおり HTTP なので、`llm_client` は変わりません。

## 動き

専用のスレッドが 1 秒ごとに、チャット用と埋め込み用のサーバーをそれぞれ見ます。

1. `chat_url` / `embed_url` がローカル (`http://127.0.0.1` か `http://localhost`) でなければ、何もしません。
2. `/health` に問い合わせて、応答があれば既存のサーバーを使います。自分では起動も停止もしません。
3. `llama-server` を探して起動し、`/health` が 200 を返すまで待ちます。
4. 起動したサーバーが落ちたら、ログの末尾と「もう一度起動」のボタンを出します。自動では起動し直しません。
5. 設定を保存すると読み直して、内容が変わったサーバーだけを起動し直します。
6. 終了するときは、自分が起動したサーバーだけを止めます。

| 状態 | 意味 | 質問を送れるか |
| --- | --- | --- |
| 起動しない | 自動起動がオフ、URL がローカルでない、モデルが空 | 送れる (今までどおり) |
| ダウンロードまたは読み込み中 | サーバーの準備中。時間では打ち切らない | チャットは送れない |
| 使えます | 自分で起動したサーバーが使える | 送れる |
| 使えます (外部のサーバー) | すでに動いているサーバーを使っている | 送れる |
| 失敗 | 起動できなかった、または落ちた | チャットは送れない |

RAG は、埋め込みが必要になったときに `WaitForEmbedServer` で準備を待ちます。

## llama-server の探し方

| 順 | 場所 |
| --- | --- |
| 1 | `server.json` の `managed.server_path` (設定画面の「llama-server の場所」) |
| 2 | 環境変数 `LLAMA_SERVER` |
| 3 | 同梱のもの: C++ 版は実行ファイルの隣の `llama/`、Python 版はユーザーごとのフォルダーの `llama/<タグ>/` |
| 4 | `LLAMA_DIR` (既定は `~/Repos/llama.cpp`、Windows は `%USERPROFILE%\Repos\llama.cpp`) のビルド結果 |
| 5 | PATH (macOS は Homebrew の場所も) |

1 と 2 を指定していて見つからないときは、ほかを探さずに失敗にします。
見つけたら `--version` で動くことを確かめ、`--list-devices` に GPU が無ければ設定画面に注意を出します。
Python 版は、どこにも見つからなければ公式リリースをダウンロードします。C++ 版で見つからないときは、設定画面で場所を指定します。

## 同梱する公式リリース

[llama-release.json](../llama-release.json) に、タグと OS ごとのファイル名、SHA256 を書いています。
C++ 版はビルド時に CMake が、Python 版は初回の起動時に、この内容で取得してハッシュを確かめます。

更新するときは、タグとファイル名、ハッシュを書き換えて、動作を確かめます。ハッシュは GitHub API の `digest` (`/repos/ggml-org/llama.cpp/releases/tags/<タグ>`) で分かります。

`llama-server` が使わないツールは取り除きます。`ggml-*` のライブラリは実行時に読み込まれるので、消しません。
取得しないときは、CMake に `-DLOCALLLM_BUNDLE_LLAMA=OFF` を付けます。

## アプリが異常終了したとき

| OS | しくみ |
| --- | --- |
| Windows | サーバーをジョブに入れて、アプリが終わると一緒に終わるようにする |
| macOS / Linux | ユーザーごとのフォルダーの `run/` に「サーバーの PID とアプリの PID」を書いておき、次の起動のときに、アプリが生きていなければサーバーを止める |

PID は再利用されるので、止める前に `llama-server` であることを確かめます。
アプリの PID も見ることで、C++ 版と Python 版を同時に起動しても、相手のサーバーを止めません。

## ログ

ユーザーごとのフォルダーの `logs/` に、`chat-server.log` と `embed-server.log` を書きます (起動するたびに上書き)。
