# Python 版 (Tk) の構成

`python/` には、C++ 版と同じ機能を Python + Tk で作ったクライアントがあります。
標準ライブラリだけで動き、ビルドは要りません。動かし方は README の「Python 版」を参照してください。

```bash
python3 python/main.py
```

ルーター・RAG・ChatGPT への聞き直しといった機能の考え方は C++ 版と同じなので、[agent.md](agent.md)、[rag.md](rag.md)、[chatgpt.md](chatgpt.md) がそのまま当てはまります。
このメモでは、**C++ 版との違い**と、Tk で作るときの工夫を中心にまとめます。

## ファイルの対応

| Python 版 | C++ 版 | 内容 |
| --- | --- | --- |
| `main.py` | `main.cpp` | ログの設定をして `chat_ui.main()` を呼ぶだけ |
| `chat_ui.py` | `chat_ui.cpp` (+ `main.cpp` のメインループ) | チャット画面 (`ChatApp`) |
| `quiz_ui.py` / `quiz_mode.py` | `quiz_ui.cpp` / `quiz_mode.cpp` | クイズタブ ([quiz.md](quiz.md)) |
| `maze_ui.py` / `maze_mode.py` | `maze_ui.cpp` / `maze_mode.cpp` | 迷路タブ ([maze.md](maze.md))。盤は `tk.Canvas` に描き、`after` で 1 コマずつ動かす |
| `settings_ui.py` | `settings_ui.cpp` | 設定画面 (`SettingsDialog`) |
| `chat.py` | `chat.cpp` | 会話の状態 (`ChatState` / `Message`) と、ワーカースレッドでの送受信 (`send_prompt` / `ask_cloud`) |
| `agent.py` | `agent.cpp` | ルーター・回答役・ChatGPT・書き直し |
| `llm_client.py` | `llm_client.cpp` | ストリーミング送信と SSE の解析、埋め込み (`ask_llm` / `embed`) |
| `rag.py` | `rag.cpp` | RAG のインデックス作成と検索 |
| `config.py` | `config.cpp` | 設定ファイルの読み書き、API キーと秘密ファイル |
| `server_manager.py` | `server_manager.cpp` | `llama-server` の起動・監視・停止 ([managed-server.md](managed-server.md))。公式リリースのダウンロードは Python 版だけ |
| — | `ime.cpp` / `font.cpp` / `macos_window.mm` | Tk が自分で扱うので要らない |

関数名は C++ 版の `SendPrompt` → `send_prompt` のように、snake_case にしただけでそろえています。片方を直したら、もう片方の同じ名前の関数も直してください。

## 設定ファイルの場所

