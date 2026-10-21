# GUI アプリとしての構成

このドキュメントでは、チャット画面を表示する GUI アプリの部分を解説します。
対象のファイルは `src/` 以下の次のファイルです。

| ファイル | 内容 |
| --- | --- |
| [main.cpp](../src/main.cpp) | SDL / ImGui の初期化、メインループ、後始末 |
| [chat.h](../src/chat.h) / [chat.cpp](../src/chat.cpp) | 会話の状態 (`ChatState`)、ワーカースレッドでの送受信 (`SendPrompt`)、UI を起こすイベント (`WakeUI`) |
| [chat_ui.h](../src/chat_ui.h) / [chat_ui.cpp](../src/chat_ui.cpp) | 画面の組み立て (`DrawUI`、`DrawMessages`) |
| [ime.h](../src/ime.h) / [ime.cpp](../src/ime.cpp) | IME の変換中の文字列の表示 |
| [font.h](../src/font.h) / [font.cpp](../src/font.cpp) | 日本語フォントの読み込み (`LoadFont`) |

LLM サーバーとの通信 (`AskLLM`) については [llm-client.md](llm-client.md) を参照してください。

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
 │   │   ├ DrawMessages()    … 会話の一覧
 │   │   ├ 入力欄・送信/中断・クリアのボタン
 │   │   └ DrawComposition() … IME の変換中の文字列 (ime.cpp)
 │   └ 描画して画面に表示
 └ 後始末: 受信中なら中断 → ワーカースレッドの終了待ち → 各ライブラリの終了

SendPrompt()  … 送信ボタンで呼ばれ、ワーカースレッドで AskLLM() を実行 (chat.cpp)
```

## 起動処理 (main.cpp の main の前半)

1. `curl_global_init()` で libcurl を初期化します。
2. `SetImeHint()` で、IME の変換中の文字列はアプリが描くことを SDL に伝えます (後述)。
3. `SDL_Init(SDL_INIT_VIDEO)` で SDL を初期化します。
4. `RegisterWakeEvent()` で独自のイベント番号を 1 つ確保します (`SDL_RegisterEvents`)。ワーカースレッドから UI を起こすのに使います (後述)。
5. ディスプレイの拡大率 (`SDL_GetDisplayContentScale`) を取得し、900×700 に拡大率を掛けた大きさでウィンドウを作ります。
   - `SDL_WINDOW_HIGH_PIXEL_DENSITY` で、Retina などの高解像度ディスプレイでもぼやけずに描きます。
   - `SDL_WINDOW_HIDDEN` で作成し、中央に移動してから表示するので、ウィンドウが一瞬別の場所に見えることはありません。
   - Linux と Windows では `SDL_WINDOW_TRANSPARENT` を付けて半透明にします。macOS ではタイトルバーまで透けてしまうため付けません (`TRANSPARENT_FLAG` と `WINDOW_OPACITY`)。
6. `SDL_CreateRenderer()` でレンダラーを作り、VSync を有効にします。
7. ImGui を初期化します。
   - `io.IniFilename = nullptr` で、ウィンドウ配置を保存する `imgui.ini` を作らないようにします。
   - キーボードでのフォーカス移動 (`NavEnableKeyboard`) を有効にします。
   - ダークテーマを使い、余白などの大きさとフォントを拡大率に合わせます (`ScaleAllSizes`、`FontScaleDpi`)。
   - ウィンドウ背景の不透明度を `WINDOW_OPACITY` にします。
8. 日本語フォントを読み込みます (`LoadFont`)。
9. ImGui の SDL3 用・SDL_Renderer 用のバックエンドを初期化します。
10. `InstallImeHook()` で、IME のカーソル位置を受け取るため、バックエンドの `Platform_SetImeDataFn` を `SetImeData` で包みます (後述)。

## 日本語フォントの読み込み (font.cpp)

ImGui に入っている標準フォントには日本語の文字がないため、次の順でフォントを探します。

1. 環境変数 `LOCALLLM_FONT` で指定されたフォント
2. 同梱フォント `Asset/MPLUSRounded1c-Bold.ttf` (実行ファイルのあるフォルダーからの相対パス。`SDL_GetBasePath()` で取得)
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
| `messages` | 画面に表示している会話 (user / assistant)。system プロンプトは含みません |
| `busy` | 回答を受信中かどうか |
| `error` | 最後に起きたエラーのメッセージ |
| `restoredPrompt` | 失敗したときに入力欄へ戻す質問 |
| `cancel` | 中断の要求 (`std::atomic<bool>`。ロックなしで読み書きします) |
| `worker` | 受信用のワーカースレッド |

### SendPrompt: 質問の送信

1. 前回のワーカースレッドが残っていれば終了を待ちます (`JoinWorker`)。
2. ロックを取り、次のことをまとめて行います。
   - `messages` に今回の質問 (user) を追加する
   - `CreateHistory()` の後ろに `messages` を全部つなげて、送信する会話履歴を作る
   - 回答を書き込むための空の assistant メッセージを追加する
   - `busy = true` にし、前回のエラーを消す
3. ワーカースレッドを起動し、`AskLLM()` を呼びます。履歴はスレッドにコピーで渡すので、UI 側で `messages` が変わっても影響しません。

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
│ You                                        │
│ 質問の内容                                  │
│ ────────────────────────────────────────── │  ← 会話の表示 (子ウィンドウ "messages")
│ AI                                         │
│ 回答の内容…                                 │
│                                            │
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

- 送信者の名前 (`You` は青、`AI` は緑) と本文を表示し、メッセージの間に区切り線を引きます。
- 本文は `TextWrapped` で、ウィンドウの幅に合わせて折り返します。
- 受信中で回答がまだ空のときは「考え中...」と表示します。
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

## 設定を変えたいとき

| 変えたいもの | 場所 |
| --- | --- |
| フォントの大きさ | `font.cpp` の `FONT_SIZE` |
| 同梱フォント・OS フォントの候補 | `font.cpp` の `BUNDLED_FONT_PATH`、`SYSTEM_FONT_PATHS` |
| 背景の不透明度 | `main.cpp` の `WINDOW_OPACITY` |
| ウィンドウの初期サイズ | `main.cpp` の `SDL_CreateWindow` (900×700) |
| 入力欄の高さ | `chat_ui.cpp` の `DrawUI` の `inputHeight` (4 行分) |
| 送信者名の色 | `chat_ui.cpp` の `DrawMessages` の `TextColored` |
| 接続先や system プロンプト | [src/llm_client.cpp](../src/llm_client.cpp) ([llm-client.md](llm-client.md) を参照) |
