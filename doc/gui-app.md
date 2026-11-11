# GUI アプリとしての構成

このドキュメントでは、チャット画面を表示する GUI アプリの部分を解説します。
対象のファイルは `src/` 以下の次のファイルです。

| ファイル | 内容 |
| --- | --- |
| [main.cpp](../src/main.cpp) | SDL / ImGui の初期化、メインループ、後始末 |
| [chat.h](../src/chat.h) / [chat.cpp](../src/chat.cpp) | 会話の状態 (`ChatState`)、ワーカースレッドでの送受信 (`SendPrompt`)、UI を起こすイベント (`WakeUI`) |
| [chat_ui.h](../src/chat_ui.h) / [chat_ui.cpp](../src/chat_ui.cpp) | 画面の組み立て (`DrawUI`、`DrawMessages`、`DrawRagBar`、聞き直しの確認ダイアログ) |
| [code_ui.h](../src/code_ui.h) / [code_ui.cpp](../src/code_ui.cpp)、[code_mode.h](../src/code_mode.h) / [code_mode.cpp](../src/code_mode.cpp) | コーディング練習タブ ([code-mode.md](code-mode.md)) |
| [settings_ui.h](../src/settings_ui.h) / [settings_ui.cpp](../src/settings_ui.cpp) | 設定画面 (`DrawSettings`) と画面の設定 (`UiSettings`) |
| [config.h](../src/config.h) / [config.cpp](../src/config.cpp) | 設定ファイルの読み書き、API キーと秘密ファイル |
| [ime.h](../src/ime.h) / [ime.cpp](../src/ime.cpp) | IME の変換中の文字列の表示 |
| [font.h](../src/font.h) / [font.cpp](../src/font.cpp) | 日本語フォントの読み込み (`LoadFont`) |
| [macos_window.h](../src/macos_window.h) / [macos_window.mm](../src/macos_window.mm) | macOS のタイトルバーの設定 (macOS のみ) |

全体の流れは [architecture.md](architecture.md)、設定ファイルは [config.md](config.md) にまとめています。
LLM サーバーとの通信 (`AskLLM`) については [llm-client.md](llm-client.md)、回答役の選び方は [agent.md](agent.md)、資料の検索は [rag.md](rag.md)、ChatGPT への聞き直しは [chatgpt.md](chatgpt.md) を参照してください。
Python 版 (`python/`) は同じ構成を Tk で作ったもので、C++ 版との違いは [python-app.md](python-app.md) にまとめています。

## 使っているライブラリ

| ライブラリ | 用途 |
| --- | --- |
| SDL3 | ウィンドウの作成、キーボード・マウス・IME のイベント、描画 (`SDL_Renderer`) |
| Dear ImGui | ボタンや入力欄などの UI 部品 (イミディエイトモード GUI) |
| `imgui_impl_sdl3` | SDL3 のイベントを ImGui に渡すバックエンド |
| `imgui_impl_sdlrenderer3` | ImGui の描画データを `SDL_Renderer` で描くバックエンド |
| `imgui_stdlib` | `ImGui::InputText` に `std::string` を直接渡せるようにする補助 |

Dear ImGui は CMake ファイルを持たないため、[CMakeLists.txt](../CMakeLists.txt) で FetchContent を使って取得し、必要なソースを静的ライブラリ `imgui` としてビルドしています。

### イミディエイトモード GUI とは

ImGui では、ボタンや入力欄をオブジェクトとして作って保持するのではなく、**毎フレーム、UI を描く関数を呼び直す** 書き方をします。

```cpp
if (ImGui::Button("送信"))   // ボタンを描き、押されたフレームだけ true を返す
    Send();
```

画面の状態 (会話の内容や入力中の文字列) はアプリ側の変数に持っておき、毎フレームそれをもとに UI を描きます。
このため、データを書き換えれば次のフレームで画面に反映されます。

## 全体の構成

