# Windowsでのビルド方法（0.8β）

Visual Studio 2022のC++デスクトップ開発ツールとWindows SDKを使用します。x64、C++20でビルドしてください。配布exeは`/O2 /MT /DNDEBUG`でビルドしています。

## Boost版とBoostなし版を同時にビルド

1. Boost 1.85をダウンロードし、展開します。
2. `boost\unordered\unordered_flat_map.hpp`があるディレクトリを確認します。
3. リポジトリのフォルダで次を実行します。

```bat
build_windows.cmd "C:\path\to\boost_1_85_0"
```

出力先は`dist`です。Boost版のexe名は`Edax_find_book_error_tool_0_8boost.exe`、Boostなし版は`Edax_find_book_error_tool_0_8.exe`です。このコードが使用するBoost機能はヘッダーのみでビルドでき、`b2`によるBoostライブラリのビルドは不要です。exeの実行時にもBoost DLLは不要です。

環境変数`BOOST_ROOT`にBoostのインクルードディレクトリを設定済みなら、引数を省いて`build_windows.cmd`を実行できます。

## Boostなし版のみビルド

```bat
build_windows.cmd --no-boost
```

このモードではBoostのダウンロードや設定は不要です。Boostなし版はWin32のファイルマッピングとC++標準ライブラリを使用します。両版で同じ`config.ini`を使用できます。

Visual Studioを自動検出できない場合は、Visual Studio 2022の「x64 Native Tools Command Prompt」からスクリプトを実行してください。
