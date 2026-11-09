LocalLLM (Windows x64)
======================

手元の PC で LLM と話すチャットアプリです。Visual C++ ランタイムなど、動かすのに必要なものはこのフォルダーに入っています。


使い方
------

1. ZIP をすべて展開します (ZIP の中から直接は起動できません)。
   展開先は、ドキュメントなど書き込めるフォルダーにしてください (C:\Program Files の下は避けてください)。
2. LocalLLM.exe をダブルクリックします。
3. 初回の起動時にモデルをダウンロードします。数 GB あるので、終わるまで時間がかかります。

「Windows によって PC が保護されました」と表示されたら、「詳細情報」から「実行」を押してください (このアプリには署名がありません)。

ファイアウォールの確認が出たときは許可しなくても動きます (サーバーは自分の PC の中だけで通信します)。


自分の文書を読ませる
--------------------

LocalLLM.exe の隣の rag フォルダーに文書を置くと、質問への回答に使います。詳しくは rag\README.md を読んでください。


設定とデータの場所
------------------

設定と API キー  %APPDATA%\LocalLLM
モデル           %USERPROFILE%\.cache\huggingface\hub

アンインストールするときは、uninstall.bat をダブルクリックしてください。設定とモデルを、それぞれ確認のうえで削除します。そのあと、展開したフォルダーを削除してください。


ライセンス
----------

LocalLLM      LICENSE.txt
使用ライブラリ licenses フォルダー
フォント      Asset\OFL.txt
llama-server  licenses\llama.cpp.txt、llama\LICENSE-LLVM-OpenMP
