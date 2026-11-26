# 全体の構成

アプリ全体の流れを 1 枚にまとめ、各メモへの入口にするためのメモです。

## 登場するもの

```
┌──────────── アプリ (C++ 版 / Python 版) ────────────┐
│ 画面 (Application + タブ) ⇄  会話の状態とワーカー (chat) │
│                      │                              │
│      エージェント (agent)        RAG (rag)            │
│                      │              │               │
│              llm_client (HTTP + SSE)                 │
│  サーバーの管理 (server_manager: 起動・監視・停止)       │
│  設定 (config: 既定値 config/ → ユーザーごとのフォルダー) │
└──────────┬───────────────┬──────────────┬──────────┘
           │               │              │ (ボタンを押したときだけ)
           ▼               ▼              ▼
   llama-server        llama-server     ChatGPT
   チャット (8080)      埋め込み (8081)   (OpenAI API, https)
```

| 部分 | C++ 版 | Python 版 | 説明 |
| --- | --- | --- | --- |
| アプリ本体 (タブと共有の状態、起動と終了) | `app/application.cpp` (`Application`) | `app/application.py` (`Application`) | [class-design.md](class-design.md)、[gui-app.md](gui-app.md)、[python-app.md](python-app.md#アプリ本体とタブ) |
| ウィンドウと描画 | `app/renderer.cpp` (`Renderer`) | (Tk が受け持つ) | [class-design.md](class-design.md)、[gui-app.md](gui-app.md) |
| タブの共通の形 | `common/tab.h` (`Tab` / `UiContext`) | `common/tab.py` (`Tab` / `UiContext`) | [class-design.md](class-design.md)、[gui-app.md](gui-app.md#タブ-tabh)、[python-app.md](python-app.md#アプリ本体とタブ) |
| 画面の部品のヘルパー | `common/widgets.h` (`ui::`) | `common/widgets.py` | [gui-app.md](gui-app.md#imgui-のラッパー-widgetsh)、[python-app.md](python-app.md#tk-のヘルパー-commonwidgetspy) |
| タブ | `chat_ui.cpp` (`ChatTab`)、`quiz_ui.cpp` (`QuizTab`)、`maze_ui.cpp` (`MazeTab`)、`sandbox_ui.cpp` (`SandboxTab`) | `chat_ui.py` (`ChatTab`)、`quiz_ui.py` (`QuizTab`)、`maze_ui.py` (`MazeTab`)、`sandbox_ui.py` (`SandboxTab`) | [gui-app.md](gui-app.md)、[python-app.md](python-app.md)、[quiz.md](quiz.md)、[maze.md](maze.md)、[sandbox.md](sandbox.md) |
| 設定画面 | `settings_ui.cpp` | `settings_ui.py` (`SettingsDialog`) | [gui-app.md](gui-app.md)、[python-app.md](python-app.md#設定画面-settings_uipy) |
| 会話の状態と送受信 | `chat.cpp` | `chat.py` | [gui-app.md](gui-app.md#会話の状態とスレッド-chatcpp) |
| エージェント | `agent.cpp` | `agent.py` | [agent.md](agent.md) |
| RAG | `rag.cpp` | `rag.py` | [rag.md](rag.md) |
| サーバーとの通信 | `llm_client.cpp` | `llm_client.py` | [llm-client.md](llm-client.md) |
| サーバーの管理 | `server_manager.cpp` | `server_manager.py` | [managed-server.md](managed-server.md) |
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
  資料を使った回答なら、設定 (allow_rag) がオフ (既定) ならボタンは押せない。オンなら確認ダイアログ
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

1. 設定ファイルの場所を決める (ユーザーごとのフォルダーの `config/`。無いファイルは既定値からコピーする)。
2. `ui.json` を読んでウィンドウとフォントを用意する。
3. API キーがあるかを調べる (`RefreshCloudStatus`)。
4. `server.json` の `managed` がオンなら、`llama-server` を起動する ([managed-server.md](managed-server.md))。
5. 別スレッドで RAG のインデックスを作る (埋め込みサーバーの準備を待ち、変わっていないファイルは保存済みのベクトルを使い回す)。

## スレッド

| スレッド | 仕事 | 共有する状態 |
| --- | --- | --- |
| UI スレッド | 画面を描く、ボタンの処理 | `ChatState` / `RagState` をロックして読む |
| 会話のワーカースレッド (1 本) | ルーター・検索・回答・聞き直し | `ChatState` の `messages` に書く |
| 箱庭の指示を処理するワーカー | チャット用サーバーで指示を 1 つのコマンドに分類 | C++ 版は future、Python 版はキューで UI に結果を返す |
| インデックス作成のスレッド | `rag/` の読み込みと埋め込み | `RagState` のチャンクと状態に書く |
| サーバー管理のスレッド | `llama-server` の起動・監視・停止 | 状態をロックして公開する |

会話のワーカーは書き換えたあと UI に更新を知らせます (C++ 版は SDL のイベント、Python 版は `threading.Event` を UI スレッドが 30 ミリ秒ごとに見る)。中断は `cancel` のフラグで、どの段階でも止められます。

## データの行き先

どのデータがどこへ送られるかは [privacy.md](privacy.md) にまとめています。