```
main()                         … main.cpp
 ├ 初期化: curl / SDL / ウィンドウ / レンダラー / ImGui / フォント
 ├ メインループ
 │   ├ イベント処理 (ImGui に渡す・IME の変換中の文字列・終了)
 │   ├ ImGui の新しいフレームを開始
 │   ├ DrawUI()          … 画面全体を組み立てる (chat_ui.cpp)
 │   │   ├ DrawServerBar()   … サーバーの状態 (タブの外)
 │   │   ├ タブ「チャット」
 │   │   │   ├ DrawRagBar()      … 資料 (RAG) の選択・再読込・インデックスの状態・設定ボタン
 │   │   │   ├ DrawMessages()    … 会話の一覧と「ChatGPT に聞き直す」ボタン
 │   │   │   ├ 聞き直しの確認ダイアログ (資料を使った回答のときだけ)
 │   │   │   ├ 入力欄・送信/中断・クリアのボタン
 │   │   │   └ DrawComposition() … IME の変換中の文字列 (ime.cpp)
 │   │   ├ タブ「コーディング練習」
 │   │   │   └ DrawCodeTab()     … 依頼・コード・解説・実行結果 (code_ui.cpp、[code-mode.md](code-mode.md))
 │   │   └ DrawSettings()    … 設定画面 (モーダル、settings_ui.cpp)
 │   └ 描画して画面に表示
 └ 後始末: 受信中なら中断 → ワーカースレッドの終了待ち → 各ライブラリの終了

SendPrompt()  … 送信ボタンで呼ばれ、ワーカースレッドでルーター → 回答役 (RunAgent → AskLLM) を実行 (chat.cpp)
AskCloud()    … 聞き直しボタンで呼ばれ、ワーカースレッドで ChatGPT → 書き直し → 違いのまとめを実行 (chat.cpp)
```

## 起動処理 (main.cpp の main の前半)

1. `curl_global_init()` で libcurl を初期化します。
2. `SetImeHint()` で、IME の変換中の文字列はアプリが描くことを SDL に伝えます (後述)。
3. `SDL_Init(SDL_INIT_VIDEO)` で SDL を初期化します。
4. `RegisterWakeEvent()` で独自のイベント番号を 1 つ確保します (`SDL_RegisterEvents`)。ワーカースレッドから UI を起こすのに使います (後述)。
5. ディスプレイの拡大率 (`SDL_GetDisplayContentScale`) を取得し、900×700 に拡大率を掛けた大きさでウィンドウを作ります。
   - `SDL_WINDOW_HIGH_PIXEL_DENSITY` で、Retina などの高解像度ディスプレイでもぼやけずに描きます。
   - `SDL_WINDOW_HIDDEN` で作成し、中央に移動してから表示するので、ウィンドウが一瞬別の場所に見えることはありません。
   - `SDL_WINDOW_TRANSPARENT` を付けて、ウィンドウの背景を透明にします。
6. `SDL_CreateRenderer()` でレンダラーを作り、VSync を有効にします。
   - macOS では続けて `ApplyMacTitleBarStyle()` を呼びます。透明にしただけではタイトルバーだけが素通しで透けてしまうため、本文をタイトルバーの下まで広げ (`NSWindowStyleMaskFullSizeContentView`)、タイトルバーも ImGui の半透明の背景で塗って本文と一体にします。
   - タイトルバーに中身が隠れないよう、毎フレーム `GetMacTitleBarHeight()` で高さを求め、`DrawUI` に `topInset` として渡します。背景はタイトルバーの下まで塗り、中身はその下から並べます。
7. ImGui を初期化します。
   - `io.IniFilename = nullptr` で、ウィンドウ配置を保存する `imgui.ini` を作らないようにします。
   - キーボードでのフォーカス移動 (`NavEnableKeyboard`) を有効にします。
   - ダークテーマを使い、余白などの大きさとフォントを拡大率に合わせます (`ScaleAllSizes`、`FontScaleDpi`)。
   - `config/ui.json` を読み (`LoadConfigOrEmpty`)、ウィンドウ背景の不透明度を `window_opacity` にします。読めない項目は `settings_ui.h` の `UiSettings` の既定値を使います。
