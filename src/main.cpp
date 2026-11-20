// アプリの入口: Application を作って動かすだけ
// (ウィンドウと ImGui は app/renderer、状態とメインループは app/application)

#include "app/application.h"

int main(int, char**)
{
    Application app;
    return app.Run();
}
