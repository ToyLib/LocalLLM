# llama-server の自動起動の設計メモ

今は利用者がスクリプトで `llama-server` を起動してからアプリを使っています。
この手間をなくし、アプリが `llama-server` を子プロセスとして起動・監視・停止するようにするための方針です。
実装済みです。使い方と動きは [managed-server.md](managed-server.md) を参照してください。

実装では、次の点をこのメモから変えています。

- 公式リリースは Windows が `.zip`、macOS / Linux は `.tar.gz` でした。固定したのは b12110 (動作確認に使った Homebrew 版と同じコミット) です。
- Linux も `PR_SET_PDEATHSIG` ではなく、macOS と同じ PID ファイルで片付けます。SDL の `SDL_CreateProcess` では子プロセス側の処理を差し込めないためです。PID ファイルにはアプリの PID も書き、C++ 版と Python 版を同時に起動しても相手のサーバーを止めないようにしました。
- Python 版のダウンロードは、ほかに `llama-server` が見つからないときだけ行います。
- Ubuntu / Arch の手順書のビルド手順は、公式リリースの動作を実機で確かめるまで、補足として残しています。
- Python 版の設定は「llama-server」のタブに分けました (サーバーのタブに入りきらないため)。
- モデルのキャッシュの場所は、このメモを書いたときの想定と違い、新しい llama.cpp では Hugging Face と同じ場所でした ([SETUP.md](../SETUP.md))。`llama-server` の場所には依存しません。

## 1. 方針

| 区分 | 内容 |
| --- | --- |
| 今回 | アプリが `llama-server` を子プロセスとして管理する。公式のリリース版をフォルダーごと同梱する |
| 今回 | C++ 版・Python 版の両方に同じ機能を入れ、手順書を簡単にする |
| 残す | 外部の `llama-server` への接続 (`chat_url` / `embed_url`)、自分でビルドしたバイナリの指定 |
| 後回し | llama.cpp のライブラリとしての内蔵 |

### ライブラリとして内蔵しない理由

今は次のことを `llama-server` に任せています。内蔵すると、これらを `common/` (安定 API ではない) を使って作り直すことになります。

- Jinja テンプレートと `chat_template_kwargs` (`enable_thinking`、`reasoning_effort`)
- JSON スキーマから文法への変換 (ルーターと回答の比較が依存)
- 推論部分と回答の分離 (`llm_client` は `delta.content` だけを読む)
- `-hf` によるモデルのダウンロードとキャッシュ
- OS ごとの GPU バックエンド

さらに、モデル側のクラッシュ (`GGML_ASSERT` など) で GUI ごと落ちるようになり、Python 版は別の仕組みが必要になります。

子プロセスなら HTTP での通信はそのままなので、`AskLLM` と `Embed` は変更不要です。
単一バイナリでの配布、ポートを開けない運用、子プロセスを起動できないサンドボックス、トークン単位の制御が必要になったら、内蔵を改めて検討します。

## 2. 動き

1. `chat_url` / `embed_url` のホストとポートを読む。ローカル以外の URL なら起動せず、外部サーバーとして使う。
2. `/health` に問い合わせる。応答があれば既存のサーバーを使い、自分では起動も停止もしない (External)。
3. `llama-server` を探して起動する (3 章)。
4. `/health` が 200 を返すまで「読み込み中」を表示する。
5. 実行中はプロセスを監視する。落ちたらログの末尾と再起動ボタンを表示する (自動では再起動しない)。
6. モデルを変えたらチャット用だけ再起動する。埋め込み用は動かしたままにする。
7. 終了時は自分が起動したサーバーだけを止める。

```
Stopped → Starting → Ready → Stopped
              │        │
              └→ Failed ←┘  (ログ表示 + 再起動ボタン)
External: 既に動いているサーバーを使っている状態 (止めない)
```

チャット用と埋め込み用で、状態は別々に持ちます。

## 3. llama-server の探し方

