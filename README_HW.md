# rtsynth ハードウェア組み立てガイド（初心者向け）

Raspberry Pi と rtsynth でハードシンセを組むための、部品購入から動作確認までの手引きです。
電子工作が初めてでも、上から順に1ステップずつ進めれば完成するように書いてあります。
各ステップは独立しているので、**全部やる必要はありません**（例: まず DAC だけ、でOK）。

```
[USB MIDI 鍵盤]──USB──▶┌─────────────┐
[MIDIコン(CC)]──USB──▶│ Raspberry Pi │──I2S──▶[DAC]──▶ スピーカー/ヘッドフォン
[ツマミ×n]──▶[MCP3008]─SPI─▶│   rtsynth    │
[ロータリーエンコーダ]──GPIO──▶│              │──I2C──▶[16x2 LCD] "L1 DCW Level 1"
[エンコーダ×16/SW]─▶[MCP23017]─I2C─▶│              │                    "0.520"
                        └─────────────┘
```

## 0. 買い物リスト

### 必須ではないが順番におすすめ

| 部品 | 目安価格 | 用途 | 対応ステップ |
|---|---|---|---|
| USB MIDI キーボード | 手持ちでOK | 演奏入力 | Step 1 |
| PCM5102A DAC モジュール（GY-PCM5102 等） | 300〜800円 | 音質改善 | Step 2 |
| ブレッドボード + ジャンパワイヤ（オス-メス/オス-オス） | 500〜1000円 | 以降全部 | Step 3〜 |
| MCP3008-I/P（DIP-16 パッケージ） | 300〜400円 | ツマミ用 ADC | Step 3 |
| 可変抵抗（ポテンショメータ）10kΩ Bカーブ ×必要数 | 1個 50〜100円 | ツマミ | Step 3 |
| ロータリーエンコーダ EC11（ノブ付きが楽） | 100〜300円 | 相対値ツマミ | Step 4 |
| 1602 LCD + I2C バックパック（PCF8574、はんだ済みが楽） | 300〜600円 | パラメータ表示 | Step 5 |
| タクトスイッチ（6mm 4本足が定番）×3〜4 | 1個 10〜30円 | プリセット切替・電源断 | Step 6 |
| LED + 抵抗 330Ω〜1kΩ（任意） | 1個 10〜20円 | ボタンの押下表示 | Step 6 |
| MCP23017-E/SP（DIP-28）×5 + 74HC595 ×10 | 1個 50〜250円 | フロントパネル | Step 7 |
| ロータリーエンコーダ ×19 + DIP 化基板、タクトスイッチ ×29、LED ×87 | — | 同上 | Step 7 |

- 秋月電子・スイッチサイエンス・Amazon・AliExpress あたりで全部揃います
- はんだ付けを避けたいなら「ピンヘッダはんだ済み」のモジュールを選ぶこと

### 工具

- 必須: なし（全部ブレッドボードで組めます）
- あると便利: テスター（導通・電圧確認）、M-F ジャンパ多め

## 1. 事前知識（3分で読める安全講座）

### ピンの数え方 — 「物理ピン番号」と「GPIO番号」は別物

Raspberry Pi の 40 ピンヘッダには2種類の呼び方があります。

- **物理ピン番号**: コネクタ上の位置。1〜40。基板の隅の四角いパッドが 1 番、その隣（外側）が 2 番、以降ジグザグに増える
- **GPIO 番号 (BCM)**: チップ側の論理番号。`--enc 17,27=...` のようにソフトが使うのは**こちら**

本ガイドの配線表は両方併記します。`pinout` コマンド（Pi 上で実行）でいつでも確認できます。

### 電圧の鉄則

- **Pi の GPIO は 3.3V**。GPIO ピンに 5V を入れると壊れます
- センサ・ADC の電源は必ず **3.3V**（物理ピン 1 or 17）から取る
- 5V（物理ピン 2, 4）を使ってよいのは、このガイドでは **LCD バックパックの VCC だけ**
  （SDA/SCL はオープンドレインなので 5V 品でも安全です）
- **配線は必ず電源を切ってから**。`sudo poweroff` して LED が消えてから触る

### GND は全部つなぐ

全モジュールの GND は Pi の GND（物理ピン 6, 9, 14, 20, 25, 30, 34, 39 のどれでも）と
共通にします。「値が暴れる」「動いたり動かなかったり」の原因の 9 割は GND の接続忘れです。

## 2. Step 1: USB MIDI 鍵盤（配線なし・5分）

1. 鍵盤を USB に挿す
2. 確認: `./build/rtsynth --list` の「MIDI Input Ports」に鍵盤名が出る
3. `./build/rtsynth -v` で起動して鍵盤を弾く → `[midi] note on ...` が出て音が鳴る

rtsynth は既定で全 MIDI ポートに接続するので、鍵盤と MIDI コン（CC 用）の同時挿しもそのまま動きます。
起動中の抜き挿しにも対応しており、**起動時に 1 台も繋がっていなくて構いません**
（電源投入と同時に自動起動させる構成では、USB の認識が rtsynth より遅れるのが普通です）。
抜いた機器が押さえていた音は自動で消音されます。

## 3. Step 2: I2S DAC — 音の出口を良くする（配線6本）

Pi のヘッドフォン端子は PWM 生成のためノイズが多く、ハードシンセには不向きです。
PCM5102A モジュールを I2S 接続すると明確に音が良くなります。

### 配線（GY-PCM5102 系モジュール）

