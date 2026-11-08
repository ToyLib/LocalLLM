# 設定ファイルと環境変数

設定の置き場所と全項目を 1 か所にまとめたメモです。機能ごとの説明は各メモ (リンク先) を参照してください。

## ファイルの一覧

| ファイル | 内容 | いつ読むか | 詳しい説明 |
| --- | --- | --- | --- |
| `ui.json` | 画面 (フォント・色・透明度) | 起動時 (設定画面で動かすとすぐ反映) | [gui-app.md](gui-app.md)、[python-app.md](python-app.md) |
| `server.json` | ローカルのサーバーと ChatGPT の接続先 | 送信のたび | [llm-client.md](llm-client.md)、[chatgpt.md](chatgpt.md) |
| `agents.json` | エージェントの名前と system プロンプト | 質問のたび | [agent.md](agent.md) |
| `personas.json` | 回答役の人格のテンプレート | 設定画面を開くたび | [agent.md](agent.md#人格のテンプレート-configpersonasjson) |
| `secrets.json` | API キー (**`config/` には置かない**) | ボタンを押せるかの確認時と、ChatGPT に送るとき | [chatgpt.md](chatgpt.md#api-キーの登録) |

`server.json` と `agents.json` は使うたびに読み直すので、書き換えれば再起動しなくても次の質問から反映されます。

## どこから読むか

既定値はリポジトリの `config/` に置き、git で管理します。アプリは `config/` を直接は書き換えません。
実際に読み書きするのは、ユーザーごとのフォルダーの `config/` です。C++ 版と Python 版で共有します (`ui.json` の文字の大きさと色は、版ごとに別のキーです)。

| OS | 設定のフォルダー |
| --- | --- |
| macOS | `~/Library/Application Support/LocalLLM/config/` |
| Linux | `$XDG_CONFIG_HOME/LocalLLM/config/` (無ければ `~/.config/LocalLLM/config/`) |
| Windows | `%APPDATA%\LocalLLM\config\` |

| | 内容 |
| --- | --- |
| 既定値のコピー | 最初に設定ファイルを読むときに、**無いファイルだけ**。元は C++ 版が実行ファイルの隣の `config/` (ビルドのたびにリポジトリの `config/` からコピー)、Python 版がリポジトリの `config/` |
| 設定画面で保存したもの | ユーザーごとのフォルダーに書く。ビルドしても `build/` を消しても残る |
| リポジトリの `config/` の更新 | 自動では取り込まない (キーごとのマージもしない) |
| 既定に戻す | 設定画面の「すべて既定に戻す」。または、アプリを閉じてから、設定のフォルダーのファイル (フォルダーごとでもよい) を消す。API キーは別のファイルなので消えない |

- 環境変数 `LOCALLLM_CONFIG_DIR` を指定すると、どちらの版もそのフォルダーを直接読み書きします (コピーはしません)。
- 保存は、一時ファイルに書いてから置き換えます。読めないファイルがあるときは、壊れたファイルを上書きしないよう、設定画面で保存できなくなります。
- 設定画面で扱わない項目 (例: C++ 版から見た `python_font_size`) も、順番を保ったまま残して保存します。

## ui.json

| キー | 既定値 | 説明 |
| --- | --- | --- |
| `window_opacity` | `0.85` | 背景の不透明度 (0〜1、C++ 版のみ) |
| `font_file` | `Asset/MPLUSRounded1c-Bold.ttf` | フォントファイル (C++ 版のみ。相対パスは実行ファイルのフォルダーから。再起動後に反映) |
| `font_size` | `18` | 文字の大きさ (ピクセル、C++ 版) |
| `colors` | (下の表) | 色 (C++ 版) |
| `python_font_size` | `14` | 文字の大きさ (ポイント、Python 版) |
| `python_colors` | (OS のテーマの色など) | 色 (Python 版。キーは `colors` と同じ) |

| `colors` のキー | 色を付ける所 | C++ 版の既定 |
| --- | --- | --- |
| `background` | チャット画面の背景 | `#0F0F0F` |
| `text` | 文字 | `#FFFFFF` |
| `sub_text` | 補足の文字 (考え中・参照した資料・RAG の状態) | `#808080` |
| `user` | 送信者名「You」 | `#73BFFF` |
| `ai` | 回答したエージェントの名前 | `#8CE68C` |
| `error` | エラー | `#FF6666` |

色は `"#RRGGBB"` で書きます。無い・正しくない項目は、ログに書いて既定値を使います。

## server.json

| キー | 既定値 | 説明 |
| --- | --- | --- |
| `chat_url` | `http://127.0.0.1:8080/v1/chat/completions` | ローカルのチャット用サーバー |
| `embed_url` | `http://127.0.0.1:8081/v1/embeddings` | ローカルの埋め込み用サーバー (RAG) |
| `managed.enabled` | `false` (`config/` の既定値は `true`) | アプリが `llama-server` を起動するか ([managed-server.md](managed-server.md)) |
| `managed.server_path` | (自動で探す) | 使う `llama-server` |
| `managed.chat_model` | `unsloth/Qwen3-4B-Instruct-2507-GGUF:Q4_K_M` | チャットのモデル (`-hf` の値) |
| `managed.embed_model` | `ggml-org/bge-m3-Q8_0-GGUF` | 埋め込みのモデル (空なら起動しない) |
| `managed.context` | `4096` | チャットのコンテキスト長 |
| `managed.embed_context` | `8192` | 埋め込みのコンテキスト長とバッチサイズ |
| `managed.gpu_layers` | `99` | GPU に載せる層の数 |
| `cloud.name` | `ChatGPT` | 画面に出す名前 |
| `cloud.chat_url` | `https://api.openai.com/v1/chat/completions` | 問い合わせ先 (https のときだけ API キーを送る) |
| `cloud.model` | `gpt-5-mini` | モデル名 |
| `cloud.api_key_name` | `OPENAI_API_KEY` | API キーを探す名前 |
| `cloud.auth` | `bearer` | `bearer` か `api-key` (Azure OpenAI) |
| `cloud.max_completion_tokens` | (送らない) | 回答の最大トークン数 |
| `cloud.reasoning_effort` | (送らない) | 推論の深さ (`minimal` / `low` / `medium` / `high`) |

`chat_url` / `embed_url` を別のマシンにすると、質問や資料がそのマシンに送られます ([privacy.md](privacy.md))。

## agents.json

| キー | 中身 | 無いとき |
| --- | --- | --- |
| `router` | `name`、`system_prompt` | エラー (資料を参照して回答を続ける) |
| `assistant` / `general` | `name`、`system_prompt`、`rewrite` | エラーを表示 |
| `cloud` | `name`、`system_prompt` | 既定のプロンプト (名前は `server.json` の `cloud.name`) |
| `rewriter` | `name`、`system_prompt` | 既定のプロンプト |
| `comparer` | `name`、`system_prompt` | 既定のプロンプト |

`system_prompt` は文字列か、文字列の配列 (区切りなしでつなげる) です。ルーターの `maxTokens` / `temperature` / JSON スキーマなど、コードと結びついた設定は `agent.cpp` / `agent.py` にあります。

## personas.json

```json
{
  "templates": [
    {"name": "侍", "system_prompt": ["..."]}
  ]
}
```

設定画面で回答役に読み込むひな形です。読み込んでから直して保存しても、`personas.json` は変わりません。

## secrets.json (API キー)

キーの値は `config/` のファイルには書かず、`server.json` の `cloud.api_key_name` の名前で次の順に探します。

1. 同じ名前の環境変数
2. ユーザーごとの秘密ファイル

| OS | 秘密ファイル |
| --- | --- |
| macOS | `~/Library/Application Support/LocalLLM/secrets.json` |
| Linux | `$XDG_CONFIG_HOME/LocalLLM/secrets.json` (無ければ `~/.config/LocalLLM/secrets.json`) |
| Windows | `%APPDATA%\LocalLLM\secrets.json` |

C++ 版と Python 版は同じファイルを使います。macOS / Linux では `0600` で作ります。

## 環境変数

| 環境変数 | 内容 | 版 |
| --- | --- | --- |
| `LOCALLLM_CONFIG_DIR` | 設定ファイルのフォルダー (ユーザーごとのフォルダーの代わり) | 両方 |
| `LOCALLLM_RAG_DIR` | RAG の資料のフォルダー (既定は C++ 版が実行ファイルの隣の `rag/`、Python 版がリポジトリの `rag/`) | 両方 |
| `LOCALLLM_SECRETS_FILE` | 秘密ファイルの場所 | 両方 |
| `LOCALLLM_FONT` | フォントファイル (`ui.json` の `font_file` より優先) | C++ 版 |
| `OPENAI_API_KEY` など | API キー (名前は `cloud.api_key_name`) | 両方 |

macOS では、Finder から起動したアプリはシェルの環境変数を読みません。環境変数で渡すなら、ターミナルから起動してください。

## コードに残している設定

設定ファイルにせず、コードの定数にしているものです。

| 内容 | 場所 |
| --- | --- |
| ローカルの回答の最大トークン数 (1024) | `llm_client.h` の `LLMOptions::maxTokens` |
| ルーターの設定 (256 トークン、temperature 0、スキーマ) | `agent.cpp` の `CreateRouter` |
| 書き直しの最大トークン数 (4096) | `agent.cpp` の `REWRITE_MAX_TOKENS` |
| 違いのまとめの設定 (512 トークン、temperature 0、スキーマ、1 つの回答は 3,600 バイトまで) | `agent.cpp` の `CompareAnswers` / `COMPARE_ANSWER_BYTES` |
| チャンクの大きさ・添える資料の数・類似度の下限 | `rag.cpp` の `MAX_CHUNK_CHARS` / `TOP_K` / `MIN_SCORE` |
| ウィンドウの初期サイズ (900×700) | `main.cpp` / `chat_ui.py` |

Python 版にも同じ名前の定数があります (`LLMOptions` の項目は `max_tokens` のように snake_case)。変えるときは両方そろえてください。