| 順 | 場所 | 想定する使い方 |
| --- | --- | --- |
| 1 | 設定の `managed.server_path` | 画面で指定した場所 |
| 2 | 環境変数 `LLAMA_SERVER` | スクリプトと同じ指定 |
| 3 | 同梱の `llama/` | C++ 版は実行ファイルの隣。Python 版はユーザーごとのフォルダーの `llama/<タグ>/` |
| 4 | `LLAMA_DIR` (既定は `~/Repos/llama.cpp`) のビルド結果 | ソースからビルドした場合 |
| 5 | PATH | winget、Homebrew、apt などで入れた場合 |
| 6 | ファイル選択ダイアログ | それ以外。選んだパスを 1 に保存する |

- 設定画面に、使用中のパスと見つけた経路を表示する。
- 見つけたら `--version` を実行して動くことを確かめる。
- `--list-devices` に GPU が出なければ警告する (起動は止めない)。

Windows の注意点:

- 必ず絶対パスで起動する (ファイル名だけだとカレントディレクトリも検索される)。PATH は自分で 1 項目ずつ調べる。
- コンソールウィンドウを出さない (SDL はバックグラウンド指定、Python は `CREATE_NO_WINDOW`)。
- 日本語を含むパスを壊さない。`getenv` ではなく `SDL_getenv` を使い、パスは UTF-8 として明示的に変換する。

## 4. バイナリの同梱

Git には入れず、公式のリリースをタグと SHA256 で固定して取得します。

固定する情報は、C++ 版と Python 版で共有する 1 つのファイルに書きます (ファイル名は実装時に決め直してよい)。

```json
{
  "tag": "<タグ>",
  "assets": {
    "windows-x64": { "file": "llama-<タグ>-bin-win-vulkan-x64.zip", "sha256": "<ハッシュ>" },
    "macos-arm64": { "file": "<macOS arm64 版>", "sha256": "<ハッシュ>" },
    "linux-x64":   { "file": "<Linux x64 版>", "sha256": "<ハッシュ>" }
  }
}
```

- C++ 版: CMake が `string(JSON ...)` でこのファイルを読み、`FetchContent` (`URL_HASH`) で取得して、実行ファイルの隣の `llama/` にコピーする。
- Python 版: ビルドが無いので、初回起動時に同じ情報でダウンロードし、ハッシュを確かめて、ユーザーごとのフォルダーに展開する。

同梱するものは次のとおりです。

| 対象 | 同梱 | 理由 |
| --- | --- | --- |
| `llama-server` と zip 内のライブラリ一式 | する | `ggml-vulkan` や `ggml-cpu-*` は実行時に読み込まれるので、依存関係の一覧を見て削らない |
| `llama-cli` など他のツール | しない | 使わない |
| ライセンス文書 | する | MIT ライセンスの表示 |
| `vulkan-1.dll` | しない | GPU ドライバーに含まれる |
| VC++ ランタイム | 確認して決める | まっさらな Windows で動作を確かめる |
| モデル (GGUF) | しない | 初回に `-hf` でダウンロードする |

Windows の第一候補は、今の手順と同じ Vulkan 版です。macOS で配布する場合は、ライブラリにも署名と公証が必要です。

## 5. 設定

`server.json` に `managed` を足します。URL は今の `chat_url` / `embed_url` をそのまま使います。

```json
{
  "chat_url": "http://127.0.0.1:8080/v1/chat/completions",
  "embed_url": "http://127.0.0.1:8081/v1/embeddings",
  "managed": {
    "enabled": true,
    "server_path": "",
    "chat_model": "unsloth/Qwen3-4B-Instruct-2507-GGUF:Q4_K_M",
    "embed_model": "ggml-org/bge-m3-Q8_0-GGUF",
    "context": 4096,
    "embed_context": 8192,
    "gpu_layers": 99
  }
}
```

(項目名は実装時に決め直してよい。)

- `managed` が無い設定ファイルは自動起動なしとして扱い、前の版の設定をそのまま読めるようにする。設定画面でオンにできる。
- `embed_model` が空なら、埋め込みサーバーを起動しない (スクリプトの `--no-embed` に相当)。
- ポートは固定のままとし、衝突したら起動の失敗をログ付きで表示する。
- モデルの候補は、今の起動スクリプトのメニュー (Qwen3 / Gemma 3 / Phi-4 mini / gpt-oss) を設定画面の選択肢にする。

起動の引数は今のスクリプトと同じです。

