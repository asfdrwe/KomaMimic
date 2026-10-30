# SPDX-License-Identifier: GPL-3.0-only
"""Manual acceptance cases shared by the local runner and report generator."""
# ID, platforms, input, operation, expected result. No personal data or artwork.
CASES = [
    ('C01', 'all', 'images', '新規プロファイルで起動。バージョン、アイコン、7分類のメニュー、初期バー非表示を確認', '1.0.1。文字欠け・起動時クラッシュなし。独自上部バー・パス欄なし。下部バーは有効化できる'),
    ('C02', 'all', '日本語 空白/書庫 & 01.cbz', '開く、D&D、日本語と空白・&を含む別フォルダへコピーして開く', '6ページ。文字化け・パス切断なし'),
    ('C03', 'all', 'images', 'フォルダを開く、矢印・Enter、フォルダを読む、上位へ、Shift+↓、Ctrl+Tを操作。Windows/Linux: Ctrl+Shift+O、Ctrl+Enter、Shift+↑。macOS: Command+Shift+O、Command+Enter、Shift+↑', '表紙一覧／読書／ツリーの選択・フォーカス・上位移動が正しい。設定変更時はメニューの実キーを使う'),
    ('C04', 'all', 'images', '→←、↓↑、Home/End、Space、バックスラッシュ、C、左／右クリックとホイールを操作', '次／前、1ページずらし、自動見開き、右／左開き、原寸。画像の大きな数字で順序を判別できる'),
    ('C05', 'all', 'images; results内にコピーした作業用フォルダ', 'Ctrl+Eでサムネイル。マウス選択後に矢印・Enter・Escape。マウス静止のままスクロール。作業用フォルダの表紙一覧を開いたままZIPを追加・改名・削除し、自動反映とF6再読込を確認', '選択が連続し、静止したマウスがキー選択を奪わない。追従OFFも有効。一覧の変更が反映され、残っている選択項目・読書位置・表示モードを保つ'),
    ('C06', 'all', 'books/01-first.zip', 'Ctrl+Eを表示しShift+→で02-next.zipへ、Shift+←で戻る。読書画面でも往復', 'サムネイル画面を維持する。古い本の非同期画像が混入せず、一覧の読込が継続する'),
    ('C07', 'all', 'images/07-wide.png', 'X/Z/C、幅・高さ合わせをメニューから選択、拡大縮小、Ctrl+ホイール、ドラッグ、二次元走査を操作', '倍率・表示位置・移動端が正常。拡大表示と横長画像の分割／走査が破綻しない'),
    ('C08', 'all', 'images', '画像加工で回転・余白除去・色調・filterを適用し、解除。拡縮方式を変更', 'プレビューと解除が機能し、元画像は変化しない。処理中も取消／本の切替ができる'),
    ('C09', 'all', 'animation', 'GIFとAPNGを各々開き再生／停止。TIFFを開いて頁送り', 'GIF/APNGは赤と青が約0.5秒間隔で交替。TIFFは黒→白の2ページ。欠けたcodecは記録する'),
    ('C10', 'all', 'books/03-cp932.zip', '表紙一覧と内部名を確認。書庫内の名前の文字コードでCP932を選ぶ', '表紙ソ.png。ソの末尾byte 0x5cをパス区切りと誤認しない。画像を開ける'),
    ('C11', 'all', 'documents/three-pages.pdf', '3頁を読み、大きく拡大して再描画。内部画像読込をONにして開き直す', '通常はPAGE 1〜3。内部画像は各頁の数字画像で計3枚。拡大後に鮮明になり終了できる'),
    ('C12', 'all', 'documents/aozora-utf8.txt', '縦横、フォント、文字サイズ、見開き、検索、Ctrl+Nの位置移動を確認。F7の目次は見出しのあるC13/C15で確認', '日本語とルビが描画される。文字拡大後も本文位置を概ね維持。このTXTの目次は空。WebEngine helperの欠落なし'),
    ('C13', 'all', 'documents/sample.md', '見出し・画像・リンク・F7の目次移動を確認。本文サイズを変更して途中で終了。--inputなし・同じプロファイルで再起動し、アクセス履歴から再開', '見出しも本文に応じて拡大。ローカルcover画像を表示。再組版・再開で本文位置を保持する（頁番号は変わり得る）'),
    ('C14', 'all', 'documents/aozora-cp932.txt', '文字コードの自動判定／指定で開き、縦横・ルビを確認。旧renderer=0の設定でも同じ文書表示で開く', '日本語本文が読め、異なる文字コードでも破損・ハングしない。表示方式の選択UIはない'),
    ('C15', 'all', 'documents/reflow.epub', '表紙一覧、目次、章リンクと戻る、縦横、フォント、文字サイズ、単頁／見開きを操作', '3章。表紙は数字1。段落20へのリンクと戻る、再組版、2頁送りが正常'),
    ('C16', 'all', 'documents/fixed.epub', '固定レイアウトEPUBの3頁を往復。文字設定、単頁／見開き、ウィンドウサイズも変更', '各頁の「固定ページ1〜3」と同じ数字1の画像全体が見える。縦書きでも端で切れず、本文はフォント変更で組み替えない'),
    ('C17', 'all', 'books/04-mixed-documents.zip', '一覧からfirst.txtとsecond.mdを個別に読む。サイズを個別設定しShift+↑で戻る', '文書単位の一覧・設定・読書位置。相対cover.pngの表示。戻ると元の文書を選択'),
    ('C18', 'all', 'images; documents/reflow.epub', 'Ctrl+Mでマーク・メニューからマーカーへ移動、本棚・お気に入りへ登録。途中位置で終了し、--inputなし・同じプロファイルで再起動してアクセス履歴から再開。各リストも開く', '画像頁と文書本文位置が復元し、登録が残り各リストから開ける。異なる本の設定が混ざらない。画像の登録名は入口のファイル名、位置は別に保存'),
    ('C19', 'all', 'images', 'Ctrl+Kでキー割当、文書固有の割当／解除、カテゴリ検索・部分初期化。再起動', '競合を表示。変更と部分初期化の対象が正しい。保存済みの割当を維持する'),
    ('C20', 'all', 'images', '上部バー・専用パス欄がないことを確認。下部バーON/OFF、ボタン順・区切り・ページバーを編集。全画面と再起動後に本を開いて再確認', '左右群と中央ページバー、編集プレビュー、バーの表示選択が保存される。通常起動は表紙一覧・ウィンドウ表示'),
    ('C21', 'all', 'images; documents/reflow.epub', '全画面切替キー（Windows/Linux: F11、macOS: Control+Command+F。保存済み設定はその割当を優先）、上端／下端呼出し、一覧と読書の切替、最大化、画面移動を操作。macOSは緑ボタンでの全画面移行・復帰も確認', 'バーの固定／自動／非表示が機能。入力を失わず、ちらつき・残像・復帰不能なし。独自上部バーや専用余白が現れず、通常表示へ戻ると下部バーの選択を復元する'),
    ('C22', 'all', 'images', 'ライト／ダーク／システム配色。日本語→English→日本語に変更し毎回再起動', '再起動案内、言語保存、ダイアログの幅・翻訳を確認。本文と画像色、保存キーは変わらない'),
    ('C23', 'all', 'import/synthetic.ini', '専用importプロファイルで「キーのみ」を適用後、全対象も確認。取消も試す', 'CMD1=0で次頁キーを解除。キーのみでは先読み・バー設定は変わらない。適用前backupが作られ、元INIは不変'),
    ('C24', 'all', 'import/invalid-values.ini', '専用importプロファイルで取り込みプレビューを開く', '範囲外値・壊れた式・未知コマンドを報告し、推測して適用しない。取消で設定は不変'),
    ('C25', 'all', 'images', '一時コピーを読書で開き、ツール→ファイルソートの逆順をCtrl+Eのページ一覧で確認。ファイル→ページ編集で削除・白紙・Undo/Redo。本を閉じる前に取消を確認。その他の保存・書き出しからPNG／ZIP／PDF／GIFを再読込', '編集列・Undo・再読込が一致。編集列は再起動後も保存、Undo履歴は本を閉じるまで。既存出力との衝突確認と取消が機能。元fixtureのhashは変化しない'),
    ('C26', 'all', 'images', '環境設定→キャッシュとヘルプの使用量を確認。再起動、上限変更、保存OFF/ON、削除', '再利用・使用量更新・保存選択が機能。macOS/Windowsの削除は専用OSユーザーで実行（共用cacheのため）'),
    ('C27', 'all', 'script-readers; script-readers.zip; images/01.png', '12組のMAG/PIC/PIC2をexpected.pngと比較しZIPからも開く。配置先readersのmag/pic/pic2/pnm/reading-filterを確認。環境設定→スクリプトで一覧・設定・filter選択を確認。合成packageを追加し取消、保存・再起動・読込、登録解除を確認。画像加工をONにしてreading-filterを試す', '色タイルと白枠が基準PNGと一致。workerと5パッケージを配置。画像フィルター選択肢はreading-filterで、gamma/contrast/grayscaleを設定できる。登録内容・設定が保存され、取消は実行中registryと保存値を変えない。追加・解除は再起動後に反映される'),
    ('C28', 'all', 'broken', 'bad.png／bad.zip／bad.pdfを個別に開き、直後に正常な本を開く', 'エラーを表示して回復。クラッシュ・無限待ち・前の本の破壊なし'),
    ('C29', 'all', 'images', '同じ設定で1つ目のウィンドウを開き、別端末から同じ設定で別画像を起動', '既存ウィンドウへ要求が届く。通常環境でIPC不可なら理由とログを記録'),
    ('C30', 'all', 'images', '消灯抑制ONで読書。最小化、別アプリ、一覧、OFF、終了を順に試す', '前面読書中のみ抑制を保持、離れると解放。取得失敗は状態表示。待機時間・実消灯・OS要求を記録'),
    ('C31', 'all', '任意の自分の大きな書庫', '読込・取消・連続本切替・終了。書庫形式と規模、所要時間、メモリを記録', '応答性と取消を確認。巨大書庫やRAR/7z/LHAは自前の実例で補完。未所持はNOT_RUN'),
    ('C32', 'all', 'images', '使用機器がある場合のみゲームパッド・単頁印刷を確認', '入力割当・実際の印刷が正常。未実施をPASSにしない。標準文書表示は印刷の対象外'),
    ('C33', 'all', 'books/05-pages.tar; books/06-pages.tar.gz', 'tarと圧縮tarを各々開いて6ページを往復', '番号1〜6を表示。圧縮書庫でもサムネイル・頁送り・終了が正常'),
    ('L01', 'Linux', 'images; documents/reflow.epub', 'Fedora 44の実Waylandでウィンドウ／全画面、DPI、複数画面、IMEとファイルダイアログ', '実画面描画・日本語入力・フォーカス・倍率が正常。XDG_SESSION_TYPEとQt platformを記録'),
    ('L02', 'Linux', 'images', 'Wayland Portal/ScreenSaverの抑制状態と実消灯、解放後の復帰を確認', 'offscreenの成功と区別。セッション側のAPI失敗・未対応も結果として残す'),
    ('L03', 'Linux', 'documents/sample.md', '既存の本体を終了。buildとは別のinstall先へcmake --installし、配置した本体でMarkdownとMAG/PIC/PIC2を開く。readers配下の5フォルダを確認', 'worker、mag/pic/pic2/pnm/reading-filter、desktop、アイコンを配置。スクリプト設定の選択肢はfilterのみ。Qtはシステム依存。AppImage検証とは別'),
    ('W01', 'Windows', 'images; documents/reflow.epub', 'stageの本体をExplorerから起動。開発PATH・QT_PLUGIN_PATHを除いた環境でも試す', 'Qt、非Qt DLL、qwindows、qsqlite、画像codec、WebEngineProcess／resources／localesが揃う'),
    ('W02', 'Windows', '日本語 空白/書庫 & 01.cbz', '空白・日本語パスにstageを移動。100/150/200% DPI、複数画面で操作', 'EXEアイコンと文字、入力、ダイアログ、画像表示が正常'),
    ('W03', 'Windows', 'images', '消灯抑制とスクリーンセーバーの実動作、最小化／終了後の解放を確認', '通常の自動消灯を抑制。手動ロックや保護ポリシーとの差を記録'),
    ('M01', 'Darwin', 'images; documents/reflow.epub', 'stageのKomaMimic.appをFinderから起動。日本語パスへ移動して再試験', 'Dock/Finderアイコン、WebEngine helper、worker、plugin、dylibと文書描画が正常'),
    ('M02', 'Darwin', 'images', 'Commandキーのメニュー表示、Ctrl相当の割当、トラックパッド、Retina、全画面／Spacesを確認', 'macOSのメニュー・フォーカス・入力が正常。自動置換されたキーは実際の表示で評価'),
    ('M03', 'Darwin', 'images', 'pmset -g assertionsも参照して消灯抑制ON/OFF・最小化・終了を確認', '前面読書中のみディスプレイ抑制を要求。システム全体のスリープ抑制とは区別'),
    ('M04', 'Darwin', 'KomaMimic.app', 'codesign検証とotool記録を確認。別のarm64 Macでも可能ならFinder起動', '署名・依存調査と実起動を別記。開発環境にだけ存在するdylibへの依存を残さない'),
    ('M05', 'Darwin', 'KomaMimic.app', 'quarantineのある取得物で警告と個別許可手順を確認できる場合のみ実施', 'ローカル生成.appだけの成功はGatekeeper検証にしない。未公証であることを明示。未実施はNOT_RUN'),
]

STATUSES = ('NOT_RUN', 'PASS', 'FAIL', 'BLOCKED', 'NA')


PLATFORM_IDS = {'C01', 'C02', 'C09', 'C10', 'C11', 'C12', 'C13', 'C16',
                'C19', 'C20', 'C21', 'C22', 'C27', 'C29', 'C30', 'W01', 'W02', 'W03'}


def cases_for(system, scope='full'):
    return [dict(zip(('id', 'platform', 'input', 'operation', 'expected'), case))
            for case in CASES if case[1] in ('all', system)
            and (scope != 'platform' or system != 'Windows' or case[0] in PLATFORM_IDS)]
