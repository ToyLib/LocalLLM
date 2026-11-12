#pragma once

#include "rag.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// クイズ: 分野の読み込み、出題 (JSON)、採点、先読み、結果のメッセージ
// 設計は doc/quiz-plan.md、動きは doc/quiz.md を参照
// 状態はワーカースレッドと UI スレッドで共有する (mutex で守る)

// 選択肢の数 (4 択だけ)
constexpr int QUIZ_CHOICE_COUNT = 4;

// 出題の分野 (config/quiz.json の categories の 1 件)
struct QuizCategory
{
    std::string key;
    std::string name;

    // "llm" (モデルの知識から出題)、"rag" (資料から出題)、"custom" (自作。useRag で決まる)
    std::string source;
    std::string guide;
    std::vector<std::string> topics;

    // 自作の分野で、資料 (rag/) から出題するか
    bool useRag = false;

    bool IsCustom() const { return source == "custom"; }

    // 資料から出題するか (guide があれば、それに近い資料を選ぶ)
    bool FromRag() const { return source == "rag" || (IsCustom() && useRag); }
};

// 自作の分野の名前・お題の最大の長さ (バイト)
constexpr size_t QUIZ_MAX_NAME_BYTES = 120;
constexpr size_t QUIZ_MAX_PROMPT_BYTES = 6000;

struct QuizQuestion
{
    std::string text;
    std::vector<std::string> choices;

    // 正解の選択肢の番号 (並べ替えたあとの choices での位置)
    int answer = 0;
    std::string explanation;

    // 出題のトピックと、資料の出典 (無ければ空)
    std::string topic;
    std::string origin;
};

enum class QuizPhase
{
    Select,
    Playing,
    Result
};

struct QuizState
{
    std::mutex mutex;

    // 開始・やめるたびに増やす。古いワーカーが新しい回に書き込まないための目印
    int session = 0;

    // 回ごとの中断用のフラグ (ワーカーも持つ)
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);

    std::thread worker;
    std::thread commentWorker;

    QuizPhase phase = QuizPhase::Select;
    QuizCategory category;

    // 出題する数 (作れなかった問題の分だけ減る)
    int total = 0;

    std::vector<QuizQuestion> questions;

    // 答えた選択肢の番号 (questions の先頭から)
    std::vector<int> answers;
    int index = 0;
    bool answered = false;

    bool generating = false;
    std::string error;

    // 結果のメッセージ (最初は固定文、LLM のひと言ができたら置き換える)
    std::string comment;
    bool commentPending = false;

    // 分野の編集画面のテスト出題 (ワーカーが結果を書くので、mutex で守る)
    std::thread testWorker;
    std::shared_ptr<std::atomic<bool>> testCancel = std::make_shared<std::atomic<bool>>(false);
    int testSession = 0;
    bool testRunning = false;
    std::string testResult;

    // ここから下は UI スレッドだけが触る
    std::vector<QuizCategory> categories;
    bool categoriesLoaded = false;
    std::string customError;

    // 分野の編集画面
    bool editing = false;
    std::vector<QuizCategory> editItems;
    int editIndex = -1;
    std::string editMessage;
    int selectedCategory = 0;
    int choice = -1;
    int shownSession = -1;
    int shownIndex = -1;

    // 呼ぶ側で mutex をロックしておく
    int Score() const;
    bool IsLast() const { return index + 1 >= total; }
};

// config/quiz.json の分野の一覧 (正しくない項目は読み飛ばす)
std::vector<QuizCategory> LoadQuizCategories();

// 自作の分野の一覧 (quiz_custom.json。無ければ空)
// ファイルが壊れているときは std::runtime_error (編集画面で上書きして中身を消さないため)
std::vector<QuizCategory> LoadCustomQuizCategories();

// 自作の分野を保存する。失敗したら std::runtime_error
void SaveCustomQuizCategories(const std::vector<QuizCategory>& categories);

// 正解率 (%) の帯ごとの固定のメッセージ。results は下限の大きい順
std::string QuizTierMessage(
    const std::vector<std::pair<float, std::string>>& results, int score, int total);

// 分野を決めて出題を始める (問題は別スレッドで 1 問ずつ作る)
void StartQuiz(QuizState& state, const QuizCategory& category, RagState& rag);

// いまの問題に答える
void AnswerQuiz(QuizState& state, int choice);

// 次の問題に進む。最後なら結果を出す (次の問題がまだ作れていなければ何もしない)
void NextQuestion(QuizState& state);

// やめて、分野の選択に戻る (結果の画面からも使う)
void QuitQuiz(QuizState& state);

// 分野の編集画面のテスト: 1 問だけ別スレッドで作り、結果を testResult に入れる
void StartQuizTest(QuizState& state, const QuizCategory& category, RagState& rag);

// テスト出題を止めて、スレッドの終了を待つ (編集画面を閉じる・別の分野に移るとき)
void StopQuizTest(QuizState& state);

// アプリを閉じるとき: 通信を止めて、スレッドの終了を待つ
void ShutdownQuiz(QuizState& state);
