// クイズタブの描画 (ImGui)

#include "quiz_ui.h"

#include "server_manager.h"

#include <imgui.h>

#include <cstdio>

namespace
{

constexpr const char* NOTICE =
    "AI が作った問題です。特に「資料から」以外の分野は、正解や解説が誤っていることがあります。"
    "気になったら調べ直してください。";

constexpr const char* RAG_DESCRIPTION =
    "rag/ フォルダーの資料の内容から出題します (資料の読み込みが終わってから)。";

void WrappedText(const char* text)
{
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
}

void WrappedDisabled(const std::string& text)
{
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", text.c_str());
    ImGui::PopTextWrapPos();
}

void WrappedColored(const ImVec4& color, const std::string& text)
{
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(color, "%s", text.c_str());
    ImGui::PopTextWrapPos();
}

void DrawSelect(QuizState& state, RagState& rag, const std::string& error, const UiColors& colors)
{
    // 設定ファイルを直したあとにも反映されるように、選択に戻るたびに読み直す
    if (!state.categoriesLoaded)
    {
        state.categories = LoadQuizCategories();
        state.categoriesLoaded = true;

        if (state.selectedCategory >= static_cast<int>(state.categories.size()))
        {
            state.selectedCategory = 0;
        }
    }

    WrappedDisabled(NOTICE);
    ImGui::Spacing();

    if (state.categories.empty())
    {
        WrappedColored(
            colors.error, "分野がありません。config/quiz.json の categories を確認してください。");
        return;
    }

    const QuizCategory& selected = state.categories[state.selectedCategory];

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("分野:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);

    if (ImGui::BeginCombo("##category", selected.name.c_str()))
    {
        for (int i = 0; i < static_cast<int>(state.categories.size()); ++i)
        {
            if (ImGui::Selectable(state.categories[i].name.c_str(), i == state.selectedCategory))
            {
                state.selectedCategory = i;
            }
        }

        ImGui::EndCombo();
    }

    ImGui::SameLine();

    // サーバーの準備ができるまでは始められない
    ImGui::BeginDisabled(!IsChatServerAvailable());
    if (ImGui::Button("開始"))
    {
        state.choice = -1;
        StartQuiz(state, selected, rag);
    }
    ImGui::EndDisabled();

    WrappedDisabled(selected.source == "rag" ? std::string(RAG_DESCRIPTION) : selected.guide);

    if (!error.empty())
    {
        ImGui::Spacing();
        WrappedColored(colors.error, error);
    }
}

struct PlayView
{
    int total = 0;
    int index = 0;
    int score = 0;
    int chosen = -1;
    bool hasQuestion = false;
    bool answered = false;
    bool readyNext = false;
    bool isLast = false;
    int session = 0;
    QuizQuestion question;
};

void DrawPlay(QuizState& state, const PlayView& view, const UiColors& colors)
{
    char header[160];
    std::snprintf(header, sizeof(header), "Q%d / %d    正解 %d", view.index + 1, view.total, view.score);
    std::string text = header;

    if (view.hasQuestion && !view.question.topic.empty())
    {
        text += "    [" + view.question.topic + "]";
    }

    ImGui::TextUnformatted(text.c_str());
    ImGui::Spacing();

    if (!view.hasQuestion)
    {
        ImGui::TextDisabled("問題を作っています…");
    }
    else
    {
        // 問題が変わったら、選択を外す
        if (state.shownSession != view.session || state.shownIndex != view.index)
        {
            state.shownSession = view.session;
            state.shownIndex = view.index;
            state.choice = -1;
        }

        const QuizQuestion& question = view.question;
        WrappedText(question.text.c_str());
        ImGui::Spacing();

        for (int i = 0; i < QUIZ_CHOICE_COUNT; ++i)
        {
            ImGui::PushID(i);

            const bool selected = view.answered ? view.chosen == i : state.choice == i;

            ImGui::BeginDisabled(view.answered);
            if (ImGui::RadioButton("##choice", selected))
            {
                state.choice = i;
            }
            ImGui::EndDisabled();

            ImGui::SameLine();

            // 答えたあと、正解は緑、間違えた選択は赤
            const bool useColor = view.answered && (i == question.answer || i == view.chosen);
            const ImVec4 color = i == question.answer ? colors.ai : colors.error;

            if (useColor)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, color);
            }

            ImGui::PushTextWrapPos(0.0f);
            ImGui::Text("%d. %s", i + 1, question.choices[i].c_str());
            ImGui::PopTextWrapPos();

            if (useColor)
            {
                ImGui::PopStyleColor();
            }

            if (!view.answered && ImGui::IsItemClicked())
            {
                state.choice = i;
            }

            ImGui::PopID();
        }
    }

    ImGui::Spacing();

