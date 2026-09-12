# rtsynth — VST ライク構造のスタンドアロンシンセ (RtAudio / RtMidi)

Raspberry Pi 上でのハードシンセ開発の起点となることを想定した、**VST プラグインに近い構造**の
ポリフォニックシンセです。標準の音源は 16 ボイスの sine シンセ、オプションで
[LouisYOSHINAGA/pd](https://github.com/LouisYOSHINAGA/pd)（CZ 系フェーズディストーション）の
DSP コアをそのまま鳴らせます。コードベース全体が「音源・ホスト・DSP 部品を差し替え可能な
テンプレート」として書かれています。

依存: RtAudio (5.x / 6.x 両対応), RtMidi, ALSA, CMake, C++20

## この README の読み方

| 目的 | 読む場所 |
|---|---|
| **鳴らす・使う**（ビルド、Pi のセットアップ、起動オプション、ハードウェア接続） | [第1部 使い方](#第1部-使い方) |
| **中身を触る**（構造、拡張のしかた、デバッグ、既知の問題） | [第2部 開発・デバッグ](#第2部-開発デバッグ) |
| **ハードを組む**（部品の買い方・配線・はんだ不要の手順） | [README_HW.md](README_HW.md) — DAC / ツマミ / エンコーダ / LCD を 1 ステップずつ |
| **鳴らない・おかしい** | [2.7 デバッグ手順](#27-デバッグ手順) — 症状別（DAC が鳴らない / 音切れ / MIDI 取りこぼし） |

第1部だけ読めば演奏できます。第2部は改造・移植・不具合調査のときに読んでください。

新しい Pi にセットアップする場合は、**[1.2 Raspberry Pi のセットアップ](#12-raspberry-pi-のセットアップ)
を上から順に全部**通してください。USB MIDI の取りこぼし対策と DAC の 3 段階手順は、
どちらも「途中で止めると別の症状に化ける」種類の設定です。

---

# 第1部 使い方

## 1.1 ビルド

```sh
sudo apt install cmake g++ pkg-config librtaudio-dev librtmidi-dev libasound2-dev

git clone --recursive https://github.com/LouisYOSHINAGA/rtsynth.git
cd rtsynth
# 既にクローン済みなら: git submodule update --init

cmake -S . -B build
cmake --build build -j4
```

サブモジュール `external/pd` が無くても sine シンセのみでビルドは通ります
（configure 時にその旨のメッセージが出ます）。
RtAudio 5.x (bullseye / bookworm) と 6.x (trixie 以降) のどちらでもビルドできます。

## 1.2 Raspberry Pi のセットアップ

演奏用途では以下を**先に**済ませてください。特に (1) は必須です。

### (1) USB MIDI の取りこぼし対策（Pi 3B 以前では必須）

Pi 3B 以前の USB ホストコントローラ (`dwc_otg`) は、**ハイスピードの内蔵ハブにぶら下がった
フルスピードの USB MIDI 機器**に対してスプリットトランザクションを行いますが、この処理で
**エラーを一切記録せずにデータを落とすこと**があります。症状は
「離鍵が届かず音が鳴り続ける」「押鍵が抜ける」で、和音を速く弾くほど頻発します。

`/boot/firmware/cmdline.txt`（1 行のファイル、末尾に追記／改行を入れない）に:

```
dwc_otg.speed=1
```

を加えて再起動すると、USB バス全体がフルスピード動作になりスプリットトランザクションが
発生しなくなります。**これで取りこぼしは解消します。**

- トレードオフ: バスがフルスピード (12 Mbps) に固定されるため、同じ USB バスに繋いだ
  高速機器（USB メモリ、有線 LAN）は遅くなります。MIDI 機器と DAC しか繋がない
  ハードシンセ用途では実害はありません
- Pi 4 / 5 は USB コントローラが別（xHCI）なので、この設定は不要です
- 機器によって発現しやすさが違います（手元では Arturia KeyLab で頻発、KORG microKEY では
  再現せず）。特定の鍵盤だけで起きても、原因は鍵盤ではなくこの経路です

### (2) オーディオ出力（I2S DAC）

Pi 本体のヘッドフォン端子は PWM 生成で品質が低いため、ハードシンセでは I2S DAC
（PCM5102A 系の安価なモジュール、HiFiBerry DAC+ 等）を推奨します。
**rtsynth のコード変更は不要**ですが、**OS 側の設定が 3 段階あり、途中で止めると
かえって音が出なくなります**。順番どおり最後まで通してください。

> 3 段階を飛ばしたときに何が起きるかは [2.7 の「DAC から音が出ない」](#dac-から音が出ない)
> に症状別でまとめてあります。詰まったらそちらへ。

#### 手順 1 — DAC を認識させる

```
# /boot/firmware/config.txt
dtparam=audio=off            # 内蔵オーディオを無効化
dtoverlay=hifiberry-dac      # PCM5102A 系はこのオーバーレイで動くものが多い
```

再起動して確認します。

```sh
aplay -l
#   カード 2: sndrpihifiberry [snd_rpi_hifiberry_dac], デバイス 0: ...
#     サブデバイス: 1/1        ← ★「1/1」なら空き。「0/1」なら手順 2 が必要
```

**`サブデバイス:` の欄を必ず見てください。** `0/1` は「1 個しかないサブデバイスの
空きが 0」＝**既に他のプロセスが掴んでいる**という意味です。

#### 手順 2 — DAC を rtsynth 専用にする（これを飛ばすと `Device or resource busy`）

Raspberry Pi OS デスクトップ版では **PipeWire が起動時に全てのサウンドカードを開き、
再生していなくても離しません**（sink のプロパティが `node.pause-on-idle = "false"`
になっているため）。この状態では rtsynth も `speaker-test` も
`-16 デバイスもしくはリソースがビジー` で弾かれます。

用途に応じてどちらかを選びます。**両方を中途半端に混ぜないこと。**

| | A: デスクトップも使う | B: シンセ専用機にする |
|---|---|---|
| 方針 | PipeWire は残し、**DAC のカードだけ無視させる** | PipeWire を止め、ALSA を rtsynth が直接使う |
| デスクトップの音 | HDMI から出る（そのまま使える） | 無し |
| 手数 | 設定ファイル 1 枚 | 停止 3 つ + 手順 3 が**必須** |

**A の場合** — `~/.config/wireplumber/wireplumber.conf.d/50-rtsynth.conf` を作成:

```
monitor.alsa.rules = [
  { matches = [ { device.name = "alsa_card.platform-soc_sound" } ]
    actions = { update-props = { device.disabled = true } } }
]
```

`device.name` は `pactl list cards short` で確認できます。再ログイン後、
`sudo fuser -v /dev/snd/*` に DAC の `pcmC*D0p` が出てこなければ成功です。

**B の場合**:

```sh
systemctl --user mask pipewire pipewire-pulse wireplumber
systemctl --user disable --now fluidsynth   # Pi OS 既定の MIDI 音源。任意
sudo systemctl disable lightdm              # GUI 自体が不要なら
```

**B は手順 3 とセットです。** PipeWire を止めると ALSA の `default` も消えるため、
手順 3 をやらないと「busy」ではなく「**どの `-d` でも開けない**」という、より厄介な
状態になります（機序は [2.6](#rtaudio-5x-の-alsa-デバイス番号が二重採番になっている)）。

#### 手順 3 — `default` を DAC に固定し、`-d` を使わない運用にする

サウンドカードの番号は**起動ごとに入れ替わります**（USB MIDI キーボードもカードを
1 枚作るため、DAC と登録順を取り合う）。`-d <番号>` での指定は原理的に安定しません。
`/etc/asound.conf` で名前固定するのが唯一の解です。

```
pcm.!default { type plug  slave.pcm { type hw  card sndrpihifiberry } }
ctl.!default { type hw    card sndrpihifiberry }
```

`card` に渡すのは**カード ID** です。`aplay -l` の
`カード 2: sndrpihifiberry [snd_rpi_hifiberry_dac]` なら **`カード N:` の直後の語**で、
その後ろの角括弧内（＝カード名）ではありません。間違えると
`Cannot get card index for ...` になります。

```sh
cat /proc/asound/cards            # ID は角括弧の中
speaker-test -D default -c 2      # ★ここで鳴ってから次へ
./build/rtsynth                   # -d は付けない
```

`type plug` で `hw` を包んでいるのは、要求されたレート／フォーマットを自動変換
させるためです。`type hw` 直だと DAC が受け付けない組み合わせでいきなり失敗します。

### (3) レイテンシと CPU

- **オーディオ API**: 既定で **ALSA 直結**を選びます。PulseAudio / PipeWire 経由は
  サウンドサーバ側でバッファリングされ（RtAudio 5.x の Pulse バックエンドは指定した
  バッファサイズを無視します）数十 ms のレイテンシが乗ります。起動時に
  `Audio stream started: <API名>, ...` が出るので確認してください。
  意図的に変えたいときだけ `--api pulse`
- **バッファサイズ**: `-b`（256 = 約 5.8 ms、128 = 約 2.9 ms @44.1 kHz）。xrun が出るなら増やす
- **RT スケジューリング**: 実装側で要求済みですが、ユーザに RT 権限が必要です。
  `/etc/security/limits.d/audio.conf` に `@audio - rtprio 95` 等。起動時に
  `Audio thread: realtime scheduling active` が出れば有効
- **CPU ガバナ**: `sudo cpufreq-set -g performance` で周波数変動によるドロップアウトを回避
- **ポリフォニー**: pd 音源は 1 ボイスあたりの演算が重く、Pi では 16 ボイスが過負荷に
  なりがちです。音が割れるなら `--voices 8`（または 6）

### (4) 自動起動（systemd）

対話入力を一切しないので、そのまま systemd unit にできます。MIDI 機器の有無は
起動条件ではない（[1.4](#14-midi-入力) の活線挿抜）ので、USB の認識を待つための
`After=` や `sleep` は不要です。

```ini
# /etc/systemd/system/rtsynth.service
[Unit]
Description=rtsynth
After=sound.target

[Service]
ExecStart=/home/pi/rtsynth/build/rtsynth --adc 0=gain
Restart=on-failure
User=pi
LimitRTPRIO=95

[Install]
WantedBy=multi-user.target
```

## 1.3 起動

```sh
./build/rtsynth --list          # オーディオデバイスと MIDI ポートの一覧
./build/rtsynth                 # 既定デバイス・全 MIDI ポートで起動
./build/rtsynth -b 128 -g 0.3
./build/rtsynth --synth pd      # PD シンセ (external/pd) で起動
./build/rtsynth --voices 8      # ポリフォニー上限を下げる（CPU 節約）
./build/rtsynth --param attack=0.001 --param release=0.1   # パラメータ初期値の上書き
```

主なオプション（全ては `-h`）:

| オプション | 内容 |
|---|---|
| `-s, --synth <name>` | 音源の選択: `sine`（既定）/ `pd` |
| `-l, --list` | オーディオデバイス・MIDI ポート・raw MIDI デバイスの一覧 |
| `-a, --api <name>` | オーディオ API (`alsa` / `pulse` / `jack` …、既定は ALSA 直結) |
| `-d, --device <id>` | オーディオ出力デバイス。**番号は起動ごとに変わるので常用しないこと**（[1.2 (2)](#2-オーディオ出力i2s-dac) 手順 3） |
| `-m, --midi <index>` | MIDI 入力を 1 ポートに限定（既定は全ポート接続） |
| `-r, -b, -g` | サンプルレート / バッファサイズ / マスターゲイン |
| `-p, --param <id=v>` | パラメータの初期値（複数指定可） |
| `--preset <n>` | 起動時のプリセット番号（[1.8](#18-プリセットprogram-change)） |
| `--list-presets` | プリセット一覧を表示して終了 |
| `--voices <n>` | ポリフォニー上限 |
| `--adc`, `--enc` | 物理コントロールの割当（[1.6](#16-物理コントロールツマミエンコーダ)） |
| `--button <pin>=<動作>` | パネルボタンの割当（`preset-next` / `preset-prev` / `panic`） |
| `--lcd <addr>` | 16x2 I2C LCD にパラメータを表示（例 `--lcd 0x27`、[1.6](#表示lcd--oledと--v-のパラメータ表示)） |
| `--lcd-bus <path>` | LCD の I2C バス（既定 `/dev/i2c-1`） |
| `-v, --verbose [種類]` | 動作トレース。引数なしで全部、カンマ区切りで種類を指定（[2.7](#診断オプション)） |

`--param` に不明な ID を渡すと利用可能なパラメータ一覧が表示されます。

## 1.4 MIDI 入力

ポート未指定時は "Midi Through" 以外の**すべての入力ポートに接続**します。
鍵盤からのノートと、別デバイス（MIDI コン）からの CC を並行して受けられます。
特定のデバイスだけ受けたい場合のみ `-m <index>` で限定してください。

起動時に「見つかったポート」と「実際に接続したポート」の両方を表示します。
コントロールサーフェス付きの鍵盤は `KeyLab mkII 61 KBD` / `... CTRL` のように
複数ポートに分かれることがあるため、鳴らない場合はまずここを確認してください。

### 活線挿抜（ホットプラグ）

**MIDI 機器は起動時に繋がっていなくて構いません。** 電源投入と同時に起動する
ハードシンセでは、USB の認識が rtsynth の起動より遅れるのが普通です。

- 機器が 1 台も無い状態でも rtsynth は**起動して動き続けます**
  （`MIDI input: ... no device connected yet` と表示）
- **後から挿せば約 1 秒以内に自動接続**します。接続・切断のたびに
  `MIDI input: ...` の行が更新表示されます
- **抜いたときは、その機器が押さえていた音を全チャンネル分消音します**。
  抜線した鍵盤はノートオフを送れないので、これが無いと音が鳴りっぱなしになります
- 再接続・抜き差しの繰り返しにも対応しています（同時接続の上限は 16 ポート）

`-m <index>` を使う場合だけ注意が必要です。ALSA シーケンサの**ポート番号は機器の
抜き差しで変わる**ため、`-m` は「起動後に最初に解決できた時点のポート名」を記憶し、
以降はその名前を追いかけます。番号を当てにする運用にはしないでください
（既定の全ポート接続なら、この問題自体が起きません）。

`--midi-raw` 側も同じです。`--midi-raw all` は「今ある機器」ではなく
**「以後に挿されるものも含めた全機器」**を意味します。

## 1.5 対応 MIDI メッセージ

| メッセージ | 動作 |
|---|---|
| Note On / Off | ベロシティ対応（2 乗カーブ）、ADSR 付き発音 |
| Pitch Bend | 既定 ±2 半音（`bend_range` パラメータで 0〜24 半音） |
| Program Change | プリセット切替（pd のみ、[1.8](#18-プリセットprogram-change)） |
| CC7 (Volume) | マスターゲイン |
| CC64 (Sustain) | サステインペダル（sine のみ、pd は未対応） |
| CC120 (All Sound Off) | 即時全消音 |
| CC123 (All Notes Off) | 全ノートリリース |

pd 音源はさらに多数の CC で音色パラメータを直接編集できます（[1.7](#17-pd-シンセ)）。

## 1.6 物理コントロール（ツマミ・エンコーダ）

パラメータの操作手段は **MIDI CC / ADC ポット / GPIO エンコーダの 3 系統を併用可能**です。
すべて同じパラメータに書くので、「後から動かした側が勝つ」という通常のシンセパネルと
同じ挙動になります。

### アナログツマミ / スライダ（MCP3008 ADC）

Pi には ADC がないため、SPI 接続の MCP3008（8ch 10bit）を使うのが定番です。
`raspi-config` で SPI を有効化し、ポットは 3.3V–GND 間・ワイパを CH0–7 へ
（配線図は `src/host/Mcp3008Input.hpp` のコメント）。

```sh
./build/rtsynth --adc 0=gain --adc 1=attack --adc 2=decay --adc 3=sustain --adc 4=release
./build/rtsynth --adc-device /dev/spidev0.1 --adc 0=gain     # SPI デバイスの変更
```

ポーリングは 100 Hz の制御スレッドで行われ、EMA ＋書込み閾値でノイズを抑えた値が
アトミックに書かれます（オーディオスレッドとのロックは不要）。

### ロータリーエンコーダ（GPIO 直結）

EC11 等を GPIO に直結できます。A/B 端子を任意の GPIO へ、C（コモン）を GND へ。
内部プルアップを使うので**外付け抵抗は不要**です。

```sh
./build/rtsynth --enc 17,27=line1_dcw_level1 --enc 22,23=volume   # GPIO17/27, GPIO22/23
./build/rtsynth --enc 17,27=attack --enc-step 0.02                # 1 デテントの変化量
./build/rtsynth --enc-chip /dev/gpiochip4                         # Pi 5 等でチップ番号が違う場合
```

カーネル標準の GPIO キャラクタデバイス（外部ライブラリ不要）でエッジイベントを受け、
専用スレッドで直交デコードします。ポット（絶対値）と違い「現在値からの相対操作」なので、
MIDI CC と取り合いになっても値が飛びません。

### パネルボタン（タクトスイッチ直結）

タクトスイッチを GPIO に直結できます。片足を GPIO、もう片足を GND へ。
エンコーダと同じく**内部プルアップを使うので外付け抵抗は不要**です。

```sh
./build/rtsynth --synth pd --button 5=preset-prev --button 6=preset-next
./build/rtsynth --synth pd --button 5=preset-prev --button 6=preset-next --button 13=panic
./build/rtsynth --button-chip /dev/gpiochip4 ...    # Pi 5 等でチップ番号が違う場合
```

| 動作 | 内容 |
|---|---|
| `preset-next` | 次のプリセットへ。最後まで行くと**先頭へ回り込みます** |
| `preset-prev` | 前のプリセットへ。先頭で押すと**末尾へ回り込みます** |
| `panic` | 全チャンネル即時消音（CC120 相当） |

チャタリング除去は**カーネル側**が行うので（GPIO chardev v2 の
`GPIO_V2_LINE_ATTR_ID_DEBOUNCE`、既定 5 ms）、コンデンサも待ち時間も不要です。
カーネルが対応していない場合は起動時に `(no kernel debounce)` と表示されます。

ボタンの押下は **Program Change / CC のイベントに変換されてオーディオスレッドに渡されます**。
MIDI から同じ操作をしたときと完全に同じ経路を通るので、LCD 表示も `-v` の出力も
MIDI 経由の場合と同一です。

**押すと光る LED を付けたい場合**、LED をスイッチと **GPIO の間に直列**に入れてはいけません
（GPIO が LED の順電圧までしか下がらず、LOW と判定されません）。次のように
**GPIO とは別の枝**にぶら下げます。

```
  3V3 ──[内部プルアップ]── GPIOn ──┬── スイッチ ── GND
                                   │
              3V3 ──[R]──▶|(LED)───┘
```

スイッチを離しているときは LED 側に電流の帰り道が無いので消灯（同時に弱いプルアップとして
働くだけ）、押すとノードが GND に落ちて点灯します。GPIO には LED の電流が流れません。
R は 330Ω〜1kΩ 程度（3.3V・Vf 2.0V で 4mA〜1.3mA）。

### 表示（LCD / OLED）と `-v` のパラメータ表示

**16x2 キャラクタ LCD（HD44780 + PCF8574 I2C バックパック）に対応済み**です。
`raspi-config` で I2C を有効化し、`i2cdetect -y 1` で出たアドレスを渡すだけです
（配線 4 本・部品選定込みの手順は [README_HW.md の Step 5](README_HW.md#6-step-5-16x2-lcd--パラメータ表示配線4本)）。

```sh
./build/rtsynth --lcd 0x27                    # LCD にパラメータを表示
./build/rtsynth --lcd 0x3f --lcd-bus /dev/i2c-0   # アドレス・バスの変更
./build/rtsynth --lcd 0x27 -v param           # LCD とコンソールに同時表示
```

上段に「最後に動いたパラメータの表示名」、下段に「その値」が出ます。CC でもツマミでも
エンコーダでも、動かした側が表示されます。Program Change でプリセットを切り替えると
`PRESET 6` / `Mono Bass` の 2 行になります。

```
+----------------+
|L1 DCW Rate 1   |  <- DisplayLine.label（表示名。無ければ ID）
|0.787 (1063 ms) |  <- DisplayLine.value（-v のコンソールと同じ整形）
+----------------+
```

「何を表示するか」は表示先から独立した共通モジュールが決めています。

```
      MIDI CC ─┐
    ADC ポット ─┼→ ParameterMonitor ──→ ParameterDisplay ─┬→ ConsoleParameterDisplay (-v)
  エンコーダ ─┤   （何を出すか決める）  （共通インタフェース）├→ LcdParameterDisplay ─→ TextDisplay
Program Change ┘                                            │                          └→ I2cLcd1602
                                                            └→ 追加の表示先（OLED 等）
```

- `ParameterMonitor`（`src/host/ParameterMonitor.hpp`）が、CC・ツマミ・エンコーダ・
  プリセット切替のどれで値が動いても検知し、表示 1 行ぶんの `DisplayLine`
  （パラメータ ID / 表示名 / 整形済みの値 / 変更のきっかけ）を組み立てます
- `ParameterDisplay`（`src/host/ParameterDisplay.hpp`）はそれを受け取るだけの
  抽象インタフェースです。**別の表示先を足すのはこのインタフェースを実装するだけ**で済み、
  楽器側・ホスト側の変更は不要です
- `LcdParameterDisplay`（`src/host/LcdParameterDisplay.hpp`）がその LCD 実装で、
  文字列を 2 行に割り付けるだけの薄い層です。実際の描画は `TextDisplay` 抽象
  （`I2cLcd1602` = 実機、セルフテストではフェイク）が担当します
- 表示先は複数登録できるので、`-v` のコンソール出力と LCD を同時に使えます
- 呼び出しは UI スレッド（`main` のポーリングループ、既定 50 ms 間隔）からのみで、
  オーディオ／MIDI スレッドからは呼ばれません。**I2C / SPI のブロッキング書込みを
  そのまま書いて構いません**。値が変わった行だけを書き直すので、放置中は I2C に
  一切トラフィックが流れません

`-v param`（または引数なしの `-v`）を付けたときのコンソール出力が、この仕組みの
動く実例です。**CC の値と、それによって動いた音色パラメータの現在値が 1 行に
まとまります**。

```
[midi]  cc       ch 0  cc 46  val 100  (nanoKONTROL2 MIDI 1)
[param] L1 DCW Rate 1 = 0.787 (1063 ms)   <- CC 46 = 100   (line1_dcw_rate1)
[param] L1 Wave 1st = Pulse               <- CC 89 = 40    (line1_wave1)
[param] Detune Fine = +35 (+58.3 ct)      <- CC 87 = 100   (detune_fine)
[preset] 6: Mono Bass
```

値は生の [0,1] ではなく**楽器が解釈した意味で表示**されます（波形名・EG のステップ番号・
EG レートのミリ秒換算・デチューンのセント数など）。この変換は `Processor::describeValue()`
が担当するので、LCD 側でも同じ表示が何もせずに得られます。

表示名と値は**どちらも 16 文字以内に収めてあります**（`Reso III Trap`、`+60 (+100.0 ct)` など）。
LCD は溢れた分を黙って切り捨ててしまうため、全パラメータを全域スイープして
16 文字を超えないことをセルフテストで検証しています（超えた場合は違反した文字列が
表示されます）。パラメータや波形を追加するときはこのテストが番人になります。

## 1.7 PD シンセ

```sh
./build/rtsynth --synth pd
./build/rtsynth --synth pd --param line_select=0.67 --param detune_fine=0.6  # 1+1' でデチューン
./build/rtsynth --synth pd --adc 0=volume --adc 1=line1_dcw_level1           # DCW をツマミで
```

パラメータは pd プラグインと同じ正規化値 [0,1] で登録されています。

| ID | 内容 |
|---|---|
| `volume`, `pitch_bend`, `mono` | システム系 |
| `line_select` | 0=Line1, 0.33=Line2, 0.67=1+1', 1.0=1+2' |
| `detune_octave` / `detune_note` / `detune_fine` | 0.5 が中央（±0） |
| `line{1,2}_wave{1,2}` | 波形選択（8 波形 / 2nd は Off+8） |
| `line{1,2}_{dco,dcw,dca}_rate{1..8}` | 8 段 EG のレート |
| `line{1,2}_{dco,dcw,dca}_level{1..7}` | 8 段 EG のレベル |
| `line{1,2}_{dco,dcw,dca}_{sustain,end}` | サステイン点・エンド点 |
| `cc_edit_line` | CC の編集対象ライン（0=Line1, 1=Line2） |
| `mono_trigger` / `poly_trigger` | pd プラグインが CC126/127 を受けるためのダミー。スタンドアロンでは無効（`mono` を直接動かします） |

**MIDI CC** の割当は pd プラグイン側（`controller.cpp` の `getMidiControllerAssignment`）と
同一です。CZ 実機と同じく「EG のツマミ 1 系統を Line1/Line2 で共有し、CC3 で編集対象を
切り替える」設計になっています。

| CC | 内容 |
|---|---|
| CC3 | **編集対象ライン切替**（値 <64 → Line1、≥64 → Line2） |
| CC7 | Volume |
| CC9 | 発音ライン選択（Line1 / Line2 / 1+1' / 1+2'） |
| CC85 / CC86 / CC87 | Detune オクターブ / ノート / ファイン |
| CC89 / CC90 | 編集対象ラインの第1 / 第2 波形 |
| CC14–30 | 編集対象ラインの DCO EG（レート1–8・レベル1–7・サステイン・エンド） |
| CC46–62 | 同 DCW EG |
| CC102–118 | 同 DCA EG |
| CC126 / CC127 | Mono / Poly 切替（MIDI 標準の Mono/Poly Mode On） |

現在の編集対象は `cc_edit_line` パラメータで確認・指定できます。
どの CC がどのパラメータに効いたかは `-v` で確認するのが確実です（[1.6](#16-物理コントロールツマミエンコーダ)）。

**使用上の注意**:

- プラグインの素の状態は無音（エディタが値を入れる前提）のため、スタンドアロンでは
  デフォルト値を「鳴る初期パッチ」（Line1 ノコギリ波・高速アタック DCA・DCW スイープ）に
  してあります。これがプリセット 0 になります（[1.8](#18-プリセットprogram-change)）
- サステインペダル (CC64) は pd の Voice が未対応のため効きません
- 波形切替パラメータの変更はオーディオスレッド上でジェネレータを再生成します
  （pd 本体と同じ挙動）。演奏中の頻繁な波形自動化は避けてください
- **EG の設定によっては音が止まらなくなります**（pd 側の既知の不具合、[2.6](#26-既知の問題)）

## 1.8 プリセット（Program Change）

pd 音源は 11 個のプリセット（ファクトリ 8 + ユーザ 3）を持ち、**MIDI Program Change**
または**パネルボタン**（[1.6](#16-物理コントロールツマミエンコーダ)）で切り替えられます。
番号はそのまま Program Change の値（0 始まり）です。

```sh
./build/rtsynth --synth pd --list-presets   # 一覧
./build/rtsynth --synth pd --preset 6       # Mono Bass で起動
./build/rtsynth --synth pd --button 5=preset-prev --button 6=preset-next
```

| # | 名前 | 概要 |
|---|---|---|
| 0 | Init Saw | 初期パッチ（ノコギリ波・高速アタック・DCW スイープ） |
| 1 | Soft Pad | 遅いアタックとリリースのパッド |
| 2 | E.Piano | 減衰系（サステインなし）、Saw Pulse |
| 3 | Brass | DCW がやや遅れて立ち上がるブラス |
| 4 | Reso Sweep | Reso I Saw 波形＋1.5 秒の DCW スイープ |
| 5 | Bell | 1+1' デチューンの減衰ベル |
| 6 | Mono Bass | Mono（SOLO）、2 段 DCW の短いベース |
| 7 | Dual Detune | 1+2'（Line1 ノコギリ＋Line2 矩形）のデチューン |
| 8–10 | User 1–3 | **編集用の空きスロット**。中身は素の初期値のみ（= Init Saw と同じ） |

ユーザスロットを末尾に置いてあるのは、今後ファクトリ音色を足してもユーザスロットの
Program Change 番号が動かないようにするためです。

**編集とプリセットの関係**（`src/core/PresetBank.hpp`）:

- CC・ツマミ・エンコーダは**常に「今選ばれているプリセット」を直接編集**します。
  プリセット用の別経路はありません
- Program Change を受けると、**離れるスロットに現在の値を保存**してから次を読み込みます。
  つまり戻ってくれば編集後の音が復元されます
- 編集内容は RAM 上のみで、**再起動するとファクトリの状態に戻ります**（保存は未実装）。
  ユーザスロットも例外ではありません
- 存在しない番号の Program Change は無視されます

`-v` で実行中にプリセットを切り替えると `[preset] 6: Mono Bass` の 1 行だけが出ます
（約 120 個のパラメータが一度に書き換わるため、個別表示は抑制されます）。

---

# 第2部 開発・デバッグ

## 2.1 ディレクトリ構成

```
.
├── CMakeLists.txt         # rtsynth_core / rtsynth_pd (任意) / rtsynth / rtsynth_selftest
├── external/
│   └── pd/                # git submodule: PD シンセ (LouisYOSHINAGA/pd) — 無改変で利用
├── src/
│   ├── core/              # [層1] プラグイン基盤 — 完全プラットフォーム非依存
│   ├── dsp/               # [層2] DSP 部品 — core にのみ依存
│   ├── synth/             # [層3] 楽器本体 — 「プラグイン」に相当
│   ├── host/              # [層4] ホスト — 「DAW」に相当（RtAudio/RtMidi はここだけ）
│   ├── vst3shim/          # VST3 SDK の型定義の代替ヘッダ（pd を SDK なしでビルドするため）
│   └── main.cpp           # エントリポイント（CLI・シグナル処理・楽器の選択）
└── tests/
    └── selftest.cpp       # オーディオデバイス不要のオフラインレンダテスト
```

依存は必ず 下の層 → 上の層 の一方向です（`host` → `synth` → `dsp` → `core`）。
`core` / `dsp` / `synth` は RtAudio・RtMidi を一切 include しないため、
この 3 層はそのまま VST3 / JUCE / 別バックエンドへ持ち出せます。

### `src/core/` — プラグイン基盤層

VST3 / JUCE が規定している「プラグインとホストの契約」に相当する最小セットです。
音を出すコードはここには一切ありません。

| ファイル | 内容 | VST3 / JUCE での対応物 |
|---|---|---|
| `Processor.hpp` | 楽器の抽象インタフェース。`prepare()`（バッファ確保）→ `process()`（1 ブロック描画、RT セーフ必須）→ `reset()`（全消音）のライフサイクルと、RT スレッド上での禁止事項（アロケーション・ロック・ブロッキング I/O）をコメントで規定 | `IComponent` / `AudioProcessor` |
| `AudioBuffer.hpp` | プレーナ（チャンネル別配列）float32 バッファへの非所有ビュー | `AudioBusBuffers` / `AudioBuffer<float>` |
| `MidiBuffer.hpp` | 生 MIDI バイト列のデコード (`MidiEvent::fromRaw`) と、1 ブロック分のイベント列（sampleOffset 順・固定容量・アロケーションなし） | `IEventList` / `MidiBuffer` |
| `Parameters.hpp` | 正規化値 [0,1] ↔ 実値のパラメータ。`std::atomic<float>` なので制御スレッドから書き、オーディオスレッドから読める（ロック不要）。書込みごとの変更カウンタを持ち、UI（LCD 等）がポーリングで変更検知できる | `IEditController` / `AudioProcessorValueTreeState` |
| `PresetBank.hpp` | `ParameterSet` のスナップショット（プリセット）を複数保持し、Program Change で切り替える。領域は構築時に確保済みなので `select()` はオーディオスレッドから呼べる。**編集は常にライブ値に対して行われ、スロットを離れるときに書き戻す**ので、CC 編集とプリセットが二重管理にならない | VST3 の program list |
| `SpscRingBuffer.hpp` | ロックフリー SPSC リングバッファ（MIDI スレッド → オーディオスレッドの受け渡し用、約 40 行） | — |
| `MidiStreamParser.hpp` | 生 MIDI バイト列の逐次パーサ（ランニングステータス・リアルタイムバイト混入・SysEx フレーミング対応）。`--midi-raw` 経路で使用、オフラインで単体テスト可能 | — |

### `src/dsp/` — DSP 部品層

楽器を構成する再利用可能な信号処理部品。1 サンプルずつ `tick()` する小さなクラス群で、
フィルタや LFO を足す場合もこの層に置きます。

| ファイル | 内容 |
|---|---|
| `SineOscillator.hpp` | 位相アキュムレータ方式の sine オシレータ |
| `AdsrEnvelope.hpp` | 直線 ADSR。クリック防止の設計（現在レベルからのアタック、ノートオフ時レベルからのリリース、スチール用 3 ms フェード）をコメントに明記 |
| `SmoothedValue.hpp` | 1 ポールのパラメータスムーザ（ジッパーノイズ防止）。ツマミ/CC の段階的な値をサンプル単位で滑らかに追従させる。マスターゲインで使用 |

### `src/synth/` — 楽器本体層（＝プラグイン）

`Processor` を実装した「楽器そのもの」。**新しい音源を作るときに読む・真似る場所**です。

| ファイル | 内容 |
|---|---|
| `Voice.hpp` | 1 ボイス（オシレータ＋エンベロープ＋ノート状態）。ボイスの状態遷移（free → held → releasing → free、スチール時のフェード→保留ノート発音）を図解コメント付きで実装 |
| `VoiceAllocator.hpp` | ボイスプールの管理。割当ポリシー（同ノート再打鍵 > 空きボイス > リリース中最古 > 発音中最古のスチール）、サステインペダル、CC120/123。ノートオフは**プール全体を走査**して同ノートを持つボイスを全て解放する（スチール保留と実発音で同じノートが 2 ボイスに載りうるため） |
| `SineSynthProcessor.{hpp,cpp}` | リファレンス実装。MIDI イベント境界でブロックを分割するサンプル精度処理、パラメータ定義（gain / ADSR / ベンドレンジ）、CC ディスパッチ、出力段（ゲイン＋クリップ） |
| `PdSynthProcessor.{hpp,cpp}` | **外部シンセ取り込みの実例**: `external/pd` の DSP コアを無改変でホストするアダプタ（[2.4](#24-pd-シンセの取り込み方)） |

### `src/host/` — ホスト層（＝DAW 相当）

プラットフォーム依存コードを全て閉じ込める層。別バックエンド（JACK ネイティブ、
PipeWire、プラグインラッパ等）への移植ではこの層だけを書き換えます。

| ファイル | 内容 |
|---|---|
| `RtAudioOutput.{hpp,cpp}` | RtAudio の薄いラッパ。**5.x / 6.x の API 差（デバイスのインデックス/ID、例外/エラーコード）をこのファイルだけに隔離**。float32・プレーナ出力、xrun のアトミックなカウント、DSP 負荷の計測 |
| `MidiInput.hpp` | MIDI 入力バックエンドの共通インタフェース（pop・カウンタ・モニタ・生バイトダンプ）。ホスト層はこの面しか見ない |
| `RtMidiInput.{hpp,cpp}` | ALSA シーケンサ経由（RtMidi）の実装。既定で全入力ポートに接続。ポートごとに専用 SPSC キュー（RtMidi はポートごとにコールバックスレッドを立てるため、SPSC の単一生産者制約を守る配置）。コールバックは `openPort()` の**前**に登録する（間に来たイベントを落とさないため）。**活線挿抜対応**: スロットは追加のみ・破棄せず、`portCount_` の release ストアで公開するので、オーディオスレッドの `pop()` と接続処理が並行しても安全 |
| `RawMidiInput.{hpp,cpp}` | カーネル rawmidi 直読みの実装（`--midi-raw`）。シーケンサと RtMidi を完全にバイパスする最短経路。poll + ノンブロッキング read → `MidiStreamParser`。抜線は読み取りスレッドが `POLLHUP` / `-ENODEV` で検知してスロットを返し、`rescan()` が回収・再接続する |
| `StandaloneHost.{hpp,cpp}` | 全体の糊。**3 スレッド（MIDI / オーディオ RT / メイン）の関係はこのヘッダのコメント参照**。ブロックごとに MIDI キューを `MidiBuffer` へ排出。バッファ満杯時は**イベントを捨てずに排出を止める**（次ブロックで続きを読む） |
| `ControlInput.hpp` | 物理コントロールの抽象 2 種: **絶対値** `ControlInput`（ポット等、[0,1] を返す）と**相対値** `RelativeControlInput`（エンコーダ等、前回からのステップ数を返す） |
| `ControlLoop.hpp` | 制御スレッド。絶対値入力は EMA ノイズ除去＋書込み閾値、相対値入力はデテント×ステップ幅でパラメータに書く。両方式を併用可能 |
| `Mcp3008Input.{hpp,cpp}` | MCP3008（SPI 8ch 10bit ADC）の `ControlInput` 実装。配線図はヘッダのコメント参照 |
| `GpioEncoderInput.{hpp,cpp}` | GPIO ロータリーエンコーダの `RelativeControlInput` 実装。GPIO キャラクタデバイス (uapi v2) でエッジイベントを受け、直交デコード（`QuadratureDecoder` は単体テスト可能に分離） |
| `ParameterWatcher.hpp` | UI スレッド（LCD・コンソール等）向けの変更検知。全パラメータの変更カウンタをポーリングし「前回から変わったパラメータ」だけを報告 |
| `ParameterDisplay.hpp` | 表示バックエンドの抽象（`showParameter` / `showPreset`）と、`-v` 用のコンソール実装。**表示先を増やすのはここを実装するだけ**。1 行ぶんのデータは `DisplayLine`（ID / 表示名 / 整形済みの値 / きっかけ）で、16x2 LCD が自分で配置できるようフィールドを分けてある |
| `LcdParameterDisplay.hpp` | `DisplayLine` をキャラクタ表示に割り付ける `ParameterDisplay` 実装（上段=表示名、下段=値、プリセットは `PRESET n`/名前）。桁数に合わせて切り詰め、**内容が変わった行だけ**書き直すので待機中の I2C トラフィックはゼロ |
| `TextDisplay.hpp` | 小型キャラクタ表示の抽象（桁数・行数・1 行書換え・クリア）。実機とセルフテストのフェイクを差し替える境界 |
| `I2cLcd1602.{hpp,cpp}` | HD44780 16x2 LCD + PCF8574 I2C バックパックのドライバ。カーネルの I2C キャラクタデバイス直叩き（外部ライブラリ不要）、4bit 初期化シーケンスと配線・アドレスの注意はヘッダのコメント |
| `ParameterMonitor.hpp` | 「何を表示するか」を決める側。CC・ツマミ・エンコーダ・プリセット切替を 1 本のストリームにまとめ、登録された全 `ParameterDisplay` へ流す。**CC とその CC が動かしたパラメータ値を同じ行にまとめる**のもここ（`Processor::parameterForCc()` で対象を引く）。プリセット切替時は全パラメータを列挙せずプリセット名 1 行にまとめる |

### `src/main.cpp`

CLI の解釈、`Processor` と `StandaloneHost` の接続、SIGINT/SIGTERM での安全な終了、
RT スレッドが記録したカウンタ（xrun / MIDI ドロップ）のメインスレッドからの報告。
**どの楽器をビルドするかを決める唯一の場所**です。

## 2.2 データフロー / スレッドモデル

```
[MIDI 鍵盤]                                          [DAC / スピーカ]
     |                                                      ^
     v                                                      |
 MIDI スレッド (RtMidi callback)                  オーディオ RT スレッド (RtAudio callback)
   生バイト列 -> MidiEvent にデコード               SpscRingBuffer -> MidiBuffer に排出
     |                                              Processor::process(audio, midi)
     +----> SpscRingBuffer (ロックフリー) ---------->     |
                                                    ボイス描画 -> ゲイン/クリップ -> 出力
 メインスレッド
   起動/停止・CLI・Parameter への書込 (atomic)・カウンタ監視 (RT スレッドはログ禁止)
```

RT スレッドの規約はひとつだけです: **アロケーション・ロック・ブロッキング I/O・ログ出力を
しない**。ログはカウンタをアトミックに増やし、メインスレッドが読んで表示します。

## 2.3 セルフテスト

`Processor` 抽象の恩恵で、オーディオデバイスなしに DSP を検証できます。

```sh
./build/rtsynth_selftest        # または ctest --test-dir build
```

内容: 発音・リリース・クリップ・ボイススチールの安定性・スチール中ノートオフの
スタックノート回帰・同ノート二重発音の回帰・サステインペダル・ピッチベンド・
ポリフォニー上限・pd 音源と CC マッピング・**全ファクトリプリセットが実際に発音するか**・
**Program Change による切替と編集内容の保持**・**`ParameterMonitor` の表示内容**・
エンコーダの直交デコード・`ParameterWatcher`・`MidiStreamParser` の各ケース・
SPSC キューの 20 万イベントマルチスレッドストレステスト。CI にそのまま載せられます。

## 2.4 PD シンセの取り込み方

外部プロジェクトの音源を**相手のリポジトリを一切変更せずに**取り込む実例です。

pd の DSP コア（`pd.{h,cpp}` / `eg.{h,cpp}` / `voice.{h,cpp}` / `const.h`）の VST3 SDK への
依存は `vsttypes.h` の型エイリアス（`ParamValue` = double 等）だけです。そこで:

1. `external/pd` に pd を git submodule として置く
2. `src/vst3shim/pluginterfaces/vst/vsttypes.h` に必要最小限の型定義を用意し、
   インクルードパスで SDK の代わりに解決させる（プラグインとしてビルドするときは
   本物の SDK が解決されるので、pd 側は両対応のまま）
3. `synth/PdSynthProcessor` が VST3 の配管に相当するホスト側ロジック — パラメータの
   ディスパッチ、ボイスプール（最古スチール）、mono (SOLO) のラストノート優先、DETUNE、
   ピッチベンド、出力ミックス — を再実装する

**注意**: VST3 では CC → パラメータの変換は DSP 側（processor.cpp）ではなく
**ホスト側**（controller.cpp の `getMidiControllerAssignment`）が行います。
スタンドアロンにはそのホストが無いため、`PdSynthProcessor::handleEvent()` で同じ
マッピングを再現しています。外部プラグインを取り込むときに見落としやすい箇所です。

pd を更新するときは `cd external/pd && git pull` 後に rtsynth 側をコミットしてください
（サブモジュールは特定コミットに固定されます）。

**サブモジュール更新時の落とし穴**: pd 側で `ParamId` に列挙子が追加されると `kNumParams`
が増えます。`PdSynthProcessor` はこの数だけ `Parameter` ハンドルの配列を持つため、
登録側が手書きリストのままだと**未登録のハンドルが nullptr のまま残り、起動即クラッシュ**
します（実際に `kParamMonoTrigger` / `kParamPolyTrigger` の追加で発生しました）。
現在は `registerParameters()` が `0 .. kNumParams-1` を走査して必ず全 ID を登録し、
rtsynth が知らない ID にも `pd_paramN` という名前で枠を用意するようにしてあります。
CC 割当を増やされた場合は追従が必要（コンパイルは通るが黙って効かなくなる）なので、
`controller.cpp` の `getMidiControllerAssignment` と
`PdSynthProcessor::paramIdForCc()` を突き合わせてください。

## 2.5 拡張ガイド

| やりたいこと | 触る場所 |
|---|---|
| 新しい音源方式 | `synth/` に `Processor` 実装を追加し、`main.cpp` の `--synth` 分岐に登録。ホスト層は変更不要（外部リポジトリの取り込みは `PdSynthProcessor` + submodule + shim が実例） |
| フィルタ・LFO 等の部品追加 | `dsp/` に部品を追加し `Voice` に組み込む |
| 新しい入力ハード（別 ADC・ボタン等） | `ControlInput` / `RelativeControlInput` を実装（`Mcp3008Input` / `GpioEncoderInput` が実例、約 60 行）。押しボタンも `GpioEncoderInput` と同じ GPIO エッジイベントで読める |
| 別の表示器（OLED・大きい LCD 等） | 16x2 I2C LCD は実装済み（`--lcd`）。別の表示器も `ParameterDisplay` を実装して `ParameterMonitor::addDisplay()` で登録するだけ（`ConsoleParameterDisplay` / `LcdParameterDisplay` が実例）。桁数だけ違う HD44780 系なら `TextDisplay` 実装の差し替えで済む。I2C の SSD1306 OLED が次の定番 |
| プリセットを増やす | `PdSynthProcessor::registerFactoryPresets()` に 1 行足す（差分だけ書くスパース定義） |
| 他の音源にプリセットを持たせる | `PresetBank` をメンバに持ち、`Processor::presets()` を override して Program Change を `handleEvent()` で処理（`PdSynthProcessor` が実例） |
| パッチ（音色）のファイル保存/読込 | `PresetBank` のスロットをシリアライズ（現状は RAM のみ） |
| VST3 / JUCE プラグイン化 | `Processor` 実装を `processBlock` から呼ぶ薄いラッパを書く。`core`〜`synth` は無変更 |
| 別オーディオバックエンド | `host/` 層のみ再実装 |

## 2.6 既知の問題

### pd の EG が停止しないケース（未修正・pd 側）

`external/pd` の `EG::update()` は、次のステップへ進む条件が `dLevel_ > 0` / `dLevel_ < 0` の
**厳密不等号**になっています。そのため EG の連続する 2 ステップのレベルが等しく
`dLevel_ == 0` になると、そのステップから永久に進めず、**ボイスが解放されません**
（`activeVoices` が減らないまま音が残る）。CC で DCA のエンド点を動かすだけでも起こります。

`dLevel_ == 0 ||` を条件に足せば解消しますが、`external/pd` は**無改変で使う方針**のため
rtsynth 側では修正していません。pd リポジトリ側で直すべき問題です。

### `snd_pcm_open error ... Unknown error 524`

RtAudio がデバイス列挙時に開けないデバイス（音声シンクの無い HDMI 出力など）を試した際の
警告で、**無害**です（そのデバイスが一覧からスキップされるだけ）。既定では非表示にしてあり、
`--verbose` 指定時のみ表示されます。

### RtAudio 5.x の ALSA デバイス番号が二重採番になっている

`RtApiAlsa::probeDeviceOpen: pcm device (default) won't open for output.` や、
「`speaker-test -D hw:2,0` は鳴るのに rtsynth だけどの `-d` でも起動しない」の原因です。
RtAudio 5.x は **デバイスを数えるときと開くときで別のルールを使っています**（`RtAudio.cpp`）。

| | ALSA `default` の扱い | ハードウェアデバイスの番号 |
|---|---|---|
| `getDeviceCount()` / `getDeviceInfo()`（＝`--list`） | `snd_ctl_open("default")` が**成功したときだけ** 0 番として数える | 0 または 1 から |
| `probeDeviceOpen()`（＝実際に開く） | **常に** 0 番を `default` に予約 | **必ず** 1 から |

`default` が開けるうちは両者が一致します。開けなくなった瞬間に数える側だけが 1 つ
少なくなり、**全ハードウェアの「表示番号」が「開く番号」より 1 小さくなる**。そして
最後のカードは `getDeviceCount()` の範囲外へ押し出され、`openStream()` がどの番号でも
受け付けなくなります。HDMI (カード 0) と HiFiBerry (カード 2) の例:

| 番号 | `--list` の解釈 | 実際に開かれるもの |
|---|---|---|
| 0 | vc4hdmi（開けないので非表示） | ALSA `default` → 失敗 |
| 1 | **HiFiBerry**（一覧に出る） | `hw:0,0` = vc4hdmi → 失敗 |
| 2 | （範囲外） | `hw:2,0` = HiFiBerry だが **`getDeviceCount()`=2 なので門前払い** |

`default` が開けなくなる条件は、**I2S DAC 導入で普通に踏む手順そのもの**です
（`dtparam=audio=off` で内蔵カードを止める ＋ PipeWire を止める）。
`asound.conf` で `default` を DAC に向けると `snd_ctl_open("default")` が成功するので
2 つの番号体系が一致し、`-d` 無しでそのまま DAC が開きます。これが唯一の解です
（手順は [1.2 (2)](#2-オーディオ出力i2s-dac) の手順 3）。

rtsynth 側の対処: `-d` 無指定時は候補を総当たりで開き、それでも駄目な場合は
`snd_ctl_open("default")` を直接プローブして**「どの `-d` も無駄である」ことを明示**し、
`asound.conf` の雛形とこのマシンのカード ID 一覧をエラーメッセージに出します。

## 2.7 デバッグ手順

### 診断オプション

| オプション | 出力 |
|---|---|
| `-v, --verbose [種類]` | 動作トレース（下表）。表示は**メインスレッド**が行うため音は途切れない |
| `--midi-dump` | 受信した生 MIDI バイト列を `[read] 90 43 5D` 形式で表示（`--midi-raw` 併用時は read() 単位、既定のシーケンサ経路ではメッセージ単位でグルーピング） |
| `--midi-raw <dev>` | ALSA シーケンサと RtMidi を完全にバイパスし、カーネル rawmidi を直読みする。`--list` の "Raw MIDI Devices" 欄の ID を渡す。複数指定可、`all` で全デバイス |

`-v` は**引数なしで全部**、カンマ区切りで種類を絞れます。調べたいものだけを出せば、
残りのログに埋もれません。

| 種類 | 出力される行 | 内容 |
|---|---|---|
| `midi` | `[midi]` | 受信 MIDI イベント（ポート名付き） |
| `param` | `[param]` / `[preset]` | パラメータ・プリセットの変更（きっかけの CC 付き） |
| `load` | `[load]` | 1 秒ごとの DSP 負荷とボイス数 |
| `voices` | `[voices]` | 発音中ボイス数（変化したときだけ） |
| `warnings` | — | オーディオバックエンドの警告 |
| `all` | | 上記すべて（引数なしと同じ） |

```sh
./build/rtsynth -v                    # 全部
./build/rtsynth -v midi               # MIDI が届いているかだけ見る
./build/rtsynth -v midi,param         # CC がどのパラメータに効いたかを追う
./build/rtsynth --verbose=load        # 負荷だけ（演奏しながら眺める用）
./build/rtsynth -v voices             # 音が止まらない疑いのとき
```

xrun・MIDI ドロップ等の警告カウンタは `-v` の指定に関わらず常に表示されます。

### DAC から音が出ない

I2S DAC を付けた直後に最も踏みやすい問題です。**症状は 2 種類あり、対処が正反対**なので、
まず自分がどちらに居るかを確定してください。

| 症状 | エラー | 状態 |
|---|---|---|
| **(a) 掴まれている** | `Device or resource busy` / `-16` | カードは正常。他のプロセス（ほぼ PipeWire）が開いている |
| **(b) 届いていない** | `pcm device (default) won't open for output` / `-d` を変えても駄目 | ALSA の `default` が無く、RtAudio の番号がズレて DAC が到達不能 |

#### 判定 — この 2 コマンドで確定する

```sh
aplay -l                       # 「サブデバイス: 0/1」なら (a)。「1/1」なら (a) ではない
sudo fuser -v /dev/snd/*       # 誰が掴んでいるか
```

`fuser` の読み方が肝です。

```
                     USER        PID ACCESS COMMAND
/dev/snd/pcmC1D0p:   lightdm     646 F...m  pipewire      ← ★これが (a) の犯人
/dev/snd/controlC1:  lightdm     650 F....  wireplumber
/dev/snd/seq:        lightdm     648 f....  fluidsynth    ← seq は MIDI。PCM とは無関係
```

- **`pcmC<N>D0p`** を掴んでいるものだけが (a) の原因です。`seq` は MIDI シーケンサなので、
  ここに `fluidsynth` が居ても busy とは無関係です
- **USER 列を必ず見ること。** `lightdm` と表示されたら、それは**ログイン画面のセッションが
  動かしている別の PipeWire** です。この場合 `systemctl --user stop pipewire` は
  **自分のユーザーのものしか止めないので効きません**（実際にこれで数時間溶かしています）

補助的な兆候:

- **`./rtsynth --list` に DAC が出てこない** → (a)。`--list` は各デバイスを実際に開いて
  調べ、開けなかったものを一覧から落とす仕様なので、掴まれているカードは消えます
- **`--list` には出るのに起動だけ失敗する** → (a) のタイミング差。ログイン直後は間に合っても、
  数秒後に PipeWire が掴みに来ます

#### 対処

- **(a)** → [1.2 (2) 手順 2](#2-オーディオ出力i2s-dac) の A 案か B 案を入れる。
  `sudo fuser -k /dev/snd/pcmC1D0p` でその場は直りますが、systemd が PipeWire を
  再起動するので**次の起動で必ず再発します**。切り分け用と割り切ってください
- **(b)** → [1.2 (2) 手順 3](#2-オーディオ出力i2s-dac) の `asound.conf`。
  `-d` の値を変えて探すのは**原理的に無駄**です（機序は
  [2.6](#rtaudio-5x-の-alsa-デバイス番号が二重採番になっている)）

#### 効かなかった／悪化させた操作の記録

同じ道を辿らないための記録です。

| 操作 | 結果 |
|---|---|
| `systemctl --user stop pipewire ...` | 犯人が `lightdm` ユーザーのインスタンスだと**無効**。`fuser` の USER 列を見れば分かる |
| `sudo systemctl disable lightdm` | greeter の PipeWire は消えるが、**SSH ログイン時に自分の PipeWire が起動する**ので翌日再発。デスクトップを失うだけだった |
| `systemctl --user disable --now fluidsynth` | `fluidsynth` は `/dev/snd/seq` しか掴んでいない。**busy とは無関係**で、変数を増やしただけ |
| PipeWire の mask 単独 | (a) は消えるが `default` も消えるため **(b) に転落する**。手順 3 とセットで初めて成立する |
| `-d` の番号を総当たり | (b) では正しい番号が `getDeviceCount()` の範囲外にあり、**どれも当たらない** |

一般則として、**症状 1 つに対して不可逆な操作（`disable` / `mask` / 設定ファイル）を
同時に複数打たないこと**。可逆な操作（`stop`、`fuser -k`）で原因を確定させてから、
恒久策を 1 つだけ入れるのが最短です。今回は不可逆な操作を短時間に重ねたため、
どれが効いてどれが悪化させたのかが一時的に切り分け不能になりました。

### 音がプツプツ切れる（crackling）

`-v load` の `[load] 62% peak / 41% now, 9 voices` が DSP 負荷（1 ブロックの締切に
対するレンダ所要時間の割合）です。

- **80% を超える** → CPU 不足。`--voices 8` でポリフォニーを下げるか `-b 512` でバッファを増やす
- **負荷が低いのに切れる** → スケジューリング側。起動時に
  `Audio thread: realtime scheduling active` が出ているか確認（出ていなければ rtprio 設定）
- リリースが長い音色（既定 0.5 秒）は離鍵後もボイスを保持するため、和音を速く弾くと容易に
  最大ポリフォニーに達します。`[voices]` 表示で確認できます

### MIDI の取りこぼし（音が鳴り続ける／押鍵が抜ける）

**まず [1.2 (1) の `dwc_otg.speed=1`](#1-usb-midi-の取りこぼし対策pi-3b-以前では必須) を
適用してください。** Pi 3B 以前でこの症状が出る場合、原因はほぼこれです。

それでも起きる場合の切り分け順序:

1. `-v` で弾き、消えるのがオンかオフか、警告（dropped / backlog / undecodable /
   read errors）が出るかを見る。**警告が出ずにイベント自体が現れない**なら、損失は
   rtsynth より上流（シーケンサ経路・USB ドライバ・機器）
2. `--midi-dump` で生バイト列を確認する。オフのバイト（`80 <note> xx` または
   `90 <note> 00`）が**無い**なら損失は rtsynth の外側、**有る**のに発音が続くなら
   シンセ側の問題
3. `--midi-raw hw:X,Y,Z` に切り替える。これで解消するなら ALSA シーケンサ経路が原因。
   `--midi-raw all` にすると、コントロールサーフェス付き鍵盤が別サブデバイス
   （`hw:X,0,1` …）に流している可能性も潰せる
4. rtsynth を完全に外し、**`aseqdump -p <port>`** または **`amidi -p hw:X,Y,Z -d`**
   （ALSA 純正ツール）で同じ演奏をダンプする。ここでもオン/オフの個数が合わないなら、
   原因は rtsynth ではないことが第三者ツールで確定する
5. `dmesg | grep -i usb` で機器の接続速度を確認する。
   `full-speed USB device using dwc_otg` と出ていれば `dwc_otg.speed=1` が
   効いていないので [1.2 (1)](#1-usb-midi-の取りこぼし対策pi-3b-以前では必須) を見直す
6. 最終手段として `usbmon` でバス上のパケットを直接見る:
   ```sh
   sudo modprobe usbmon
   lsusb                                    # 鍵盤のバス番号を確認
   sudo cat /sys/kernel/debug/usb/usbmon/<bus>u
   ```
   バス上に有るのに rawmidi が渡さない → カーネルの USB-MIDI 解釈側。
   バス上にも無い → 機器のファームウェア

### 調査済み・シロだったもの

同じ症状を追う際に再調査しなくて済むよう、過去の切り分け結果を残しておきます。

- **SPSC キュー**: 20 万イベントのマルチスレッドストレステストで、損失・重複・順序入替が
  一切ないことを確認済み（`tests/selftest.cpp`）
- **`MidiStreamParser`**: データバイトが途中で切れたシステムコモンメッセージが、次の
  メッセージの先頭バイトを食い潰すバグを修正済み（回帰テストあり）
- **`VoiceAllocator::noteOff`**: 同じノートを持つボイスが複数あるとき、最も新しい 1 つしか
  解放していなかったバグを修正済み（回帰テストあり。修正前のコードでテストが落ちることも確認）
- **MIDI の受け渡し経路**: 排出時のイベント破棄、`openPort()` 後のコールバック登録、
  キュー長不足をいずれも修正済み
- **鍵盤側**: 同一 Pi・同一プロセスで KeyLab のみ落とし microKEY は落とさなかったが、これは
  接続速度（フルスピード／ハイスピード）の差であり、鍵盤のファームウェアの問題ではない
