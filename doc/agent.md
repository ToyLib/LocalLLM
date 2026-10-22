# エージェント (役割ごとに LLM を呼び分ける)

同じ LLM サーバー (8080 番) に、役割 (system プロンプト) と設定を変えて問い合わせるしくみです。
エージェントは 3 種類あります。

| エージェント | 役割 | 口調 |
| --- | --- | --- |
| ルーター | 質問ごとに `rag/` の資料を参照するかを決める (画面には出ない) | — |
| アシスタント | 資料を参照する質問に、資料に基づいて答える | 普通の丁寧な口調 |
| 侍 | 資料の要らない質問 (雑談や一般的な質問) に答える | 時代劇の侍の口調 (「拙者」「〜でござる」) |

回答役の 2 つは口調を変えてあるので、ルーターがどちらに振り分けたかが回答を見るだけで分かります。
回答の上には、答えたエージェントの名前 (`アシスタント` / `侍`) も表示されます。

## ルーター

画面上部の「資料 (RAG)」で選びます。

| 選択 | 動作 |
| --- | --- |
| 自動 (既定) | ルーターが質問を見て、資料を参照するかを決める |
| 常に | 毎回資料を参照する (ルーターを入れる前と同じ) |
| 使わない | 資料を参照しない |

資料を参照するときはアシスタントが、参照しないときは侍が答えます (「常に」ならいつもアシスタント、「使わない」ならいつも侍)。

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
- `rag/` に資料が無いときは、ルーターを呼ばずに資料なしで回答します (侍が答えます)。
- アシスタントが選ばれても、検索で関係のある資料が見つからなかったときは、資料なしでアシスタントが答えます。
- 会話履歴には両方のエージェントの回答が混ざります。口調がつられないように、それぞれの system プロンプトで「ほかの口調の回答があっても自分の口調で答える」と指示しています。

## しくみ

```
[質問のたび]  (chat.cpp: SendPrompt → DecideRag → agent.cpp: RouteQuestion)
  ルーターに「資料の一覧 (名前と見出し) + 直前の会話 + 質問」を送る
    → {"reason": "...", "use_rag": true/false} の JSON で返ってくる
  use_rag が true なら アシスタント、false なら 侍 を回答役に選ぶ
  use_rag が true なら、いつもどおり SearchRag で資料を検索して質問に添える
  回答役が RunAgent でストリーミング回答 (system プロンプトは回答役のもの)
```

ルーターの回答は画面には流さず、全部受け取ってから JSON として読みます。

### 小さいモデルでも JSON を崩さないために

`LLMOptions::schema` に JSON スキーマを入れると、リクエストに `response_format` (`json_schema`) を付けます。
llama-server はスキーマから文法を作って出力を縛るので、4B クラスのモデルでも決まった形の JSON が返ります。

ルーターではほかに次の設定をしています。

| 設定 | 値 | 理由 |
| --- | --- | --- |
| `maxTokens` | 128 | 判断と短い理由だけなので |
| `temperature` | 0 | 同じ質問には同じ判断を返すように |
| `disableThinking` | true | Qwen3 などの考える過程を出すモデルで、考えずにすぐ答えさせる (`chat_template_kwargs.enable_thinking = false`) |

スキーマでは `reason` を `use_rag` より先に置いています。先に理由を書かせてから結論を出させるほうが、判断が安定します。

## API (src/agent.h)

```cpp
struct Agent
{
    std::string name;
    std::string systemPrompt;
    LLMOptions options;
};

std::string RunAgent(const Agent&, const json& messages,
                     const DeltaCallback& onDelta, const std::atomic<bool>& cancel);

json RunAgentJson(const Agent&, const json& messages, const std::atomic<bool>& cancel);

RouteDecision RouteQuestion(const json& history, const std::string& question,
                            const std::vector<std::string>& sources,
                            const std::atomic<bool>& cancel);
```

| 名前 | 役割 |
| --- | --- |
| `Agent` | 役割 (system プロンプト) と、1 回の問い合わせの設定 (`LLMOptions`) |
| `RunAgent()` | system プロンプトを先頭に付けて `AskLLM` を呼ぶ。`messages` は user / assistant だけでよい |
| `RunAgentJson()` | `RunAgent` の回答を JSON として読む (`options.schema` を入れておく) |
| `CreateAssistant()` / `CreateSamurai()` | 回答役のエージェントを作る |
| `RouteQuestion()` | ルーターを作って呼び、資料が必要かの判断 (`RouteDecision`) を返す |

どれも `AskLLM` と同じくブロッキングなので、ワーカースレッドから呼びます。

## エージェントを増やすとき

`agent.cpp` の `CreateRouter()` や `CreateSamurai()` と同じように `Agent` を作り、`RunAgent` / `RunAgentJson` で呼びます。
呼び出しはワーカースレッド (`chat.cpp` の `SendPrompt`) の中に足します。

- 呼ぶ回数のぶんだけ回答が遅くなります。画面に見せない段階は `maxTokens` を小さくしてください。
- 途中経過を見せたいときは、ルーターと同じように `Message::note` に書いて `WakeUI()` を呼びます。
