# ObjectArc

ObjectArc は Raycast のようなキーボードランチャーです。`Win` キー (または `Alt+Space`) で画面中央に検索窓が開き、アプリの起動、開いているウィンドウへの切り替え、計算、URL やフォルダーを開く、PC の操作などを、キーボードだけで行えます。
[ObjectFrame](https://github.com/Object-Works/ObjectFrame) が起動していれば、ワークスペースやレイアウトの切り替えもここからできます。

- C++20 / Win32 + Direct2D で書かれた単体の `ObjectArc.exe`(ランタイム不要)
- 入力は Windows 標準の入力欄なので、日本語入力 (IME) もそのまま使えます
- よく使う候補ほど上に出ます

---

## 目次

1. [ビルド](#ビルド)
2. [起動と終了](#起動と終了)
3. [使い方](#使い方)
4. [検索できるもの](#検索できるもの)
5. [設定ファイル](#設定ファイル)

---

## ビルド

必要なもの:

- Visual Studio 2022 (「C++ によるデスクトップ開発」ワークロード)
- CMake 3.20 以上

```powershell
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

`build\Release\ObjectArc.exe` ができます。
実行中の `ObjectArc.exe` があるとリンクに失敗するので、先に `ObjectArc.exe quit` で終了してください。

### インストーラー

GitHub Actions ([.github/workflows/build.yml](../.github/workflows/build.yml)) が push のたびに exe をビルドし、
[Inno Setup](https://jrsoftware.org/isinfo.php) でインストーラー (`ObjectArc-<version>-setup.exe`) とポータブル版 zip を作ります。
どちらも Actions の実行結果の「Artifacts」からダウンロードできます。
`VERSION` と同じ番号のタグ (`v0.1.0` など) を push すると、GitHub の Releases にインストーラーと zip が公開されます。

インストーラーは既定でサインイン時の自動起動を登録します。
ローカルでインストーラーを作る場合は Inno Setup 6 を入れて次を実行します。

```powershell
& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" /DAppVersion=0.1.0 installer\ObjectArc.iss
```

---

## 起動と終了

```powershell
ObjectArc.exe                          # 裏で起動 (ホットキーで開く)。起動中にもう一度実行すると検索窓を開きます
ObjectArc.exe --config D:\my\arc.conf
ObjectArc.exe toggle                   # 起動中の ObjectArc の検索窓を開く / 閉じる (show も同じ)
ObjectArc.exe quit                     # 起動中の ObjectArc を終了
ObjectArc.exe --version
```

通知領域のアイコンを右クリックして「ObjectArc を終了」、または検索窓で「ObjectArc を終了」を実行しても終了できます。
`ofwm.conf` に `exec-once = "C:\Tools\ObjectArc\ObjectArc.exe"` と書けば、ObjectFrame と一緒に起動します。

> `Alt+Space` は PowerToys Run なども使うキーです。ほかのアプリが使っていると起動時に通知が出るので、`arc.conf` の `hotkey` を変えてください。

---

## 使い方

`Win` キーを単独で押して離すか、ホットキー (`Alt+Space`) を押すと、マウスカーソルのあるモニターの中央上寄りに検索窓が開きます。入力すると候補が絞り込まれます。
`Win` キーで開くときはスタートメニューの代わりになります。`Win+E` や `Win+D` などの組み合わせは今までどおり使えます。スタートメニューは `Ctrl+Esc` で開けます。`win_key = false` にすると `Win` キーは元の動作に戻ります。
何も入力していないときは、最近使ったものと、開いているウィンドウが並びます。
`compact = true` にすると、検索バーだけの小さな表示になり、入力したときだけ候補が下に広がります。

| キー | 動作 |
| --- | --- |
| `↑` `↓` / `Ctrl+P` `Ctrl+N` / `Tab` `Shift+Tab` | 候補を選ぶ |
| `PageUp` `PageDown` | 1 ページ分移動 |
| `Enter` | 選んだ候補を実行 (下端に何をするかが出ます) |
| `Ctrl+Enter` | アプリ・ファイル: エクスプローラーで場所を開く |
| `Shift+Enter` | アプリ・ファイル・シェル: 管理者として実行 |
| `Tab` (パスの候補) | そのパスを入力欄に補完 |
| `Ctrl+Backspace` | 1 単語削除 |
| `Esc` | 入力を消す。空なら閉じる |
| `Win` / ホットキー / ほかのウィンドウをクリック | 閉じる |

マウスでも候補をクリックして実行できます。ホイールでスクロールします。

---

## 検索できるもの

| 種類 | 例 | 動作 |
| --- | --- | --- |
| アプリ | `code`、`メモ`、`calc` | スタートメニューと同じアプリ一覧から起動。Store アプリは英語名 (`calc` → 電卓) でも見つかります |
| ウィンドウ | `chrome`、ウィンドウのタイトル | そのウィンドウに切り替え (最小化していれば元に戻す) |
| 計算 | `2^10/3`、`sqrt(2)*pi`、`１＋２×３` | 結果を表示し、`Enter` でクリップボードにコピー |
| URL | `github.com`、`https://...` | ブラウザーで開く |
| パス | `C:\Users\`、`%APPDATA%`、`~\Downloads` | ファイル・フォルダーを開く。フォルダーの中身も候補に出て、`Tab` で補完できます |
| シェル | `> ipconfig /all` | コマンド プロンプトで実行 |
| コマンド | `ロック`、`sleep`、`再起動` | 下の表を参照 |
| Web 検索 | (いつでも最後に表示) | 入力した言葉を Google などで検索 |

あいまい検索なので、`vsc` で Visual Studio Code、`wt` で Windows Terminal のように、単語の頭文字でも見つかります。
一度使った候補は、次から上に出やすくなります (`arc.conf` と同じフォルダーの `history.txt` に記録されます)。

計算で使えるもの: `+ - * / % ^`、かっこ、`sqrt abs sin cos tan asin acos atan log ln exp round floor ceil`、`pi`、`e`。全角の数字や `×` `÷` も使えます。

### コマンド

| コマンド | 動作 |
| --- | --- |
| ロック / スリープ | 画面をロック / スリープ |
| シャットダウン / 再起動 / サインアウト | 確認してから実行 |
| Windows の設定 / タスク マネージャー | 開く |
| ごみ箱を空にする | 確認してから空にする |
| ObjectArc の設定ファイルを開く / 設定を再読み込み / 終了 | ObjectArc 自身の操作 |

ObjectFrame が起動中なら次も出ます (`frame_commands = false` で出さない)。

| コマンド | 動作 |
| --- | --- |
| ワークスペース 1-9 | 切り替え (`ws3` や `3` でも見つかります) |
| レイアウト: dwindle / master / monocle / scroll | 現在のワークスペースのレイアウトを変更 |
| フローティングの切り替え / 全画面の切り替え | ObjectArc を開く前にフォーカスしていたウィンドウに対して実行 |
| 再タイル / 一時停止・再開 / 設定を再読み込み | ObjectFrame の操作 |

---

## 設定ファイル

`%USERPROFILE%\.config\objectarc\arc.conf` (初回起動時にコメント付きで作られます)。書式は `key = value`、`#` 以降はコメントです。
編集したら、検索窓で「ObjectArc の設定を再読み込み」を実行するか、通知領域のアイコン、または `ObjectArc reload` で再読み込みしてください。
[ObjectManager](https://github.com/Object-Works/ObjectManager) から主な項目を編集できます。`folder` と `search_url` は、このファイルで編集します。

| キー | 既定値 | 内容 |
| --- | --- | --- |
| `win_key` | `true` | `Win` キーを単独で押して離すと開く (スタートメニューは開かなくなります) |
| `hotkey` | `alt+space` | 開く / 閉じるキー。`ctrl` `alt` `shift` `win` と、`space` `a`-`z` `0`-`9` `f1`-`f24` などを `+` でつなぐ |
| `rows` | `8` | 一度に表示する候補の数 (4-12) |
| `compact` | `false` | `true` にすると、開いたときは検索バーだけを表示し、入力すると候補の数だけ下に広がります (最近使ったものとウィンドウの一覧は出ません) |
| `scale` | `100` | 大きさ (%, 50-200) |
| `keep_query` | `false` | 開き直したときに前回の入力を残す |
| `tray_icon` | `true` | 通知領域にアイコンを表示 |
| `show_windows` | `true` | 開いているウィンドウを候補に出す |
| `frame_commands` | `true` | ObjectFrame の操作を候補に出す |
| `folder` | (なし) | このフォルダーの `.exe` `.lnk` `.bat` `.cmd` `.url` `.ps1` なども候補に出す (2 階層下まで。複数行書けます) |
| `search_name` | `Google` | Web 検索の表示名 |
| `search_url` | `https://www.google.com/search?q=%s` | Web 検索の URL。`%s` が入力に置き換わります |
| `accent` | `auto` | アイコンの色。`auto` = Windows のアクセントカラー / `0xRRGGBB` |

アプリの一覧は起動時と、前回から 5 分以上たって検索窓を開いたときに、裏で読み直します。
