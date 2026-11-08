# 全体の構成

アプリ全体の流れを 1 枚にまとめ、各メモへの入口にするためのメモです。

## 登場するもの

```
┌──────────── アプリ (C++ 版 / Python 版) ────────────┐
│ 画面 (chat_ui)  ⇄  会話の状態とワーカースレッド (chat)   │
│                      │                              │
│      エージェント (agent)        RAG (rag)            │
│                      │              │               │
│              llm_client (HTTP + SSE)                 │
│  設定 (config: config/ / user-config/ / secrets.json) │
└──────────┬───────────────┬──────────────┬──────────┘
           │               │              │ (ボタンを押したときだけ)
           ▼               ▼              ▼
   llama-server        llama-server     ChatGPT
   チャット (8080)      埋め込み (8081)   (OpenAI API, https)
```

| 部分 | C++ 版 | Python 版 | 説明 |
| --- | --- | --- | --- |
| 画面 | `main.cpp`、`chat_ui.cpp`、`settings_ui.cpp` | `chat_ui.py`、`settings_ui.py` | [gui-app.md](gui-app.md)、[python-app.md](python-app.md) |
| 会話の状態と送受信 | `chat.cpp` | `chat.py` | [gui-app.md](gui-app.md#会話の状態とスレッド-chatcpp) |
| エージェント | `agent.cpp` | `agent.py` | [agent.md](agent.md) |
| RAG | `rag.cpp` | `rag.py` | [rag.md](rag.md) |
| サーバーとの通信 | `llm_client.cpp` | `llm_client.py` | [llm-client.md](llm-client.md) |
| 設定 | `config.cpp` | `config.py` | [config.md](config.md) |
| ChatGPT への聞き直し | (上のすべてにまたがる) | | [chatgpt.md](chatgpt.md) |

## 質問を送ったとき

```
送信ボタン (UI スレッド)
  └ SendPrompt: 質問と空の回答を messages に足し、ワーカースレッドを起動
       │
       ▼ (ワーカースレッド)
  資料 (RAG) が「自動」なら
    ルーター ── 資料の一覧 + 直前の会話 + 質問 ──▶ チャット (8080)
      ◀── {"reason": ..., "use_rag": true/false}
  回答役を選ぶ (資料を使う → assistant、使わない → general)
  資料を使うなら
    SearchRag ── 質問 ──▶ 埋め込み (8081)
      → 保存済みのチャンクと比べて上位 3 件 (類似度 0.5 以上) を質問に添える
  ルーターの判断と参照した資料を、回答の note に入れる (回答より先に表示される)
  RunAgent ── system プロンプト + 履歴 + 質問 ──▶ チャット (8080)
    ◀── SSE でテキスト片 → messages の最後の回答に足す → WakeUI
```

## 「ChatGPT に聞き直す」を押したとき

```
ボタン (UI スレッド)
  資料を使った回答なら、確認ダイアログ
  └ AskCloud: 元の回答を superseded にし、新しい回答を足してワーカースレッドを起動
       │
       ▼ (ワーカースレッド)
  ChatGPT ── 資料を使っていない過去のやりとり + 最新の質問 ──▶ OpenAI (https)
    ◀── 全部受け取るまで画面に流さない (進み具合だけ表示)
  回答役の rewrite が true なら
    RewriteAnswer ── 回答役のプロンプト + 書き直しの指示 + 質問と回答 ──▶ チャット (8080)
      ◀── 言い直した回答をストリーミング表示
  false なら ChatGPT の回答をそのまま表示
  CompareAnswers ── 違いのまとめの指示 + 質問 + 聞き直す前の回答 + ChatGPT の元の回答 ──▶ チャット (8080)
    ◀── 違いのまとめ (JSON) を回答の下に添える
```

## 起動したとき

1. 設定ファイルの場所を決める (C++ 版は実行ファイルの隣、Python 版は `user-config/` を必要なら作る)。
2. `ui.json` を読んでウィンドウとフォントを用意する。
3. API キーがあるかを調べる (`RefreshCloudStatus`)。
4. 別スレッドで RAG のインデックスを作る (変わっていないファイルは保存済みのベクトルを使い回す)。

## スレッド

| スレッド | 仕事 | 共有する状態 |
| --- | --- | --- |
| UI スレッド | 画面を描く、ボタンの処理 | `ChatState` / `RagState` をロックして読む |
| 会話のワーカースレッド (1 本) | ルーター・検索・回答・聞き直し | `ChatState` の `messages` に書く |
| インデックス作成のスレッド | `rag/` の読み込みと埋め込み | `RagState` のチャンクと状態に書く |

ワーカースレッドは書き換えたあと UI を起こします (C++ 版は SDL のイベント、Python 版は `threading.Event` を UI スレッドが 30 ミリ秒ごとに見る)。中断は `cancel` のフラグで、どの段階でも止められます。

## データの行き先

どのデータがどこへ送られるかは [privacy.md](privacy.md) にまとめています。
