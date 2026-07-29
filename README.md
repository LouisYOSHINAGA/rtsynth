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

第1部だけ読めば演奏できます。第2部は改造・移植・不具合調査のときに読んでください。

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

### (2) オーディオ出力

Pi 本体のヘッドフォン端子は PWM 生成で品質が低いため、ハードシンセでは I2S DAC
（PCM5102A 系の安価なモジュール、HiFiBerry DAC+ 等）を推奨します。

```
# /boot/firmware/config.txt
dtparam=audio=off            # 内蔵オーディオを無効化（デバイス一覧が整理される）
dtoverlay=hifiberry-dac      # PCM5102A 系はこのオーバーレイで動くものが多い
```

再起動後 `./build/rtsynth --list` にカードが現れるので `-d` で指定します。**コード変更は不要**です。

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

対話入力を一切しないので、そのまま systemd unit にできます。

```ini
# /etc/systemd/system/rtsynth.service
[Unit]
Description=rtsynth
After=sound.target

[Service]
ExecStart=/home/pi/rtsynth/build/rtsynth -d <id> --adc 0=gain
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
./build/rtsynth -d 2 -b 128 -g 0.3
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
| `-d, --device <id>` | オーディオ出力デバイス |
| `-m, --midi <index>` | MIDI 入力を 1 ポートに限定（既定は全ポート接続） |
| `-r, -b, -g` | サンプルレート / バッファサイズ / マスターゲイン |
| `-p, --param <id=v>` | パラメータの初期値（複数指定可） |
| `--voices <n>` | ポリフォニー上限 |
| `--adc`, `--enc` | 物理コントロールの割当（[1.6](#16-物理コントロールツマミエンコーダ)） |

`--param` に不明な ID を渡すと利用可能なパラメータ一覧が表示されます。

## 1.4 MIDI 入力

ポート未指定時は "Midi Through" 以外の**すべての入力ポートに接続**します。
鍵盤からのノートと、別デバイス（MIDI コン）からの CC を並行して受けられます。
特定のデバイスだけ受けたい場合のみ `-m <index>` で限定してください。

起動時に「見つかったポート」と「実際に接続したポート」の両方を表示します。
コントロールサーフェス付きの鍵盤は `KeyLab mkII 61 KBD` / `... CTRL` のように
複数ポートに分かれることがあるため、鳴らない場合はまずここを確認してください。

## 1.5 対応 MIDI メッセージ

| メッセージ | 動作 |
|---|---|
| Note On / Off | ベロシティ対応（2 乗カーブ）、ADSR 付き発音 |
| Pitch Bend | 既定 ±2 半音（`bend_range` パラメータで 0〜24 半音） |
| CC7 (Volume) | マスターゲイン |
| CC64 (Sustain) | サステインペダル（sine のみ、pd は未対応） |
| CC120 (All Sound Off) | 即時全消音 |
| CC123 (All Notes Off) | 全ノートリリース |

pd 音源はさらに CC3 / CC14–30 / CC46–62 / CC102–118 で EG を編集できます（[1.7](#17-pd-シンセ)）。

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

### 表示（LCD / OLED）

現状の main ブランチには表示デバイスのドライバは入っていませんが、**表示に必要な
変更検知の仕組みは実装済み**です（`ParameterWatcher`、[2.5](#25-拡張ガイド)）。
`-v` を付けると同じ仕組みでコンソールに `[param] line1_dcw_level1 = 0.52` と表示され、
これが LCD 表示のプレースホルダになります。

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

**MIDI CC** は CZ 実機と同じ「EG のツマミ 1 系統を Line1/Line2 で共有し、CC3 で編集対象を
切り替える」設計です。

| CC | 内容 |
|---|---|
| CC7 | Volume |
| CC3 | 編集対象ライン切替（値 <64 → Line1、≥64 → Line2） |
| CC14–30 | 編集対象ラインの DCO EG（レート1–8・レベル1–7・サステイン・エンド） |
| CC46–62 | 同 DCW EG |
| CC102–118 | 同 DCA EG |

現在の編集対象は `cc_edit_line` パラメータで確認・指定できます。

**使用上の注意**:

- プラグインの素の状態は無音（エディタが値を入れる前提）のため、スタンドアロンでは
  デフォルト値を「鳴る初期パッチ」（Line1 ノコギリ波・高速アタック DCA・DCW スイープ）に
  してあります
- サステインペダル (CC64) は pd の Voice が未対応のため効きません
- 波形切替パラメータの変更はオーディオスレッド上でジェネレータを再生成します
  （pd 本体と同じ挙動）。演奏中の頻繁な波形自動化は避けてください
- **EG の設定によっては音が止まらなくなります**（pd 側の既知の不具合、[2.6](#26-既知の問題)）

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
| `RtMidiInput.{hpp,cpp}` | ALSA シーケンサ経由（RtMidi）の実装。既定で全入力ポートに接続。ポートごとに専用 SPSC キュー（RtMidi はポートごとにコールバックスレッドを立てるため、SPSC の単一生産者制約を守る配置）。コールバックは `openPort()` の**前**に登録する（間に来たイベントを落とさないため） |
| `RawMidiInput.{hpp,cpp}` | カーネル rawmidi 直読みの実装（`--midi-raw`）。シーケンサと RtMidi を完全にバイパスする最短経路。poll + ノンブロッキング read → `MidiStreamParser` |
| `StandaloneHost.{hpp,cpp}` | 全体の糊。**3 スレッド（MIDI / オーディオ RT / メイン）の関係はこのヘッダのコメント参照**。ブロックごとに MIDI キューを `MidiBuffer` へ排出。バッファ満杯時は**イベントを捨てずに排出を止める**（次ブロックで続きを読む） |
| `ControlInput.hpp` | 物理コントロールの抽象 2 種: **絶対値** `ControlInput`（ポット等、[0,1] を返す）と**相対値** `RelativeControlInput`（エンコーダ等、前回からのステップ数を返す） |
| `ControlLoop.hpp` | 制御スレッド。絶対値入力は EMA ノイズ除去＋書込み閾値、相対値入力はデテント×ステップ幅でパラメータに書く。両方式を併用可能 |
| `Mcp3008Input.{hpp,cpp}` | MCP3008（SPI 8ch 10bit ADC）の `ControlInput` 実装。配線図はヘッダのコメント参照 |
| `GpioEncoderInput.{hpp,cpp}` | GPIO ロータリーエンコーダの `RelativeControlInput` 実装。GPIO キャラクタデバイス (uapi v2) でエッジイベントを受け、直交デコード（`QuadratureDecoder` は単体テスト可能に分離） |
| `ParameterWatcher.hpp` | UI スレッド（LCD・コンソール等）向けの変更検知。全パラメータの変更カウンタをポーリングし「前回から変わったパラメータ」だけを報告 |

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
ポリフォニー上限・pd 音源と CC マッピング・エンコーダの直交デコード・
`ParameterWatcher`・`MidiStreamParser` の各ケース・SPSC キューの 20 万イベント
マルチスレッドストレステスト。CI にそのまま載せられます。

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

## 2.5 拡張ガイド

| やりたいこと | 触る場所 |
|---|---|
| 新しい音源方式 | `synth/` に `Processor` 実装を追加し、`main.cpp` の `--synth` 分岐に登録。ホスト層は変更不要（外部リポジトリの取り込みは `PdSynthProcessor` + submodule + shim が実例） |
| フィルタ・LFO 等の部品追加 | `dsp/` に部品を追加し `Voice` に組み込む |
| 新しい入力ハード（別 ADC・ボタン等） | `ControlInput` / `RelativeControlInput` を実装（`Mcp3008Input` / `GpioEncoderInput` が実例、約 60 行）。押しボタンも `GpioEncoderInput` と同じ GPIO エッジイベントで読める |
| LCD / OLED 表示 | UI スレッドから `ParameterWatcher::pollChanges()` で「前回から変わったパラメータ」を受け取って描画（雛形は `ParameterWatcher.hpp` のコメント、動く実例は `-v` の `[param]` 表示）。I2C の SSD1306 OLED / HD44780+I2C バックパックが定番 |
| パッチ（音色）の保存/読込 | `ParameterSet` を走査してシリアライズ |
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

## 2.7 デバッグ手順

### 診断オプション

| オプション | 出力 |
|---|---|
| `-v, --verbose` | 受信 MIDI イベント（ポート名付き）、パラメータ変更、1 秒ごとの DSP 負荷とボイス数、各種警告カウンタ。表示は**メインスレッド**が行うため音は途切れない |
| `--midi-dump` | 受信した生 MIDI バイト列を `[read] 90 43 5D` 形式で表示（`--midi-raw` 併用時は read() 単位、既定のシーケンサ経路ではメッセージ単位でグルーピング） |
| `--midi-raw <dev>` | ALSA シーケンサと RtMidi を完全にバイパスし、カーネル rawmidi を直読みする。`--list` の "Raw MIDI Devices" 欄の ID を渡す。複数指定可、`all` で全デバイス |

### 音がプツプツ切れる（crackling）

`-v` の `[load] 62% peak / 41% now, 9 voices` が DSP 負荷（1 ブロックの締切に対する
レンダ所要時間の割合）です。

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
