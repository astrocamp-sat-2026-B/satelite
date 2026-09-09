# ref_monitor

MCP3008(ADC) 経由でフォトリフレクタ(LBR-127HLD, ネット名 REF, CH4)の
生の読み値をひたすら出力し続けるだけの動作確認用プログラム。

しきい値判定・エッジ検出・RPM算出などは行わない。まずは値の挙動を実測するためのもの。

## 配線 (MAIN基板固定・変更禁止)

| 信号 | Pico W GPIO | Pico 物理pin | MCP3008 pin |
| --- | --- | --- | --- |
| MISO (DOUT) | GP16 | 21 | pin12 |
| CS/SHDN | GP17 | 22 | pin10 |
| CLK | GP18 | 24 | pin13 |
| MOSI (DIN) | GP19 | 25 | pin11 |
| VDD / VREF | 3V3(OUT) | 36 | pin16 / pin15 |
| GND | GND | — | pin9(DGND) / pin14(AGND) |

読み取りチャンネルは [ref_monitor.c](ref_monitor.c) 冒頭の `#define REF_ADC_CH 4` で指定。
ブレッドボードで単体確認する場合は `7` (未使用の空きチャンネル)に書き換える。

## ビルド

このプロジェクトは Raspberry Pi Pico 用 VS Code拡張機能が管理する SDK
(`~/.pico-sdk` 配下、SDK 2.3.1 / ARM GCC toolchain 15_2_Rel1 / CMake v4.3.4 / Ninja)
を使う前提。別の場所に Pico SDK を用意している場合は環境変数 `PICO_SDK_PATH` を
自分の環境に合わせて読み替えること。

### PowerShell

```powershell
$sdkHome = "$env:USERPROFILE\.pico-sdk"
$env:PICO_SDK_PATH = "$sdkHome\sdk\2.3.1"
$env:PICO_TOOLCHAIN_PATH = "$sdkHome\toolchain\15_2_Rel1"
$env:PATH = "$sdkHome\cmake\v4.3.4\bin;$sdkHome\ninja\v1.13.2;$sdkHome\toolchain\15_2_Rel1\bin;$sdkHome\picotool\2.3.1\picotool;$env:PATH"

cmake -G Ninja -B build .
cmake --build build
```

### bash (Git Bash 等)

```bash
SDK_HOME="$HOME/.pico-sdk"
export PICO_SDK_PATH="$SDK_HOME/sdk/2.3.1"
export PICO_TOOLCHAIN_PATH="$SDK_HOME/toolchain/15_2_Rel1"
export PATH="$SDK_HOME/cmake/v4.3.4/bin:$SDK_HOME/ninja/v1.13.2:$SDK_HOME/toolchain/15_2_Rel1/bin:$SDK_HOME/picotool/2.3.1/picotool:$PATH"

cmake -B build && cmake --build build
```

成功すると `build/ref_monitor.uf2` が生成される。

## 書き込み手順

1. Pico W の BOOTSEL ボタンを押しながら USB ケーブルで PC に接続する
   (すでに書き込み済みで動いている場合は、BOOTSELを押しながらリセットするか
   USB を挿し直す)
2. `RPI-RP2` という名前のドライブがマウントされるので、そこに
   `build/ref_monitor.uf2` をドラッグ&ドロップ (またはコピー) する
3. コピーが終わると自動的に再起動し、プログラムが実行される

## シリアルモニタの開き方

- ボーレート: 任意 (USB CDC なので実際のボーレート値は無視される。9600 や
  115200 など何でもよい)
- データビット/パリティ/ストップビット: 8N1 (デフォルトのままでよい)
- 接続後、最初の3秒はヘッダ表示待ちのウェイトなので出力が出るまで少し待つ

Windows での確認方法の例:

- デバイスマネージャーで「ポート(COM と LPT)」を開き、割り当てられた
  `COMxx` を確認する
- PuTTY や Tera Term、`Serial Monitor` (VS Code拡張) などで該当 COMポートを
  開く

出力例:

```
raw min= 812 max= 998 avg= 964  ( 3.108 V )  p-p=186  |####################################    |
```

- `min` / `max`: 100ms ウィンドウ(2ms x 50サンプル)内での最小値・最大値
- `avg`: 同ウィンドウの平均値とその電圧換算値
- `p-p`: `max - min` (振れ幅)
- 末尾のバーは `avg` を 0〜1023 を 0〜40文字にマッピングしたもの

## 実機確認時のチェックポイント

- 値が 0 や 1023 に張り付かず、中間のどこかで安定していれば正常
- センサ前に指や白い紙をかざして `avg` がはっきり動き、離すと元に戻るか確認
- 反射物を近づけたとき raw値が上がるか下がるか(極性)は実測で確定させる
- うまく値が出ない/おかしい場合に疑う順番:
  1. 共通GND (Pico / MCP3008 / センサ間)
  2. SPIモード (0,0 以外だとビットがずれる)
  3. CS制御 (3バイトの間 Low を保てているか)
  4. チャンネル番号の指定ミス (CH4 を読んでいるか)
  5. VDD/VREF近傍のデカップリング (0.1uF) — ジッタが気になる場合
