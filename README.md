# Pico 2 W AP TCP通信デモ

Pico 2 WをWi-Fiアクセスポイント（AP）として動かし、接続したWindows PCとTCPで双方向通信する最小構成です。

- PCから任意のコマンド文字列を送信できる
- Picoは受信した文字列を`PICO_REPLY:`付きで返信する
- Picoは2秒ごとに疑似テレメトリをPCへ送信する
- テレメトリ送信時にPico 2 Wの内蔵LEDが約200 ms点灯する

## 通信構成

```text
Windows PC（TCPサーバー: 192.168.4.2:4242）
        ↑              ↓
        └──── TCP ────┘
Pico 2 W（AP / TCPクライアント: 192.168.4.1）
```

| 項目 | 値 |
| --- | --- |
| SSID | `PICO2W_DEMO` |
| Password | `pico2w-demo` |
| PicoのAP IPアドレス | `192.168.4.1` |
| PCの固定IPアドレス | `192.168.4.2` |
| サブネットマスク | `255.255.255.0` |
| TCPポート | `4242` |

## ファイル構成

```text
.
├── README.md
├── pc_tcp_server.c
└── pico_ap_tcp_client/
    ├── CMakeLists.txt
    ├── pico_sdk_import.cmake
    ├── lwipopts.h
    ├── pico_ap_tcp_client.c
    ├── .gitignore
    └── .vscode/
```

| ファイル・フォルダー | 役割 |
| --- | --- |
| `pc_tcp_server.c` | Windows PCで動くTCPサーバー。キーボード入力をPicoへ送り、コマンド応答とテレメトリを表示する。 |
| `pico_ap_tcp_client/pico_ap_tcp_client.c` | Pico 2 Wで動く本体コード。AP開始、TCP接続、コマンド応答、テレメトリ送信、LED点滅を行う。 |
| `pico_ap_tcp_client/CMakeLists.txt` | Pico SDK向けビルド設定。Wi-Fi/lwIP、ADC、乱数、USB Serial Monitorを有効にする。 |
| `pico_ap_tcp_client/lwipopts.h` | Picoで使用するlwIP（TCP/IPスタック）の設定。 |
| `pico_ap_tcp_client/pico_sdk_import.cmake` | インストール済みのPico SDKをCMakeから読み込むためのファイル。 |
| `pico_ap_tcp_client/.vscode/` | Raspberry Pi Pico VS Code拡張機能用のプロジェクト設定。 |
| `pico_ap_tcp_client/.gitignore` | Picoプロジェクトのビルド生成物をGitの管理対象から外す設定。 |

## ビルド

### Pico 2 W用UF2

1. VS Codeで`pico_ap_tcp_client`フォルダーを開きます。
2. Raspberry Pi Pico拡張機能でBoardが`pico2_w`であることを確認します。
3. Buildを実行します。
4. 成功すると、次のファイルが生成されます。

   ```text
   pico_ap_tcp_client/build/pico_ap_tcp_client.uf2
   ```

### Windows PCサーバー

プロジェクト直下で、MinGW gccを使ってビルドします。

```powershell
gcc -Wall -Wextra pc_tcp_server.c -o pc_tcp_server.exe -lws2_32
```

## 実行手順

### 1. Pico 2 WへUF2を書き込む

1. Pico 2 WのBOOTSELボタンを押したままUSBでPCへ接続します。
2. エクスプローラーに`RPI-RP2`ドライブが表示されたら、BOOTSELを離します。
3. `pico_ap_tcp_client.uf2`を`RPI-RP2`ドライブ直下へコピーします。
4. ドライブが自動的に消え、Picoが再起動します。

### 2. PCをPicoのAPへ接続する

1. PCのWi-Fiで`PICO2W_DEMO`へ接続します。
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

## テレメトリ受信とLED

Picoは接続中、2秒ごとに次の形式でテレメトリを送ります。

```text
Telemetry <- Pico: uptime_s=12,temp_c=26.45,random=381
```

| 項目 | 内容 |
| --- | --- |
| `uptime_s` | Picoが起動してからの累積秒数。再起動時に0へ戻る。 |
| `temp_c` | RP2350の内蔵温度センサ値。目安として利用する。 |
| `random` | 0〜999の乱数。通信データが更新されていることを確認するための疑似値。 |

テレメトリを送信するたび、Pico 2 Wの内蔵LEDが約200 ms点灯します。LEDはPico側の送信動作、PC画面の`Telemetry <- Pico:`表示はPC側の受信動作の確認に使えます。

> 現在は動作検証用に、改行区切りの文字列をTCPで送受信しています。TCPでは送信単位と受信単位が必ず一致するわけではないため、実機機能へ拡張する際はメッセージ種別・連番・データ長・CRCを持つ通信プロトコルへ発展させます。
