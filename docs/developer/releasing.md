# Tagged releases

`.github/workflows/release.yml` runs when a stable version tag is pushed. It reuses
the existing Build and Rocky/Ubuntu AppImage workflows, assembles their verified
outputs, uploads them to a draft, checks the uploaded assets, and publishes the
complete Release. It never creates or moves Git tags.

## v2.0.0の公開準備

公開対象の版番号は`2.0.0`です。CMake・両vcpkg manifest・README・日英の更新履歴を揃えます。
ローカル編集だけではtag・Releaseを公開しません。既存のv1タグは変更しません。
公開文書はmain-v1がv1、mainがv2を各branchの原稿から生成します。
共通のgh-pagesにv1をルートと`/v1/`、v2を`/v2/`へ保存し、他版を保持してPagesへ公開します。
PagesとReleaseの文書検査はv2の原稿だけを同じbuild_guide関数で生成し、固定v1原稿を取得しません。
Release workflowはgh-pagesやPagesを更新しません。
公開条件と初回移行は[CIの文書公開設定](ci.md#documentation-and-github-pages)を参照してください。
READMEの主リンクは`/v2/`と`/v2/en/`へ向け、v1の従来URLも併記します。
v2のReleaseノートのインストール案内も`/v2/`以下を使います。
README・ガイド・版切替メニューはv2.0.0向けの表記に揃えます。
`main-v1`はv1.x、`main`はv2.xの保守先とし、文書URLは維持します。

通常の手動Release実行は検査と成果物組立のみです。公開前に管理者が3 OSの検査・
依存ライセンス表示・対応ソース・checksumを確認し、最終commitと一致する新しいtagを
明示的に作成してpushします。既存tagや既存Releaseを再利用・移動しません。tagのcommitが`origin/main`へ反映済みであることもworkflowで検査します。
キャッシュ形式`thumbnails-v2`は維持し、版番号変更だけで消去・再生成しません。
v2の手動検査はRun workflowでブランチを`main`に指定する。3 OS Buildと
Rocky/Ubuntu AppImageの検査を新しいrunで実行し、`komamimic-release-bundle`を保存する。
既存Release・Gitタグ・Pagesは更新しない。

1. 変更をmainへcommit/pushし、BuildとDocumentation and Pagesの結果を確認します。
2. **Release → Run workflow → main**を実行します。手動実行では公開jobがスキップされ、
   3 OSの配布物を含む`komamimic-release-bundle`が保存されます。
3. bundleの版番号・commit・SHA256SUMS、対応ソース・依存通知を確認し、配布物を起動して
   画像・EPUB・動画を確認します。過去の成功報告を、変更後の最終commitの検査結果として流用しません。
4. 成功した最終commitへ管理者が注釈付きタグを付けてpushします。

   ```sh
   git tag -a v2.0.0 <confirmed-commit> -m "KomaMimic v2.0.0"
   git push origin v2.0.0
   ```

5. タグで起動したReleaseの完了と公開assetを確認します。v2日英ガイドへのリンクも確認します。
   公開後、main-v1のトップに残るv2.0.0の公開準備中表記はmain-v1で更新します。

既存タグ・Releaseの上書きはしません。手動実行の成功だけで正式公開済みとは扱いません。

## Version and tag checks

Use `vMAJOR.MINOR.PATCH`, for example `v1.0.0`, `v1.1.0`, or `v2.0.0`. The numeric
version must match all of:

- `project(KomaMimic VERSION ...)` in `CMakeLists.txt`
- `version-string` in `vcpkg.json`
- `version-string` in `cmake/windows-fast/vcpkg.json`
- The release version identified in `README.md`

Leading zeros, prerelease suffixes, build metadata, and incomplete versions are
rejected. The checkout must be clean and match the triggering commit. On a tag
push, the local tag must resolve to that commit. The remote tag is checked again
before upload and immediately before publication; annotated tags are supported.
The version examples above describe tag syntax, not a product roadmap.

## Build and publication gates

1. Validate the version and source selection, run release-logic regression tests,
   and build both documentation languages with MkDocs strict mode.
2. Call `build.yml`: common source/actionlint/reproducibility checks, Windows
   platform tests plus extracted-ZIP checks, and macOS full tests plus signed
   app/ZIP/DMG checks. Release sets `skip_linux: true`; Linux is built and fully
   tested in the AppImage workflow instead of repeating the Fedora build.
3. Call `rocky8-probe.yml`: Rocky full tests and AppDir staging, then AppImage
   packaging and extracted-runtime checks on Ubuntu 22.04.
4. Require both reusable workflows to succeed. Download only named artifacts from
   this workflow run and check every input checksum, version, commit, and source.
5. Build the final Linux ZIP and the common release bundle, recheck their checksums,
   and save **komamimic-release-bundle** as a 14-day Actions artifact.
6. Only for tag pushes, publish using the final job's `contents: write` token.
   Every earlier job has read-only contents permission. Manual runs never publish.

Releaseのvalidateでは、libarchive・patchelfのsourceとappimagetool・runtimeの
計4ファイルを先に取得し、固定SHA-256と照合する。成功した同runの中間artifactを
RockyとWindowsへ渡し、Ubuntuへも生成用ツールを引き継ぐ。後続ジョブでは再ダウンロードせず
hashを再確認して使う。取得失敗は3 OSビルド開始前に検出し、取得ログを
`diagnostics-release-inputs`へ保存する。フルビルド・CTestの回数は増やさない。
AppImageを単独実行した場合も、Rockyのビルド依存導入前に同じ入力検査を行う。

Windows retains the official Qt SDK and vcpkg binary-cache reuse. Only libarchive
is overlaid with the pinned 3.8.11 source; other ports keep the existing baseline.
The existing OS jobs and tests are reused without duplicating them.
Windows/macOSの配布検査もCTestの動画入力を再利用する。ZIP／DMGから取り出した
実行物を専用環境で起動し、映像・音声を復号する検査は維持する。
Tagged runs are not cancelled by subsequent runs. The Release and called-workflow
concurrency groups are distinct; normal branch/PR Build cancellation is retained.
The regular source-change Build trigger and the Pages workflow remain separate.

## Release assets

For version 2.0.0, the binary assets are:

- `KomaMimic-2.0.0-windows-x86_64.zip`
- `KomaMimic-2.0.0-macos-arm64.dmg`
- `KomaMimic-2.0.0-macos-arm64.app.zip`
- `KomaMimic-2.0.0-linux-x86_64.zip`

The Linux ZIP contains the AppImage, `komamimic.desktop`, `komamimic.png`, its build
manifest/checksums, and `sources/`. The launcher targets `~/.local/bin/`. AppImage
executable permissions are recorded in the ZIP. macOS remains ad-hoc signed and
unnotarized. Windows still requires Microsoft's Visual C++ v14 x64 Redistributable.
Windows symbols remain a separate Actions artifact and are not published here.

Other assets include the single application source tar.gz generated by Ubuntu's
source-checks job, its versioned SOURCE_MANIFEST, per-OS versioned BUILD_MANIFESTs,
Homebrew metadata, the Linux dependency source archives, `RELEASE_MANIFEST.json`,
`RELEASE_NOTES.md`, and `SHA256SUMS`. No build logs, probes, or arbitrary artifact
directory contents are published.
macOSでQt Multimediaを別ビルドした場合は、原本`qtmultimedia-<Qt版>.tar.xz`と
版番号・macOSを付けた`QT_MULTIMEDIA_BUILD.json`も保持する。ビルド記録はappの
manifest内のhashと照合し、その記録が指定する原本sourceのhashを検査する。
記録されていない追加ファイルは引き続き拒否する。

Source archives from each OS are checked against their manifests and the canonical
source. Windows may differ only by CRLF/LF in valid UTF-8 text without NUL bytes;
these differences are recorded, and its alternative archive is not published.
Other changes are rejected. The Linux ZIP also uses the canonical application
source, so same-named archives cannot differ across published assets.

`SHA256SUMS` covers every attached file except itself. The Release manifest records
the tag, commit, canonical source hash, asset hashes, and Windows line-ending
differences. Original package manifests retain their generation-time manual-test
status; CI does not invent a manual pass or a completed license audit.

## Before merging a branch

On a pull request, confirm the Build and Pages/documentation checks.
The Windows Build job uses the `platform` test scope,
which includes the Windows settings-import regression and single-instance IPC
smoke test; Fedora and macOS Build jobs run the `full` CTest scope.
Do not treat a local CTest pass as proof that the staged Windows ZIP, macOS
app/DMG, or Linux AppImage works after relocation. Check each packaging job
and its distribution candidate artifacts. For the complete multi-OS release
bundle, use the manual Release workflow on the pushed branch **before tagging**.
The manual run assembles and verifies release assets but does not publish.
Inspect its bundle and complete the separate dependency-notice and
corresponding-source audit before creating a public version tag.

## Trying the workflow before tagging

After committing and pushing the workflow, select **Release → Run workflow** and
the desired branch or tag. A manual run derives the version from the selected
source, builds and checks all packages, and saves **komamimic-release-bundle**.
Its publish job is skipped even if a tag was selected. This is the way to check
native integration before the first public tag; it does not create a Release.

Local release-helper tests do not contact GitHub:

```sh
python3 -m unittest discover -s tests -p test_release.py
```

The existing native package checks and actual GitHub Actions execution cannot be
replaced by cross-platform helper simulations. Additional GUI retesting should be
limited to changes that need it.

## Publishing and retrying

Before the administrator tags a public commit, finish dependency-notice,
corresponding-source, patch/build-information, version, and documentation review as
described in [packaging](packaging.md) and the [README](../../README.md#distribution).
Review the dry-run bundle. Push the public commit and workflow updates to the
default branch before tagging it. The administrator then creates and pushes the
matching tag. No Apple secret or PAT is required for this path; the publish job
uses `GITHUB_TOKEN`.

`tools/release.py` checks the existing remote tag, creates a draft through the
Release API, and retains the creation response's numeric Release ID. Subsequent
uploads, reads, and publication use that ID. This avoids relying on a newly
created draft appearing immediately in a tag search or release listing. A draft's
`untagged-...` browser URL does not mean that its selected tag is incorrect.

Uploads never replace an existing asset. The helper checks asset names, uploaded
states, sizes, and SHA-256 digests, and only then removes draft status. If GitHub
omits a digest, it downloads the asset and checks its bytes. It does not pass a
latest-release override or create/move a Git tag as a separate operation.

A failure before publication leaves no public Release; an upload failure can
leave a partial draft. Re-run the failed jobs in the same run to reuse successful
build artifacts. Matching draft assets are checked and retained; only missing
assets are uploaded. Conflicting assets or a draft explicitly recording another commit stop the
process. A matching already-published Release is left unchanged; different
published assets are never replaced. Do not move a public tag to retry a build.

If an older tag's run fails only in publication, re-running that run still uses
the helper stored at that tag. After repairing the helper, an administrator can
resume publication of the **original verified bundle** to an existing Release ID:

```sh
python3 tools/release.py resume --repo OWNER/REPOSITORY \
  --release-id 123456 --bundle ../tmp/release-recovery/bundle
```

This command publishes after verification; it is not a dry run. Use authenticated
`gh` or an existing `GH_TOKEN` with repository contents write access. Download the
`komamimic-release-bundle` artifact from the failed tag run first. The command
requires the specified Release to exist and checks its tag, commit metadata,
remote tag commit, and all asset hashes. It never creates a replacement Release
or overwrites conflicting assets. The helper checkout may be newer than the
bundle; the built application's source, manifests, and tag remain unchanged.
No native rebuild or tag replacement is required for a publisher-only repair.

References: [reusable workflows](https://docs.github.com/en/actions/how-tos/reuse-automations/reuse-workflows),
[Release API](https://docs.github.com/en/rest/releases/releases),
[asset upload API](https://docs.github.com/en/rest/releases/assets#upload-a-release-asset), and
[GitHub CLI API requests](https://cli.github.com/manual/gh_api).
