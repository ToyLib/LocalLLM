# エージェント (役割ごとに LLM を呼び分ける)

同じ LLM サーバー (8080 番) に、役割 (system プロンプト) と設定を変えて問い合わせるしくみです。
今は「ルーター」というエージェントが、質問ごとに `rag/` の資料を参照するかどうかを判断します。

## ルーター

画面上部の「資料 (RAG)」で選びます。

| 選択 | 動作 |
| --- | --- |
| 自動 (既定) | ルーターが質問を見て、資料を参照するかを決める |
| 常に | 毎回資料を参照する (ルーターを入れる前と同じ) |
| 使わない | 資料を参照しない |

「自動」では、回答の下にルーターの判断と理由が表示されます。

```
ルーター: 資料を参照します (ミケのおやつについての質問)
参照:
  [1] samples/ごはん.md (おやつ) 0.71
```

- 「こんにちは」「1+1は？」のような質問では資料を検索しないので、関係のない資料が添えられて回答がぶれることがなくなります。
- ルーターには資料のファイル名の一覧と、直前の会話 (最大 4 件、1 件 300 バイトまで) を渡します。「それは何グラム？」のような続きの質問も判断できます。
- ルーターの呼び出しに失敗したときは、資料を参照して回答を続けます (「常に」と同じ動き)。
- `rag/` に資料が無いときは、ルーターを呼ばずに資料なしで回答します。

## しくみ

```
[質問のたび]  (chat.cpp: SendPrompt → DecideRag → agent.cpp: RouteQuestion)
  ルーターに「資料の一覧 + 直前の会話 + 質問」を送る
    → {"reason": "...", "use_rag": true/false} の JSON で返ってくる
  use_rag が true なら、いつもどおり SearchRag で資料を検索して質問に添える
  回答役 (CreateHistory の system プロンプト) が AskLLM でストリーミング回答
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
| `RouteQuestion()` | ルーターを作って呼び、資料が必要かの判断 (`RouteDecision`) を返す |

どれも `AskLLM` と同じくブロッキングなので、ワーカースレッドから呼びます。

## エージェントを増やすとき

`agent.cpp` の `CreateRouter()` と同じように `Agent` を作り、`RunAgent` / `RunAgentJson` で呼びます。
呼び出しはワーカースレッド (`chat.cpp` の `SendPrompt`) の中に足します。

- 呼ぶ回数のぶんだけ回答が遅くなります。画面に見せない段階は `maxTokens` を小さくしてください。
- 途中経過を見せたいときは、ルーターと同じように `Message::note` に書いて `WakeUI()` を呼びます。
