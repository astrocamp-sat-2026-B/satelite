# OV7675で写真を撮り、PCで確認する

ゼミのPico W + OV7675（B0070）用の初期動作確認プログラムです。
`camera` は320×240 / RGB565をPIO + DMAで取り込み、USB CDCでPCに送ります。
PC側はCRCを確認してPNGを保存し、標準画像ビューアで開きます。
既存のLチカは `blink` ターゲットとして残しています。

**検証状況:** camera.uf2をビルド・実機書き込み済み。COM4でPID=76 / VER=73を確認し、
カラーバーと実写を320×240で受信、CRC一致を確認しました。PC側6テストも合格。
静的RAM使用量は156,912バイトです。RGB565のバイト順は実機のカラーバーで確認しています。
初期化はOV7675データシートの公開レジスタと工場出荷時ISP設定を使った最小構成です。
実写では対象の文字が判読できることまで確認済みです。露出・画質は初期調整段階で、
現状は左端に約8ピクセルの帯が残ります。後段で出力窓と露出を調整してください。
撮影結果: [実写](photos/first-photo.png) / [カラーバー](photos/bars.png)。

このPCでは仮想環境と依存ライブラリを準備済みです。現在の接続はCOM4です。
すぐに撮り直すには、このフォルダで次を実行します。

```powershell
.\.venv\Scripts\python.exe tools/capture.py --port COM4
```

## ゼミの要求と今回の範囲

