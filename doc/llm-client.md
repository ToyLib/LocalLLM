# LLM への問い合わせ (llm_client)

このドキュメントでは、ローカルの LLM サーバーに質問を送り、回答をストリーミングで受け取る部分を解説します。
対象のファイルは [src/llm_client.h](../src/llm_client.h) と [src/llm_client.cpp](../src/llm_client.cpp) です。

GUI 側のコード (画面の描画やスレッドの扱い) については [gui-app.md](gui-app.md) を参照してください。

## 概要

- OpenAI 互換の Chat Completions API (`POST /v1/chat/completions`) を呼び出します。
- `"stream": true` を指定して、回答を Server-Sent Events (SSE) として少しずつ受け取ります。
- HTTP 通信には libcurl、JSON の組み立てと解析には nlohmann/json を使います。
- 画面のことは何も知らず、受け取ったテキスト片をコールバックで呼び出し元に渡すだけです。

```
chat.cpp (ワーカースレッド)
    │  AskLLM(history, onDelta, cancel)
    ▼
llm_client.cpp
    │  libcurl で POST  ──────────────▶  llama-server など
    │                                     (127.0.0.1:8080)
    │  WriteCallback ◀── SSE (data: {...}\n\n) ──┘
    │    └ 行に分割 → ProcessLine → delta.content を取り出す
    ▼
onDelta(text)  … テキスト片ごとに呼び出し元へ通知
```

## 公開しているインターフェイス (llm_client.h)

```cpp
using json = nlohmann::json;

json CreateHistory();

using DeltaCallback = std::function<void(std::string_view)>;

struct LLMOptions
{
    int maxTokens = 1024;
    float temperature = -1.0f;   // 負ならサーバーの既定値
    json schema = nullptr;       // JSON スキーマ (出力の形を縛る)
    bool disableThinking = false;
};

std::string AskLLM(
    const json& messages,
    const DeltaCallback& onDelta,
    const std::atomic<bool>& cancel,
    const LLMOptions& options = {});
```

| 名前 | 役割 |
| --- | --- |
| `CreateHistory()` | system プロンプトだけを入れた会話履歴 (JSON 配列) を作ります。呼び出し元はこの後ろに user / assistant のメッセージを追加して `AskLLM` に渡します。 |
| `DeltaCallback` | 回答のテキスト片を受け取るコールバックです。`AskLLM` を呼んだスレッド (GUI ではワーカースレッド) から呼ばれます。 |
| `LLMOptions` | 1 回の問い合わせごとの設定です。省略すると今までどおり `max_tokens: 1024` だけを付けます。`schema` を入れると `response_format` (`json_schema`) を、`disableThinking` を true にすると `chat_template_kwargs.enable_thinking = false` を付けます ([エージェント](agent.md) で使っています)。 |
| `AskLLM()` | 会話履歴を送り、回答が終わるまで処理を返しません (ブロッキング)。戻り値は回答の全文です。失敗したときや中断されたときは `std::runtime_error` を投げます。 |

`AskLLM` は処理が終わるまで戻らないため、GUI から呼ぶときは必ず別スレッドで実行します。

## 設定値

[src/llm_client.cpp](../src/llm_client.cpp) の先頭で定数として定義しています。

| 定数 | 値 | 説明 |
| --- | --- | --- |
| `SERVER_URL` | `http://127.0.0.1:8080/v1/chat/completions` | 問い合わせ先 |
| `SYSTEM_PROMPT` | 「あなたは親切なAIアシスタントです。日本語で回答してください。」 | 会話の先頭に入れる system メッセージ |

リクエストの本文にはこのほかに次の値を入れています。

```json
{
  "model": "local",
  "messages": [ ... ],
  "max_tokens": 1024,
  "stream": true
}
```

`llama-server` は読み込んでいるモデルを 1 つしか持たないため、`model` の値は何でも構いません。
`max_tokens` は 1 回の回答の最大トークン数で、これを超えた回答は途中で打ち切られます。

## 会話履歴の形式

API はステートレスなので、毎回それまでの会話をすべて送ります。履歴は次のような JSON 配列です。

```json
[
  {"role": "system",    "content": "あなたは親切なAIアシスタントです。…"},
  {"role": "user",      "content": "最初の質問"},
  {"role": "assistant", "content": "最初の回答"},
  {"role": "user",      "content": "今回の質問"}
]
```

system メッセージは `CreateHistory()` が入れます。user と assistant のメッセージは GUI 側 (`src/chat.cpp` の `SendPrompt`) が画面に表示しているメッセージから組み立てます。

## AskLLM の処理の流れ

1. `curl_easy_init()` で libcurl のハンドルを作ります。
2. リクエストの JSON を組み立てて文字列にします (`request.dump()`)。
3. 受信の状態を持つ `StreamContext` を用意し、コールバックとキャンセルフラグへのポインタを入れます。
4. libcurl のオプションを設定します。

   | オプション | 設定内容 |
   | --- | --- |
   | `CURLOPT_URL` | `SERVER_URL` |
   | `CURLOPT_HTTPHEADER` | `Content-Type: application/json` と `Accept: text/event-stream` |
   | `CURLOPT_POSTFIELDS` | リクエストの本文 (これを設定すると POST になります) |
   | `CURLOPT_WRITEFUNCTION` / `CURLOPT_WRITEDATA` | 受信データを `WriteCallback` に `StreamContext` 付きで渡します |
   | `CURLOPT_XFERINFOFUNCTION` / `CURLOPT_XFERINFODATA` | 中断の確認に使う `ProgressCallback` |
   | `CURLOPT_NOPROGRESS` = 0 | 進捗コールバックを有効にします (既定では無効) |
   | `CURLOPT_TIMEOUT` = 0 | タイムアウトなし。回答が長いと数分かかることがあるためです |

