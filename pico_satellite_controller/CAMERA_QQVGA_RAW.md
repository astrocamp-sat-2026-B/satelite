# OV7675 QQVGA RAW RGB565低遅延送信

## 実装仕様

```text
解像度       160 × 120 (QQVGA)
画素形式     RGB565、2 byte/pixel
圧縮         なし
画像本体     160 × 120 × 2 = 38,400 byte
TCPヘッダ    FRAME,160,120,RGB565,38400,<CRC32>\n
PC出力       24-bit BMP（RGB565をBGR888へ変換）
```

OV7675公式ページはVGA、QVGA、QQVGAとRGB565出力をサポートすると明記している。[^1] QQVGAの具体的なレジスタ値は、OV7675対応の公式Arduino OV767Xライブラリに合わせた。[^2]

## 低遅延化の内容

Pico側では、従来の153,600 byte QVGAフレームを38,400 byteへ削減した。RLE/JPEGエンコードは行わず、DMAフレームバッファをそのままTCPへ渡す。転送量はQVGA比25%、削減率75%である。

TCPはヘッダの後に、`tcp_sndbuf()`が受け入れられる最大量をループ内で連続してキューへ積み、1 pollあたり1回 `tcp_output()`する。画像バッファを次の撮影で再利用できるよう `TCP_WRITE_FLAG_COPY` を用いる。画像送信中のACK、イベント、テレメトリは別キューへ退避し、38,400 byteの途中へ文字列が混入しない。

PC側は既存のRAW RGB565受信器を使用する。フレーム長とCRC32を確認後、RGB565を24-bit BGRへ変換してBMPを保存する。従来は1画素ごとに3 byteを `fwrite()`していたが、全BMP画素をメモリ上で変換して1回で書くように変更した。これにより19,200回の小さいファイル書込みを1回へ削減する。

## OV7675設定

主なQQVGAレジスタは次のとおりである。

| Register | Value | Purpose |
|---|---:|---|
| COM7 `0x12` | `0x04` | RGB、VGA source window |
| COM3 `0x0c` | `0x04` | DCW/crop/window enable |
| COM14 `0x3e` | `0x1a` | manual scaling、DCW/PCLK divide by 4 |
| DCWCTR `0x72` | `0x22` | horizontal/vertical downsample by 4 |
| PCLK_DIV `0x73` | `0xf2` | scaling pixel-clock divide by 4 |
| HSTART/HSTOP/HREF | `0x16/0x04/0xa4` | QQVGA horizontal window |
| VSTART/VSTOP/VREF | `0x22/0x7a/0x0a` | OV7675-specific vertical centering |
| COM15 `0x40` | `0xd0` | RGB565 full range |

## PC受信形式

Picoから送るASCIIヘッダ例：

```text
FRAME,160,120,RGB565,38400,12ab34cd
```

連続撮影では先頭語を `FRAME_STREAM` にする。改行直後から38,400 byteが続く。TCPはメッセージ境界を保持しないため、PCは1回の`recv()`が1フレームに一致すると仮定せず、必要バイト数へ達するまで蓄積する。現行PC実装はこの方式で、同じ`recv()`の末尾に次のテキスト行が続いても処理できる。

RGB565の各成分は次で8-bitへ展開する。

```text
B8 = (B5 × 255) / 31
G8 = (G6 × 255) / 63
R8 = (R5 × 255) / 31
```

PIO/DMAバッファ内のバイト順は現行実機実装に合わせてlow byte、high byteとしている。色が青赤反転する場合は、解像度設定ではなくCOM3のbyte swapまたはPC側の16-bit組立順を確認する。

## 理論転送時間

画像本体だけの下限は、実効TCPスループットを `R` byte/sとして

```text
t_wire = 38,400 / R
```

である。例として実効1 Mbit/s（125,000 byte/s）なら約0.307秒、5 Mbit/sなら約0.061秒である。実測総遅延は次を別々に記録する。

```text
コマンド受信 → 撮影開始
撮影開始     → DMA完了
DMA完了      → 最終byte送信キュー投入
PC先頭受信   → 38,400 byte受信完了
受信完了     → BMP保存・画面更新
```

CRC32計算は38,400 byteを1回走査する。これを省けば数ms程度短縮し得るが、TCPのチェックサムだけではアプリケーションのフレーミング誤りを検出しにくいため、現状は完全性確認を優先して残している。

## 検証

- `camera.h`のコンパイル時フレームサイズは38,400 byte。
- Pico側変更ファイルはARM GCCの`-Wall -Wextra -Werror`構文検査を通過。
- CMake/Ninjaのリンクが完了し、QQVGA RAW版UF2を生成済み。
- PC側はMSVCの`/W4 /WX /Zs`構文検査を通過。
- PC TCPサーバー実行ファイルをMSVCで再ビルド済み。
- 実機の色、向き、フレーム欠落、総遅延は未測定。最初はカラーバーで確認し、その後通常画像で確認する。

## Sources

[^1]: OMNIVISION, [OV7675 product page](https://www.ovt.com/products/ov7675/), accessed 2026-09-11. QQVGA 160×120 and RGB565 support.
[^2]: Arduino, [Official Arduino_OV767X OV7670/OV7675 driver](https://github.com/arduino-libraries/Arduino_OV767X/blob/master/src/utility/ov7670.c), accessed 2026-09-11. OV7675-specific QQVGA register table.
