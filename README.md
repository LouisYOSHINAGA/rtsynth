# rtsynth — VST ライク構造のスタンドアロンシンセ (RtAudio / RtMidi)

Raspberry Pi 上でのハードシンセ開発の起点となることを想定した、**VST プラグインに近い構造**の
ポリフォニックシンセです。音源は 16 ボイスの sine シンセですが、コードベース全体が
「音源・ホスト・DSP 部品を差し替え可能なテンプレート」として書かれています。

- 依存: RtAudio (5.x / 6.x 両対応), RtMidi, CMake, C++20
- 旧実装（`gen.cpp` / `rcv.cpp` の最小構成）からのリファクタリングです

## ディレクトリ構成

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
| `Parameters.hpp` | 正規化値 [0,1] ↔ 実値のパラメータ。`std::atomic<float>` なので制御スレッドから書き、オーディオスレッドから読める（ロック不要） | `IEditController` / `AudioProcessorValueTreeState` |
| `SpscRingBuffer.hpp` | ロックフリー SPSC リングバッファ（MIDI スレッド → オーディオスレッドの受け渡し用、約 40 行） | — |

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
| `VoiceAllocator.hpp` | ボイスプールの管理。割当ポリシー（同ノート再打鍵 > 空きボイス > リリース中最古 > 発音中最古のスチール）、サステインペダル、CC120/123 |
| `SineSynthProcessor.{hpp,cpp}` | リファレンス実装。MIDI イベント境界でブロックを分割するサンプル精度処理、パラメータ定義（gain / ADSR / ベンドレンジ）、CC ディスパッチ（CC7・CC64・CC120・CC123）、出力段（ゲイン＋クリップ） |
| `PdSynthProcessor.{hpp,cpp}` | **外部シンセ取り込みの実例**: `external/pd`（CZ 系フェーズディストーション VST3 プラグイン）の DSP コアを無改変でホストするアダプタ。詳細は下記「PD シンセの取り込み」 |

### `src/host/` — ホスト層（＝DAW 相当）

プラットフォーム依存コードを全て閉じ込める層。別バックエンド（JACK ネイティブ、
PipeWire、プラグインラッパ等）への移植ではこの層だけを書き換えます。

| ファイル | 内容 |
|---|---|
| `RtAudioOutput.{hpp,cpp}` | RtAudio の薄いラッパ。**5.x / 6.x の API 差（デバイスのインデックス/ID、例外/エラーコード）をこのファイルだけに隔離**。float32・プレーナ出力、xrun のアトミックなカウント（RT スレッドではログしない） |
| `RtMidiInput.{hpp,cpp}` | RtMidi の薄いラッパ。既定で全入力ポートに接続（鍵盤＋MIDI コン併用可）。RtMidi はポートごとにコールバックスレッドを持つため、SPSC の単一プロデューサ制約を守るべく**ポートごとに専用キュー**を持ち、オーディオスレッドが全キューを排出する。満杯時はブロックせず破棄してカウント。ALSA シーケンサがない環境でも落ちない |
| `StandaloneHost.{hpp,cpp}` | 全体の糊。**3 スレッド（MIDI / オーディオ RT / メイン）の関係と役割はこのヘッダのコメント参照**。ブロックごとに MIDI キューを `MidiBuffer` へ排出して `Processor::process()` を呼ぶ。ドライバが確定したブロックサイズで `prepare()` してからストリーム開始 |
| `ControlInput.hpp` | 物理コントロール群（ツマミ・スライダ）の抽象。チャンネルごとに [0,1] を返すだけの小さなインタフェース |
| `ControlLoop.hpp` | 制御スレッド。`ControlInput` を一定周期（既定 100 Hz）でポーリングし、EMA によるノイズ除去＋書込み閾値（10bit ADC の約 2LSB）を通して `Parameter` に書く |
| `Mcp3008Input.{hpp,cpp}` | MCP3008（SPI 接続 8ch 10bit ADC）の `ControlInput` 実装。spidev 経由。配線図はヘッダのコメント参照 |

### `src/main.cpp` — エントリポイント

