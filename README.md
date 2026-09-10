# Pico W AP TCP通信デモ

Pico WをWi-Fiアクセスポイント（AP）として動かし、接続したWindows PCとTCPで双方向通信する最小構成です。

- PCから任意のコマンド文字列を送信できる
- PCから任意のタイミングでOV7675を撮影し、画像をBMPとして保存できる
- Picoは受信した文字列を`PICO_REPLY:`付きで返信する
- Picoは2秒ごとに疑似テレメトリをPCへ送信する
- テレメトリ送信時にPico Wの内蔵LEDが約200 ms点灯する

## 通信構成

```text
Windows PC（TCPサーバー: 192.168.4.2:4242）
        ↑              ↓
        └──── TCP ────┘
Pico W（AP / TCPクライアント: 192.168.4.1）
```

| 項目 | 値 |
| --- | --- |
| SSID | `PICOW_DEMO` |
| Password | `pico-w-demo` |
| PicoのAP IPアドレス | `192.168.4.1` |
| PCの固定IPアドレス | `192.168.4.2` |
| サブネットマスク | `255.255.255.0` |
| TCPポート | `4242` |

## ファイル構成

```text
.
├── README.md
├── pc_tcp_server.c
└── pico_satellite_controller/
    ├── CMakeLists.txt
    ├── pico_sdk_import.cmake
    ├── lwipopts.h
    ├── main.c
    ├── icm42688.c
    ├── icm42688.h
    ├── .gitignore
    └── .vscode/
```

| ファイル・フォルダー | 役割 |
| --- | --- |
| `pc_tcp_server.c` | Windows PCで動くTCPサーバー。キーボード入力をPicoへ送り、コマンド応答とテレメトリを表示する。 |
| `pico_satellite_controller/main.c` | Pico Wで動く本体コード。AP開始、TCP接続、コマンド応答、テレメトリ送信、LED点滅を行う。 |
| `pico_satellite_controller/icm42688.c` | ICM-42688のI2C初期化とZ軸角速度取得を行うドライバ。 |
| `pico_satellite_controller/photoreflector.c` | MCP3008のCH4からLBR-127HLDの生ADC値を読み取る。 |
| `pico_satellite_controller/camera.c` | OV7675を初期化し、QVGA RGB565画像をPIO/DMAで撮影する。 |
| `pico_satellite_controller/CMakeLists.txt` | Pico SDK向けビルド設定。Wi-Fi/lwIP、ADC、I2C、乱数、USB Serial Monitorを有効にする。 |
| `pico_satellite_controller/lwipopts.h` | Picoで使用するlwIP（TCP/IPスタック）の設定。 |
| `pico_satellite_controller/pico_sdk_import.cmake` | インストール済みのPico SDKをCMakeから読み込むためのファイル。 |
| `pico_satellite_controller/.vscode/` | Raspberry Pi Pico VS Code拡張機能用のプロジェクト設定。 |
| `pico_satellite_controller/.gitignore` | Picoプロジェクトのビルド生成物をGitの管理対象から外す設定。 |

## ビルド

### Pico W用UF2

1. VS Codeで`pico_satellite_controller`フォルダーを開きます。
2. Raspberry Pi Pico拡張機能でBoardが`pico_w`であることを確認します。
3. Buildを実行します。
4. 成功すると、次のファイルが生成されます。

   ```text
   pico_satellite_controller/build/pico_satellite_controller.uf2
   ```

### Windows PCサーバー

プロジェクト直下で、MinGW gccを使ってビルドします。

```powershell
gcc -Wall -Wextra pc_tcp_server.c -o pc_tcp_server.exe -lws2_32 -lwlanapi
```

## 実行手順

### 1. Pico WへUF2を書き込む

1. Pico WのBOOTSELボタンを押したままUSBでPCへ接続します。
2. エクスプローラーに`RPI-RP2`ドライブが表示されたら、BOOTSELを離します。
3. `pico_satellite_controller.uf2`を`RPI-RP2`ドライブ直下へコピーします。
4. ドライブが自動的に消え、Picoが再起動します。

### 2. PCをPicoのAPへ接続する

1. PCのWi-Fiで`PICOW_DEMO`へ接続します。
2. Wi-FiアダプターのIPv4設定を手動設定します。

   ```text
   IPアドレス: 192.168.4.2
   サブネットマスク: 255.255.255.0
   デフォルトゲートウェイ: 空欄
   DNS: 空欄
   ```

### 3. PCサーバーを起動する

```powershell
.\pc_tcp_server.exe
```

正常に接続されると、次のように表示されます。

```text
Waiting on TCP port 4242...
Pico connected
Pico -> PC: PICO_CONNECTED
PC -> Pico >
```

接続中は5秒ごとに、Windowsが測定したAPの受信信号品質も表示します。

```text
Wi-Fi link: PICOW_DEMO, about -63 dBm, 74% (Good)
```

