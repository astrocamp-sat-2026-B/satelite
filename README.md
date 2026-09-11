# Pico W 模擬衛星コントローラー

Raspberry Pi Pico W をアクセスポイント兼衛星コントローラーとして動かし、Windows PC からリアクションホイール、姿勢、光センサー、カメラを監視・操作するプロジェクトです。

現在の統合ファームウェアは `pico_satellite_controller/` です。主な機能は次のとおりです。

- Pico W の Wi-Fi AP と Windows PC 間の TCP 通信
- ICM-42688-P による姿勢推定（roll / pitch / 相対 yaw）
- FS90R とタイヤを使ったリアクションホイールの手動回転・目標角旋回・姿勢保持
- MCP3008 に接続した4面フォトダイオードとフォトリフレクタの計測
- 光センサーの整列条件を使った自動撮影
- OV7675 による 160 x 120 RGB565 撮影と CRC32 付き転送
- Windows 上の Web ダッシュボード、CSV ログ、画像ギャラリー

## システム構成

```text
ブラウザー
  http://localhost:8080
          |
          | HTTP
          v
Windows PC: pc_tcp_server.exe
  Wi-Fi: 192.168.4.2
          ^
          | TCP 4242（Pico から接続）
          |
Pico W: 192.168.4.1
  Wi-Fi AP / センサー取得 / 姿勢制御 / カメラ撮影
```

| 項目 | 設定値 |
| --- | --- |
| SSID | `PICOW_DEMO` |
| パスワード | `pico-w-demo` |
| Pico W の AP アドレス | `192.168.4.1` |
| Windows PC の固定 IPv4 アドレス | `192.168.4.2` |
| サブネットマスク | `255.255.255.0` |
| Pico - PC TCP ポート | `4242` |
| ダッシュボード HTTP ポート | `8080` |

Pico W は TCP クライアント、Windows プログラムは TCP サーバーです。Pico は接続が切れた場合も `192.168.4.2:4242` への再接続を続けます。

## 必要なもの

### ハードウェア

- Raspberry Pi Pico W
- ICM-42688-P
- FS90R 連続回転サーボとリアクションホイール
- MCP3008、フォトダイオード4個、LBR-127HLD フォトリフレクタ
- OV7675 カメラ
- USB ケーブルと各基板に必要な電源

### ソフトウェア

- Windows 10 / 11（PC サーバーは Winsock と Windows WLAN API を使用）
- Raspberry Pi Pico SDK、CMake、Ninja、ARM GCC
- PC サーバーの再ビルド時は MinGW GCC
- 姿勢制御の回帰試験を実行する場合は Node.js
- 任意のモダンブラウザー

Pico プロジェクトの VS Code 設定は Pico SDK `2.3.1`、ARM toolchain `15_2_Rel1`、Ninja `1.13.2` を前提に生成されています。

## 配線

| 機能 | Pico W | 接続先 |
| --- | --- | --- |
| カメラ D0 - D7 | GP0 - GP7 | OV7675 D0 - D7 |
| サーボ PWM | GP11 | FS90R 信号線 |
| カメラ SDA / SCL | GP14 / GP15 | OV7675 SCCB（I2C1） |
| MCP3008 MISO / CS / CLK / MOSI | GP16 / GP17 / GP18 / GP19 | MCP3008 pin 12 / 10 / 13 / 11 |
| IMU SDA / SCL | GP20 / GP21 | ICM-42688-P（I2C0、アドレス `0x69`） |
| カメラ PCLK | GP22 | OV7675 PCLK |
| カメラ HREF / VSYNC | GP26 / GP27 | OV7675 HREF / VSYNC |
| カメラ XCLK | GP28 | OV7675 XCLK |

MCP3008 の CH0 - CH3 を4個のフォトダイオード、CH4 をフォトリフレクタに使用します。MCP3008 の `VDD` / `VREF` は 3.3 V、`AGND` / `DGND` は GND へ接続します。