C++ 版と同じ、ユーザーごとのフォルダーの `config/` を読み書きします ([場所](config.md#どこから読むか))。既定値はリポジトリの `config/` で、無いファイルだけを初めて使うときにコピーします (`config.config_directory`)。

- コピーは最初に設定ファイルを読んだときに 1 回だけ行います (`_copied` とロックで守っています)。
- `config/` にあとから増えたファイル (例: `personas.json`) は次の起動でコピーされますが、すでにあるファイルは上書きしません。`config/` 側の更新を取り込むには「すべて既定に戻す」を押すか、設定のフォルダーのそのファイルを消します。
- 環境変数 `LOCALLLM_CONFIG_DIR` を指定したときは、コピーせずにそのフォルダーを直接読み書きします。
- 保存 (`config.save`) は C++ 版と同じく、一時ファイル (`.tmp`) に書いてから `os.replace` で置き換えます。
- 秘密ファイル (`secrets.json`) の場所は C++ 版と同じなので、どちらで API キーを登録しても両方で使えます。

## 画面の更新とスレッド

### ワーカースレッドから画面を直接触らない

Tk の部品は、作ったスレッド (UI スレッド) からしか触れません。そこで C++ 版の `WakeUI()` (SDL のイベントで UI を起こす) の代わりに、次のようにしています。

```
ワーカースレッド
  state.lock を取って messages を書き換える
  state.notify()  … = threading.Event の set (ChatApp._dirty)

UI スレッド (ChatApp._poll、30 ミリ秒ごとに root.after で呼ばれる)
  _dirty が立っていたら下ろして _refresh()
    state.lock を取って、表示に使う値を _Rendered のタプルに写す (スナップショット)
    ロックを放してから、スナップショットで画面を描く
```

- `ChatState` と `RagState` は、作るときに `notify` (再描画を頼む関数) を受け取ります。どちらにも `ChatApp._dirty.set` を渡しています。
- C++ 版と同じく、ロックを持ったまま画面を描かないよう、先に必要な値を写してからロックを放します。
- 受信中は、何も届かなくても 0.5 秒ごとに描き直します (`STATUS_REFRESH_SECONDS`)。ChatGPT を待っている間の経過時間を進めるためです。

### 変わった所から下だけを描き直す

会話の表示は 1 つの `tk.Text` に、メッセージを順に書き込んで作っています。毎回全部を書き直すと、ストリーミング中にちらついたり、長い会話で重くなったりします。

- 前回描いた `_Rendered` の一覧と今回のスナップショットを先頭から比べて、最初に違うメッセージから下だけを消して書き直します (`_render_messages`)。ストリーミング中は、ふつう最後の 1 件だけになります。
- 何も変わっていなければ何もしません。そのため、0.5 秒ごとに描き直しを頼んでも重くなりません。
- 各メッセージの先頭には `msg{番号}.start` のマークを置き、どこから消せばよいか分かるようにしています。
- 一番下までスクロールしていたときだけ、描いた後に一番下へ追従します (C++ 版の自動スクロールと同じ考え方)。

### 中断と終了

- 中断は `CancelToken` で行います。`cancel()` は、フラグを立てるだけでなく**通信中のソケットを `shutdown` して**受信待ちを抜けさせます。C++ 版 (libcurl の進捗コールバックで 1 秒ごとに確認) より速く止まります。
- ワーカースレッドとインデックス作成のスレッドは `daemon=True` です。ウィンドウを閉じるときは中断を頼むだけで、スレッドの終了は待ちません (C++ 版は終了を待ちます)。

## 画面の組み立て (chat_ui.py)

部品の並びは C++ 版と同じです。

```
┌─────────────────────────────────────────┐
│                                    [設定] │  top_bar (タブの上、どのタブからでも開ける)
├─────────────────────────────────────────┤
│ 資料 (RAG): ◉自動 ○常に ○使わない [再読込] 状態 │  _build_rag_bar
├─────────────────────────────────────────┤
│ 会話の表示 (tk.Text、読み取り専用)          │  _build_messages
├─────────────────────────────────────────┤
│ Error: ...                               │
├──────────────────────────────┬──────────┤
│ 入力欄 (tk.Text、4 行)         │ 送信/中断 │  _build_input
│                              │ クリア    │
└──────────────────────────────┴──────────┘
```

- 部品は、RAG の行 → 入力欄 (下から詰める) → 会話の表示 の順に `pack` します。ウィンドウを小さくしたときに、入力欄やボタンではなく会話の表示が縮むようにするためです。
- 文字の色や太さは `Text` のタグ (`user` / `ai` / `note` / `superseded` / `toggle` / `comparison` / `conflict` など) で付けます。違いのまとめの「食い違いあり」は `conflict` タグでエラーの色にします。

### ImGui との違い

| 機能 | C++ 版 (ImGui) | Python 版 (Tk) |
| --- | --- | --- |
| 「ChatGPT の元の回答」の折りたたみ | `TreeNode` | `toggle` タグの付いた「▶ / ▼」の行をクリック (`_toggle_original`)。開いている番号を `_open_originals` に覚える |
| 「ChatGPT に聞き直す」ボタン | 回答の下に `SmallButton` | `Text.window_create` で会話の中に `ttk.Button` を埋め込む。描き直すたびに作り直す |
| 押せない理由 | ツールチップ (`SetItemTooltip`) | Tk にはツールチップが無いので、ボタンの横に文章で書く |
| 右クリックのコピー | `BeginPopupContextItem` | `tk.Menu`。macOS 用に `<Button-2>` と Control+クリックにも割り当てる |
| Enter で送信 / Shift+Enter で改行 | `EnterReturnsTrue` / `CtrlEnterForNewLine` | `<Return>` / `<Shift-Return>` に割り当て、`"break"` を返して既定の改行を止める |
| 日本語入力 (IME) | `ime.cpp` で変換中の文字列を自前で描く | Tk が OS の IME をそのまま使う |
| フォント | 同梱フォント + OS フォントのマージ | OS の標準フォント (`TkDefaultFont`) |
| 背景の半透明 | `window_opacity` | 対応していない |

## 画面の設定 (python_font_size / python_colors)

C++ 版と Python 版で、`ui.json` の別の項目を使います。

| 項目 | C++ 版 | Python 版 |
| --- | --- | --- |
| 文字の大きさ | `font_size` (ピクセル、既定 18) | `python_font_size` (ポイント、既定 14) |
| 色 | `colors` | `python_colors` (キーは同じ) |

- 単位が違う (ピクセルとポイント) うえ、C++ 版は暗い背景、Python 版は OS の明るいテーマを前提に既定の色を決めているので、項目を分けています。
- `python_colors` の `background` / `text` の既定値は、起動時に会話欄の今の色 (OS のテーマの色) を読んで決めます (`_to_hex`)。
- 正しくない値は、標準エラーに書いて既定値を使います。

## 設定画面 (settings_ui.py)

`tk.Toplevel` に `ttk.Notebook` で「画面」「システムプロンプト」「サーバー」「llama-server」のタブを並べます。開いている間は `grab_set` で後ろのチャット画面を操作できなくします。

- 「画面」タブで変えられるのは Python 版の `python_font_size` と `python_colors` だけです (C++ 版の画面の設定は C++ 版で変えます)。動かすたびに画面に反映し、キャンセルすると元に戻します。「色を既定に戻す」もあります。
- 「システムプロンプト」タブは、エージェントごとのタブに `tk.Text` を置き、1 行を `system_prompt` の配列の 1 要素にします。回答役のタブには、人格のテンプレートの選択欄と「書き直す」のチェックがあります。
- 「サーバー」タブには、ローカルの URL、ChatGPT の接続先と API キーの登録欄があります。キーは `save_api_key` で秘密ファイルに書き、画面には末尾 4 文字だけを出します (`mask_key`)。
- 「llama-server」タブでは、自動起動のオン・オフ、モデル、`llama-server` の場所を変えられます ([managed-server.md](managed-server.md))。
- 下の「すべて既定に戻す」は、確認したうえで、ユーザーごとの設定を `config/` の内容で上書きし、画面の設定を読み直してから設定画面を開き直します。
- 読めないファイルがあるときは、壊れたファイルを上書きしないように保存できなくします (C++ 版と同じ)。
- 保存したら `on_saved` で `refresh_cloud_status` を呼び、「ChatGPT に聞き直す」ボタンを押せるかを調べ直します。

## LLM との通信 (llm_client.py)

libcurl の代わりに `http.client` を使います。

- URL が `https` なら `HTTPSConnection` (既定のポート 443)、それ以外は `HTTPConnection` (既定のポート 80) を使います。
- パスにクエリ文字列も付けて送ります (Azure OpenAI の `?api-version=` のため)。
- 接続するまではタイムアウトを付け (埋め込みは 3 秒、チャットはなし)、接続した後はタイムアウトを外して回答を待ちます。
- SSE の解析 (`_parse_line`)、クラウドのときに送る項目の違い、https 以外には API キーを送らないこと、HTTP エラーの表示は C++ 版と同じです ([llm-client.md](llm-client.md))。
- エラーは `LLMError`、中断は `Cancelled` (`LLMError` の子クラス) を投げます。

## RAG (rag.py)

- ビルドしないので、リポジトリの `rag/` をそのまま読みます (`LOCALLLM_RAG_DIR` で変更可)。
- インデックスは `python/rag-index.json` に保存します。形式とチャンクの分け方、`TOP_K` / `MIN_SCORE` / `INDEX_VERSION` は C++ 版と同じです。値を変えるときは両方そろえてください。

## C++ 版と機能をそろえるとき

機能を足すときは、両方に同じものを入れる方針です。次の順で進めると漏れにくくなります。

1. C++ 版で作り、同じ名前 (snake_case) の関数を Python 版に作る。
2. ImGui の部品で書いた所は、上の「ImGui との違い」の表を見て Tk の書き方にする。
3. 設定ファイルに項目を足したら、`config/` の既定値を更新する。すでにある設定ファイルには自動で入らないので、「すべて既定に戻す」で確かめる。
4. `doc/` の説明に Python 版の違いがあれば、このメモに書き足す。
