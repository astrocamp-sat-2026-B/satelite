# Pico W - PC Wi-Fi communication test

Pico W を AP モードで起動し、PC から `get_number` コマンドを送って、数値 `42` を受け取る最小サンプルです。

## 必要なもの

- Raspberry Pi Pico W
- Pico SDK
- CMake
- ARM GNU Toolchain
- PC 側の Python

## Pico W 側

1. Pico SDK の `pico_sdk_import.cmake` をこのフォルダに置きます。
2. ビルドします。

```powershell
mkdir build
cd build
cmake ..
cmake --build .
```

生成された `pc_commu.uf2` は Pico W 用です。無印 Pico 用に作った UF2 は Wi-Fi 機能がないため、Pico W では使用しません。

### Pico W への書き込み

1. Pico W の USB ケーブルを抜きます。
2. Pico W の `BOOTSEL` ボタンを押し続けます。
3. `BOOTSEL` を押したまま USB ケーブルを PC に接続します。
4. エクスプローラーに `RPI-RP2` ドライブが表示されたら、`BOOTSEL` を離します。
5. `build\pc_commu.uf2` を `RPI-RP2` ドライブへコピーします。
6. コピーが終わると Pico W が自動的に再起動します。

Pico 無印を使っていたときの USB ケーブルをそのまま使えます。ただし、充電専用ケーブルでは認識されないため、データ通信対応の USB ケーブルを使用してください。

シリアルモニターに次が表示されれば AP 起動済みです。

```text
SSID: satelite-ap
IP address: 192.168.4.1
Waiting for PC on port 5000
```

## PC 側

PC の Wi-Fi 設定で次に接続します。

- SSID: `satelite-ap`
- Password: `pico-wifi`

その後、このフォルダで実行します。

```powershell
python pc_client.py
```

次が表示されれば通信確立です。

```text
Pico W response: 42
Wi-Fi communication succeeded!
```
