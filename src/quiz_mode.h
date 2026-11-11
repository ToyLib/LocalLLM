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

    // "llm" (モデルの知識から出題) か "rag" (資料から出題)
    std::string source;
    std::string guide;
    std::vector<std::string> topics;
};

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

    // ここから下は UI スレッドだけが触る
    std::vector<QuizCategory> categories;
    bool categoriesLoaded = false;
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

// アプリを閉じるとき: 通信を止めて、スレッドの終了を待つ
void ShutdownQuiz(QuizState& state);