| モジュール側 | Pi 物理ピン | Pi GPIO | 備考 |
|---|---|---|---|
| VIN (VCC) | 2 | 5V | モジュール内で 3.3V に降圧される |
| GND | 6 | GND | |
| BCK | 12 | GPIO18 | ビットクロック |
| LCK (LRCK/WS) | 35 | GPIO19 | LR クロック |
| DIN | 40 | GPIO21 | オーディオデータ |
| SCK | — | — | **GND に接続**（モジュール上の SCK ピンを GND へ。内部 PLL を使うため） |

裏面にはんだジャンパがあるモジュールの場合（GY-PCM5102 の定番設定）:
`1(FLT)=L, 2(DEMP)=L, 3(XSMT)=H, 4(FMT)=L`。既にそうなっている個体が多いです。

### 設定

`/boot/firmware/config.txt`（古い OS では `/boot/config.txt`）に追記して再起動:

```
dtparam=audio=off
dtoverlay=hifiberry-dac
```

### 確認

```sh
aplay -l                      # "sndrpihifiberry" のカードが出れば認識OK
./build/rtsynth --list        # rtsynth からも見えているか
```

**ここから先（音を実際に出すまで）はソフト側の設定が 3 手順あります。**
「カードは見えるのに `Device or resource busy`」「`-d` を付けても開けない」は
全部そこが原因なので、[README.md 1.2 (2) オーディオ出力](README.md#2-オーディオ出力i2s-dac)
を上から順に通してください。要点だけ先に書くと:

- Pi OS デスクトップ版では **PipeWire が DAC を掴んだまま離さない**ので、
  DAC だけ無視させるか PipeWire を止める必要がある
- **カード番号は起動ごとに入れ替わる**（USB MIDI 鍵盤もカードを 1 枚作るため）。
  `-d <番号>` は原理的に安定しないので、`/etc/asound.conf` で
  `default` を DAC に固定し、**`-d` を付けない運用**にするのが正解

## 4. Step 3: MCP3008 + ポット — 絶対値ツマミ（配線8本+ポット3本ずつ）

Pi 自体にはアナログ入力がないため、SPI 接続の ADC「MCP3008」を挟みます。
ツマミの位置がそのままパラメータ値になる**絶対値方式**です。

### MCP3008 の向き

DIP チップの端の**半月の切り欠きを上**に見て、左上が 1 番ピン、そこから左列を下へ 1→8、
右列を下から上へ 9→16 と数えます。

```
        ┌──∪──┐
  CH0 ─┤1   16├─ VDD  → 3.3V
  CH1 ─┤2   15├─ VREF → 3.3V
  CH2 ─┤3   14├─ AGND → GND
  CH3 ─┤4   13├─ CLK  → GPIO11
  CH4 ─┤5   12├─ DOUT → GPIO9
  CH5 ─┤6   11├─ DIN  → GPIO10
  CH6 ─┤7   10├─ CS   → GPIO8
  CH7 ─┤8    9├─ DGND → GND
        └─────┘
```

### 配線表

| MCP3008 ピン | 行き先 | Pi 物理ピン |
|---|---|---|
| 16 (VDD) | 3.3V | 1 |
| 15 (VREF) | 3.3V | 1（VDD と同じレールでOK） |
| 14 (AGND) | GND | 6 |
| 13 (CLK) | GPIO11 (SCLK) | 23 |
| 12 (DOUT) | GPIO9 (MISO) | 21 |
| 11 (DIN) | GPIO10 (MOSI) | 19 |
| 10 (CS) | GPIO8 (CE0) | 24 |
| 9 (DGND) | GND | 6（AGND と同じレールでOK） |

ポット（各ツマミ）は3端子:

| ポット端子 | 行き先 |
|---|---|
| 端 A | 3.3V |
| 端 B | GND |
| 中央（ワイパ） | MCP3008 の CH0〜CH7 のどれか |

### 設定と確認

```sh
sudo raspi-config     # Interface Options → SPI → Enable → 再起動
ls /dev/spidev*       # /dev/spidev0.0 が見えればOK

./build/rtsynth -v --adc 0=gain
# CH0 のツマミを回す → コンソールに [param] gain = 0.xxx が流れ、音量が変わる
```

チャンネルとパラメータの対応は自由です（`--adc 2=attack --adc 3=release` など。
不明な ID を渡すと使えるパラメータの全一覧が表示されます）。

## 5. Step 4: ロータリーエンコーダ — 相対値ツマミ（配線3本）

EC11 系のエンコーダは「回した量」を伝える**相対値方式**です。ADC 不要で GPIO に直結でき、
MIDI CC と同じパラメータを割り当てても値が飛ばない利点があります。

### 配線

エンコーダの3本足側（2本足側はプッシュスイッチなので今回は未使用）:

| エンコーダ端子 | 行き先 | Pi 物理ピン |
|---|---|---|
| A（端） | GPIO17 | 11 |
| C（**中央**） | GND | 9 |
| B（端） | GPIO27 | 13 |

プルアップ抵抗は Pi 内蔵のものをソフトが自動で有効にするので**外付け不要**です。
2個目以降は空いている GPIO を自由に使えます（例: GPIO22=物理15, GPIO23=物理16）。

### 確認

```sh
./build/rtsynth -v --enc 17,27=line1_dcw_level1
# 回す → [param] line1_dcw_level1 = ... がクリック単位で増減
```

- 回転方向が逆に感じたら `--enc 27,17=...` のように A/B を入れ替えるだけ
- 1クリックの変化量は `--enc-step 0.02` などで調整（既定 0.01 = 100 クリックで端から端）

## 6. Step 5: 16x2 LCD — パラメータ表示（配線4本）

「最後に動かしたパラメータ名と現在値」を表示します。書き手が MIDI CC でもツマミでも
エンコーダでも拾います。

### 部品の注意

「1602 LCD **I2C バックパック付き**（PCF8574）」を買ってください。バックパックなし（16ピン
剥き出し）だと配線が 12 本以上になります。バックパック付きなら 4 本です。

**「I2C 付き」でも 2 方式あり、rtsynth が対応するのは PCF8574 方式だけです。**

| 方式 | 見分け方 | rtsynth |
|---|---|---|
| **PCF8574 バックパック**（定番・大多数） | 基板裏に 16 ピンの小基板が載っている。`i2cdetect` で **0x27 / 0x3F** | ✅ そのまま動く |
| ネイティブ I2C（AiP31068 等） | 「IIC 一体型」で小基板が無い。`i2cdetect` で **0x3E** | ❌ コマンド体系が別物 |

商品ページで `PCF8574` / `0x27` / `0x3F` のどれかが書いてあれば PCF8574 方式です。
動作確認が取れている例: **Freenove FNK0079**（1602 I2C モジュール 2 個パック）—
PCF8574T なら 0x27 / PCF8574AT なら 0x3F、5V 駆動、4P ケーブル同梱ではんだ付け不要
（[メーカー資料](https://github.com/Freenove/Freenove_LCD_Module)。ピン割当も
`RS=P0, RW=P1, EN=P2, バックライト=P3, D4-D7=P4-P7` で `I2cLcd1602` と一致）。

### 配線

**基板の並び順ではなく、端子に印刷された文字を見て 1 本ずつ挿すこと。** モジュールに
よって並びが違います（`VCC GND SDA SCL` 順のものと `GND VDD SDA SCL` 順のものがある）。
`VCC` と `VDD` は同じ意味（電源プラス）です。

| バックパック端子 | 行き先 | Pi 物理ピン | Pi GPIO |
|---|---|---|---|
| VCC（= VDD） | 5V | **4** | — |
| GND | GND | **9** | — |
| SDA | SDA1 | **3** | GPIO2 |
| SCL | SCL1 | **5** | GPIO3 |

はんだ付けは不要です。両端メス（F/F）の 4P ジャンパで、Pi のオスピンと
モジュールのオスピンを直結します。並び順が違うので**ケーブルはまっすぐ隣に渡らず、
交差します**。それが正常です。

（LCD は 5V 動作ですが、I2C はオープンドレインのため Pi 側 3.3V と混在しても安全です）

**Step 2 の I2S DAC とはピンが一切重なりません**（DAC = GPIO18/19/21 の I2S、
LCD = GPIO2/3 の I2C）。5V ピンは物理 2 と 4 の 2 本しかないので、
**DAC の VIN を物理 2、LCD の VCC/VDD を物理 4** に分けます。GND は 8 本あるので
好きなところで構いません（DAC=物理6, LCD=物理9 など）。

> **通電前に必ず指差し確認。** Pi ヘッダの端は
> `1=3.3V, 2=5V, 3=SDA, 4=5V, 5=SCL, 6=GND` と、**5V と I2C が交互に並んでいます**。
> VCC を 1 つ隣にずらして物理 3 か 5 に挿すと、3.3V 耐圧の GPIO に 5V が入って
> **Pi 側が壊れます**。配線は電源を切った状態で行い、挿し終えたら
> 「VCC は 4 番、その両隣が SDA(3) と SCL(5)」を目視してから電源を入れてください。

### 設定と確認

```sh
sudo raspi-config          # Interface Options → I2C → Enable → 再起動
sudo apt install i2c-tools
i2cdetect -y 1             # 表に 27 か 3f が出る。それが LCD のアドレス

./build/rtsynth --lcd 0x27 -v     # 出たアドレスを指定
```

起動すると LCD に楽器名（`rtsynth.sine` / `pd`）と、プリセットを持つ楽器なら
そのプリセット名が出ます。ツマミ・CC・エンコーダのどれでパラメータを動かしても、
**上段にパラメータの表示名、下段にその値**が出ます。

```
+----------------+
|L1 DCW Rate 1   |
|0.787           |
+----------------+
```

値は楽器が解釈した意味（波形名・ステップ・セント数など）で出ます。
表示の文言は `lcd/pd_lcd.conf` を編集し、`--lcd-layout lcd/pd_lcd.conf` を付けて起動すると
変えられます（例: EG のレートを 0〜99 で表示）。
Program Change でプリセットを切り替えたときは `PRESET 6` / `Mono Bass` の 2 行になります。

`-v param` を併用すると、同じ内容がコンソールにも流れるので突き合わせて確認できます
（表示が出ない原因が「LCD 側」か「そもそも値が動いていない」かの切り分けに使えます）。

- **何も表示されない/黒い四角だけ**: バックパック裏の青いポテンショ（コントラスト）を
  マイナスドライバで回して調整
- **i2cdetect に何も出ない**: SDA/SCL の入れ替わり・I2C 未有効化を疑う
- **2 個パックを 2 枚同時に挿したい**: 同じアドレスなので衝突します。バックパックの
  A0/A1/A2 のはんだジャンパを片方だけ short してアドレスをずらす必要があります
  （rtsynth 側は現状 1 枚のみ対応）

## 7. Step 6: パネルボタン — プリセット切替と電源断（配線3〜4本）

タクトスイッチを GPIO に直結します。**片足を GPIO、もう片足を GND** へ。
内部プルアップを使うので**外付け抵抗もコンデンサも不要**、チャタリング除去は
カーネルがやります。4 本足のタクトスイッチは向かい合う 2 本が内部で繋がっているので、
**対角の 2 本**を使えば確実です。

### ピン割当（他機能と衝突しないもの）

物理ピン 29/31/33 は**同じ列に並んでいて、間と隣が GND** なので、3 個のボタンで
GND を共用でき配線が短く済みます。

| 用途 | GPIO | 物理ピン | 近い GND |
|---|---|---|---|
| プリセット ← | GPIO5 | 29 | 30 |
| プリセット → | GPIO6 | 31 | 30 |
| revert（編集を戻す） | GPIO13 | 33 | 34 |
| 電源断（shutdown） | GPIO26 | 37 | 39 |

いずれも SPI(7–11) / I2C(2,3) / I2S(18–21) / UART(14,15) と重なりません。

### 配線後の確認と起動

```sh
./build/rtsynth --synth pd --lcd 0x27 \
  --button 5=preset-prev --button 6=preset-next --button 13=revert
```

`preset-next` / `preset-prev` は端まで行くと反対の端へ回り込みます。押すと LCD の表示が
切り替わります。`revert` は今のプリセットへの編集を全部捨てて**起動時の音**に戻します
（LCD にはプリセット名が再表示されます）。`panic`（全音即時停止・CC120 相当）も
同じように割り当てられます。

### ⚠ shutdown ボタンのピンは GPIO3 から移すこと

`dtoverlay=gpio-shutdown` の**既定値は GPIO3 ですが、GPIO3 は I2C の SCL** です。
Step 5 の LCD と同じ線を共有することになり、I2C 通信中の意図しないシャットダウンや
LCD の不安定化を招きます。別ピンへ移してください。

```
# /boot/firmware/config.txt
dtoverlay=gpio-shutdown,gpio_pin=26,active_low=1,gpio_pull=up
```

- トレードオフ: **halt 状態から電源ボタンで復帰できるのは GPIO3 だけ**です。
  その機能が要るなら GPIO3 のまま使い、代わりに LCD を別の I2C バス
  （`dtoverlay=i2c6,pins_22_23` 等）へ逃がしてください
- このオーバーレイは「押した瞬間」にシャットダウンします。**長押しで確定**にしたい場合は
  systemd 側で切り替えます（systemd 248 以降、長押し判定は 5 秒）:

```
# /etc/systemd/logind.conf.d/powerkey.conf
[Login]
HandlePowerKey=ignore
HandlePowerKeyLongPress=poweroff
```

### 押すと光る LED（任意）

**LED をスイッチと GPIO の間に直列に入れてはいけません。** GPIO が LED の順電圧
（1.8〜3.2V）までしか下がらず、押しても LOW と判定されません。GPIO とは**別の枝**に
ぶら下げます。

```
                    ┌── GPIO5 (物理29)
  3V3 ─[R]─▶|(LED)──┤
  (物理1)           └── SW1 ── GND (物理30)
```

LED は**足の長い方（アノード）を 3V3 側、短い方（カソード）を GPIO 側**に向けます。
R は LED のどちら側に入れても構いません。preset − / + の 2 個に付けるなら、これを
GPIO5 と GPIO6 に 1 組ずつ、3V3 は共用で作ります（**GPIO は 1 本も増えません**）。

> ⚠ **この枝の電源は必ず 3V3（物理 1 または 17）から取ってください。5V は不可です。**
> スイッチを離している間、この枝は GPIO ピンに直結された状態になります。3V3 なら
> ピンと同電位なので電流は流れませんが、5V だと GPIO の絶対最大定格 3.3V を超えて
> 保護ダイオードに電流が流れ込み、ピンを壊す可能性があります。

- スイッチを離している間: LED 側に電流の帰り道が無いので**消灯**（弱いプルアップとして
  働くだけで、GPIO は HIGH のまま）
- スイッチを押した瞬間: ノードが GND に落ちて**点灯**。電流は GPIO ではなくスイッチを
  通って GND に流れるので、GPIO の電流制限を気にする必要はありません
- R は 330Ω〜1kΩ（3.3V・Vf 2.0V で 4mA〜1.3mA）。暗ければ R を小さく

「押している間だけ光る」で良ければこれで十分です。**現在のプリセット番号を光らせる**
ような使い方をしたい場合は、LED を GPIO 出力に直結して（R 直列）ソフトから制御する
形になります。その場合は LED 1 個につき GPIO を 1 本消費します。

### 全部つないだときのピン一覧

組み上げるときはこの表だけ見れば足ります（Step 3 の MCP3008 と Step 4 の
エンコーダは使う場合のみ）。

| 機能 | 行き先 | Pi 物理ピン | Pi GPIO |
|---|---|---|---|
| **DAC** VIN | 5V | 2 | — |
| **DAC** GND | GND | 6 | — |
| **DAC** BCK | I2S BCLK | 12 | GPIO18 |
| **DAC** LCK | I2S LRCLK | 35 | GPIO19 |
| **DAC** DIN | I2S DOUT | 40 | GPIO21 |
| **DAC** SCK | **GND へ** | 6 など | — |
| **LCD** VCC | 5V | 4 | — |
| **LCD** GND | GND | 9 | — |
| **LCD** SDA | SDA1 | 3 | GPIO2 |
| **LCD** SCL | SCL1 | 5 | GPIO3 |
| **SW1** preset − | 入力 | **29** | **GPIO5** |
| **SW2** preset + | 入力 | **31** | **GPIO6** |
| **SW3** revert | 入力 | **33** | **GPIO13** |
| SW1/SW2 の GND | GND | **30** | — |
| SW3 の GND | GND | **34** | — |
| **shutdown SW** | 入力 | 37 | GPIO26 |
| shutdown SW の GND | GND | 39 | — |
| LED を付ける場合の 3V3 | **3V3**（5V 不可） | 1 または 17 | — |
| **MCP23017**（Step 7）VDD / RESET | **3V3**（5V 不可） | 1 または 17 | — |
| **MCP23017** SDA / SCL | LCD と共用 | 3 / 5 | GPIO2 / GPIO3 |
| **MCP23017** INTA（チップ 0 / 1 / 2 / 3・4 共通） | 入力 | 7 / 32 / 36 / 18 | GPIO4 / 12 / 16 / 24 |
| **74HC595** VCC / /SRCLR | **3V3**（5V 不可） | 1 または 17 | — |
| **74HC595** SER（チップ 0）/ SRCLK / RCLK | SPI MOSI / SCLK / CE1 | 19 / 23 / 26 | GPIO10 / 11 / 7 |

物理 29・30・31 が連続しているので、SW1 と SW2 は**真ん中の 30 番 GND を共用**できます。
SW3 は 33 番と、隣の 34 番 GND です。

**Pi に配線しないが必要な作業**（表に出てこないので忘れやすい箇所）:

- **DAC の SCK をモジュール上で GND に落とす。** Pi 側のピンには繋ぎません。
  内部 PLL を使わせるための設定で、**これを浮かせたままだと音が出ません**
- **DAC のはんだジャンパ**（GY-PCM5102 系）: `1(FLT)=L, 2(DEMP)=L, 3(XSMT)=H, 4(FMT)=L`。
  出荷時にこの設定の個体が多いので、まず現物を確認するだけで足りることが多いです
- **DAC の音声出力**: モジュール上の 3.5mm ジャック（付いていない個体は L / R / GND
  ピン）から、アンプやアクティブスピーカーへ。Pi 側とは無関係の配線です
- **LCD のコントラスト**: バックパックの半固定抵抗を回して文字を出します。
  **バックライトは点いているのに何も見えない**ときは、ほぼこれです
- GND の物理ピン（6 / 9 / 14 / 20 / 25 / 30 / 34 / 39）は**全て Pi 内部で共通**なので、
  どれを使っても構いません。近いピンを選んでください
- MCP3008（Step 3）とロータリーエンコーダ（Step 4）を使う場合の配線は、この表には
  含めていません。それぞれの Step の表を参照してください

## 8. Step 7: フロントパネル（MCP23017 ＋ 74HC595）

CZ-101 風のフロントパネルです。操作の一覧は
[README の「フロントパネル」](README.md#フロントパネルmcp23017--74hc595)、ピン割当は
`panels/pd_panel.conf`（**編集すれば自由に変えられます**。再ビルド不要）。

| 部 | スイッチ | エンコーダ | LED |
|---|---|---|---|
| 音色（preset/user・tone 1〜8・page・compare・initialize） | 13 | — | 12 |
| line ごと（DCx・波形 1/2（同時押しで key follow）・rate/level・sus・end）× 2 | 6 × 2 | 8 × 2 | 32 × 2 |
| line select | 1 | — | 4 |
| detune / vibrato | 3 | 3 | 7 |
| **合計** | **29** | **19** | **87** |

入力（スイッチ 29 ＋ エンコーダ 19 × 2 本 = **67 本**）は **MCP23017 × 5**、
LED 87 個は **74HC595 × 10**（80 個）と MCP23017 の出力専用ピン 7 本に載せます。Pi 側の配線は I2C 2 本・SPI 3 本・INT 4 本・電源だけです。

### 部品

| 部品 | 数 | 備考 |
|---|---|---|
| MCP23017-E/SP（DIP-28） | 5 | 入力用。**3.3V で使う**。入力に使えるのは 1 個 14 本（計 70 本、3 本余り）、出力専用ピンは LED に |
| 74HC595（U74HC595AG 等） | 10 | LED 用。**3.3V で使う**（計 80 出力、余りなし） |
| ロータリーエンコーダ（ALPS 無段階など）＋ DIP 化基板（RECNV-3 等） | 19 | A/B/C の 3 本を使う |
| タクトスイッチ | 29 | |
| LED ＋ 抵抗 1kΩ 前後 | 87 | |
| 0.1µF セラミックコンデンサ | 15 | 各 IC の VCC–GND 間、足の近くに |

LED は 87 個ありますが同時に点くのは多くても 23 個（ほとんどが「選択中の 1 個だけ点灯」の表示のため）で、
1kΩ（約 1.3mA）なら合計 30mA 程度です。595 1 個あたりは最大 8 個 ≒ 10mA で、どちらも余裕があります。

### MCP23017 の配線（入力 × 5）

| MCP23017 ピン | 接続先 |
|---|---|
| 9 VDD | **3V3**（Pi 物理 1 / 17） |
| 10 VSS | GND |
| 12 SCL | Pi 物理 5（GPIO3）— LCD と共用 |
| 13 SDA | Pi 物理 3（GPIO2）— LCD と共用 |
| 18 RESET | **3V3 へ直結**（浮かせると勝手にリセットされる） |
| 15/16/17 A0/A1/A2 | アドレス = 0x20 + (A2 A1 A0 の 2 進)。チップ 0〜4 = 0x20〜0x24（例: 0x23 = A0・A1 を 3V3、A2 を GND） |
| 20 INTA | 下表の Pi GPIO へ |
| 21〜28 GPA0〜7 / 1〜8 GPB0〜7 | エンコーダ・スイッチ（`pd_panel.conf` の割当） |

| チップ | アドレス | 載っているもの | INTA → Pi |
|---|---|---|---|
| 0 | 0x20 | line 1 のエンコーダ 1〜7 | GPIO4（物理 7） |
| 1 | 0x21 | line 2 のエンコーダ 1〜7 | GPIO12（物理 32） |
| 2 | 0x22 | 両 line のエンコーダ 8、detune/vib エンコーダ 3 個、line 1 の field/sus/end、line select | GPIO16（物理 36） |
| 3 | 0x23 | 両 line の DCx/波形 1/波形 2、line 2 の field/sus/end、vibrato のスイッチ 3 個 | GPIO24（物理 18）※ |
| 4 | 0x24 | preset/user・tone 1〜8・page・compare・initialize | GPIO24 ※ |

※ チップ 3・4 はスイッチだけなので **INTA を 2 本まとめて GPIO24 に**繋ぎます（オープンドレインに
設定されるので直結で安全）。エンコーダの載ったチップ 0〜2 は**それぞれ専用の INT 線**にしてあり、
ツマミを回したときは**そのチップだけ**を読みます（I2C 既定の 100 kHz で 1 回約 0.5 ms）。
全部を 1 本にまとめると 5 チップ全部を毎回読む（約 2.3 ms）ことになり、速く回したときに
取りこぼしやすくなります。

> ⚠ **GPA7 / GPB7 は入力に使えません。** 現行の MCP23017 はこの 2 本の入力機能が
> 削除されています（入力にすると I2C 通信を壊すことがある）。設定ファイルでここに
> エンコーダやスイッチを割り当てると起動時にエラーになります。このためエンコーダは
> チップ 1 個に 7 個までです。

> ⚠ **アドレス 0x27 は使わない。** LCD バックパック（PCF8574）が 0x27 にいます。
> 0x25・0x26 は空いているので、将来あと 2 個足せます（それ以上は 2 本目の I2C バスが必要）。

- **エンコーダ**: A, B をエキスパンダのピンへ、C を GND へ。MCP23017 の内部プルアップ
  （約 100kΩ）を使うので外付け抵抗は不要です。DIP 化基板にプルアップ抵抗や RC フィルタが
  載っている場合はそのまま使えます（**プルアップの電源は 3V3 に**。5V に繋がない）。
  RECNV-3 の実装内容は手元の基板の回路で確認してください
- **スイッチ**: 片足をエキスパンダのピン、もう片足を GND。チャタリングはソフトで除去（5 ms）
- エンコーダが逆回転したら、設定ファイルでその行の A と B を入れ替えます

### 74HC595 の配線（LED × 10 チップ数珠つなぎ）

| 74HC595 ピン | 接続先 |
|---|---|
| 16 VCC | **3V3**（5V にすると Pi の 3.3V 信号を確実に H と読めない） |
| 8 GND | GND |
| 14 SER | チップ 0 は **Pi 物理 19（GPIO10 / MOSI）**、チップ n は チップ n−1 の **9 番 QH'** |
| 11 SRCLK | 全チップ共通で **Pi 物理 23（GPIO11 / SCLK）** |
| 12 RCLK | 全チップ共通で **Pi 物理 26（GPIO7 / CE1）** |
| 13 /OE | GND |
| 10 /SRCLR | 3V3 |
| 15, 1〜7 QA〜QH | `Q → 抵抗 → LED（アノード→カソード） → GND` |

| 595 | LED |
|---|---|
| sr0 / sr1 | line 1 / line 2 の sus・end ステップ 1〜8（QA〜QH = ステップ 1〜8、ツマミの下に並べる） |
| sr2 | line 1 の DCO/DCW/DCA・RATE/LEVEL、line 2 の DCO/DCW/DCA |
| sr3 | line 2 の RATE/LEVEL、line select 1 / 2 / 1' / 2'、vib on、detune |
| sr4 / sr5 | line 1 の波形 1 / 波形 2（QA〜QH = 波形 1〜8） |
| sr6 / sr7 | line 2 の波形 1 / 波形 2 |
| sr8 | vib、vib 波形 4 個、preset、user、page |
| sr9 | tone 1〜8 |
| MCP 0 の GPA7 / GPB7 | line 1 の波形 1 / 波形 2 の 9 番（key follow 9 用） |
| MCP 1 の GPA7 / GPB7 | line 2 の波形 1 / 波形 2 の 9 番 |
| MCP 2 の GPA7 / GPB7 | line 1 / line 2 の KF（key follow モード表示） |
| MCP 4 の GPA7 | compare（compare スイッチのすぐ隣） |

- SPI の CE1 をラッチ（RCLK）に使います。転送が終わって CE1 が上がった瞬間に全チップの
  出力が一斉に切り替わるので、シフト途中の状態で LED がちらつくことはありません
- CE0（GPIO8）は MCP3008 用に空いたままです（SPI バスは共用できます）
- 設定ファイルのピン名は `sr<チップ>.<ビット>`（ビット 0 = QA … 7 = QH、チップ 0 = MOSI に
  繋いだもの）。LED を逆向き（3V3 → 抵抗 → LED → Q）にするなら `active-low` を付けます
- **波形の LED** は値ごとに 1 個（9 個）で、今の値の 1 個だけが点きます。通常は波形番号
  （1〜8、9 番は消灯、波形 2 の Off は全消灯）、key follow モード中（KF の LED が点灯）は
  DCW / DCA の key follow（1〜9、0 は全消灯）
- 9 番目の LED と KF の LED は MCP23017 の出力専用ピンに載せています（595 は 10 個で満杯のため）。
  出力専用ピンはあと 3 本（3.A7、3.B7、4.B7）空いています

### 少しずつ組む（段階的な立ち上げ）

**設定ファイルは最終形（`panels/pd_panel.conf`）のまま、部品を足すたびに rtsynth を
起動し直すだけ**で進められます。まだ繋いでいない部分は次のように扱われます。

- 応答しない MCP23017 は起動時に 1 行警告が出るだけで、そのチップのスイッチ・ツマミが
  反応しないだけです（他は動きます）
  ```
  [panel] MCP23017 expander 3 (0x23) does not answer on /dev/i2c-1 — its controls are inactive. ...
  Panel: 2 of 5 MCP23017 on /dev/i2c-1, ...
  ```
- まだ付けていないスイッチ・ツマミ・LED は、単に何も起きない / 点かないだけです
- 74HC595 は**チェーンの先頭（sr0 = MOSI 側）から**つないでいれば、途中までで動きます
  （後ろのチップ宛てのデータは最後のチップの QH' から抜けていくだけ）

**最初に一度だけ**やること:

```sh
sudo raspi-config                        # Interface Options → I2C と SPI を有効化
sudo usermod -aG i2c,spi,gpio $USER      # 再ログインで有効
git pull && cmake --build build -j4      # 最新をビルド
```

**毎回の確認コマンド**（部品を足したら Ctrl+C → 再実行）:

```sh
./build/rtsynth --synth pd --lcd 0x27 --panel panels/pd_panel.conf -v panel,param
#   [panel] encoder row1.3 (0.A4/0.A5) +1      ← どのツマミがどのピンで動いたか
#   [param] L1 DCO Rate 3 = 0.510     ← それで何が変わったか
#   [panel] button row1.eg (3.A0) down
./build/rtsynth --synth pd --panel panels/pd_panel.conf --panel-led-test
#   [panel] LED row1.step1 (sr0.0)              ← 今光っている LED の役割とピン（0.4 秒ずつ順番に）
```

**配線は必ず電源を切ってから**行い、終わったら起動し直してください（I2C の活線挿抜はできません）。

おすすめの順番（各段階で上の 2 コマンドで確認）:

| 段階 | つなぐもの | 確認 |
|---|---|---|
| 1 | MCP23017 チップ 0（0x20）の電源・I2C・アドレス・RESET・**INTA → GPIO4** | `i2cdetect -y 1` に 20。起動時の警告からチップ 0 が消える |
| 2 | line 1 のエンコーダ 1 個（row1.1 = 0.A0/0.A1） | 回すと `[panel] encoder row1.1 ...`。逆向きなら設定の A/B を入れ替え。反応が鈍いときは INTA の配線（下記） |
| 3 | 残りのエンコーダ row1.2〜7 | 同上 |
| 4 | 74HC595 を 10 個まとめて数珠つなぎ（電源・SER/QH'・SRCLK・RCLK・/OE・/SRCLR） | `ls /dev/spidev0.1`。LED はまだ無くてよい |
| 5 | line 1 の sus/end LED 8 個（sr0） | `--panel-led-test` で 1〜8 の順に光る |
| 6 | チップ 2（0x22, INTA → GPIO16）: row1.8・row1 の field/sus/end | sus / end を押すと sus/end LED が動く |
| 7 | チップ 3（0x23, INTA → GPIO24）: row1 の eg / wave1 / wave2 ＋ その LED（sr2, sr4, sr5, 0.A7/0.B7, 2.A7） | eg で DCx LED、wave で波形 LED、wave1+wave2 同時押しで KF LED |
| 8 | line 2 側（チップ 1 = 0x21・INTA → GPIO12、残りのスイッチと LED） | line 1 と同じ |
| 9 | 音色部（チップ 4 = 0x24・INTA → GPIO24、sr8・sr9・4.A7） | tone を押すと LCD に `PRESET n` |
| 10 | detune / vib、line select | — |

INTA は**チップと同時に**繋いでください。設定ファイルでは各チップに INT 線を割り当ててあるので、
INTA が未配線のチップは 20 ms ごとの補助読み取りでしか読まれず、ツマミの反応が鈍くなります
（その場合は起動中に `its pins change but GPIOn never signals it — is its INTA wired?` と出ます）。

### 設定と確認

```sh
sudo raspi-config        # Interface Options → I2C と SPI を有効化
i2cdetect -y 1           # 20〜24 (+ 27 = LCD) が見えること
ls /dev/spidev0.1        # SPI が有効なら存在する

./build/rtsynth --synth pd --lcd 0x27 --panel panels/pd_panel.conf -v param
#   [panel] vibrato: the instrument has no parameter 'vibrato' ...   ← pd に vibrato が入るまで
#   Panel: 5 of 5 MCP23017 on /dev/i2c-1, 19 encoders, 29 buttons, 87 LEDs
#          (10 x 74HC595 on /dev/spidev0.1), per-chip interrupts
# line 1 のツマミ 3 を回す → [param] L1 DCO Rate 3 = ...
```

- 1 ステップの細かさは `encoder-quarters`（無段階 24 パルスなら 4 = 1 周 24 ステップ、
  2 = 48 ステップ）と `encoder-step`（1 ステップでの変化量、0.01 = 100 ステップで全域）で調整。
  detune など離散値のパラメータは常に 1 クリック = 1 段です
- 速く回して取りこぼすようなら I2C を 400 kHz に上げます（`/boot/firmware/config.txt` に
  `dtparam=i2c_arm_baudrate=400000`）。ただし LCD の PCF8574 は 100 kHz 品なので、
  LCD の表示が乱れないか確認してください
- `[warning] I2C errors on the expander panel` が出る場合は配線（特に SDA/SCL・GND・RESET）
  を確認してください
- LED が 1 つもつかない / 全部ずれる: SPI 有効化、RCLK が CE1（物理 26）か、
  チップ間の QH' → SER の順番を確認

## 9. Step 8: 仕上げ — 権限・安定化・自動起動

### 権限（Permission denied が出たら）

```sh
sudo usermod -aG audio,spi,i2c,gpio $USER   # 追加後、再ログイン
```

### リアルタイム優先度と CPU

```sh
# /etc/security/limits.d/audio.conf を作成（再ログインで有効）
@audio - rtprio 95
@audio - memlock unlimited

# CPU クロックを固定（音切れ対策。cpufrequtils を apt install）
sudo cpufreq-set -g performance
```

### 電源投入で自動起動（systemd）

```ini
# /etc/systemd/system/rtsynth.service
[Unit]
Description=rtsynth
After=sound.target

[Service]
ExecStart=/home/pi/rtsynth/build/rtsynth --synth pd \
  --adc 0=volume --adc 1=line1_dcw_level1 \
  --enc 17,27=line1_dca_rate1 \
  --button 5=preset-prev --button 6=preset-next --button 13=revert \
  --lcd 0x27
Restart=on-failure
User=pi
SupplementaryGroups=audio gpio i2c spi
LimitRTPRIO=95

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl enable --now rtsynth
journalctl -u rtsynth -f      # ログ確認
```

## 10. トラブルシューティング早見表

| 症状 | まず疑うこと |
|---|---|
| 音が出ない | DAC の SCK→GND 忘れ → `aplay -l` にカードが出るか → 出るのに開けないなら PipeWire が掴んでいる（[README 1.2 (2)](README.md#2-オーディオ出力i2s-dac)） |
| 起動時に `snd_pcm_open error ... 524` | **無害**（音声シンクのない HDMI をスキップした警告）。`-v` 時のみ表示 |
| 音がプチプチ切れる（xrun 警告） | `-b 512` に増やす。performance ガバナ。rtprio 設定 |
| ツマミの値が暴れる | GND 共通化・VREF(15番)→3.3V の接続を確認 |
| ツマミが効かない | `ls /dev/spidev*` で SPI 有効か。CS を CE0(物理24) 以外に挿してないか |
| エンコーダが逆回転 | `--enc` の A,B を入れ替える |
| エンコーダが飛ぶ/戻る | C（中央端子）が GND に確実に刺さっているか |
| `--panel` で `does not answer` | `i2cdetect -y 1` にそのアドレスが出るか。A0〜A2 の配線、RESET(18番) が 3V3 か、VDD が 3V3 か |
| `--panel` で `output-only` エラー | GPA7/GPB7 に入力を割り当てている。別のピンに移す（Step 7 の注意書き） |
| パネルの LED が点かない | SPI 有効化（`ls /dev/spidev0.1`）、595 の VCC が 3V3 か、RCLK が CE1（物理 26）か |
| パネルのエンコーダが逆回転 | 設定ファイルのその行の A と B を入れ替える |
| パネルのボタンで何も起きない | `--synth pd` で起動しているか（EG ロールは pd 専用）。`-v param` で値が出るか |
| LCD に何も出ない | コントラスト調整 → `i2cdetect -y 1` でアドレス確認 → `--lcd` に合わせる |
| LCD の 1 行目だけ黒い四角が並ぶ | コントラストが強すぎるだけ。ポテンショを逆に回す |
| 起動画面は出るが値が更新されない | 表示ではなく入力側の問題。`-v param` を併用し、コンソールにも出ないなら CC/ツマミ側を疑う |
| `Failed to open I2C bus` | I2C 未有効化（`raspi-config`）か、`i2c` グループ未加入（下記の `usermod`） |
| `Permission denied` 系 | 上記グループ追加、再ログイン |
| ボタンが効かない | `--button` に渡すのは **GPIO 番号**（物理ピン番号ではない）。`gpio` グループに入っているか。起動時に `Buttons: N on /dev/gpiochip0` が出ているか |
| ボタンを 1 回押すとプリセットが 2 つ進む | チャタリング。起動時に `(no kernel debounce)` と出ていないか確認する |
| MIDI が来ない | `--list` のポート一覧に出ているか。`-v` で `[midi]` 行が出るか |
| 和音でノートオフが取りこぼされ音が鳴りっぱなしになる | `--midi-raw hw:X,Y,Z`（ID は `--list` の Raw MIDI 欄）でカーネル直読みに切替。それでも出るなら rtsynth を止め `aseqdump` で鍵盤の送信自体を確認し、鍵盤のファームウェア更新・別ケーブル・セルフパワー USB ハブを試す（詳細手順は README の運用メモ参照） |

## 11. 全部載せの起動例

```sh
./build/rtsynth --synth pd \
  --adc 0=volume --adc 1=line1_dcw_level1 --adc 2=detune_fine \
  --enc 17,27=line1_dca_rate1 --enc 22,23=line1_dca_rate2 \
  --lcd 0x27 \
  --button 5=preset-prev --button 6=preset-next --button 13=revert \
  --panel panels/pd_panel.conf \
  -v
```

（`-d` を付けていないのは、Step 2 の手順で `default` を DAC に固定してあるからです。
固定していない場合は `-d <番号>` が必要ですが、番号は起動ごとに変わります）

動作が確認できたら `-v` を外して systemd に登録すれば、電源を入れるだけで立ち上がる
ハードシンセの完成です。ソフト側の構造・パラメータ一覧・設計の背景は [README.md](README.md) を
参照してください。
