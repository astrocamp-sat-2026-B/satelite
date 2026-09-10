OV7675 カメラプログラム 使用方法
================================

概要
----

このプログラムは、Astrocamp Pico Wボードに接続されたOV7675カメラから、
320 x 240ピクセルのRGB565画像を取得します。

camera.cはカメラ制御を行う再利用可能なコンポーネントです。
camera_app.cは、そのコンポーネントをUSBシリアルから操作するアプリです。


配線
----

OV7675       Raspberry Pi Pico W
--------------------------------
D0～D7       GP0～GP7
SDA          GP14
SCL          GP15
PCLK         GP22
HREF         GP26
VSYNC        GP27
XCLK         GP28
VCC          3.3V
GND          GND

カメラ機能は、PIO0のステートマシン1個、DMAチャンネル1個、I2C1、および
GP0～GP7、GP14、GP15、GP22、GP26～GP28を使用します。


ビルド方法
----------

Raspberry Pi Pico SDKのビルド環境が必要です。
VS Codeの「Raspberry Pi Pico」拡張機能を使用する場合は、blinkフォルダを
プロジェクトとして開き、ボードにPico Wを指定してCompile Projectを実行します。

コマンドラインからビルドする場合は、Pico SDKの環境を有効にしたPowerShellで
次のコマンドを実行します。

  cd C:\Users\masaa\personal_dev\satelite\blink
  cmake -S . -B build -G Ninja
  cmake --build build

ビルドに成功すると、次のファイルが生成されます。

  build\camera.uf2


Pico Wへの書き込み
-------------------

1. Pico WからUSBケーブルを抜きます。
2. BOOTSELボタンを押したままUSBケーブルを接続します。
3. Windowsに表示されたRPI-RP2ドライブへbuild\camera.uf2をコピーします。
4. コピーが終わるとPico Wが自動的に再起動します。


実行方法
--------

1. デバイスマネージャーでPico WのCOMポート番号を確認します。
2. Tera Term、PuTTY、VS CodeのSerial MonitorなどでCOMポートを開きます。
3. 最初に半角大文字のIを送信し、カメラを初期化します。

USB CDCを使用しているため、ボーレートは実質使用されません。
ターミナル側では115200などを指定して構いません。

初期化成功時の出力例:

  INFO sensor PID=76 VER=73
  OK OV7675 320 240 RGB565 XCLK=12500000


コマンド
--------

  I    カメラを初期化する
  C    通常画像を1枚撮影する
  T    カラーバーテストパターンを有効にして1枚撮影する
  ?    コマンド一覧を表示する

Iで初期化してから、CまたはTを送信してください。


撮影データ
----------

撮影に成功すると、次の形式のテキストヘッダーが返されます。

  FRAME 320 240 RGB565 153600 1234abcd

各項目は、幅、高さ、画像形式、データサイズ、CRC32です。
このヘッダーの直後に153600バイトのRGB565画像データが送信されます。

CまたはTを通常のシリアルターミナルから実行すると、バイナリ画像データが
文字として表示されるため、画面が文字化けします。これは異常ではありません。

画像として保存・表示するには、PC側で以下を行う受信プログラムが必要です。

1. FRAMEヘッダーを読み取る。
2. ヘッダーに記載されたサイズのバイナリデータを受信する。
3. RGB565データをPNGなどの画像形式へ変換する。


エラー表示
----------

エラー時は「ERR」に続いて原因が表示されます。

  ERR SCCB communication failed
      カメラとのI2C通信に失敗しています。配線と電源を確認してください。

  ERR unsupported sensor ID
      接続されたセンサのIDがOV7675の想定値と異なります。

  ERR sensor register write failed
      カメラのレジスタ設定に失敗しています。

  ERR camera is not initialized
      Iコマンドによる初期化が行われていません。

  ERR capture timeout, short line, or overflow
      画像取得がタイムアウトしたか、同期信号またはデータ転送に問題があります。
