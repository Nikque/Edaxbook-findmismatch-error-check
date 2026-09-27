# Edax book error tool 0.8β

Edax形式のオセロbookを読み、設定したmodeに該当する棋譜を出力するWindows x64向けツールです。Egaroucid形式のbookには対応していません。bookの学習機能は含みません。

## 配布ファイル

| ファイル | 内容 |
| --- | --- |
| `Edax_find_book_error_tool_0_8boost.exe` | Boost版。大きなbookを扱う場合はこちらを推奨します。実行時のBoost DLLは不要です。 |
| `Edax_find_book_error_tool_0_8.exe` | Boostなし版。Windowsのメモリマップと`std::unordered_map`を使用します。大きなbookではBoost版よりメモリ使用量が増える可能性があります。 |
| `Edax find book error tool0_8boost.cpp` | Boost版ソース。 |
| `Edax find book error tool0_8.cpp` | Boostなし版ソース。 |
| `config.ini` | 両版で共通の設定例。 |

## 使い方

1. Releaseから使用するexeを、リポジトリ（またはReleaseのSource code zip）から`config.ini`を取得し、同じフォルダに置きます。
2. Edax形式のbookを`book.dat`という名前で同じフォルダに置きます。book本体は配布物に含まれません。
3. `config.ini`の`mode`や探索上限を設定します。
4. そのフォルダを作業ディレクトリにしてexeを実行します。結果は`mismatched_positions.txt`に1行1棋譜で追記されます。再実行時に前回の結果を混ぜたくない場合は、このファイルを先に削除してください。

mode 5では`specified_positions.txt`を同じフォルダに置きます。各行は`0x0000000810000000 0x0000001008000000`のように自分の石と相手の石の16進数です。読み取った局面はログに出力されます。

## mode

「親」は現在のbook局面、「子」は親から1手進めた局面です。棋譜にPassの文字列は含めません。

| mode | 検出するもの |
| --- | --- |
| 1 | 子のleaf評価値が、子のlink評価値の最大値より大きい場合。子のleaf手まで出力します。 |
| 2 | 子の局面評価値と、子のlink・leaf評価値の最大値が一致しない場合。条件に合う子の手まで出力します。 |
| 3 | 親から子へ進む手の評価値と、符号を反転した子の局面評価値が一致しない場合。 |
| 4 | 親から子へ進む手の評価値と、符号を反転した子のlink・leaf評価値の最大値が一致しない場合。 |
| 5 | `specified_positions.txt`に指定した局面をbookから読み、ログに出力して終了します。 |
| 6 | bookのleafの直後の局面がbookにない一方、その次の合法手の局面はbookにある場合。その合法手まで出力します。 |
| 7 | mode 6と同じ条件で、次の合法手の直前まで出力します。 |

mode 2～4では、評価値の大小関係に応じ、基準値を超える子の手の棋譜、または最大評価値を持つ子の手の棋譜を出力します。

## `config.ini`

| 設定 | 内容 |
| --- | --- |
| `log_level` | `DEBUG`、`INFO`、`WARNING`、`ERROR`、`NONE`。 |
| `auto_adjust_level` / `adjusted_level` | ログレベルの自動調整と、その調整先。 |
| `mode` | 1～7。 |
| `per_move_error` | 1手の石損の許容上限。`-1`で無制限。 |
| `cumulative_error` | 黒白合計の累積石損の許容上限。`-1`で無制限。 |
| `max_book_move_depth` | 初期局面からのbook手数による探索上限。Passも1手として数え、`-1`で無制限。 |
| `auto_detect_book_depth` | `True`で開始局面から到達できるbook局面の最深手数を計算し、コンソールに表示して探索上限を自動設定します。このとき`max_book_move_depth`は使いません。 |
| `progress_update_interval` | 探索中のコンソール進捗表示間隔。1以上。 |

自動判定では、検出した最深局面を調べられるように内部上限を設定します。深さの計算にはbookを読み込んだ後に追加の走査が必要です。mode 5には探索上限を適用しません。

## ビルド

Visual Studio 2022のx64 C++20環境を使用します。`build_windows.cmd`で両版をビルドできます。Boost版はBoost 1.85のヘッダーで確認しました。Boostのヘッダーを含むディレクトリを引数に渡すか、`BOOST_ROOT`に設定してください。生成したexeは`dist`に置かれます。

```bat
build_windows.cmd "C:\path\to\boost_1_85_0"
build_windows.cmd --no-boost
```

Boostなし版のビルドにはBoostは不要です。詳細は[boost版のビルドの方法.md](boost版のビルドの方法.md)を参照してください。

## 更新履歴

- **0.8β:** mode 6/7追加、mode 1～4の判定と棋譜出力の修正、石損による枝切り、book深さの自動検出、`max_book_move_depth`への名称変更、探索・出力・book格納の高速化とメモリ改善、Boostなし版の更新。詳細は[RELEASE_NOTES_0_8.md](RELEASE_NOTES_0_8.md)を参照してください。
- **0.6β:** Passの文字列が棋譜に混入する問題を修正し、処理速度を改善。
- 以前の履歴はGitの過去のREADMEを参照してください。

## 謝辞・ライセンス

開発にあたり助言やツールを提供していただいた山名琢翔様、きしまろ様、kroud様に感謝します。従来版の開発にはClaude 3.5 Sonnetが使用されています。

このプロジェクトはMITライセンスで公開されています。Copyright (c) 2024 わんりゅー。