5. `curl_easy_perform()` で送信し、受信が終わるまで待ちます。この間、データが届くたびに `WriteCallback` が呼ばれます。
6. HTTP のステータスコードを取得し、ヘッダーのリストとハンドルを解放します。
7. 結果を次の順で確認し、問題があれば例外を投げます。

   | 確認する内容 | 例外のメッセージ |
   | --- | --- |
   | キャンセルされたか | `Cancelled` |
   | 受信中にエラーがあったか (`context.error`) | そのエラーの内容 |
   | libcurl の結果が `CURLE_OK` か | `curl_easy_strerror()` の文字列 (例: 接続できないとき `Couldn't connect to server`) |
   | HTTP ステータスが 200 か | `HTTP 500` など |
   | `[DONE]` を受け取ったか | `Stream ended before [DONE]` |

   キャンセルを一番先に確認するのは、中断したときに libcurl が返すエラー (`CURLE_ABORTED_BY_CALLBACK`) をエラーとして表示しないためです。

8. すべて問題なければ、回答の全文 (`context.answer`) を返します。

`curl_global_init()` はプログラム全体で 1 回だけ呼ぶ必要があるため、`main()` の先頭で呼んでいます。

## SSE の受信と解析

### SSE の形式

ストリーミングを指定すると、サーバーは次のような行を少しずつ送ってきます。

```
data: {"choices":[{"delta":{"role":"assistant"}}], ...}

data: {"choices":[{"delta":{"content":"こんにち"}}], ...}

data: {"choices":[{"delta":{"content":"は"}}], ...}

data: [DONE]
```

- 1 つのイベントは `data: ` で始まる行で、イベントの間は空行で区切られます。
- `:` で始まる行はコメント (接続を保つためのものなど) です。
- 回答の本文は `choices[0].delta.content` に少しずつ入ってきます。
- 最後に `data: [DONE]` が来たら回答は終わりです。

### WriteCallback: 受信データを行に分ける

libcurl は受信したデータを都合のよい大きさで区切って渡すため、1 回の呼び出しで行の途中までしか届かないことがあります。
そこで `WriteCallback` は受け取ったデータをいったん `context.buffer` の後ろに追加し、改行 (`\n`) が見つかるたびに 1 行を取り出して `ProcessLine` に渡します。改行が来ていない残りはバッファに残し、次の呼び出しで続きとつなげます。

行末の `\r` は取り除くので、改行が `\r\n` のサーバーにも対応します。

UTF-8 の 1 文字が受信データの区切りで分かれて届いても、行がそろってから解析するので文字化けしません。

`ProcessLine` で例外が出たときは、そのメッセージを `context.error` に保存して `0` を返します。
受け取ったバイト数と違う値を返すと libcurl は転送を止めるので、エラーが起きた時点で受信が終わります。
例外を libcurl (C のライブラリ) の外に投げてはいけないので、ここですべて受け止めています。

### ProcessLine: 1 行を解析する

1. 空行と `:` で始まるコメント行は無視します。
2. `data:` で始まらない行 (`event:` など) も無視します。
3. `data:` と、その後ろのスペース 1 つを取り除きます。
4. 中身が `[DONE]` なら `context.done = true` にして終わります。
5. JSON として解析します。解析できなければ `Invalid SSE JSON` の例外を投げます。
6. `error` キーがあれば、その内容を例外にします (サーバー側のエラー)。
7. `choices[0].delta.content` が文字列なら、`context.answer` に追加し、`onDelta` を呼んで呼び出し元に渡します。

`role` だけが入った最初のイベントや、`finish_reason` だけが入った最後のイベントのように `content` を含まない行は、何もせずに次へ進みます。

## 中断の仕組み

GUI の「中断」ボタンを押すと、`std::atomic<bool>` の `cancel` が `true` になります。

`CURLOPT_NOPROGRESS` を 0 にしているので、libcurl は転送中に `ProgressCallback` を何度も呼びます。
このコールバックは `cancel` を確認し、`true` なら `1` を返します。0 以外を返すと libcurl は転送を止め、`curl_easy_perform()` が戻ります。

進捗コールバックは、データが届かない間もおよそ 1 秒に 1 回は呼ばれます。そのため、モデルが最初のトークンを出す前でも、遅くとも 1 秒ほどで中断できます。

中断したときは `Cancelled` の例外を投げます。途中まで受け取った回答をどうするかは GUI 側で決めます ([gui-app.md](gui-app.md) の「エラーと中断の扱い」を参照)。

## スレッドについて

- `AskLLM` はワーカースレッドから呼ばれるので、`onDelta` もワーカースレッドで実行されます。コールバックで GUI のデータを書き換えるときは、呼び出し側で排他制御が必要です。
- `cancel` は GUI のスレッドが書き、ワーカースレッドが読むため `std::atomic<bool>` にしています。
- libcurl のハンドルは `AskLLM` の中で作って捨てるので、ほかのスレッドと共有しません。

## 別のサーバーに問い合わせたいとき

- 接続先を変えるときは、`SERVER_URL` を書き換えます。OpenAI 互換の API を持つサーバー (Ollama や LM Studio など) であれば、そのまま使えます。
- 認証が必要なサーバーでは、`Authorization: Bearer ...` のヘッダーを `curl_slist_append` で追加する必要があります。
- `max_tokens` や `temperature` は `LLMOptions` で変えられます。`model` など、ほかのパラメーターは `AskLLM` の中の `request` で変更できます。