> FS90R の電源は基板の設計に従って接続してください。Pico W の GPIO からサーボへ給電しないでください。また、サーボの個体差により 1500 us で完全停止しない場合があるため、自由回転試験の前に中立値を校正してください。

## ファイル構成

```text
.
|-- README.md
|-- pc_tcp_server.c                 Windows TCP/HTTPサーバーとダッシュボード
|-- pc_tcp_server_dummy.c           実機不要のダミー実行用ラッパー
|-- pc_tcp_server.exe               ビルド済みWindows実行ファイル
|-- pico_satellite_controller/      現在の統合Pico Wファームウェア
|   |-- main.c                      初期化、周期処理、TCP、コマンド、撮影
|   |-- protocol.c/.h               改行区切り受信とテレメトリ生成
|   |-- telemetry.c/.h              センサー・制御状態の収集
|   |-- icm42688.c/.h               IMU取得と姿勢推定
|   |-- angle_integrator.c/.h        相対yawの数値積分
|   |-- attitude_control.c/.h       旋回・保持制御と異常監視
|   |-- servo.c/.h                  GP11のFS90R PWM制御
|   |-- photodiode.c/.h             MCP3008 CH0-CH3
|   |-- photoreflector.c/.h         MCP3008 CH4
|   |-- wheel_sensor.c/.h           マーカー周期からのrpm推定
|   |-- sun_capture.c/.h            光条件による撮影トリガー
|   |-- camera.c/.h                 OV7675 QQVGA RGB565撮影
|   |-- camera_capture.pio          PIOカメラ入力
|   |-- CMakeLists.txt
|   |-- tests/slew_sils.mjs         1軸姿勢制御のSILS回帰試験
|   `-- *.md                        姿勢推定・制御・カメラの技術資料
|-- blink/                          320 x 240 USBシリアル式カメラ単体試験
|-- captures/                       PCが保存した撮影画像
`-- *.csv                           接続セッションごとのテレメトリログ
```

`jpeg_encoder.*`、`rle.*`、`third_party/JPEGENC/` は過去方式や互換検証用として残っていますが、現在の `pico_satellite_controller/CMakeLists.txt` のビルド対象ではありません。現在の Pico ファームウェアが送信する画像形式は無圧縮 `RGB565` です。PC サーバー側は旧 `RGB565RLE` / `JPEG` フレームも受信できます。

## ビルド

### Pico W ファームウェア

VS Code の Raspberry Pi Pico 拡張機能を使う場合は、`pico_satellite_controller` フォルダーをプロジェクトとして開き、ボードに `pico_w` を指定して `Compile Project` を実行します。

Pico SDK の環境変数とツールへの PATH が設定済みなら、コマンドラインでもビルドできます。

```powershell
cd pico_satellite_controller
cmake -S . -B build -G Ninja
cmake --build build
```

生成物は次の場所です。

```text
pico_satellite_controller/build/pico_satellite_controller.uf2
```

### Windows PC サーバー

リポジトリ直下で MinGW GCC を使います。

```powershell
gcc -Wall -Wextra -std=c11 pc_tcp_server.c -o pc_tcp_server.exe -lws2_32 -lwlanapi
```

実機なしでダッシュボードを確認する実行ファイルは次のように作成できます。

```powershell
gcc -Wall -Wextra -std=c11 pc_tcp_server_dummy.c -o pc_tcp_server_dummy.exe -lws2_32 -lwlanapi
```

## セットアップと起動

### 1. Pico W へ書き込む

1. BOOTSEL ボタンを押したまま Pico W を USB 接続します。
2. `RPI-RP2` ドライブが表示されたら BOOTSEL を離します。
3. `pico_satellite_controller.uf2` を `RPI-RP2` へコピーします。
4. 自動再起動後、Pico W が `PICOW_DEMO` AP を開始します。

電源投入直後は IMU の静止バイアス校正を行います。少なくとも最初の2秒間は、機体とホイールを動かさないでください。制御開始前にダッシュボードまたはテレメトリで `attitude_calibrated=1` を確認します。

### 2. Windows PC を Pico W の AP へ接続する