```
チャット:  -hf <モデル> -ngl <gpu_layers> -c <context> --alias local --host 127.0.0.1 --port 8080
埋め込み:  -hf <モデル> --embeddings -ngl <gpu_layers> -c <embed_context> -b <embed_context> -ub <embed_context> --alias embed --host 127.0.0.1 --port 8081
```

## 6. コードの変更

| ファイル | 変更 |
| --- | --- |
| `src/server_manager.cpp/.h` (新規) | 探索、起動、`/health` の待機、監視、停止。専用のスレッドで動かし、状態が変わったら UI を起こす |
| `python/server_manager.py` (新規) | 同じ内容を `subprocess` で。初回のダウンロードもここ |
| `src/main.cpp` | `StartRagIndexing` より前に起動し、`JoinRagWorker` のあと、`SDL_Quit` の前に止める |
| `src/chat*` | Ready になるまで送信を無効にし、状態を表示する |
| `src/rag.cpp` | 埋め込みサーバーが Ready になるまでインデックス作成を待つ |
| `src/settings_ui.cpp`、`python/settings_ui.py` | 自動起動のオン・オフ、モデルの選択、`llama-server` の場所、状態の表示 |
| `CMakeLists.txt` | リリースの取得と `llama/` へのコピー |

C++ 版は SDL3 の `SDL_CreateProcessWithProperties` を使い、OS ごとの分岐を減らします。

## 7. 気をつけること

| 問題 | 対策 |
| --- | --- |
| 起動直後のインデックス作成が、準備前の埋め込みサーバーに接続して失敗する (`Embed` の接続タイムアウトは 3 秒) | Ready まで待たせる |
| 初回のモデルのダウンロードに数分以上かかり、その間 `/health` が応答しない | 時間で打ち切らず、プロセスが生きている間は待つ。状態は「ダウンロード中または読み込み中」とログの末尾で見せる |
| 標準出力をパイプにして読まないと、バッファが詰まってサーバーが止まる | ログファイルへ直接流す |
| `Program Files` などには書き込めない | ログはユーザーごとのフォルダーに書く |
| 終了時に止まらないサーバーを待ち続ける | 通常の停止を数秒待ってから強制終了する |
| アプリが異常終了するとサーバーが残る | Windows は Job Object、Linux は `PR_SET_PDEATHSIG`、macOS は PID ファイルで次回起動時に片付ける (自分が起動したものか確かめてから) |

## 8. 手順書の変更

- `SETUP.md` と各 OS の手順書から、llama.cpp のビルド手順と、Windows の vcpkg の `vulkan` / `shaderc` の説明を消す。
- 残すのは、C++ 版のビルドに必要なもの、GPU ドライバー、初回にモデルをダウンロードすること。
- 今の「6. llama.cpp サーバー」は、外部サーバーに接続したい人と自分でビルドしたい人向けの補足に移す。起動スクリプトも補足として残す。
- `doc/architecture.md`、`doc/config.md`、`doc/llm-client.md`、`doc/privacy.md`、README も合わせて更新する。

## 9. 進める順番

1. C++ 版の `server_manager` (探索、起動、待機、監視、停止)。この時点では PATH などの既存のバイナリで試す。
2. `main.cpp`、チャット、RAG の待機と、状態の表示。
3. 設定画面。
4. 公式リリースの取得と同梱。
5. 異常終了の後始末を OS ごとに入れる。
6. Python 版。
7. 手順書とドキュメント。

## 10. 実装時に確かめること

macOS (Apple Silicon) と Windows では、C++ 版と Python 版の動作を確かめました。次の点は実機で確かめていません。

- Windows: まっさらな環境で VC++ ランタイムが要るか (Windows Sandbox などで確認)。
- Linux: 公式リリースが Ubuntu と Arch の両方で動くか。動かない OS があれば、その手順書のビルド手順を残す。
- モデルのダウンロード中の `/health` の挙動 (時間で打ち切らないので、どちらでも動く想定)。

## 11. 決めていないこと

- 他の人への配布をどこまで想定するか (macOS の署名と公証、Windows のインストーラーを作るか)。自分の環境だけなら、4 章の同梱までで十分です。
