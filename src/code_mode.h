#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

struct SDL_Process;

// コーディング練習: コードの生成・解説と、サブプロセスでの実行
// 設計は doc/code-mode-plan.md、動きは doc/code-mode.md を参照
// 状態はワーカースレッドと UI スレッドで共有する (mutex で守る)
struct CodeState
{
    std::mutex mutex;
    std::atomic<bool> cancel = false;

    // 最後に頼んだこと
    std::string request;

    std::string code;
    std::string originalCode;
    std::string explanation;
    std::string output;

    // コードを LLM の生成や「元に戻す」で書き換えるたびに増やす (画面のコード欄に反映する目印)
    int codeVersion = 0;

    // 作っている / 解説している / 実行している
    bool generating = false;
    bool explaining = false;
    bool running = false;

    // 表示するエラー (空なら無し) と、実行の結果の一言
    std::string error;
    std::string runStatus;

    // 実行中のプロセス (mutex の中でだけ触る)
    SDL_Process* process = nullptr;

    std::thread worker;

    // ここから下は UI スレッドだけが触る
    std::string prompt;
    std::string editor;
    int shownVersion = -1;
    bool focusPrompt = true;

    bool Busy() const { return generating || explaining; }
};

// 回答からコードブロックの中身を取り出す
// ブロックが閉じていなくても (受信の途中でも) 読めた所までを返す
// ブロックが無ければ、回答の全体をコードとみなす
std::string ExtractCode(const std::string& text);

// 解説を頼むときに渡す、行番号付きのコード
std::string NumberedCode(const std::string& code);

// プロンプトからコードを作り、続けて解説を作る (別スレッド)
// 直前のコード (editor の内容) があれば、それを直す依頼として送る
void StartGenerate(CodeState& state, const std::string& prompt, const std::string& currentCode);

// いまのコード (編集後でも) の解説を作り直す (別スレッド)
void StartExplain(CodeState& state, const std::string& currentCode);

// コードを実行する (別スレッド)
void StartRun(CodeState& state, const std::string& code);

// 生成・解説を中断する
void CancelGenerate(CodeState& state);

// 実行中のコードを止める
void StopRun(CodeState& state);

// コードを生成直後の状態に戻す
void RevertCode(CodeState& state);

// コード・解説・実行結果・エラーを消して、最初の状態に戻す (作っている間と実行中は何もしない)
// 依頼の入力欄 (state.prompt) は UI スレッドが消す
void ClearCode(CodeState& state);

// アプリを閉じるとき: 通信と実行中のコードを止めて、スレッドの終了を待つ
void ShutdownCode(CodeState& state);