[ミッションと制約](https://astrocamp-sat-dev.pages.dev/hardware/overview/)は、
既設ハードウェアを使い、太陽光源と対象の相対位置が分かる状況で対象画像を取得することです。
今回の到達点は、開発中の固定した機体で1枚撮影し、PC上でその画像を目視確認することです。
USBは開発確認用です。本番は無線のみという制約があるため、後段でWi-Fiダウンリンクと姿勢制御が必要です。

機能は、センサID確認と初期化、写真/センサ内蔵カラーバー撮影、USB転送、CRC検証、PNG保存・表示です。
画像バッファは153,600バイト。VGA RGB565の614,400バイトはRP2040のRAMに収まらないためQVGAを選択しています。

## 配線

[ゼミのGPIO表](https://astrocamp-sat-dev.pages.dev/hardware/components/)に合わせています。
完成済みのゼミ基板では配線変更は不要です。以下は確認用で、カメラのコネクタ端子番号ではありません。

| カメラ信号 / 基板ネット | Pico GPIO | Pico物理ピン |
|---|---|---|
| D0〜D7 | GP0〜GP7（同じビット順） | 1, 2, 4, 5, 6, 7, 9, 10 |
| CAM_SDA | GP14 / I2C1 SDA | 19 |
| CAM_SCL | GP15 / I2C1 SCL | 20 |
| PCLK | GP22 | 29 |
| HS（HREFとして使用） | GP26 | 31 |
| VS | GP27 | 32 |
| XCLK_PICO | GP28 / PWM | 34 |
| モジュール電源 | 3V3 | 36 |
| GND | GND | 例: 38 |

B0070モジュールのVCCは3.3Vです。裸のセンサの電源仕様とは区別してください。
GP11のサーボ、GP12/13のデバッグUART、GP16〜21のADC/IMUは本プログラムでは制御しません。
画像用COMポートは**OBCのUSBコネクタ**です。デバッグプローブ側UARTではありません。
基板を使わずモジュール単体を接続する場合は、PEN/PDNとコネクタの向き・端子名を実物と回路図で確認してください。
モジュール資料の端子表には欠番があるため、表の行番号から配線を推測しないでください。

機体はゼミの開発制約どおり固定します。カメラ撮影には電池・サーボ駆動は必要ありません。

## ビルドと書き込み

このフォルダのPowerShellで実行します。既存のPico SDK環境を利用します。

```powershell
$cmakeExe = "$env:USERPROFILE/.pico-sdk/cmake/v4.3.4/bin/cmake.exe"
& $cmakeExe -S . -B build
& $cmakeExe --build build --target camera -j 4
```

成功時に `build/camera.uf2` ができます。
Pico WのBOOTSELを押しながらPCへUSB接続し、表示される `RPI-RP2` ドライブへこのUF2をコピーします。
再起動するとUSBシリアルポートとして認識されます。
`blink.uf2` では撮影できません。

## 初回の画像確認

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe tools/capture.py --list
```

一覧からOBCのCOM番号を選びます。以下の `COM5` は実際の番号に置き換えてください。
シリアルモニタなど、そのCOMポートを使う他のアプリは閉じておきます。

まずカメラ内蔵カラーバーを撮影します（PC上で作ったダミー画像ではありません）。

```powershell
.\.venv\Scripts\python.exe tools/capture.py --port COM5 --test-pattern --output photos/bars.png
```

次に、明るい場所で文字や色のある物体にカメラを向けて実写します。

```powershell
.\.venv\Scripts\python.exe tools/capture.py --port COM5
```

`photos/日時.png` に保存し、画像ビューアが開きます。同じコマンドで何度でも撮り直せます。
表示しない場合は `--no-show`、保存場所指定は `--output photos/object.png` を使います。
ピント調整はモジュールのレンズ構造に従って行ってください。

確認の合格条件:

1. `PID=76 VER=73` と `OK OV7675 ...` が出る。
2. カラーバーが崩れず表示され、`CRC OK` と320×240のPNGが得られる。
3. 実写で対象が識別でき、対象を動かして再撮影すると画像も変わる。

CRC OKは転送成功の確認です。センサの色・画角・タイミングが正しいことまでは保証しません。

## 動作のしくみ

`camera.c` はGP28に12.5MHz（標準125MHzシステムクロック時）のXCLKを出し、
100kHzのSCCBで7ビットアドレス `0x21` に設定を書き込みます。
資料の8ビット書込アドレス `0x42` はSDKには渡しません。
識別値はOV7675資料に記載の `0x76:0x73` を確認します。
PLLをバイパスし、内部クロックを4分周して取り込みの余裕を確保します。

`camera_capture.pio` がVSYNCのHigh→Lowでフレームに同期し、HREF中のPCLK立ち上がりで8ビットを読みます。
4バイトごとにDMAでRAMへ送り、240行×640バイトを取得します。
画像取り込み中にUSBへ画像データを送らず、取得後にCRC付きで転送します。
同期不成立や短い行、FIFOオーバーフローはエラーにし、5秒でタイムアウトします。
フレーム窓や余分な画素の有無は実機/ロジックアナライザでの確認が必要です。

USBの簡易プロトコル（コマンドは改行不要）:

| 送信 | 応答 |
|---|---|
| `I` | 初期化。`INFO ...` の後に `OK ...` または `ERR ...` |
| `C` | 実写1枚 |
| `T` | センサ内蔵カラーバー1枚 |
| `?` | コマンド説明 |

画像はASCII行 `FRAME 320 240 RGB565 153600 cccccccc\n` の直後に153,600バイトの生データです。
`cccccccc` はペイロードのCRC32（IEEE、Python zlib互換）です。
この実機のRGB565は下位バイト→上位バイトの順で、PCでRGB888に変換します。
USB CDCでは設定値115200は物理転送速度ではありません。

## 問題の切り分け

| 症状 | 確認すること |
|---|---|
| COMが出ない | データ対応USBケーブル、UF2書き込み、OBC側USBか |
| ポートが開けない | COM番号、他のシリアルモニタによる占有 |
| SCCBエラー | 3.3V/GND、GP14/15、SDA/SCLのプルアップ、XCLK、PEN/PDN |
| unsupported sensor ID | 表示されたPID/VERとモジュール型番。IDチェックを無条件で外さない |
| captureエラー | GP22=PCLK、GP26=HREF、GP27=VSYNC、GP28=XCLK。クロックと同期信号を計測 |
| バーも画像も崩れる | D0〜D7順序、HREF中640バイト×240行か、PCLK極性/タイミング |
| バー正常・実写が暗い | 照明、レンズ、露出待ち。初期設定は画質調整未実施 |
| 色だけ不自然 | カラーバーで検証。診断用 `--swap-bytes` / `--swap-rb` でバイト順とR/B順を個別確認 |
| CRC不一致・途中で止まる | USB再接続後に再撮影。転送中の他アプリ接続を避ける |

PC側テスト（追加パッケージ不要）:

```powershell
python -m unittest discover -s tools -v
```

## 参照資料

- [ゼミのシステム構成・回路図リンク](https://astrocamp-sat-dev.pages.dev/hardware/)
- [ゼミのコンポーネントとGPIO表](https://astrocamp-sat-dev.pages.dev/hardware/components/)
- [B0070モジュール仕様](https://akizukidenshi.com/goodsaffix/b0070.pdf)
- [OV7675データシート](https://akizukidenshi.com/goodsaffix/ov7675.pdf)（COM7/COM15、CLKRC/DBLV、SCCB、テストパターン）

OV7670とOV7675では予約レジスタの意味が異なります。本実装ではOV7670の長い初期化表をコピーせず、OV7675の公開レジスタを設定しています。