1. Windows の Wi-Fi から `PICOW_DEMO` へ接続します。
2. その Wi-Fi アダプターの IPv4 を次のように手動設定します。

```text
IP address:       192.168.4.2
Subnet mask:      255.255.255.0
Default gateway:  空欄
DNS:              空欄
```

### 3. PC サーバーを起動する

```powershell
.\pc_tcp_server.exe
```

起動後に Pico が接続すると、概ね次のように表示されます。

```text
Dashboard: http://localhost:8080
Waiting on Pico TCP port 4242...
Pico connected
Auto-save session: MMDDHHMM.csv
PC -> Pico >
```

ブラウザーで <http://localhost:8080> を開きます。コンソールからもコマンドを入力でき、`/quit` で PC サーバーを終了します。

### 実機なしで確認する

次のどちらかで、2秒周期の疑似テレメトリと疑似カメラ画像を使えます。このモードでは TCP 4242 を開きません。

```powershell
.\pc_tcp_server.exe --dummy
# または
.\pc_tcp_server_dummy.exe
```

## Web ダッシュボード

ダッシュボードには次の機能があります。

- 稼働時間、温度、Gyro Z、相対 yaw、PD0 - PD3、フォトリフレクタ、Wi-Fi 品質、手動指令値の表示
- Gyro Z、相対 yaw、フォトダイオード、フォトリフレクタの時系列グラフ
- ホイール速度、相対旋回、中止、角度リセットの操作
- 任意コマンドの送信、遅延付きコマンドシーケンス、連続撮影シーケンス
- 撮影画像の表示とギャラリー移動
- コマンド・応答履歴と全履歴の切り替え
- CSV の保存・再読込、画像の再読込、HTML へのエクスポート

ダッシュボードは最大2000件の履歴をメモリに保持し、1秒ごとに `/api/telemetry` を取得します。主な HTTP エンドポイントは次のとおりです。

| メソッドとパス | 内容 |
| --- | --- |
| `GET /` | ダッシュボード |
| `GET /api/telemetry` | 最新値、接続状態、履歴の JSON |
| `POST /api/command` | 本文の1行を Pico へ送信 |
| `GET /camera/<filename>` | 保存済み画像 |
| `GET /camera/latest.bmp` | 最新画像 |
| `GET /board-image.png` | フォトダイオード配置図 |

配置図は `pc_tcp_server.c` の `BOARD_IMAGE_PATH` にある PNG を読み込みます。別の PC で使用するときは、この定数を実在する画像パスへ変更して PC サーバーを再ビルドしてください。画像がなくてもテレメトリ取得と操作には影響しません。

## コマンド

すべて ASCII の1行として送信し、改行で終端します。Web 画面または PC サーバーのコンソールから入力できます。

### 手動ホイール制御

| コマンド | 内容 | 応答例 |
| --- | --- | --- |
| `SET_VALUE,-100` - `SET_VALUE,100` | FS90R 速度指令を即時反映 | `ACK,SET_VALUE,-100` |
| `-100` - `100` | `SET_VALUE` の短縮形 | `ACK,SET_VALUE,50` |
| `GET_VALUE` | 現在の手動指令値を取得 | `VALUE,50` |
| `SERVO_CONFIG,neutral_us,deadband_us` | 中立パルスとデッドバンドをRAM上で設定 | `ACK,SERVO_CONFIG,1500,90` |
| `SERVO_STATUS` | 現在のサーボ設定を取得 | `SERVO_STATUS,neutral_us=1500,deadband_us=90` |

速度指令は `-100` - `100` を、設定した中立値から 700 - 2300 us の範囲へ線形変換します。`0` は中立パルスです。`SERVO_CONFIG` の許容範囲は `neutral_us=1400..1600`、`deadband_us=0..200` で、再起動すると既定値 `1500,90` に戻ります。手動速度指令は実行中の姿勢制御を解除します。

### 姿勢制御

