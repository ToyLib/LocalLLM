# エージェント (役割ごとに LLM を呼び分ける)

同じ LLM サーバー (8080 番) に、役割 (system プロンプト) と設定を変えて問い合わせるしくみです。
ふだんの質問で働くのは次の 3 つです。

| エージェント (キー) | 役割 | 口調 |
| --- | --- | --- |
| ルーター (`router`) | 質問ごとに `rag/` の資料を参照するかを決める (画面には出ない) | — |
| アシスタント (`assistant`) | 資料を参照する質問に、資料に基づいて答える | 普通の丁寧な口調 |
| アシスタント (`general`) | 資料の要らない質問 (雑談や一般的な質問) に答える | 普通の丁寧な口調 (`assistant` と同じ) |

「ChatGPT に聞き直す」ボタンを押したときは、さらに次の 3 つが働きます ([ChatGPT に聞き直す](chatgpt.md))。

| エージェント (キー) | 役割 | 接続先 |
| --- | --- | --- |
| ChatGPT (`cloud`) | 最新の質問に答え直す | クラウド (`server.json` の `cloud`) |
| 書き直し (`rewriter`) | ChatGPT の回答を、元の回答役の口調で言い直す。独立したエージェントではなく、回答役の system プロンプトの後ろに指示を足して使う | ローカル |
| 違いのまとめ (`comparer`) | 聞き直す前の回答と ChatGPT の回答の違いを、短くまとめる (目安) | ローカル |

既定では、回答役の 2 つはどちらも同じアシスタントです。ルーターがどちらに振り分けたかは、回答の下のルーターの判断で分かります。
設定画面で人格のテンプレート (侍など) を読み込めば、回答役ごとに口調を変えられます。口調を変えると、振り分けが回答を見るだけで分かります。
回答の上には、答えたエージェントの名前 (`アシスタント` など。聞き直したときは `アシスタント (ChatGPT)` や、書き直したときは `侍 (ChatGPT → ローカルで書き直し)` など) も表示されます。

## system プロンプトの設定 (config/agents.json)

各エージェントの名前と system プロンプトは `config/agents.json` に書きます (C++ 版・Python 版で共通)。

```json
{
  "assistant": {
    "name": "アシスタント",
    "system_prompt": [
      "あなたは親切なAIアシスタントです。",
      "日本語で回答してください。"
    ]
  }
}
```

