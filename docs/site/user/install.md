# インストール

## Linux（AppImage版）

Linux版はx86_64（amd64）用のAppImageです。Qtなどの主要ライブラリを同梱しているため、
ソースからビルドするための開発用パッケージを追加せずに使えます。AppImageを好きな場所に置いて起動できます。

Ubuntu 22.04相当（glibc 2.35）以降の環境を基準に、自動検査を行っています。Fedora 44でも起動と基本動作を確認しています。

### 入手と展開

[GitHubのReleases](https://github.com/asfdrwe/KomaMimic/releases)で配布するLinux用AppImageを取得します。
`komamimic-linux-x86_64-appimage.zip`をダウンロードし、ZIPを展開してください。

実行するファイルは `KomaMimic-1.0.0-linux-x86_64.AppImage` です。
同じ場所の `sources/` は対応ソースであり、起動のために展開・ビルドする必要はありません。
以下のファイル名はv1.0.0の例です。別の版を使う場合は取得した名前に置き換えてください。

`SHA256SUMS`が付属している場合は、ZIPを展開したフォルダーで端末を開き、整合性を確認できます。

```sh
sha256sum --check SHA256SUMS
```

### 配置と起動

AppImageを置く場所の例として、ホームフォルダー内の `.local/bin/` を使います。
AppImageがあるフォルダーで端末を開き、次を実行します。

```sh
mkdir -p "$HOME/.local/bin"
mv KomaMimic-1.0.0-linux-x86_64.AppImage "$HOME/.local/bin/"
chmod u+x "$HOME/.local/bin/KomaMimic-1.0.0-linux-x86_64.AppImage"
"$HOME/.local/bin/KomaMimic-1.0.0-linux-x86_64.AppImage"
```

ファイルマネージャーから起動する場合も、先にファイルのプロパティで実行権限を許可します。
その後、AppImageをダブルクリックするか、ファイルマネージャーの「実行」を選びます。
画面の名称やダブルクリック時の動作はデスクトップ環境によって異なります。
メニューから起動したい場合は、デスクトップ環境の機能でこのファイルへのショートカットを作成します。

起動後の操作は[基本操作](basic.md)を参照してください。

### FUSEのエラーで起動できない場合

通常起動ではFUSEを使います。`Cannot mount AppImage`、`No suitable fusermount binary`などが
表示される場合は、OSのFUSE設定を確認するか、次の展開実行を使ってください。
端末から起動するとエラー内容を確認できます。

```sh
"$HOME/.local/bin/KomaMimic-1.0.0-linux-x86_64.AppImage" --appimage-extract-and-run
```

この方法はFUSEを使わず、一時フォルダーへ展開して起動します。展開先には十分な空き容量が必要です。
通常起動に比べて起動までに時間がかかることがあります。

展開したファイルを残して繰り返し使いたい場合は、新しい専用フォルダーを作って展開します。

```sh
mkdir "$HOME/.local/bin/KomaMimic-1.0.0-extracted"
cd "$HOME/.local/bin/KomaMimic-1.0.0-extracted"
../KomaMimic-1.0.0-linux-x86_64.AppImage --appimage-extract
./squashfs-root/AppRun
```

以後はこのフォルダー内の `squashfs-root/AppRun` から起動します。
展開した内容を一式保持してください。
FUSEや展開実行については[AppImage公式の説明](https://docs.appimage.org/user-guide/troubleshooting/fuse.html)も参照できます。

### 更新

1. KomaMimicを終了します。
2. 新しい版のAppImageを取得し、付属するチェックサムを確認します。
3. 使いたい場所へ配置し、実行権限を付けて起動します。
4. ショートカットを使っている場合は、参照先を新しいファイルへ変更します。

設定や本棚・履歴はAppImageとは別の場所に保存されるため、同じユーザーで起動すれば引き継ぎます。
更新前に[設定フォルダーをバックアップ](settings.md)しておくと、元の状態を残せます。
新しい版の動作を確認してから、不要になった古いAppImageを削除してください。
手動展開で使っている場合は、新しい版も別フォルダーへ展開して起動します。

### 削除

KomaMimicを終了し、AppImageと、自分で作成したショートカットを削除します。
手動展開した場合は、その版の専用展開フォルダーも削除します。

AppImageを削除しても、設定・本棚・履歴・キャッシュは残ります。
これらも削除したい場合は、[保存場所](settings.md)を確認し、必要なデータをバックアップしてから
該当フォルダーを削除してください。本棚・履歴・読書状態も失われるため、
後から使い直す予定がある場合は設定フォルダーを残します。
読んでいた画像や書庫などの作品ファイルは、アプリの削除とは別です。