| 表示 | 意味 |
| --- | --- |
| `dBm` | 0に近いほど強い。Windowsの品質値から換算した概算値。 |
| `%` | Windows WLAN APIが返す信号品質（0〜100%）。 |
| `Excellent` | 80%以上。非常に良好。 |
| `Good` | 60〜79%。良好。 |
| `Fair` | 40〜59%。通常利用可能。 |
| `Weak` | 20〜39%。切断や速度低下に注意。 |
| `Very weak` | 0〜19%。非常に弱い。 |

PicoはAPとして動作するため、Pico SDKの公開RSSI取得API（STA専用）は
利用できません。この表示は、実際にデータを受け取るWindows側から見た
`PICOW_DEMO`の電波強度です。テレメトリにも`wifi_mode=AP`を含めます。

## コマンド送信

`PC -> Pico >`の後に文字列を入力してEnterを押します。

```text
PC -> Pico > hello
Pico -> PC: PICO_REPLY: hello
```

PicoのUSB Serial Monitorには、Picoが受信した値が表示されます。

```text
PC -> Pico: hello
```

終了する場合は、PC側で`/quit`を入力します。

### サーボとカメラのコマンド

| 入力 | 動作 |
| --- | --- |
| `-100` ～ `100` | 連続回転サーボの速度を指定する（負数は逆転、0は停止）。 |
| `SET_VALUE,-100` ～ `SET_VALUE,100` | 上記と同じ。 |
| `GET_VALUE` | 現在のサーボ速度を取得する。 |
| `CAPTURE` | OV7675で通常画像を1枚撮影し、Windowsへ送る。 |
| `CAPTURE_TEST` | カラーバーを有効にして1枚撮影し、Windowsへ送る。 |

最初の撮影時にカメラを自動初期化します。Windows側は受信データのCRC32を
検証し、成功するとサーバーを起動したフォルダーへ次の名前で保存します。

```text
camera_YYYYMMDD_HHMMSS_mmm.bmp
```

転送中はテレメトリ送信を一時停止し、画像バイナリとテキストメッセージが
混ざらないようにします。撮影中または転送中に再度撮影を要求すると
`ERROR,CAMERA_BUSY`が返ります。

### OV7675の配線

| OV7675 | Pico W |
| --- | --- |
| D0～D7 | GP0～GP7 |
| SDA / SCL | GP14 / GP15（I2C1） |
| PCLK | GP22 |
| HREF / VSYNC | GP26 / GP27 |
| XCLK | GP28 |
| VCC / GND | 3.3V / GND |

カメラはPIO0のステートマシン1個とDMAチャンネル1個を使用します。

### 画像転送プロトコル

Picoは撮影後、改行で終わるヘッダーとRGB565バイナリを連続送信します。

```text
FRAME,320,240,RGB565,153600,1234abcd\n
<153600 bytes RGB565>
```

末尾の値は8桁16進のCRC32です。TCPの受信境界には依存せず、Windows側は
ヘッダーのサイズに従って画像データを復元します。

## テレメトリ受信とLED

Picoは接続中、2秒ごとに次の形式でテレメトリを送ります。

```text
Telemetry <- Pico: wifi_mode=AP,uptime_s=12,temp_c=26.45,random=381,command_value=50,gyro_z_dps=1.25,photodiode_adc=123|234|345|456,photoreflector_adc=512
```

| 項目 | 内容 |
| --- | --- |
| `uptime_s` | Picoが起動してからの累積秒数。再起動時に0へ戻る。 |
| `temp_c` | RP2040の内蔵温度センサ値。目安として利用する。 |
| `random` | 0〜999の乱数。通信データが更新されていることを確認するための疑似値。 |
| `photoreflector_adc` | LBR-127HLDを接続したMCP3008 CH4の生値（0〜1023）。通信・フレーミング異常時は`NA`。 |

### LBR-127HLD / MCP3008の配線

LBR-127HLDの信号はMAIN基板上でMCP3008のCH4（ネット名`REF`）へ接続します。
Pico WとMCP3008の接続は次のとおりです。

| 信号 | Pico W GPIO | MCP3008 pin |
| --- | --- | --- |
| MISO / DOUT | GP16 | pin12 |
| CS / SHDN | GP17 | pin10 |
| CLK | GP18 | pin13 |
| MOSI / DIN | GP19 | pin11 |
| VDD / VREF | 3.3V | pin16 / pin15 |
| GND | GND | pin9 / pin14 |

Windows側で`pc_tcp_server.exe`を起動すると、LBR-127HLDの値をほかの
テレメトリと一緒に2秒ごとに確認できます。

テレメトリを送信するたび、Pico Wの内蔵LEDが約200 ms点灯します。LEDはPico側の送信動作、PC画面の`Telemetry <- Pico:`表示はPC側の受信動作の確認に使えます。

> 現在は動作検証用に、改行区切りの文字列をTCPで送受信しています。TCPでは送信単位と受信単位が必ず一致するわけではないため、実機機能へ拡張する際はメッセージ種別・連番・データ長・CRCを持つ通信プロトコルへ発展させます。