- キーは `router` / `assistant` / `general` / `cloud` / `rewriter` / `comparer` の 6 つです。`general` は資料の要らない質問に答える汎用の回答役で、既定のプロンプトは `assistant` と同じアシスタントです。
- `system_prompt` は文字列か、文字列の配列です。配列は区切りなしでつなげて 1 つのプロンプトにします。
- 回答役 (`assistant` / `general`) には `rewrite` (true / false) も書けます。ChatGPT に聞き直した回答をこの回答役の口調で書き直すかどうかです (既定はどちらも `false`。侍などの口調に変えたときは `true` にします)。
- `cloud` / `rewriter` / `comparer` は無くても既定のプロンプト (`agent.cpp` の `DefaultAgentPrompt`) で動きます。`cloud` の表示名は `agents.json` ではなく `server.json` の `cloud.name` を使います。
- 質問のたびに読み直すので、ファイルを書き換えればアプリを再起動しなくても次の質問から反映されます。
- アプリの「設定」ボタンから開く設定画面の「システムプロンプト」タブでも編集できます。1 行が配列の 1 要素になります (元が文字列なら文字列のまま保存します)。
- 回答役 (`assistant` / `general`) のタブでは、人格のテンプレートを選んで名前とプロンプトに読み込めます (下の「人格のテンプレート」)。
- アプリが読むのは、ユーザーごとのフォルダーの `config/agents.json` です ([場所](config.md#どこから読むか))。リポジトリの `config/agents.json` は既定値で、無いときにコピーされます。設定画面の「すべて既定に戻す」で既定値に戻せます。
- 環境変数 `LOCALLLM_CONFIG_DIR` で `config/` の代わりのフォルダを指定できます (`server.json` もそこから読みます)。
- ルーターの `maxTokens` / `temperature` / JSON スキーマなどの設定は、コードと結びついているので `agent.cpp` に残しています。
- ファイルが無い・読めないときは、回答の代わりにエラーを表示します (ルーターの失敗は、いつもどおり資料を参照して回答を続けます)。

## 人格のテンプレート (config/personas.json)

設定画面で回答役に読み込む、名前と system プロンプトのひな形です。`templates` の配列に、`agents.json` と同じ形 (`name` と `system_prompt`) で並べます。

| テンプレート | 口調 |
| --- | --- |
| アシスタント | 普通の丁寧な口調 (`assistant` / `general` の既定と同じ) |
| 侍 | 「拙者」「お主」「〜でござる」 |
| 悟空風 | 「オラ」「おめえ」「〜だぞ」「ワクワクすっぞ」 |
| バカボンのパパ風 | 「わし」「〜なのだ」、締めは「これでいいのだ！」 |
| 大阪のおばちゃん | 関西弁、「あんた」「飴ちゃんいる？」 |
| 執事 | 「ご主人様」「〜でございます」 |
| 猫 | 「吾輩」「〜にゃ」 |

- 「このテンプレートにする」を押すと、編集中の名前とプロンプトがテンプレートの内容に置き換わります。保存するまでファイルは変わらず、キャンセルすれば元に戻ります。
- 読み込んだあとに直して保存しても、`personas.json` は変わりません。自分のテンプレートを増やしたいときは、`personas.json` に書き足します (C++ 版はビルドすると実行ファイルの隣にコピーされます)。
- 設定画面は開くたびに `personas.json` を読み直します。読めなくても、テンプレートが選べないだけで設定の保存はできます。
- 小さいモデルでは、口調が混ざったり崩れたりすることがあります。気になるときはプロンプトに口調の例を足すと安定します。

## ルーター

画面上部の「資料 (RAG)」で選びます。

| 選択 | 動作 |
| --- | --- |
| 自動 (既定) | ルーターが質問を見て、資料を参照するかを決める |
| 常に | 毎回資料を参照する (ルーターを入れる前と同じ) |
| 使わない | 資料を参照しない |

資料を参照するときは `assistant` が、参照しないときは `general` が答えます (「常に」ならいつも `assistant`、「使わない」ならいつも `general`)。既定ではどちらもアシスタントです。

「自動」では、回答の下にルーターの判断と理由が表示されます。

```
ルーター: 資料を参照します (ミケのおやつについての質問)
参照:
  [1] samples/ごはん.md (おやつ) 0.71
```

- 「こんにちは」「1+1は？」のような質問では資料を検索しないので、関係のない資料が添えられて回答がぶれることがなくなります。
- ルーターには資料の一覧と、直前の会話 (最大 4 件、1 件 300 バイトまで) を渡します。「それは何グラム？」のような続きの質問も判断できます。
- 資料の一覧は、資料ごとに「名前: 見出し / 見出し …」の 1 行です (`rag.cpp` の `DescribeRagSources`)。見出しの無い `.txt` は書き出しの 40 文字を載せます。ファイル名だけだと、「ノリ」が猫の名前だとルーターが分からず、資料を参照しないことがあったためです。
- それでも 4B クラスのモデルでは判断を外すことがあります (Gemma 3 4B で試したところ、「日曜日の夜に猫の具合が悪くなったら？」「鍵はどこ？」は資料を参照しませんでした)。確実に資料を使いたいときは「常に」にしてください。
- ルーターの呼び出しに失敗したときは、資料を参照して回答を続けます (「常に」と同じ動き)。
- `rag/` に資料が無いときは、ルーターを呼ばずに資料なしで回答します (`general` が答えます)。
- アシスタントが選ばれても、検索で関係のある資料が見つからなかったときは、資料なしでアシスタントが答えます。
- 会話履歴には両方のエージェントの回答が混ざります。口調がつられないように、それぞれの system プロンプトで「ほかの口調の回答があっても自分の口調で答える」と指示しています。

## しくみ

```
[質問のたび]  (chat.cpp: SendPrompt → DecideRag → agent.cpp: RouteQuestion)
  ルーターに「資料の一覧 (名前と見出し) + 直前の会話 + 質問」を送る
    → {"reason": "...", "use_rag": true/false} の JSON で返ってくる
  use_rag が true なら assistant、false なら general を回答役に選ぶ
  use_rag が true なら、いつもどおり SearchRag で資料を検索して質問に添える
  回答役が RunAgent でストリーミング回答 (system プロンプトは回答役のもの)
```

ルーターの回答は画面には流さず、全部受け取ってから JSON として読みます。

```
[「ChatGPT に聞き直す」を押したとき]  (chat.cpp: AskCloud)
  ChatGPT (CreateCloud) に「資料を使っていない過去のやりとり + 最新の質問」を送る
    → 全部受け取るまで画面には流さない
  回答役の rewrite が true なら、RewriteAnswer で回答役がローカルで言い直す (ストリーミング表示)
  false なら ChatGPT の回答をそのまま表示する
  最後に CompareAnswers で、聞き直す前の回答と ChatGPT の元の回答の違いをまとめて添える
```

書き直しでは、回答役の system プロンプトの後ろに `rewriter` の指示を足し、「## 質問」「## 回答」をまとめた 1 つの user メッセージだけを送ります (会話履歴は送りません)。`maxTokens` は 4096 (`REWRITE_MAX_TOKENS`)、`disableThinking` は true です。

違いのまとめでは、`comparer` の system プロンプトに、「## 質問」「## ローカルの回答」「## 聞き直した回答」をまとめた 1 つの user メッセージを送ります。回答はそれぞれ 3,600 バイト (`COMPARE_ANSWER_BYTES`) までに縮めます。ルーターと同じく JSON スキーマで形を決め (`points`: 1〜3 個の文字列、`verdict`: 「ほぼ同じ / 情報が増えた / 食い違いあり」のどれか)、先に違いを書かせてから判定させます。`maxTokens` は 512、`temperature` は 0、`disableThinking` は true です。`points` に `minItems: 1` を付けているのは、付けないと空の配列だけを返しがちだったためです。

### 小さいモデルでも JSON を崩さないために

`LLMOptions::schema` に JSON スキーマを入れると、リクエストに `response_format` (`json_schema`) を付けます。
llama-server はスキーマから文法を作って出力を縛るので、4B クラスのモデルでも決まった形の JSON が返ります。

ルーターではほかに次の設定をしています。

| 設定 | 値 | 理由 |
| --- | --- | --- |
| `maxTokens` | 256 | 判断と短い理由だけなので。gpt-oss は推論のトークンもここに含まれるので少し余裕を持たせている |
| `temperature` | 0 | 同じ質問には同じ判断を返すように |
| `disableThinking` | true | Qwen3 などの考える過程を出すモデルで、考えずにすぐ答えさせる (`chat_template_kwargs.enable_thinking = false`)。gpt-oss は推論を切れないので `reasoning_effort = "low"` も送って推論を最小にする |

スキーマでは `reason` を `use_rag` より先に置いています。先に理由を書かせてから結論を出させるほうが、判断が安定します。

## API (src/common/agent.h)

```cpp
struct Agent
{
    std::string key;           // agents.json のキー
    std::string name;
    std::string systemPrompt;
    LLMOptions options;        // options.cloud が true ならクラウドに送る
    bool rewrite = true;       // ChatGPT の回答をこの口調で書き直すか
};

std::string RunAgent(const Agent&, const json& messages,
                     const DeltaCallback& onDelta, const std::atomic<bool>& cancel);

json RunAgentJson(const Agent&, const json& messages, const std::atomic<bool>& cancel);

std::string RewriteAnswer(const Agent& persona, const std::string& question,
                          const std::string& answer,
                          const DeltaCallback& onDelta, const std::atomic<bool>& cancel);

RouteDecision RouteQuestion(const json& history, const std::string& question,
                            const std::vector<std::string>& sources,
                            const std::atomic<bool>& cancel);
```

| 名前 | 役割 |
| --- | --- |
| `Agent` | 役割 (system プロンプト) と、1 回の問い合わせの設定 (`LLMOptions`) |
| `RunAgent()` | system プロンプトを先頭に付けて `AskLLM` を呼ぶ。`messages` は user / assistant だけでよい |
| `RunAgentJson()` | `RunAgent` の回答を JSON として読む (`options.schema` を入れておく) |
| `CreateAssistant()` / `CreateGeneral()` / `CreateAnswerer(key)` | 回答役のエージェントを作る |
| `CreateCloud()` | ChatGPT に聞き直すエージェントを作る (`options.cloud = true`) |
| `RewriteAnswer()` | クラウドの回答を、回答役 `persona` の口調でローカルの LLM に書き直させる |
| `CompareAnswers()` | ローカルの回答とクラウドの回答の違いを、ローカルの LLM にまとめさせる (`Comparison`) |
| `DefaultAgentName()` / `DefaultAgentPrompt()` | `agents.json` に `cloud` / `rewriter` / `comparer` が無いときの既定値 |
| `RouteQuestion()` | ルーターを作って呼び、資料が必要かの判断 (`RouteDecision`) を返す |

どれも `AskLLM` と同じくブロッキングなので、ワーカースレッドから呼びます。

## エージェントを増やすとき

`config/agents.json` にエントリーを足し、`agent.cpp` の `CreateRouter()` や `CreateGeneral()` と同じように `LoadAgent("キー")` で `Agent` を作り、`RunAgent` / `RunAgentJson` で呼びます。
呼び出しはワーカースレッド (`chat.cpp` の `SendPrompt`) の中に足します。

- 呼ぶ回数のぶんだけ回答が遅くなります。画面に見せない段階は `maxTokens` を小さくしてください。
- 途中経過を見せたいときは、ルーターと同じように `Message::note` に書いて `WakeUI()` を呼びます。
