# 0.8β

Edax形式のbookを走査して問題のある棋譜を出力するツールの更新版です。Windows x64向けのBoost版とBoostなし版を配布します。

## 主な変更

- mode 6/7を追加しました。bookのleaf直後の局面が欠落し、その次の局面がbookにある経路を探します。mode 6は次の手まで、mode 7はその直前までの棋譜を出力します。
- mode 1の判定を、子局面のleaf評価値が子局面のlink評価値の最大値より大きい場合に限定し、子のleaf手までの棋譜を出力するよう修正しました。mode 2～4の候補手の選択と、Pass・欠落した兄弟経路の扱いも修正しました。
- `per_move_error`と`cumulative_error`による石損の枝切りを追加しました。
- 従来の`max_leaf_moves`を`max_book_move_depth`に改名しました。`auto_detect_book_depth=True`では開始局面から到達できるbookの最深手数を計算してコンソールに表示し、手動の深さ設定を使わずに探索上限を決めます。
- Boost版のbook索引に`boost::unordered_flat_map`を導入し、bookのlinkを連続したarenaに格納してレコードあたりの保持量を削減しました。探索では正規化結果を再利用し、候補ごとの棋譜文字列のコピーを減らしました。結果ファイルへの書き込みもバッファを再利用します。
- Boostなし版にも同じ判定・設定・棋譜処理を反映しました。book読込にはWin32のメモリマップ、索引には`std::unordered_map`を使用します。大きなbookではBoost版の方がメモリ効率に優れます。

## 以前の設定からの移行

新しい`config.ini`を使用してください。旧名`max_leaf_moves`は読み取られません。深さを手動指定する場合は`max_book_move_depth`へ移してください。`auto_detect_book_depth=True`にすると手動値は無視されます。`-1`は手動の深さ制限なしです。

結果は`mismatched_positions.txt`へ追記されます。新しい走査の結果だけを得る場合は実行前に旧ファイルを削除してください。`book.dat`は同梱していません。

## 配布物

- `Edax_find_book_error_tool_0_8boost.exe` — Boost版、Windows x64
- `Edax_find_book_error_tool_0_8.exe` — Boostなし版、Windows x64
- `config.ini`と`specified_positions.txt` — リポジトリまたはSource code zipに含まれる設定例とmode 5の入力例
- 対応するソースはリポジトリの`Edax find book error tool0_8boost.cpp`と`Edax find book error tool0_8.cpp`です。

Visual Studio 2022、C++20、x64で両版をコンパイルし、mode 1～4・6/7の回帰テストと小さなbookの読込テストを確認しました。ピークメモリの測定は完了していません。