CLI オプションの解釈、`SineSynthProcessor` と `StandaloneHost` の接続、
SIGINT/SIGTERM での安全な終了、RT スレッドが記録したカウンタ（xrun / MIDI ドロップ）の
メインスレッドからの報告。**どの楽器をビルドするかを決める唯一の場所**なので、
音源を差し替えるときはここの 1 行を変えます。

### `tests/selftest.cpp` — オフラインセルフテスト

`Processor` 抽象の恩恵で、オーディオデバイスなしに DSP を検証できます
（発音・リリース・クリップ・ボイススチールの安定性・スチール中ノートオフの
スタックノート回帰・サステインペダル・ピッチベンド等 15 項目）。CI にそのまま載せられます。

## データフロー / スレッドモデル

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

## ビルド (Raspberry Pi OS / Debian 系)

```sh
sudo apt install cmake g++ pkg-config librtaudio-dev librtmidi-dev

git clone --recursive <このリポジトリ>   # PD シンセごと取得
# 既にクローン済みなら: git submodule update --init

cmake -S . -B build
cmake --build build -j4
```

サブモジュール `external/pd` が無い場合も sine シンセのみでビルドは通ります
（configure 時にその旨のメッセージが出ます）。

RtAudio 5.x (bullseye / bookworm) と 6.x (trixie 以降) のどちらでもビルドできます。

## 使い方

```sh
./build/rtsynth --list          # オーディオデバイスと MIDI ポートの一覧
./build/rtsynth                 # 既定デバイス・自動 MIDI ポートで起動
./build/rtsynth -d 2 -m 1 -b 128 -g 0.3
./build/rtsynth --api pulse     # バックエンドを明示指定 (alsa / pulse / jack / ...)
./build/rtsynth --param attack=0.001 --param release=0.1   # パラメータ上書き
./build/rtsynth --adc 0=gain --adc 1=attack --adc 2=release  # 物理ツマミの割当
./build/rtsynth --synth pd      # PD シンセ (external/pd) で起動
./build/rtsynth -v              # デバッグ: 受信 MIDI を表示・バックエンド警告も表示
```

MIDI ポート未指定時は "Midi Through" 以外の**すべての入力ポートに接続**します。
鍵盤からのノートと別デバイス（MIDI コン）からの CC を並行して受けられます。
特定のデバイスだけ受けたい場合は `-m <index>` で単一ポートに限定してください。
オーディオ API 未指定時は、デバイスを持つ **ALSA（直結）を優先**して選択します（下記「レイテンシ」参照）。
`--param` は `ParameterSet` に登録された任意のパラメータを起動時に設定できます
（不明な ID を渡すと利用可能な一覧が表示されます）。

セルフテスト（オーディオデバイス不要・CI 可）:

```sh
./build/rtsynth_selftest        # または ctest --test-dir build
```

## 対応 MIDI メッセージ

| メッセージ | 動作 |
|---|---|
| Note On / Off | ベロシティ対応（2 乗カーブ）、ADSR 付き発音 |
| Pitch Bend | 既定 ±2 半音（`bend_range` パラメータで 0〜24 半音） |
| CC7 (Volume) | マスターゲイン |
| CC64 (Sustain) | サステインペダル |
| CC120 (All Sound Off) | 即時全消音 |
| CC123 (All Notes Off) | 全ノートリリース |

## PD シンセの取り込み（external/pd サブモジュール）