| コマンド | 内容 |
| --- | --- |
| `ANGLE_RESET` | 現在の積分 yaw を 0° に設定 |
| `SLEW,target_deg` | 原点に対する目標角へ最短方向で旋回（`-3600..3600`） |
| `SLEW_REL,delta_deg` | 現在角から指定方向・指定量だけ旋回（`-360..360`） |
| `SLEW_STATUS` | モード、異常、目標、誤差、角速度、指令などを取得 |
| `SLEW_ABORT` | 旋回・保持を中止し、サーボ指令を 0 に設定 |

`SLEW_REL_CAPTURE,<angle>` も旧ダッシュボードとの互換性のため相対旋回として受け付けますが、旋回後の自動撮影は行いません。

旋回開始時は、IMU 校正済み、手動指令値が 0、推定ホイール回転数が 5 rpm 以下、カメラがアイドルである必要があります。角度誤差 3°以内かつ機体角速度 0.8 deg/s 以下が1秒継続すると `SLEW` から `HOLD` に移ります。IMU 異常、90秒のタイムアウト、または指令飽和が1.5秒継続すると `FAULT` になりサーボを停止します。

調整コマンドは制御停止中だけ使用でき、値は再起動時に既定値へ戻ります。

| コマンド | 引数の範囲 |
| --- | --- |
| `SLEW_CONFIG,angle_gain,rate_gain,wheel_gain,max_rate,max_wheel` | `0.05..2`, `0.1..5`, `0.1..10`, `0.5..30`, `10..90` |
| `HOLD_CONFIG,integral_gain,max_integral_rate,integral_zone` | `0..0.5`, `0..5`, `1..45` |
| `BREAKAWAY_CONFIG,min_accel,rate_threshold,angle_threshold,delay_ms` | `0..30`, `0.05..2`, `0.25..10`, `0..2000` |

既定値と実機調整の手順は `pico_satellite_controller/SLEW_CAPTURE_IMPLEMENTATION.md` を参照してください。

### カメラと太陽検出

| コマンド | 内容 |
| --- | --- |
| `CAPTURE` | OV7675 の通常画像を1枚撮影 |
| `CAPTURE_TEST` | カラーバーを1枚撮影 |
| `SET_SUN_THRESHOLD,0..1023` | 自動撮影に必要な PD2 / PD3 の最低値を設定 |
| `SET_SUN_TOLERANCE,0..1023` | PD2 と PD3 の許容差を設定 |
| `GET_SUN_CONFIG` | 現在のしきい値と許容差を取得 |

既定値は `threshold=570`、`tolerance=50` です。PD2 と PD3 がともにしきい値以上で、両者の差が許容値以内になった瞬間に1回だけ自動撮影します。その際、取得時の yaw を保持するため制御モードは `HOLD` になります。設定値は再起動すると既定値へ戻ります。

現在の統合ファームウェアには `STREAM_START` / `STREAM_STOP` コマンドはありません。撮影は上記コマンド、Web 画面、または太陽検出条件から1枚ずつ要求します。

不明な文字列は、初期の通信デモとの互換動作として `PICO_REPLY: <入力>` の形で返されます。

## テレメトリ

Pico は接続中、500 ms ごとに1行のテレメトリを送ります。送信成功時は Pico W の内蔵 LED が約200 ms 点灯します。IMU は core 1 で 200 Hz、姿勢制御は core 0 で 50 Hz、回転センサーは 500 Hz で処理します。

```text
TELEMETRY,wifi_mode=AP,uptime_s=12,temp_c=26.45,random=381,command_value=0,gyro_z_dps=1.25,gyro_z_angle_deg=45.30,roll_deg=0.42,pitch_deg=-0.18,attitude_calibrated=1,imu_samples=2400,imu_rejected=0,photodiode_adc=123|234|345|456,photoreflector_adc=512,wheel_rpm=36.50,control_mode=HOLD,control_fault=NONE,control_target_deg=45.00,control_error_deg=-0.30,control_rate_ref_dps=-0.11,control_wheel_command_percent=12.50,control_elapsed_ms=4200,control_settled_ms=1000
```

