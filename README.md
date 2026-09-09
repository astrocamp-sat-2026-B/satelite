# satelite
カメラ機能付きの衛星開発

## APでの双方向通信(最小構成)

### ⓪ Pico 2 WへUF2を書き込む
1. Pico 2 Wの基板上にある BOOTSEL ボタンを押したままにします。
2. そのままUSBケーブルでPCへ接続します。
3. エクスプローラーに RPI-RP2 ドライブが現れたら、BOOTSELを離します。
4. ap_tcp_client.uf2 を RPI-RP2 ドライブの直下へコピーします。
5. コピー後、RPI-RP2 ドライブは自動的に消え、Pico 2 Wが再起動します。
数秒後、PCのWi-Fi一覧に次が現れれば書き込み成功です。

SSID: PICO2W_DEMO<br>
Password: pico2w-demo

### ① PCをPicoのAPへ接続する
1. PCのWi-Fiで PICO2W_DEMO に接続します。
2. Wi-FiアダプターのIPv4設定でIPアドレスとサブネットマスクを手動設定します。
    Win+Rからncpa.cplを選び,プロパティのTCP/IPv4を選ぶと設定画面になる


### ② PCサーバを起動する
1. pc_tcp_server.exe のあるフォルダを開きターミナルで次を実行
`.\pc_tcp_server.exe`	
2. 成功すると次のように表示される

**PicoからPCへの送信**<br>
Waiting on TCP port 4242…<br>
Pico connected<br>
PC -> Pico: PC_READY<br>
PC -> Pico: PC_ACK

### 通信設定

| 項目 | 値 |
| --- | --- |
| Pico 2 W（AP）のIPアドレス | `192.168.4.1` |
| PCの固定IPアドレス | `192.168.4.2` |
| サブネットマスク | `255.255.255.0` |
| TCPポート | `4242` |

PicoのAPはインターネットへ接続しないため、PCに「インターネットなし」と表示されても正常です。

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

| ファイル・フォルダ | 役割 |
| --- | --- |
| `README.md` | 接続、書き込み、動作確認の手順と通信設定を記録する。 |
| `pc_tcp_server.c` | Windows PCで動かすTCPサーバ。Picoからのデータを表示し、`PC_READY` と `PC_ACK` をPicoへ返す。 |
| `pico_ap_tcp_client/CMakeLists.txt` | Pico SDK向けのビルド設定。Pico 2 W用のWi-Fi/lwIPライブラリと、出力するUF2を定義する。 |
| `pico_ap_tcp_client/pico_sdk_import.cmake` | ローカルに導入したPico SDKをCMakeから読み込むための補助ファイル。 |
| `pico_ap_tcp_client/lwipopts.h` | Pico上の軽量TCP/IPスタック lwIP のメモリ量とTCP/IP機能を設定する。 |
| `pico_ap_tcp_client/pico_ap_tcp_client.c` | Pico 2 Wで動く本体コード。APを開始し、PCのTCPサーバへ接続して定期送信と受信ログ出力を行う。 |
| `pico_ap_tcp_client/.vscode/` | Raspberry Pi Pico VS Code拡張機能向けのビルド、書き込み、デバッグ設定。開発者間で共有する。 |
| `pico_ap_tcp_client/.gitignore` | Picoプロジェクト内のビルド生成物をGitの管理対象から外す。 |

## GitHubへ含めないファイル

次はPCごとに生成されるため、GitHubには通常コミットしません。

```text
pico_ap_tcp_client/build/       # CMakeのビルド生成物
*.uf2                           # Picoへ書き込む生成済みファームウェア
*.elf, *.o, *.map               # コンパイル・リンク生成物
*.exe                           # PCサーバの生成済み実行ファイル
debug.log                       # ローカルのログ
```

別PCでそのまま実行できるように完成済みの `pico_ap_tcp_client.uf2` と
`pc_tcp_server.exe` を配布したい場合は、ソースと分けてGitHub Releasesへ添付します。