8. 日本語フォントを読み込みます (`LoadFont`。ファイルと大きさは `ui.json` の `font_file` / `font_size`)。
9. ImGui の SDL3 用・SDL_Renderer 用のバックエンドを初期化します。
10. `InstallImeHook()` で、IME のカーソル位置を受け取るため、バックエンドの `Platform_SetImeDataFn` を `SetImeData` で包みます (後述)。

## 日本語フォントの読み込み (font.cpp)

ImGui に入っている標準フォントには日本語の文字がないため、次の順でフォントを探します。

1. 環境変数 `LOCALLLM_FONT` で指定されたフォント
2. `config/ui.json` の `font_file` (既定は同梱フォント `Asset/MPLUSRounded1c-Bold.ttf`。相対パスは実行ファイルのあるフォルダーから。`SDL_GetBasePath()` で取得)
3. OS の日本語フォント (`SYSTEM_FONT_PATHS`。Windows は游ゴシックなど、macOS はヒラギノ、Linux は Noto Sans CJK)

1 か 2 で読み込めた場合でも、3 の OS フォントを **マージモード** (`ImFontConfig::MergeMode`) で追加します。
こうすると、同梱フォントにない文字 (一部の漢字や記号) は OS フォントの字形で表示されます。
どのフォントも見つからなければ、ImGui の標準フォントを使います (日本語は表示できません)。

同梱フォントは、ビルド後に CMake が `Asset` フォルダーごと実行ファイルの隣にコピーします。

## メインループと省電力

```cpp
bool hasEvent = framesToRender > 0
    ? SDL_PollEvent(&event)            // 待たずに確認
    : SDL_WaitEventTimeout(&event, 500); // イベントが来るまで最大 0.5 秒待つ
```

ゲームのように毎フレーム描き続けると、何もしていなくても CPU や GPU を使い続けます。
このアプリでは、イベント (キー入力、マウス移動、受信など) があったら `framesToRender = 3` にして数フレームだけ描き、その後は `SDL_WaitEventTimeout` でイベントが来るまで眠ります。

数フレーム続けて描くのは、ImGui ではボタンを押した結果などが次のフレームで画面に反映されることがあるためです。
0.5 秒のタイムアウトは、入力欄のカーソルの点滅などを止めないための保険です。

ウィンドウが最小化されているときは描画を省き、10 ミリ秒待ってから次のループに進みます。

描画では、まず画面を透明な色でクリアし、その上に ImGui の描画データ (半透明の背景を含む) を重ねます。
高解像度ディスプレイでは `SDL_SetRenderScale` でフレームバッファの倍率を設定してから描きます。

## 会話の状態とスレッド (chat.cpp)

### ChatState

UI スレッドとワーカースレッドで共有するデータを 1 つの構造体にまとめています。

| メンバー | 内容 |
| --- | --- |
| `mutex` | 下のメンバーを守るための排他ロック |
| `messages` | 画面に表示している会話 (`Message` の配列、user / assistant)。system プロンプトは含みません |
| `busy` | 回答を受信中かどうか |
| `error` | 最後に起きたエラーのメッセージ |
| `restoredPrompt` | 失敗したときに入力欄へ戻す質問 |
| `cancel` | 中断の要求 (`std::atomic<bool>`。ロックなしで読み書きします) |
| `worker` | 受信用のワーカースレッド |
| `ragMode` | 資料 (RAG) を「自動 / 常に / 使わない」のどれにするか (UI スレッドだけが触ります) |
| `hasCloudKey` / `cloudName` | ChatGPT の API キーがあるかと、その表示名。起動時と設定を保存したときに `RefreshCloudStatus` で調べ直します (毎フレームファイルを読まないため) |

### Message

