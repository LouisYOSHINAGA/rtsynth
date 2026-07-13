# rtsynth — VST ライク構造のスタンドアロンシンセ (RtAudio / RtMidi)

Raspberry Pi 上でのハードシンセ開発の起点となることを想定した、**VST プラグインに近い構造**の
ポリフォニックシンセです。音源は 16 ボイスの sine シンセですが、コードベース全体が
「音源・ホスト・DSP 部品を差し替え可能なテンプレート」として書かれています。

- 依存: RtAudio (5.x / 6.x 両対応), RtMidi, CMake, C++20
- 旧実装（`gen.cpp` / `rcv.cpp` の最小構成）からのリファクタリングです

## ディレクトリ構成

```
.
├── CMakeLists.txt         # rtsynth_core (静的ライブラリ) / rtsynth (実行形式) / rtsynth_selftest
├── src/
│   ├── core/              # [層1] プラグイン基盤 — 完全プラットフォーム非依存
│   ├── dsp/               # [層2] DSP 部品 — core にのみ依存
│   ├── synth/             # [層3] 楽器本体 — 「プラグイン」に相当
│   ├── host/              # [層4] ホスト — 「DAW」に相当（RtAudio/RtMidi はここだけ）
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

### `src/synth/` — 楽器本体層（＝プラグイン）

`Processor` を実装した「楽器そのもの」。**新しい音源を作るときに読む・真似る場所**です。

| ファイル | 内容 |
|---|---|
| `Voice.hpp` | 1 ボイス（オシレータ＋エンベロープ＋ノート状態）。ボイスの状態遷移（free → held → releasing → free、スチール時のフェード→保留ノート発音）を図解コメント付きで実装 |
| `VoiceAllocator.hpp` | ボイスプールの管理。割当ポリシー（同ノート再打鍵 > 空きボイス > リリース中最古 > 発音中最古のスチール）、サステインペダル、CC120/123 |
| `SineSynthProcessor.{hpp,cpp}` | リファレンス実装。MIDI イベント境界でブロックを分割するサンプル精度処理、パラメータ定義（gain / ADSR / ベンドレンジ）、CC ディスパッチ（CC7・CC64・CC120・CC123）、出力段（ゲイン＋クリップ） |

### `src/host/` — ホスト層（＝DAW 相当）

プラットフォーム依存コードを全て閉じ込める層。別バックエンド（JACK ネイティブ、
PipeWire、プラグインラッパ等）への移植ではこの層だけを書き換えます。

| ファイル | 内容 |
|---|---|
| `RtAudioOutput.{hpp,cpp}` | RtAudio の薄いラッパ。**5.x / 6.x の API 差（デバイスのインデックス/ID、例外/エラーコード）をこのファイルだけに隔離**。float32・プレーナ出力、xrun のアトミックなカウント（RT スレッドではログしない） |
| `RtMidiInput.{hpp,cpp}` | RtMidi の薄いラッパ。コールバックスレッドで `MidiEvent` にデコードし SPSC キューへ。キュー満杯時はブロックせず破棄してカウント。ALSA シーケンサがない環境でも落ちない |
| `StandaloneHost.{hpp,cpp}` | 全体の糊。**3 スレッド（MIDI / オーディオ RT / メイン）の関係と役割はこのヘッダのコメント参照**。ブロックごとに MIDI キューを `MidiBuffer` へ排出して `Processor::process()` を呼ぶ。ドライバが確定したブロックサイズで `prepare()` してからストリーム開始 |

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

cmake -S . -B build
cmake --build build -j4
```

RtAudio 5.x (bullseye / bookworm) と 6.x (trixie 以降) のどちらでもビルドできます。

## 使い方

```sh
./build/rtsynth --list          # オーディオデバイスと MIDI ポートの一覧
./build/rtsynth                 # 既定デバイス・自動 MIDI ポートで起動
./build/rtsynth -d 2 -m 1 -b 128 -g 0.3
./build/rtsynth --api pulse     # バックエンドを明示指定 (alsa / pulse / jack / ...)
./build/rtsynth --param attack=0.001 --param release=0.1   # パラメータ上書き
```

MIDI ポート未指定時は "Midi Through" 以外の最初のポートに自動接続します。
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

## 拡張ガイド（どこに何を足すか）

| やりたいこと | 触る場所 |
|---|---|
| 新しい音源方式（例: フェーズディストーション） | `synth/` に `Processor` 実装を追加し、`main.cpp` で差し替え。ホスト層は変更不要 |
| フィルタ・LFO 等の部品追加 | `dsp/` に部品を追加し `Voice` に組み込む |
| ハード UI（エンコーダ・OLED 等） | `host/` に UI スレッドを追加し、`ParameterSet` 経由で書き込む（アトミックなのでロック不要） |
| パッチ（音色）の保存/読込 | `ParameterSet` を走査してシリアライズ |
| VST3 / JUCE プラグイン化 | `SineSynthProcessor` を `processBlock` から呼ぶ薄いラッパを書く。`core`〜`synth` は無変更 |
| 別オーディオバックエンド | `host/` 層のみ再実装 |
