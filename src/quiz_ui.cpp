// クイズタブの描画 (ImGui)

#include "quiz_ui.h"

#include "ime.h"
#include "server_manager.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <stdexcept>

namespace
{

constexpr const char* NOTICE =
    "AI が作った問題です。特に「資料から」以外の分野は、正解や解説が誤っていることがあります。"
    "気になったら調べ直してください。";

constexpr const char* RAG_DESCRIPTION =
    "rag/ フォルダーの資料の内容から出題します (資料の読み込みが終わってから)。";

constexpr const char* RAG_CUSTOM_DESCRIPTION =
    "rag/ フォルダーの資料のうち、お題に近い内容から出題します (資料の読み込みが終わってから)。";

// 新しい分野の仮の名前 (変えないと保存できない)
constexpr const char* NEW_NAME = "新しい分野";

constexpr const char* PROMPT_HELP =
    "お題や出題の方針を自由に書きます (例: 「Git のブランチとマージ。初心者向けに、コマンドの意味を中心に」)。"
    "問題の形式 (4 択・正解・解説) はアプリが決めます。"
    "「資料を使う」にしたとき、お題が空なら資料からランダムに、書いてあればお題に近い資料から出題します。";

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

std::string CategoryLabel(const QuizCategory& category)
{
    return category.IsCustom() ? category.name + " (自作)" : category.name;
}

// 保存できない理由 (問題なければ空)
std::string ValidateEdit(const QuizCategory& item, size_t position)
{
    const std::string label = std::to_string(position + 1) + " 番目の分野";

    if (item.name.empty() || item.name == NEW_NAME)
    {
        return label + "の名前を入れてください";
    }

    if (item.guide.empty() && !item.useRag)
    {
        return "「" + item.name + "」のお題を書くか、「資料を使う」にしてください";
    }

    return "";
}

// 前後の空白を除く (入力欄の内容を保存用に整える)
std::string Strip(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");

    if (first == std::string::npos)
    {
        return "";
    }

    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

void OpenEditor(QuizState& state)
{
    try
    {
        state.editItems = LoadCustomQuizCategories();
    }
    catch (const std::exception& e)
    {
        // 壊れたファイルを上書きして中身を消さないように、編集させない
        state.customError = std::string(e.what()) + "。直すか削除してください";
        return;
    }

    state.customError.clear();
    state.editing = true;
    state.editIndex = state.editItems.empty() ? -1 : 0;
    state.editMessage.clear();
}

void CloseEditor(QuizState& state)
{
    StopQuizTest(state);
    state.editing = false;

    // 選択の画面で、分野を読み直す
    state.categoriesLoaded = false;
}

// 編集画面で、別の分野に移る
void SelectEditIndex(QuizState& state, int index)
{
    StopQuizTest(state);
    state.editIndex = index;
    state.editMessage.clear();
}

void SaveEditor(QuizState& state)
{
    for (auto& item : state.editItems)
    {
        item.name = Strip(item.name);
        item.guide = Strip(item.guide);
    }

    for (size_t i = 0; i < state.editItems.size(); ++i)
    {
        const std::string reason = ValidateEdit(state.editItems[i], i);

        if (!reason.empty())
        {
            SelectEditIndex(state, static_cast<int>(i));
            state.editMessage = "保存できません: " + reason;
            return;
        }
    }

    try
    {
        SaveCustomQuizCategories(state.editItems);
    }
    catch (const std::exception& e)
    {
        state.editMessage = std::string("保存できません: ") + e.what();
        return;
    }

    CloseEditor(state);
}

void DrawEditor(QuizState& state, RagState& rag, const UiColors& colors)
{
    ImGui::TextUnformatted("自作の分野");
    WrappedDisabled(PROMPT_HELP);
    ImGui::Spacing();

    const ImGuiStyle& style = ImGui::GetStyle();
    const float footer = ImGui::GetFrameHeight() + style.ItemSpacing.y;
    ImGui::BeginChild("editor", ImVec2(0.0f, -footer));

    const float listWidth = ImGui::GetFontSize() * 12.0f;

    ImGui::BeginGroup();
    ImGui::BeginChild(
        "list", ImVec2(listWidth, -footer), ImGuiChildFlags_Borders);

    for (int i = 0; i < static_cast<int>(state.editItems.size()); ++i)
    {
        ImGui::PushID(i);
        const std::string& name = state.editItems[i].name;

        if (ImGui::Selectable(name.empty() ? "(名前なし)" : name.c_str(), i == state.editIndex) &&
            i != state.editIndex)
        {
            SelectEditIndex(state, i);
        }

        ImGui::PopID();
    }

    ImGui::EndChild();

    if (ImGui::Button("追加"))
    {
        QuizCategory item;
        item.source = "custom";
        item.name = NEW_NAME;
        state.editItems.push_back(std::move(item));
        SelectEditIndex(state, static_cast<int>(state.editItems.size()) - 1);
        state.editFocusName = true;
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(state.editIndex < 0);
    if (ImGui::Button("削除"))
    {
        state.editItems.erase(state.editItems.begin() + state.editIndex);
        SelectEditIndex(
            state, state.editItems.empty()
                       ? -1
                       : std::min(state.editIndex, static_cast<int>(state.editItems.size()) - 1));
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::BeginGroup();

    if (state.editIndex >= 0 && state.editIndex < static_cast<int>(state.editItems.size()))
    {
        QuizCategory& item = state.editItems[state.editIndex];

        ImGui::PushID(state.editIndex);
        ImGui::SetNextItemWidth(-FLT_MIN);

        // 追加した直後は、分野名の欄に入力できるようにして、仮の名前を選んでおく
        ImGuiInputTextFlags nameFlags = ImGuiInputTextFlags_None;

        if (state.editFocusName)
        {
            ImGui::SetKeyboardFocusHere();
            nameFlags = ImGuiInputTextFlags_AutoSelectAll;
            state.editFocusName = false;
        }

        ImGui::InputText("分野名", &item.name, nameFlags);
        ImGui::TextUnformatted("お題・出題の方針:");
        ImGui::InputTextMultiline(
            "##prompt", &item.guide, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8.0f));
        ImGui::Checkbox("資料 (rag/) を使う", &item.useRag);

        bool running;
        std::string result;
        {
            std::lock_guard lock(state.mutex);
            running = state.testRunning;
            result = state.testResult;
        }

        ImGui::BeginDisabled(running || !IsChatServerAvailable());
        if (ImGui::Button("テスト出題"))
        {
            const std::string reason = ValidateEdit(item, static_cast<size_t>(state.editIndex));

            if (reason.empty())
            {
                QuizCategory test = item;
                test.name = Strip(test.name);
                test.guide = Strip(test.guide);
                state.editMessage.clear();
                StartQuizTest(state, test, rag);
            }
            else
            {
                state.editMessage = reason;
            }
        }
        ImGui::EndDisabled();

        ImGui::SameLine();

        if (running)
        {
            ImGui::TextDisabled("作っています…");
        }
        else if (!IsChatServerAvailable())
        {
            ImGui::TextDisabled("サーバーの準備ができるとテストできます");
        }

        ImGui::BeginChild("test", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
        WrappedText(result.c_str());
        ImGui::EndChild();
        ImGui::PopID();
    }

    ImGui::EndGroup();
    ImGui::EndChild();

    // 変換中 (未確定) の文字を、入力カーソルの位置に重ねて描く
    DrawComposition();

    if (ImGui::Button("保存して戻る"))
    {
        SaveEditor(state);
    }

    ImGui::SameLine();
    if (ImGui::Button("破棄して戻る"))
    {
        CloseEditor(state);
    }

    if (!state.editMessage.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(colors.error, "%s", state.editMessage.c_str());
    }
}

void DrawSelect(QuizState& state, RagState& rag, const std::string& error, const UiColors& colors)
{
    // 設定ファイルを直したあとにも反映されるように、選択に戻るたびに読み直す
    if (!state.categoriesLoaded)
    {
        state.categories = LoadQuizCategories();
        state.categoriesLoaded = true;
        state.customError.clear();

        try
        {
            for (auto& category : LoadCustomQuizCategories())
            {
                state.categories.push_back(std::move(category));
            }
        }
        catch (const std::exception& e)
        {
            state.customError = std::string("自作の分野を読めません: ") + e.what();
        }

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

    const QuizCategory selected = state.categories[state.selectedCategory];

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("分野:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);

    if (ImGui::BeginCombo("##category", CategoryLabel(selected).c_str()))
    {
        for (int i = 0; i < static_cast<int>(state.categories.size()); ++i)
        {
            ImGui::PushID(i);
            const bool clicked =
                ImGui::Selectable(CategoryLabel(state.categories[i]).c_str(), i == state.selectedCategory);
            ImGui::PopID();

            if (clicked)
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

    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 1.5f);
    if (ImGui::Button("分野を編集…"))
    {
        OpenEditor(state);
    }

    std::string note = selected.guide;

    if (selected.source == "rag")
    {
        note = RAG_DESCRIPTION;
    }
    else if (selected.IsCustom() && selected.useRag)
    {
        note = (note.empty() ? "" : note + "\n") +
               std::string(note.empty() ? RAG_DESCRIPTION : RAG_CUSTOM_DESCRIPTION);
    }

    WrappedDisabled(note);

    const std::string& shown = error.empty() ? state.customError : error;

    if (!shown.empty())
    {
        ImGui::Spacing();
        WrappedColored(colors.error, shown);
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

    if (phase == QuizPhase::Select && state.editing)
    {
        DrawEditor(state, rag, colors);
    }
    else if (phase == QuizPhase::Select)
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