`messages` の 1 件です。画面に出す本文のほかに、エージェントや ChatGPT の聞き直しのための情報を持ちます。

| メンバー | 内容 |
| --- | --- |
| `role` / `content` | `user` か `assistant` と、本文 |
| `note` | 回答の下に薄く出す補足 (ルーターの判断、参照した資料など) |
| `agent` / `agentKey` | 答えたエージェントの表示名と、`agents.json` のキー (聞き直したときの書き直しに使う) |
| `ragPrompt` | user: 資料を添えて送った質問 (聞き直しで資料も送るときに使う) |
| `usedRag` | assistant: 資料を使って答えたか (ChatGPT 宛ての履歴から除く印) |
| `superseded` | assistant: 聞き直して置き換えられた回答 (LLM に送る履歴から除き、薄く表示する) |
| `cloudOriginal` | assistant: 書き直す前の ChatGPT の回答 (折りたたんで表示する) |
| `compareVerdict` / `comparePoints` | assistant: 聞き直す前の回答との違いのまとめ (判定と要点。まとめていなければ空) |
| `status` / `statusSince` / `receivedChars` / `totalChars` | 回答の途中の状態 (「ChatGPT に問い合わせ中…」など)。`FormatStatus` で経過時間と文字数を添えて表示する |

### SendPrompt: 質問の送信

1. 前回のワーカースレッドが残っていれば終了を待ちます (`JoinWorker`)。
2. ロックを取り、次のことをまとめて行います。
   - `messages` に今回の質問 (user) を追加する
   - `messages` をつなげて、送信する会話履歴を作る (聞き直して置き換えた `superseded` の回答は除く。system プロンプトは回答役のエージェントが付ける)
   - 回答を書き込むための空の assistant メッセージを追加する
   - `busy = true` にし、前回のエラーを消す
3. ワーカースレッドを起動し、回答役のエージェントを決めて ([エージェント](agent.md)) `RunAgent()` から `AskLLM()` を呼びます。履歴はスレッドにコピーで渡すので、UI 側で `messages` が変わっても影響しません。

`AskLLM()` に渡すコールバックでは、ロックを取ってから最後の assistant メッセージにテキスト片を追加し、`WakeUI()` で UI を起こします。
これにより、回答が届くそばから画面に表示されます。

### WakeUI: ワーカースレッドから画面を更新する

UI スレッドは `SDL_WaitEventTimeout` で眠っていることがあるため、ワーカースレッドがデータを書き換えただけでは、すぐに画面に反映されません。
そこで、起動時に確保した独自のイベント (`g_wakeEvent`) を `SDL_PushEvent` で送ります。`SDL_PushEvent` はほかのスレッドから呼んでも安全です。
イベントを受け取った UI スレッドは目を覚まし、`framesToRender = 3` にして画面を描き直します。

### エラーと中断の扱い

`AskLLM()` が例外を投げたとき、ワーカースレッドは次のように処理します。

| 状況 | 会話の扱い | エラー表示 |
| --- | --- | --- |
| 中断した・回答が途中まで届いていた | 途中までの回答を残す | なし |
| 中断した・回答がまだ何も届いていなかった | 質問と空の回答を取り除き、質問を入力欄に戻す | なし |
| エラー (接続できない、HTTP エラーなど) | 質問と空の回答を取り除き、質問を入力欄に戻す | 入力欄の上に赤字で表示 |

質問を入力欄に戻すのは、サーバーを起動し忘れたときなどに、同じ質問を打ち直さずに再送信できるようにするためです。
戻された質問は、次のフレームで `DrawUI` が入力欄に入れます (`restoredPrompt`)。

最後に `busy = false` にし、`WakeUI()` で画面を更新します。

### 終了時

ウィンドウを閉じたときに受信中であれば、`cancel = true` にしてからワーカースレッドの終了を待ちます。
`AskLLM()` は遅くとも 1 秒ほどで中断に気づくので、終了が長く待たされることはありません。

## 画面の組み立て (chat_ui.cpp)

