# UIの翻訳

UIは日本語・英語に対応し、環境設定の表示言語で選択します。
カタログは `translations/`、読込は `src/i18n.*` です。Qt Linguist Toolsで
ビルド時にコンパイルし、アプリへ埋め込みます。Qt共通ダイアログ用のqttranslationsも必要です。

翻訳対象は表示文字列です。操作ID・設定キー・APIのフィールド名・ファイル名を翻訳しません。
placeholder、HTML、改行、数値の意味を維持し、メニューのshortcut表記を確認します。
`python3 tools/check_translations.py` と `i18n_tests` で検査し、言語を変更して
再起動したGUIでメニュー・環境設定・エラーを確認してください。

文書は日本語原稿を確定して英訳を揃えます。READMEは英語、本体開発者文書も最終的には
英語にします。画像中のUI言語と、その画像を参照する本文の言語を対応させます。
