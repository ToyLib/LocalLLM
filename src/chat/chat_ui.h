#pragma once

#include "chat/chat.h"
#include "common/rag.h"
#include "common/tab.h"

#include <string>

// チャットのタブ: 会話の状態と入力欄を持ち、資料 (RAG) は Application と共有する
class ChatTab : public Tab
{
public:
    explicit ChatTab(RagState& rag);

    const char* Label() const override { return "チャット"; }
    void Draw(const UiContext& context) override;

    // API キーの有無を調べ直す
    void OnSettingsSaved() override;

    // 受信を中断して、ワーカースレッドの終了を待つ
    void Shutdown() override;

private:
    ChatState mState;
    RagState& mRag;

    std::string mInput;
    bool mFocusInput = true;
};
