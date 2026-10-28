# 同梱readerとfilter

同梱packageはアプリに組み込まれ、追加設定なしで使えます。
編集用ソースはリポジトリの `scripts/readers/` にあります。
単独のファイル・フォルダ・書庫・サムネイルで共通のreaderを使用します。

| package | 対応範囲 | 主な制限 |
|---|---|---|
| mag | MAKI02の16/256色MAG、MAG系MAX | MAKI01、MSX screen 8/YJKは対象外 |
| pic | 旧Yanagisawa PICの対応機種・mode | PC-88VA tile、汎用32bit等は対象外。他形式のPICとは別 |
| pic2 | P2DT、15/24bit、P2BM/P2BI/P2SF/P2SS | パレット・他深度・未知blockは対象外。複数blockは静止画へ合成 |
| pnm | 単画像/連結P6 PPM、RGB、maxval 1〜65535 | P3/P5などをこのreaderでは実装しない |
| reading-filter | グレースケール・ガンマ・コントラスト | 設定を保存し、画像加工が有効なとき適用 |

PIC/PIC2の一部はwuimgの0BSD実装を移植しています。元の著作者・許諾・免責を
ソースに保持し、upstream.jsonで出所と内容snapshotを記録しています。
MAG/PPMは独立した実装です。

同梱版を編集して試す場合は、コピーしたpackageを同じIDで明示登録します。
組込みを直接書き換えず、再起動して読み直してください。
