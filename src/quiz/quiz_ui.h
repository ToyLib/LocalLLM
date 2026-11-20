#pragma once

#include "quiz/quiz_mode.h"
#include "common/rag.h"
#include "common/tab.h"

// クイズのタブ: クイズの状態を持ち、資料 (RAG) は Application と共有する
class QuizTab : public Tab
{
public:
    explicit QuizTab(RagState& rag) : mRag(rag) {}

    const char* Label() const override { return "クイズ"; }
    void Draw(const UiContext& context) override;
    void Shutdown() override;

private:
    QuizState mState;
    RagState& mRag;
};
