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
| `VoiceAllocator.hpp` | ボイスプールの管理。割当ポリシー（同ノート再打鍵 > 空きボイス > リリース中最古 > 発音中最古のスチール）、サステインペダル、CC120/123 |
| `SineSynthProcessor.{hpp,cpp}` | リファレンス実装。MIDI イベント境界でブロックを分割するサンプル精度処理、パラメータ定義（gain / ADSR / ベンドレンジ）、CC ディスパッチ（CC7・CC64・CC120・CC123）、出力段（ゲイン＋クリップ） |
| `PdSynthProcessor.{hpp,cpp}` | **外部シンセ取り込みの実例**: `external/pd`（CZ 系フェーズディストーション VST3 プラグイン）の DSP コアを無改変でホストするアダプタ。詳細は下記「PD シンセの取り込み」 |

### `src/host/` — ホスト層（＝DAW 相当）

プラットフォーム依存コードを全て閉じ込める層。別バックエンド（JACK ネイティブ、
PipeWire、プラグインラッパ等）への移植ではこの層だけを書き換えます。

| ファイル | 内容 |
|---|---|
| `RtAudioOutput.{hpp,cpp}` | RtAudio の薄いラッパ。**5.x / 6.x の API 差（デバイスのインデックス/ID、例外/エラーコード）をこのファイルだけに隔離**。float32・プレーナ出力、xrun のアトミックなカウント（RT スレッドではログしない） |
| `MidiInput.hpp` | MIDI 入力バックエンドの共通インタフェース（pop・カウンタ・モニタ）。ホスト層はこの面しか見ない |
| `RtMidiInput.{hpp,cpp}` | ALSA シーケンサ経由（RtMidi）の `MidiInput` 実装。既定で全入力ポートに接続（鍵盤＋MIDI コン併用可）。ポートごとに専用 SPSC キュー。満杯時はブロックせず破棄してカウント。ALSA シーケンサがない環境でも落ちない |
| `RawMidiInput.{hpp,cpp}` | カーネル rawmidi 直読みの `MidiInput` 実装（`--midi-raw`）。シーケンサと RtMidi を完全にバイパスする最短経路。poll + ノンブロッキング read → `MidiStreamParser` |
| `StandaloneHost.{hpp,cpp}` | 全体の糊。**3 スレッド（MIDI / オーディオ RT / メイン）の関係と役割はこのヘッダのコメント参照**。ブロックごとに MIDI キューを `MidiBuffer` へ排出して `Processor::process()` を呼ぶ。ドライバが確定したブロックサイズで `prepare()` してからストリーム開始 |
| `ControlInput.hpp` | 物理コントロールの抽象2種: **絶対値** `ControlInput`（ポット等、[0,1] を返す）と**相対値** `RelativeControlInput`（エンコーダ等、前回からのステップ数を返す） |
| `ControlLoop.hpp` | 制御スレッド。絶対値入力は EMA ノイズ除去＋書込み閾値を通して、相対値入力はデテント×ステップ幅で、`Parameter` に書く。両方式を併用可能 |
| `Mcp3008Input.{hpp,cpp}` | MCP3008（SPI 接続 8ch 10bit ADC）の `ControlInput` 実装。spidev 経由。配線図はヘッダのコメント参照 |
| `GpioEncoderInput.{hpp,cpp}` | GPIO ロータリーエンコーダの `RelativeControlInput` 実装。カーネル標準の GPIO キャラクタデバイス (uapi v2) でエッジイベントを受け、直交デコード（`QuadratureDecoder` は単体テスト可能に分離）。内部プルアップ使用で外付け抵抗不要 |
| `ParameterWatcher.hpp` | UI スレッド（LCD・コンソール等）向けの変更検知。全パラメータの変更カウンタをポーリングし「前回から変わったパラメータ」だけを報告。`-v` のパラメータ表示が使用例 |

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
./build/rtsynth --adc 0=gain --adc 1=attack --adc 2=release  # 物理ツマミ(ポット)の割当
./build/rtsynth --enc 17,27=line1_dcw_level1                 # ロータリーエンコーダの割当
./build/rtsynth --synth pd      # PD シンセ (external/pd) で起動
./build/rtsynth -v              # デバッグ: 受信 MIDI・パラメータ変更を表示
```

パラメータの操作手段は **MIDI CC / ADC ポット / GPIO エンコーダの3系統を併用可能**です
（すべて同じ `ParameterSet` に書くので、後から動かした側が勝つ、通常のシンセパネルと
同じ挙動になります）。

MIDI ポート未指定時は "Midi Through" 以外の**すべての入力ポートに接続**します。
鍵盤からのノートと別デバイス（MIDI コン）からの CC を並行して受けられます。
特定のデバイスだけ受けたい場合は `-m <index>` で単一ポートに限定してください。

**MIDI バックエンドは2系統**あります:

| バックエンド | 経路 | 向き不向き |
|---|---|---|
| 既定（ALSA シーケンサ / RtMidi） | USB ドライバ → rawmidi → seq ブリッジ → seq FIFO → RtMidi スレッド | 仮想ポートやソフト音源も見える。経路が長い |
| `--midi-raw hw:X,Y,Z` | USB ドライバ → rawmidi → 直接 read() | **物理デバイス直結の最短経路**。高密度な和音でシーケンサ経由の取りこぼしが疑われるときはこちら |

デバイス ID は `--list` の「Raw MIDI Devices」欄に表示されます（`--midi-raw` は複数指定可）。
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
- **音がプツプツ切れる（crackling）の切り分け**: `-v` で 1 秒ごとに
  `[load] 62% peak / 41% now, 9 voices` のように **DSP 負荷**（1 ブロックの締切に対する
  レンダ所要時間の割合）が表示されます。
  - **80% を超える**なら CPU 不足です。`--voices 8`（または 6）でポリフォニーを下げるか、
    `-b 512` でバッファを増やしてください。pd 音源は 1 ボイスあたりの演算が重く
    （サンプル毎に `pow()`／三角関数）、16 ボイスは Pi では過負荷になりがちです
  - 負荷が低いのに切れる場合はスケジューリング側です。起動時に
    `Audio thread: realtime scheduling active` が出ているか確認してください
    （出ていなければ警告に従って rtprio を設定）
  - リリースが長い音色（既定 0.5 秒）は離鍵後もボイスを保持するため、和音を速く弾くと
    容易に最大ポリフォニーに達します。`[voices]` 表示で確認できます
- **MIDI の取りこぼし（音が鳴りっぱなし）の切り分け手順**:
  1. `-v` で弾いて、消えるのがオンかオフか・警告（dropped / backlog / undecodable /
     read errors）が出るかを確認する。警告が出ずにイベント自体が表示されない場合、
     損失は rtsynth より上流（シーケンサ経路・USB ドライバ・鍵盤本体）
  2. `--midi-raw hw:X,Y,Z`（ID は `--list`）に切り替える。カーネルドライバ直読みの
     最短経路なので、ALSA シーケンサ経路が原因ならこれで解消する
  3. **`--midi-dump` で生バイト列を確認する**（どちらのバックエンドでも可。
     `--midi-raw` 併用時は read() 単位、既定のシーケンサ経路ではメッセージ単位で
     `[read] 90 43 5D` のように表示される）。
     「鍵盤が本当にノートオフを送ったか」を推測せず確定できる。
     オフのバイト（`80 <note> xx` または `90 <note> 00`）が**無い**なら損失は
     rtsynth の外側、**有る**のに発音が続くならシンセ側の問題
  4. rtsynth を完全に外して **`amidi -p hw:X,Y,Z -d`**（ALSA 純正ツール）で同じ演奏を
     ダンプする。ここでもオフが欠けるなら、原因は rtsynth ではなく
     カーネル USB ドライバ／機器側であることが第三者ツールで裏付けられる
  5. **`--midi-raw all`** で全ての raw デバイスを開いて試す。コントロールサーフェス付き
     鍵盤は 1 つの USB エンドポイントに複数の MIDI ケーブルを載せており、Linux では
     別々のサブデバイス（`hw:X,0,0` / `hw:X,0,1` …）として見えるため、
     別ケーブルに流れている可能性を潰せる
  6. **`usbmon` で USB バス上のパケットを直接見る**（最終確認）:
     ```sh
     sudo modprobe usbmon
     lsusb                                    # 鍵盤のバス番号を確認
     sudo cat /sys/kernel/debug/usb/usbmon/<bus>u
     ```
     ノートオフを含む USB パケットが**バス上に存在するのに** rawmidi が渡してこない
     → カーネルの USB-MIDI 解釈（後述）の問題。
     **バス上にも無い** → 機器のファームウェアが本当に送っていない
  7. 機器側が原因のときは、別ケーブル・別 USB ポート・セルフパワー USB ハブ、
     ファームウェア更新を試す

### 参考: Linux 特有の USB-MIDI 取りこぼしについて

「同じ鍵盤が PC では問題ないのに Linux/Raspberry Pi でだけノートオフを落とす」場合、
**USB-MIDI パケットの解釈の違い**が疑わしい典型パターンです。

USB-MIDI 1.0 では MIDI メッセージが 4 バイトのイベントパケット
`[ケーブル番号<<4 | CIN][data0][data1][data2]` に包まれて転送されます。CIN
(Code Index Number) がメッセージ長を示すのですが、**Linux の `snd-usb-audio` は CIN を
厳密に解釈し、想定外の CIN のパケットを黙って捨てます**。一方 Windows / macOS の
クラスドライバは寛容で、CIN が不正でも中身の MIDI バイトを拾うことがあります。
このため「機器のファームウェアが時々おかしな CIN を出す」場合、**PC では問題なく
Linux でだけメッセージが消える**という現象になります
（カーネルにこの種の機器向けクォークテーブルが存在するのはこのためです）。

この場合 rtsynth 側で対処できることはありません（バイトがユーザ空間に届いていない）。
上記 6 の `usbmon` で切り分けたうえで、機器メーカーへの報告か、
別の鍵盤／USB MIDI インタフェース経由での接続が現実的な回避策になります。

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

### ロータリーエンコーダ（GPIO・実装済み）

EC11 等のロータリーエンコーダを GPIO に直結できます。A/B 端子を任意の GPIO へ、
C（コモン）を GND へ。内部プルアップを使うので外付け抵抗は不要です。

```sh
./rtsynth --enc 17,27=line1_dcw_level1 --enc 22,23=volume   # GPIO17/27, GPIO22/23
./rtsynth --enc 17,27=attack --enc-step 0.02                # 1 デテントの変化量
```

実装はカーネル標準の GPIO キャラクタデバイス（`/dev/gpiochip0`、外部ライブラリ不要）で
エッジイベントを受け、専用スレッドで直交デコードします。デテント数は atomic に蓄積され、
`ControlLoop` が相対マッピングとして `Parameter` に反映します。
ポット（絶対値）と違い「現在値からの相対操作」なので、MIDI CC と取り合いになっても
値が飛ばない利点があります。

### LCD / 状態表示への布石（実装済みの仕組み）

「どのパラメータがいつ変わったか」を任意のスレッドから安全に検知する仕組みを
用意してあります:

- `Parameter` は書込みごとに増えるカウンタ（`changeCount()`）を持つ
- `host/ParameterWatcher.hpp` がそれをポーリングし、**前回から変わったパラメータだけ**を
  コールバックで報告する（書き手が MIDI CC / ポット / エンコーダのどれでも検知できる）

LCD を付けるときは、表示スレッドを1本立てて `pollChanges()` で「最後に動いた
パラメータ名＋現在値」を描画するだけです（`ParameterWatcher.hpp` のコメントに雛形あり）。
現在は `-v` オプションが同じ仕組みでコンソールに `[param] line1_dcw_level1 = 0.52` の
ように表示します — これが LCD 表示のコンソール版プレースホルダです。

### ボタン・OLED（未実装の定番構成）

- 押しボタンは GPIO のエッジイベントで読める（`GpioEncoderInput` の実装が参考になる）
- 表示は I2C の SSD1306 OLED / キャラクタ LCD (HD44780 + I2C バックパック) が定番。
  上記 `ParameterWatcher` を UI スレッドから使う

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
| 新しい入力ハード（別 ADC・ボタン等） | `ControlInput` / `RelativeControlInput` を実装（`Mcp3008Input` / `GpioEncoderInput` が実例） |
| LCD / OLED 表示 | UI スレッドから `ParameterWatcher::pollChanges()` で変更を検知して描画（`-v` の `[param]` 表示が実例） |
| パッチ（音色）の保存/読込 | `ParameterSet` を走査してシリアライズ |
| VST3 / JUCE プラグイン化 | `SineSynthProcessor` を `processBlock` から呼ぶ薄いラッパを書く。`core`〜`synth` は無変更 |
| 別オーディオバックエンド | `host/` 層のみ再実装 |