    const bool canAnswer = view.hasQuestion && !view.answered && state.choice >= 0;
    ImGui::BeginDisabled(!canAnswer);
    if (ImGui::Button("回答する"))
    {
        AnswerQuiz(state, state.choice);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!(view.answered && (view.readyNext || view.isLast)));
    if (ImGui::Button(view.isLast ? "結果を見る" : "次の問題へ"))
    {
        NextQuestion(state);
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 1.5f);
    if (ImGui::Button("やめる"))
    {
        QuitQuiz(state);
    }

    if (view.hasQuestion && view.answered)
    {
        ImGui::Spacing();

        const bool correct = view.chosen == view.question.answer;

        if (correct)
        {
            ImGui::TextColored(colors.ai, "○ 正解です！");
        }
        else
        {
            ImGui::TextColored(
                colors.error, "× 不正解です。正解は %d. です", view.question.answer + 1);
        }

        WrappedText(view.question.explanation.c_str());

        if (!view.question.origin.empty())
        {
            WrappedDisabled("(出典: " + view.question.origin + ")");
        }

        if (!view.readyNext && !view.isLast)
        {
            WrappedDisabled("次の問題を準備中…");
        }
    }
}

struct ResultView
{
    int score = 0;
    int count = 0;
    std::string comment;
    bool pending = false;
    std::vector<int> missIndexes;
    std::vector<QuizQuestion> questions;
    std::vector<int> answers;
};

void DrawResult(QuizState& state, RagState& rag, const ResultView& view, const UiColors& colors)
{
    const int percent =
        view.count > 0 ? static_cast<int>(100.0f * view.score / view.count + 0.5f) : 0;
    ImGui::Text("%d / %d 問正解 (%d%%)", view.score, view.count, percent);
    WrappedText((view.comment + (view.pending ? "…" : "")).c_str());
    ImGui::Spacing();
    ImGui::TextUnformatted("間違えた問題");

    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::BeginChild(
        "review", ImVec2(0.0f, -(ImGui::GetFrameHeight() + style.ItemSpacing.y)),
        ImGuiChildFlags_Borders);

    if (view.missIndexes.empty())
    {
        ImGui::TextColored(colors.ai, "間違えた問題はありません。全問正解です！");
    }

    for (const int i : view.missIndexes)
    {
        const QuizQuestion& q = view.questions[i];
        const int chosen = view.answers[i];

        ImGui::PushTextWrapPos(0.0f);
        ImGui::Text("Q%d. %s", i + 1, q.text.c_str());
        ImGui::TextColored(colors.error, "  あなたの答え: %d. %s", chosen + 1, q.choices[chosen].c_str());
        ImGui::TextColored(colors.ai, "  正解: %d. %s", q.answer + 1, q.choices[q.answer].c_str());

        if (!q.explanation.empty())
        {
            ImGui::TextDisabled("  %s", q.explanation.c_str());
        }

        if (!q.origin.empty())
        {
            ImGui::TextDisabled("  (出典: %s)", q.origin.c_str());
        }

        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Separator();
    }

    ImGui::EndChild();

    if (ImGui::Button("もう一度"))
    {
        const QuizCategory category = state.category;
        state.choice = -1;
        StartQuiz(state, category, rag);
    }

    ImGui::SameLine();
    if (ImGui::Button("分野を変える"))
    {
        QuitQuiz(state);
    }
}

} // namespace

void DrawQuizTab(QuizState& state, RagState& rag, const UiColors& colors)
{
    QuizPhase phase;
    std::string error;
    PlayView play;
    ResultView result;

    {
        std::lock_guard lock(state.mutex);
        phase = state.phase;
        error = state.error;

        play.total = state.total;
        play.index = state.index;
        play.score = state.Score();
        play.session = state.session;
        play.answered = state.answered;
        play.hasQuestion = state.index < static_cast<int>(state.questions.size());
        play.readyNext = state.index + 1 < static_cast<int>(state.questions.size());
        play.isLast = state.IsLast();

        if (play.hasQuestion)
        {
            play.question = state.questions[state.index];

            if (state.answered && state.index < static_cast<int>(state.answers.size()))
            {
                play.chosen = state.answers[state.index];
            }
        }

        if (phase == QuizPhase::Result)
        {
            result.score = play.score;
            result.count = static_cast<int>(state.answers.size());
            result.comment = state.comment;
            result.pending = state.commentPending;
            result.questions = state.questions;
            result.answers = state.answers;

            for (size_t i = 0; i < state.answers.size() && i < state.questions.size(); ++i)
            {
                if (state.questions[i].answer != state.answers[i])
                {
                    result.missIndexes.push_back(static_cast<int>(i));
                }
            }
        }
    }

    // 選択に戻ったとき、分野を読み直せるようにする
    if (phase != QuizPhase::Select)
    {
        state.categoriesLoaded = false;
    }

    if (phase == QuizPhase::Select)
    {
        DrawSelect(state, rag, error, colors);
    }
    else if (phase == QuizPhase::Playing)
    {
        DrawPlay(state, play, colors);
    }
    else
    {
        DrawResult(state, rag, result, colors);
    }
}