```
┌────────────────────────────────────────────┐
│ 資料 (RAG): ◉自動 ○常に ○使わない [再読込] 6 ファイル / 54 チャンク   [設定] │
├────────────────────────────────────────────┤
│ You                                        │
│ 質問の内容                                  │
│ ────────────────────────────────────────── │  ← 会話の表示 (子ウィンドウ "messages")
│ アシスタント                               │
│ 回答の内容…                                 │
│ ルーター: …  (補足)                         │
│ [ChatGPT に聞き直す]  (最新の回答だけ)        │
├────────────────────────────────────────────┤
│ Error: ...  (エラーがあるときだけ)            │
├──────────────────────────────────┬─────────┤
│                                  │  送信    │  ← 受信中は「中断」
│ 入力欄 (4 行分)                   ├─────────┤
│                                  │  クリア  │  ← 受信中は押せない
└──────────────────────────────────┴─────────┘
```

- ImGui のウィンドウ "Chat" を、OS のウィンドウいっぱいの大きさで、枠やタイトルなしで表示します (`NoDecoration`、`NoMove`)。OS のウィンドウの大きさを変えると、画面全体が追従します。
- 最初にロックを取り、`busy` と `error` をローカル変数にコピーします。ロックを持ったまま UI 全体を描くと、その間ワーカースレッドが回答を書き込めなくなるためです。
- 入力欄とエラー表示の高さを先に計算し、残りの高さを会話の表示に使います。`BeginChild` に負の高さを渡すと「残りからその分を引いた高さ」という意味になります。
- ボタンの幅は「クリア」の文字幅の 2.5 倍にしています。入力欄の幅は、ウィンドウの幅からボタンの幅を引いた残りです。

### 会話の表示 (DrawMessages)

- 送信者の名前 (`You` は青、AI の回答は答えたエージェントの名前を緑) と本文を表示し、メッセージの間に区切り線を引きます。
- 本文は `TextWrapped` で、ウィンドウの幅に合わせて折り返します。
- 受信中で回答がまだ空のときは「考え中...」と表示します。`status` があるとき (ChatGPT に聞き直している間) は、代わりに `FormatStatus` の結果 (段階・文字数・経過時間) を出します。
- `superseded` の回答は、名前に「(聞き直す前の回答)」を付けて、本文ごと `TextDisabled` で薄く表示します。
- `cloudOriginal` があれば、「ChatGPT の元の回答」を `TreeNode` で折りたたんで表示します。
- `compareVerdict` があれば、聞き直す前の回答との違いのまとめを出します。「食い違いあり」はエラーの色、要点は `Bullet` を付けて薄く表示します。
- `note` (ルーターの判断や参照した資料) は本文の下に薄く表示します。
- 最新の回答 (`superseded` でなく、`agentKey` があるもの) の下にだけ「ChatGPT に聞き直す」ボタン (`DrawAskCloudButton`) を出します。押せないとき (`CanAskCloud` が false、API キーが無い) は `BeginDisabled` にします。キーが無いときと、資料も送ることになるときは `SetItemTooltip` で理由を出します。資料を使った回答なら、確認ダイアログで「送る」を選んだときだけ `AskCloud` を呼びます。
- メッセージを右クリックすると「コピー」のメニューが出て、本文をクリップボードにコピーできます。
- 各メッセージは `PushID` で番号を付けて区別しています。同じ名前 ("copy") のポップアップをメッセージごとに作るためです。

**自動スクロール**: 描画の前に、今のスクロール位置が一番下かどうかを調べておきます。一番下にいたら、描画の後に `SetScrollHereY(1.0f)` で一番下までスクロールします。
こうすると、回答の受信中は最新の文字を追いかけて表示し、ユーザーが上にスクロールして過去の会話を読んでいるときは、その位置を保ちます。

### 入力欄とキー操作