[LouisYOSHINAGA/pd](https://github.com/LouisYOSHINAGA/pd)（CZ 系フェーズディストーションの
VST3 プラグイン）の DSP コアを、**pd リポジトリ側は一切変更せずに** rtsynth の楽器として
ホストしています。外部プロジェクトの音源を取り込むときのテンプレートでもあります。

**仕組み** — pd の DSP コア（`pd.{h,cpp}` / `eg.{h,cpp}` / `voice.{h,cpp}` / `const.h`）の
VST3 SDK への依存は `vsttypes.h` の型エイリアス（`ParamValue` = double 等）だけです。
そこで:

1. `external/pd` に pd を git submodule として置く
2. `src/vst3shim/pluginterfaces/vst/vsttypes.h` に必要最小限の型定義を用意し、
   インクルードパスで SDK の代わりに解決させる（プラグインとしてビルドするときは
   本物の SDK が解決されるので、pd 側は両対応のまま）
3. `synth/PdSynthProcessor` が VST3 の配管（processor.cpp のうち SDK 依存部分）に
   相当するホスト側ロジック — パラメータのディスパッチ、ボイスプール（最古スチール）、
   mono (SOLO) のラストノート優先、DETUNE、ピッチベンド、出力ミックス — を再実装する

**使い方**:

```sh
./build/rtsynth --synth pd
./build/rtsynth --synth pd --param line_select=0.67 --param detune_fine=0.6   # 1+1' でデチューン
./build/rtsynth --synth pd --adc 0=volume --adc 1=line1_dcw_level1            # DCW をツマミで
```

**パラメータ命名** — プラグインの全パラメータを正規化値 [0,1]（プラグインと同じ
エンコーディング）で `ParameterSet` に登録しています。`--param` に不明な ID を渡すと
全一覧が表示されます。命名規則:

| ID | 内容 |
|---|---|
| `volume`, `pitch_bend`, `mono` | システム系 |
| `line_select` | 0=Line1, 0.33=Line2, 0.67=1+1', 1.0=1+2' |
| `detune_octave` / `detune_note` / `detune_fine` | 0.5 が中央（±0） |
| `line{1,2}_wave{1,2}` | 波形選択（8 波形 / 2nd は Off+8） |
| `line{1,2}_{dco,dcw,dca}_rate{1..8}` | 8 段 EG のレート |
| `line{1,2}_{dco,dcw,dca}_level{1..7}` | 8 段 EG のレベル |
| `line{1,2}_{dco,dcw,dca}_{sustain,end}` | サステイン点・エンド点 |

**MIDI CC マッピング** — pd の VST3 版では、CC→パラメータの変換は DSP 側
（processor.cpp）ではなく **ホスト側**（controller.cpp の `getMidiControllerAssignment`）が
行います。スタンドアロンにはそのホストが無いため、`PdSynthProcessor::handleEvent()` で
同じマッピングを再現しています:

| CC | 内容 |
|---|---|
| CC7 | Volume |
| CC3 | CC Edit Line 切替（値 <64 → Line1 を編集対象、≥64 → Line2） |
| CC14–30 | 編集対象ラインの DCO EG（レート1–8・レベル1–7・サステイン・エンド） |
| CC46–62 | 同 DCW EG |
| CC102–118 | 同 DCA EG |
| CC120 / CC123 | All Sound Off / All Notes Off |

ハード側の CZ 実機同様、EG のツマミ 1 系統を Line1/Line2 で共有し、CC3（パネルの
LINE SELECT に相当）で編集対象を切り替える設計です。`cc_edit_line` パラメータで
現在の編集対象を確認・`--param` での直接指定もできます。

**注意点**:

- プラグインの素の状態は無音（エディタが値を入れる前提）のため、スタンドアロンでは
  デフォルト値を「鳴る初期パッチ」（Line1 ノコギリ波・高速アタック DCA・DCW スイープ）
  にしてあります
- サステインペダル (CC64) は pd の Voice が対応していないため未対応です
- 波形切替パラメータの変更はオーディオスレッド上でジェネレータを再生成（アロケーション）
  します（pd 本体と同じ挙動）。演奏中の頻繁な波形自動化は避けてください
- pd を更新するときは `cd external/pd && git pull` 後に rtsynth 側をコミット
  （サブモジュールは特定コミットに固定されます）

## Raspberry Pi での運用メモ

- **レイテンシと ALSA 直結**: RtAudio の自動選択（`UNSPECIFIED`）は JACK → Pulse → ALSA の
  順に探索するため、デスクトップ環境では PulseAudio / PipeWire が選ばれます。この経路は
  サウンドサーバ側でバッファリングされ（RtAudio 5.x の Pulse バックエンドは再生側バッファ長を
  サーバ任せにするため、指定したバッファサイズが効きません）、数十 ms 単位のレイテンシが
  加わります。本実装の既定はこれを避けて **ALSA 直結** です。起動時に
  `Audio stream started: <API名>, ...` と表示されるので、意図した API か確認できます。
  Pulse 経由にしたい場合のみ `--api pulse` を指定してください
- **バッファサイズ**: `-b` で変更（256 = 約 5.8 ms、128 = 約 2.9 ms @44.1kHz）。
  xrun 警告が出るなら増やす
- **RT スケジューリング**: `RTAUDIO_SCHEDULE_REALTIME` を指定済み。
  ユーザに RT 権限が必要（`/etc/security/limits.d/` に `@audio - rtprio 95` 等）
- **CPU ガバナ**: `sudo cpufreq-set -g performance` で周波数変動によるドロップアウトを回避
- **自動起動**: 対話入力なしで動くので systemd unit にそのまま書ける
  （`ExecStart=/path/to/rtsynth -d <id>`）
- **`snd_pcm_open error ... Unknown error 524` について**: RtAudio がデバイス列挙時に
  開けないデバイス（音声シンクの無い HDMI 出力など）を試した際の警告で、**無害**です
  （そのデバイスが一覧からスキップされるだけ）。既定では非表示にしてあり、
  `--verbose` 指定時のみ表示されます
- **MIDI デバッグ**: `-v / --verbose` で受信した MIDI イベント（ノート/CC/ベンド）を
  ポート名付きで表示。表示は RT スレッドではなくメインスレッドが行うため、
  デバッグ中も音は途切れません

## ハードウェア連携（ツマミ・スライダ・DAC）

### I2S DAC（音の出口）

Pi 本体のヘッドフォン端子は PWM 生成で品質が低いため、ハードシンセでは I2S DAC
（PCM5102A 系の安価なモジュール、HiFiBerry DAC+ 等）を推奨します。

```
# /boot/firmware/config.txt
dtparam=audio=off            # 内蔵オーディオを無効化（デバイス一覧が整理される）
dtoverlay=hifiberry-dac      # PCM5102A 系はこのオーバーレイで動くものが多い
```

再起動後 `./rtsynth --list` にカードが現れるので `-d` で指定します。コード変更は不要です。

### アナログツマミ / スライダ（MCP3008 ADC）

Pi には ADC がないため、SPI 接続の MCP3008（8ch 10bit）を使うのが定番です。
配線は `src/host/Mcp3008Input.hpp` のコメント参照（ポットは 3.3V–GND 間、ワイパを CH0–7 へ）。
`raspi-config` で SPI を有効化したうえで:

```sh
./rtsynth --adc 0=gain --adc 1=attack --adc 2=decay --adc 3=sustain --adc 4=release
```

チャンネル→パラメータの対応は起動時に自由に組めます。ポーリングは制御スレッド
（`ControlLoop`、100 Hz）で行われ、EMA＋書込み閾値でノイズを抑えた値が
アトミックに `Parameter` へ書かれるため、オーディオスレッドとのロックは不要です。
16bit が欲しい場合や I2C で済ませたい場合は ADS1115 用の `ControlInput` 実装を
足すだけです（`Mcp3008Input` を参考に約 60 行）。

### ロータリーエンコーダ・ボタン・OLED

- エンコーダ/ボタンは GPIO（libgpiod）で読み、`ControlInput` または独自の制御スレッドから
  `ParameterSet` へ書く（相対操作なのでパッチ切替やメニュー操作に向く）
- 表示は I2C の SSD1306 OLED が定番。`ParameterSet` を走査して現在値を描画する
  UI スレッドを `host/` に足す

### 自動起動（systemd）

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

## 拡張ガイド（どこに何を足すか）

| やりたいこと | 触る場所 |
|---|---|
| 新しい音源方式 | `synth/` に `Processor` 実装を追加し、`main.cpp` の `--synth` 分岐に登録。ホスト層は変更不要（外部リポジトリの取り込みは `PdSynthProcessor` + submodule + shim が実例） |
| フィルタ・LFO 等の部品追加 | `dsp/` に部品を追加し `Voice` に組み込む |
| ハード UI（エンコーダ・OLED 等） | `host/` に UI スレッドを追加し、`ParameterSet` 経由で書き込む（アトミックなのでロック不要） |
| パッチ（音色）の保存/読込 | `ParameterSet` を走査してシリアライズ |
| VST3 / JUCE プラグイン化 | `SineSynthProcessor` を `processBlock` から呼ぶ薄いラッパを書く。`core`〜`synth` は無変更 |
| 別オーディオバックエンド | `host/` 層のみ再実装 |
