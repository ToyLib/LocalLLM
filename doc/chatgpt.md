# ChatGPT に聞き直す

ふだんの回答はローカルの LLM が作ります。物足りないときは、最新の回答の下にある「ChatGPT に聞き直す」ボタンで ChatGPT (OpenAI API) に聞き直せます。
ChatGPT の回答は、元の回答役 (侍など) がローカルの LLM で自分の口調に言い直して表示します。

設計の考え方は [chatgpt-plan.md](chatgpt-plan.md)、ローカルも含めたデータの行き先は [privacy.md](privacy.md) にまとめています。

## 流れ

```
質問 → ルーター (ローカル) → 回答役 (ローカル: アシスタント / 侍など) が答える
   ↓ 「ChatGPT に聞き直す」を押す
ChatGPT が答えを作る (全部受け取るまで画面には流さない)
   ↓
同じ回答役がローカルで口調を書き直す → ストリーミングで表示
```

- ルーターと RAG の検索はいつもローカルで行います。ChatGPT に送るのは、ボタンを押したときだけです。
- 回答役の「書き直す」がオフなら (既定ではアシスタントがオフ)、ChatGPT の回答をそのまま表示します。
- 書き直した回答の下の「▶ ChatGPT の元の回答」を開くと、書き直す前の文章を見られます。小さいモデルは数字や手順を変えてしまうことがあるので、気になるときは見比べてください。
- 聞き直す前の回答は薄く残り、これ以降の質問で LLM に送る履歴からは外れます。
- 書き直しに失敗したときは、ChatGPT の回答をそのまま表示します。ChatGPT に聞けなかったとき (キーが無効・回数制限・ネットワークエラー) は、元の回答に戻してエラーを表示します。
- 待っている間は、回答の上に今の段階と進み具合を出します (下の「進み具合の表示」)。
- 中断ボタンでいつでも止められます。ChatGPT の回答を待っている間に止めると元の回答に戻ります。書き直しの途中で止めると、そこまでの文章を残します。

## 進み具合の表示

ChatGPT の回答は書き直してから表示するので、届くまで本文は出ません。代わりに、回答の上に次のような行を出します。経過時間は、何も届かなくても 1 秒ごとに進みます。

| 段階 | 表示の例 |
| --- | --- |
| 問い合わせて、最初の文字を待っている | `ChatGPT に問い合わせ中… (12 秒)` |
| 回答を受け取っている | `ChatGPT から受信中… 1,234 文字 (18 秒)` |
| 回答役が書き直している | `侍が言い直し中… 340 文字 (元の回答は 1,234 文字) (5 秒)` |

- 推論するモデル (gpt-5 系など) は、考え終わるまで 1 文字も返さないので、最初の段階が長くなることがあります。
- 最初の文字が届かないまま 20 秒たつと、「時間がかかっています。中断ボタンで止めると、元の回答に戻ります。」と添えます。
- 書き直すと長さが変わるので、元の回答の文字数は目安です。
- 待ち時間を短くしたいときは、`server.json` の `cloud` に `"reasoning_effort": "low"` (または `"minimal"`) を書くと、推論が短くなります。

## 送る内容 (資料を守るために)

`rag/` の資料は個人のメモなので、ChatGPT にはなるべく送りません。

- 資料を使って答えたやりとりは、ChatGPT に送る履歴から除きます。
- 聞き直す回答そのものが資料を使っていたときは、確認ダイアログを出し、「送る」を選んだときだけ、その質問と資料を送ります。
- ルーターに渡す資料の一覧 (ファイル名と見出し) は送りません。

## ボタンが押せないとき

| 状態 | 表示 |
| --- | --- |
| API キーが無い | 押せない (C++ 版はツールチップ、Python 版はボタンの横に「設定画面で API キーを登録すると使えます」) |
| 回答の受信中・書き直し中 | 押せない |

API キーの有無は、起動したときと設定画面で保存したときに調べます。キーが無効かどうかは押してみるまで分からないので、無効なら「HTTP 401 (API キーが無効です)」と表示します。

## API キーの登録

キーの値は設定ファイルに書きません。`server.json` の `cloud.api_key_name` (既定 `OPENAI_API_KEY`) の名前で、次の順に探します。

1. 同じ名前の環境変数
2. ユーザーごとの秘密ファイル