| フィールド | 内容 |
| --- | --- |
| `uptime_s` / `temp_c` / `random` | 稼働時間、RP2040 内部温度、通信確認用乱数 |
| `command_value` | 手動ホイール指令値 |
| `gyro_z_dps` / `gyro_z_angle_deg` | Z 軸角速度、原点からの非ラップ相対 yaw |
| `roll_deg` / `pitch_deg` | 重力基準の傾斜角 |
| `attitude_calibrated` | IMU バイアス校正状態 |
| `imu_samples` / `imu_rejected` | IMU の処理済み・棄却サンプル数 |
| `photodiode_adc` | MCP3008 CH0 - CH3 の10 bit ADC値 |
| `photoreflector_adc` / `wheel_rpm` | CH4 の値、マーカー周期から求めた符号付き回転数 |
| `control_mode` | `IDLE` / `SLEW` / `HOLD` / `ABORTED` / `FAULT` |
| `control_fault` | `NONE` / `IMU` / `TIMEOUT` / `SATURATION` |
| `control_target_deg` / `control_error_deg` | 制御目標角と角度誤差 |
| `control_rate_ref_dps` | 外側ループの目標機体角速度 |
| `control_wheel_command_percent` | 姿勢制御が算出したホイール指令 |
| `control_elapsed_ms` / `control_settled_ms` | 制御経過時間、連続整定時間 |

利用できないセンサー値は `NA` になります。PC ダッシュボードと CSV は現在、このうち稼働時間、温度、Gyro Z、相対 yaw、フォトダイオード、フォトリフレクタ、手動指令、Windows が取得した Wi-Fi 品質を保存・表示します。追加の姿勢・制御フィールドはコンソールの受信行で確認できます。

## 画像転送と保存

Pico は撮影後、次の ASCII ヘッダーと 38,400 byte の RGB565 データを同じ TCP 接続で送ります。

```text
FRAME,160,120,RGB565,38400,1234abcd
<RGB565 binary data>
```

末尾の値は RGB565 データの CRC32 です。PC はサイズと CRC32 を検証し、24 bit BMP に変換して `pc_tcp_server.exe` と同じ場所の `captures/` に保存します。

```text
captures/camera_YYYYMMDD_HHMMSS_mmm.bmp
```

画像転送中は ACK、イベント、テレメトリを Pico 側のキューへ退避し、バイナリデータへの混入を防ぎます。転送中の再撮影要求には `ERROR,CAMERA_BUSY` が返ります。

## CSV ログ

実機モードでは Pico が接続するたびに、PC サーバーの作業ディレクトリへ新しい CSV を作成します。

```text
MMDDHHMM.csv
MMDDHHMM_01.csv
```

受信したテレメトリ、PC コマンド、Pico 応答、接続イベント、カメライベントを接続セッション単位で自動保存します。CSV を Excel などで開いたままにすると更新できない場合があります。Web 画面の `Save CSV` からブラウザー側でも履歴を保存できます。

## 安全上の注意と制約

- ICM-42688-P は6軸 IMU で磁気センサーを持たないため、yaw は絶対方位ではなく相対角です。時間とともにドリフトします。
- `SLEW_ABORT` や通信断はサーボ指令を中立へ戻しますが、機体やホイールを瞬時に物理停止させるものではありません。
- 初回の自由回転試験では機体を拘束できる状態にし、低い指令・低い制御上限から確認してください。
- USB シリアル出力は有効です。起動失敗やセンサー初期化状態の診断には Serial Monitor を使用できます。
- 実機調整、制御則、姿勢推定の詳細は `pico_satellite_controller/ATTITUDE_ESTIMATION.md`、`pico_satellite_controller/SLEW_CAPTURE_IMPLEMENTATION.md`、`pico_satellite_controller/REACTION_WHEEL_ROTATION_RESEARCH.md` を参照してください。

## テスト

姿勢制御のソフトウェア回帰試験には Node.js を使用します。

```powershell
node pico_satellite_controller/tests/slew_sils.mjs
```

PC 側だけを確認するときはダミーモードを起動し、<http://localhost:8080> でテレメトリ、コマンド、CSV、カメラ表示を確認してください。