```cpp
ImGui::InputTextMultiline("##input", &input, size,
    ImGuiInputTextFlags_EnterReturnsTrue |
    ImGuiInputTextFlags_CtrlEnterForNewLine |
    ImGuiInputTextFlags_WordWrap);
```

| キー | 動作 |
| --- | --- |
| Enter | 送信 (`EnterReturnsTrue` で、Enter が押されたフレームに true を返します) |
| Shift+Enter / Ctrl+Enter | 改行 |

- `WordWrap` で、長い行は入力欄の幅で折り返します。
- ラベルの `##input` は、`##` より後ろが ID として使われ、画面には表示されません。
- Enter を押すと ImGui は入力欄のフォーカスを外すので、次のフレームで `SetKeyboardFocusHere()` を呼んで入力欄にフォーカスを戻します (`focusInput`)。送信後やクリア後、質問が戻されたときも同じようにフォーカスを戻します。
- 空白や改行だけの入力は送信しません。

### ボタン

- 「送信」: 受信中でなければ表示します。押すか入力欄で Enter を押すと `SendPrompt()` を呼び、入力欄を空にします。
- 「中断」: 受信中は「送信」の代わりに表示します。押すと `state.cancel = true` にします。
- 「クリア」: 会話とエラーを消します。受信中は `BeginDisabled` で押せなくしています。受信中に `messages` を消すと、ワーカースレッドが書き込む先 (最後の assistant メッセージ) がなくなるためです。

## IME の変換中の文字列 (ime.cpp)

### 何が問題か

日本語を入力するとき、IME は確定する前の文字列 (変換中の文字列) をアプリに渡します。SDL3 はこれを `SDL_EVENT_TEXT_EDITING` イベントとして送り、確定した文字列は `SDL_EVENT_TEXT_INPUT` として送ります。
ImGui の入力欄は確定した文字列しか扱わないため、何もしないと、確定するまで入力中の文字が画面に出ません。

### 対処の内容

1. **SDL に伝える** (`SetImeHint`): `SDL_HINT_IME_IMPLEMENTED_UI` に `"composition"` を設定し、変換中の文字列はアプリが描くことを伝えます。Windows や Linux で OS の変換ウィンドウと二重に表示されるのを防ぎます。変換候補の一覧は、これまでどおり OS が表示します。
2. **変換中の文字列を覚える** (`HandleCompositionEvent`): `SDL_EVENT_TEXT_EDITING` が来たら、文字列と、選択中の文節の位置 (`start`) と長さ (`length`) を `g_composition` に保存します。確定したとき (`SDL_EVENT_TEXT_INPUT`) とウィンドウのフォーカスが外れたときは消します。
3. **カーソルの位置を知る** (`InstallImeHook` / `SetImeData`): ImGui は入力欄のカーソルの画面上の位置を、フレームの終わりに `Platform_SetImeDataFn` でバックエンドへ通知します (バックエンドはこれを使って、OS の変換候補ウィンドウの位置を決めます)。この関数を `SetImeData` で包み、位置を `g_imeData` に保存してから元の関数を呼びます。
   変換中の文字列とカーソル位置の変数は `ime.cpp` の中に閉じていて、ほかのファイルからは `ime.h` の 4 つの関数だけを使います。
4. **描く** (`DrawComposition`): 入力欄を描いた後、カーソルの位置に変換中の文字列を重ねて描きます。
   - ほかの UI より手前に描くため、`GetForegroundDrawList()` を使います。
   - 下の文字が透けないよう、入力欄の背景色を不透明にした四角形を先に描きます。
   - 文字列全体に細い下線を引き、選択中の文節には太い下線を引きます。文節を選んでいないときは、変換中のカーソルを縦線で描きます。
   - `start` と `length` は文字数で届くので、`AdvanceChars` で UTF-8 のバイト位置に変換してから文字幅を測ります。

カーソルの位置は前のフレームのものを使いますが、入力中はカーソルがほとんど動かないので、見た目には問題ありません。

### 既知の制限