| OS | 秘密ファイル |
| --- | --- |
| macOS | `~/Library/Application Support/LocalLLM/secrets.json` |
| Linux | `$XDG_CONFIG_HOME/LocalLLM/secrets.json` (無ければ `~/.config/LocalLLM/secrets.json`) |
| Windows | `%APPDATA%\LocalLLM\secrets.json` |

- 設定画面の「サーバー」タブで「新しい API キー」に貼り付けて保存すると、秘密ファイルに書きます。C++ 版と Python 版は同じファイルを使います。
- 画面には末尾 4 文字だけを表示します (例: `設定済み (…abcd)`)。
- 「秘密ファイルから削除」を押してから保存すると、キーを消します。環境変数のキーは画面からは消せません。
- macOS / Linux では、秘密ファイルを本人だけが読み書きできる権限 (`0600`) で作ります。
- 環境変数 `LOCALLLM_SECRETS_FILE` で秘密ファイルの場所を変えられます。
- macOS では、Finder から起動したアプリはシェルの環境変数を読みません。環境変数で渡すなら、ターミナルから起動してください。

### キーの作り方のおすすめ

- OpenAI の管理画面で、このアプリ用のプロジェクトを 1 つ作り、その中に環境 (マシン) ごとに別のキーを作ります (例: `localllm-macbook`)。漏れたときやマシンを手放すときに、そのキーだけを無効にできます。
- 利用上限 (予算) はプロジェクトに設定します。
- キーの権限は「制限付き」にして、モデルの呼び出しだけを許可しておくと安心です。

## 設定

### config/server.json の cloud

```json
{
  "chat_url": "http://127.0.0.1:8080/v1/chat/completions",
  "embed_url": "http://127.0.0.1:8081/v1/embeddings",
  "cloud": {
    "name": "ChatGPT",
    "chat_url": "https://api.openai.com/v1/chat/completions",
    "model": "gpt-5-mini",
    "api_key_name": "OPENAI_API_KEY",
    "auth": "bearer"
  }
}
```

| キー | 既定値 | 説明 |
| --- | --- | --- |
| `name` | `ChatGPT` | 画面に出す名前 (ボタンや回答者名) |
| `chat_url` | `https://api.openai.com/v1/chat/completions` | 問い合わせ先。**https のときだけ** API キーを送ります |
| `model` | `gpt-5-mini` | モデル名 |
| `api_key_name` | `OPENAI_API_KEY` | API キーを探す名前 |
| `auth` | `bearer` | `bearer` なら `Authorization: Bearer`、`api-key` なら `api-key:` ヘッダー (Azure OpenAI 用) |
| `max_completion_tokens` | (送らない) | 回答の最大トークン数 |
| `reasoning_effort` | (送らない) | 推論の深さ (`minimal` / `low` / `medium` / `high`) |

- `cloud` が無い (前の版の) `server.json` でも、既定値で動きます。設定画面で保存すると `cloud` が書き足されます。
- 表示名・URL・モデル・API キーの名前は、設定画面の「サーバー」タブでも変えられます。
- ChatGPT には `max_tokens`・`temperature`・`chat_template_kwargs` (llama-server 専用) を送りません。OpenAI の推論モデルは `temperature` を受け付けないためです。

### config/agents.json

| キー | 説明 |
| --- | --- |
| `assistant.rewrite` / `general.rewrite` | ChatGPT の回答をこの回答役の口調で書き直すか (既定: アシスタントは `false`、侍は `true`) |
| `cloud` | ChatGPT に送る system プロンプト |
| `rewriter` | 書き直すときに、回答役の system プロンプトの後ろに付ける指示 |

- どれも設定画面の「システムプロンプト」タブで変えられます。
- 前の版の `agents.json` に無くても、既定のプロンプトで動きます。

## コード

| 内容 | C++ 版 | Python 版 |
| --- | --- | --- |
| API キーと秘密ファイル | `config.cpp` (`FindApiKey` / `SaveApiKey`) | `config.py` (`find_api_key` / `save_api_key`) |
| クラウドへのリクエスト | `llm_client.cpp` (`LLMOptions::cloud`、`LoadCloudSettings`) | `llm_client.py` |
| ChatGPT と書き直し役 | `agent.cpp` (`CreateCloud` / `RewriteAnswer`) | `agent.py` |
| 聞き直しの流れと送る履歴 | `chat.cpp` (`AskCloud`) | `chat.py` (`ask_cloud`) |
| ボタン・確認・表示 | `chat_ui.cpp` | `chat_ui.py` |
| API キーの登録画面 | `settings_ui.cpp` | `settings_ui.py` |
