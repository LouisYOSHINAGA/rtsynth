# rtsynth ハードウェア組み立てガイド（初心者向け）

Raspberry Pi と rtsynth でハードシンセを組むための、部品購入から動作確認までの手引きです。
電子工作が初めてでも、上から順に1ステップずつ進めれば完成するように書いてあります。
各ステップは独立しているので、**全部やる必要はありません**（例: まず DAC だけ、でOK）。

```
[USB MIDI 鍵盤]──USB──▶┌─────────────┐
[MIDIコン(CC)]──USB──▶│ Raspberry Pi │──I2S──▶[DAC]──▶ スピーカー/ヘッドフォン
[ツマミ×n]──▶[MCP3008]─SPI─▶│   rtsynth    │
[ロータリーエンコーダ]──GPIO──▶│              │──I2C──▶[16x2 LCD] "line1_dcw_level1"
                        └─────────────┘                            "0.520"
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
./build/rtsynth --list        # 一覧に "sndrpihifiberry" 等のカードが出る
./build/rtsynth -d <その番号>  # 出力先に指定して起動、鍵盤で音出し
```

うまくいったら以降ずっと `-d <番号>` を付けて起動します（systemd 化するときも同様）。

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

### 配線

| バックパック端子 | 行き先 | Pi 物理ピン |
|---|---|---|
| VCC | 5V | 2 |
| GND | GND | 6 |
| SDA | GPIO2 (SDA1) | 3 |
| SCL | GPIO3 (SCL1) | 5 |

（LCD は 5V 動作ですが、I2C はオープンドレインのため Pi 側 3.3V と混在しても安全です）

### 設定と確認

```sh
sudo raspi-config          # Interface Options → I2C → Enable → 再起動
sudo apt install i2c-tools
i2cdetect -y 1             # 表に 27 か 3f が出る。それが LCD のアドレス

./build/rtsynth --lcd 0x27 -v     # 出たアドレスを指定
```

起動すると LCD に `rtsynth.sine` / `ready` と出ます。ツマミや CC を動かすと
上段にパラメータ ID、下段に値が表示されます。

- **何も表示されない/黒い四角だけ**: バックパック裏の青いポテンショ（コントラスト）を
  マイナスドライバで回して調整
- **i2cdetect に何も出ない**: SDA/SCL の入れ替わり・I2C 未有効化を疑う

## 7. Step 6: 仕上げ — 権限・安定化・自動起動

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
ExecStart=/home/pi/rtsynth/build/rtsynth --synth pd -d 0 \
  --adc 0=volume --adc 1=line1_dcw_level1 \
  --enc 17,27=line1_dca_rate1 \
  --lcd 0x27
Restart=on-failure
User=pi
LimitRTPRIO=95

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl enable --now rtsynth
journalctl -u rtsynth -f      # ログ確認
```

## 8. トラブルシューティング早見表

| 症状 | まず疑うこと |
|---|---|
| 音が出ない | `--list` で意図したカードか確認 → `-d` で明示。DAC の SCK→GND 忘れ |
| 起動時に `snd_pcm_open error ... 524` | **無害**（音声シンクのない HDMI をスキップした警告）。`-v` 時のみ表示 |
| 音がプチプチ切れる（xrun 警告） | `-b 512` に増やす。performance ガバナ。rtprio 設定 |
| ツマミの値が暴れる | GND 共通化・VREF(15番)→3.3V の接続を確認 |
| ツマミが効かない | `ls /dev/spidev*` で SPI 有効か。CS を CE0(物理24) 以外に挿してないか |
| エンコーダが逆回転 | `--enc` の A,B を入れ替える |
| エンコーダが飛ぶ/戻る | C（中央端子）が GND に確実に刺さっているか |
| LCD に何も出ない | コントラスト調整 → `i2cdetect -y 1` でアドレス確認 → `--lcd` に合わせる |
| `Permission denied` 系 | 上記グループ追加、再ログイン |
| MIDI が来ない | `--list` のポート一覧に出ているか。`-v` で `[midi]` 行が出るか |
| 和音でノートオフが取りこぼされ音が鳴りっぱなしになる | `--midi-raw hw:X,Y,Z`（ID は `--list` の Raw MIDI 欄）でカーネル直読みに切替。それでも出るなら rtsynth を止め `aseqdump` で鍵盤の送信自体を確認し、鍵盤のファームウェア更新・別ケーブル・セルフパワー USB ハブを試す（詳細手順は README の運用メモ参照） |

## 9. 全部載せの起動例

```sh
./build/rtsynth --synth pd -d 0 \
  --adc 0=volume --adc 1=line1_dcw_level1 --adc 2=detune_fine \
  --enc 17,27=line1_dca_rate1 --enc 22,23=line1_dca_rate2 \
  --lcd 0x27 \
  -v
```

動作が確認できたら `-v` を外して systemd に登録すれば、電源を入れるだけで立ち上がる
ハードシンセの完成です。ソフト側の構造・パラメータ一覧・設計の背景は [README.md](README.md) を
参照してください。