- 変換中の文字列は入力欄の右端で折り返さないため、長い文を変換すると入力欄からはみ出します。
- 変換中の文字列は、カーソルより後ろの文字の上に重ねて描きます。文の途中で入力すると、その部分が一時的に隠れます (確定すれば正しく表示されます)。

## 設定画面 (settings_ui.cpp)

RAG の行の右端の「設定」ボタンで `SettingsState::requestOpen` を立て、`DrawSettings()` が次のフレームで `ui.json` / `server.json` / `agents.json` を読み直してモーダル (`BeginPopupModal`) を開きます。

- タブは「画面」「システムプロンプト」「サーバー」「ChatGPT」の 4 つです。下端のボタンが隠れないように、中身は `DrawScrollableTab` でスクロールさせます。「システムプロンプト」はエージェントごとのタブで、回答役には人格のテンプレート (`personas.json`) の選択欄と「書き直す」(`rewrite`) のチェックがあります。「サーバー」にはローカルの URL と `llama-server` の自動起動 ([managed-server.md](managed-server.md))、「ChatGPT」には ChatGPT の接続先と API キーの登録欄があります ([chatgpt.md](chatgpt.md#api-キーの登録))。
- API キーは `server.json` ではなく秘密ファイルに `SaveApiKey()` で書きます。保存したら `SettingsState::saved` を立て、チャット画面が `RefreshCloudStatus` でキーの有無を調べ直します。
- ファイルは `LoadConfigForEdit()` で `nlohmann::ordered_json` として読みます。項目の順番を保ち、設定画面で扱わない項目 (`python_font_size` など) もそのまま残して保存するためです。読めないファイルがあるときは、上書きして中身を消さないように保存ボタンを無効にします。
- 保存は `SaveConfig()` で、一時ファイルに書いてから置き換えます。保存先 (`ConfigDirectoryPath()`) は設定画面の上に出します。
- 「すべて既定に戻す」は、確認したうえで `ResetConfigToDefaults()` でユーザーごとの設定を既定値で上書きし、`ui.json` を読み直して反映してから、編集中の値を読み直します (API キーの有無も調べ直します)。`LOCALLLM_CONFIG_DIR` を指定しているときは、そのフォルダーが既定値なのでボタンを出しません。
- 背景の不透明度・文字の大きさ・色は、動かすたびに `ApplyUiSettings()` で反映します。背景・文字・補足の文字の色は ImGui のスタイル (`WindowBg` / `Text` / `TextDisabled`) に入れ、送信者名とエラーの色は描くときに `ActiveUiSettings()` から読みます (設定画面を開いている間は編集中の値)。文字の大きさはフレームの途中で `style.FontSizeBase` を変えても戻されるので、`style._NextFrameFontSizeBase` で次のフレームの始めに変えます (ImGui 1.92 のフォントは大きさを後から変えられます)。キャンセルすると開く前の値 (`SettingsState::current`) に戻します。
- フォントファイルは起動時に読み込むので、変えても再起動するまで反映されません。

## 設定を変えたいとき

画面・system プロンプト・サーバーの URL はアプリの設定画面でも変えられます。

| 変えたいもの | 場所 |
| --- | --- |
| フォントの大きさ | `config/ui.json` の `font_size` |
| フォントファイル | `config/ui.json` の `font_file` |
| OS フォントの候補 | `font.cpp` の `SYSTEM_FONT_PATHS` |
| 背景の不透明度 | `config/ui.json` の `window_opacity` |
| ウィンドウの初期サイズ | `main.cpp` の `SDL_CreateWindow` (900×700) |
| 入力欄の高さ | `chat_ui.cpp` の `DrawUI` の `inputHeight` (4 行分) |
| 色 (背景・文字・送信者名・エラーなど) | `config/ui.json` の `colors` (既定値は `settings_ui.h` の `UiColors`) |
| 接続先や system プロンプト | `config/server.json`、`config/agents.json` ([llm-client.md](llm-client.md)、[agent.md](agent.md) を参照) |
