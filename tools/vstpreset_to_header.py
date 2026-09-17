#!/usr/bin/env python3
"""Turn the pd plugin's .vstpreset files into rtsynth's factory bank.

The plugin and rtsynth share pd's ParamId enumeration (rtsynth's
kNumPdParams *is* Steinberg::Vst::kNumParams), so a preset saved from the
VST3 build is already in rtsynth's parameter order — the conversion is a
container unwrap, not a remapping.

Usage:
    tools/vstpreset_to_header.py presets/cz101 src/synth/PdFactoryPresets.hpp

Preset order and display names come from the file names, which look like
cz101_07_harpsichord.vstpreset -> slot 7, "Harpsichord".
"""
import os
import re
import struct
import sys

# The plugin's component state: int32 version, then one double per
# parameter (PDProcessor::getState).
STATE_VERSION_WITH_ALL_PARAMS = 3


def read_component_chunk(path):
    """The 'Comp' chunk out of a VST3 preset container."""
    data = open(path, 'rb').read()
    if data[:4] != b'VST3':
        raise ValueError(f'{path}: not a VST3 preset')
    list_offset, = struct.unpack_from('<q', data, 40)
    if data[list_offset:list_offset + 4] != b'List':
        raise ValueError(f'{path}: chunk list not found')
    count, = struct.unpack_from('<i', data, list_offset + 4)
    position = list_offset + 8
    for _ in range(count):
        chunk_id = data[position:position + 4]
        offset, size = struct.unpack_from('<qq', data, position + 4)
        if chunk_id == b'Comp':
            return data[offset:offset + size]
        position += 20
    raise ValueError(f'{path}: no component chunk')


def read_values(path):
    chunk = read_component_chunk(path)
    version, = struct.unpack_from('<i', chunk, 0)
    if version != STATE_VERSION_WITH_ALL_PARAMS:
        raise ValueError(f'{path}: state version {version}, expected '
                         f'{STATE_VERSION_WITH_ALL_PARAMS} — re-save it from '
                         f'the current plugin')
    body = chunk[4:]
    if len(body) % 8 != 0:
        raise ValueError(f'{path}: {len(body)} bytes is not whole doubles')
    return list(struct.unpack(f'<{len(body) // 8}d', body))


def display_name(stem):
    """cz101_04_strings_ens1 -> 'Strings Ens 1'."""
    parts = stem.split('_')
    if len(parts) >= 3 and parts[0].lower().startswith('cz') and parts[1].isdigit():
        parts = parts[2:]
    words = []
    for part in parts:
        # a trailing number is a variant number, not part of the word
        match = re.fullmatch(r'([A-Za-z]+)(\d+)', part)
        if match:
            words.extend([match.group(1).capitalize(), match.group(2)])
        else:
            words.append(part.capitalize())
    return ' '.join(words)


def slot_number(stem):
    match = re.search(r'_(\d+)_', stem)
    return int(match.group(1)) if match else 0


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    source_dir, out_path = argv[1], argv[2]

    files = sorted((f for f in os.listdir(source_dir) if f.endswith('.vstpreset')),
                   key=lambda f: (slot_number(os.path.splitext(f)[0]), f))
    if not files:
        print(f'no .vstpreset files in {source_dir}')
        return 1

    presets = []
    width = None
    for name in files:
        stem = os.path.splitext(name)[0]
        values = read_values(os.path.join(source_dir, name))
        if width is None:
            width = len(values)
        elif len(values) != width:
            raise ValueError(f'{name}: {len(values)} values, expected {width}')
        presets.append((display_name(stem), stem, values))

    lines = [
        '// GENERATED FILE — do not edit.',
        '//',
        '// Built by tools/vstpreset_to_header.py from the .vstpreset files in',
        '// presets/cz101/, which are saved straight out of the pd VST3 plugin.',
        '// Add or replace a preset there and re-run the tool; nothing here is',
        '// meant to be readable, and hand edits are lost on the next run.',
        '//',
        '// Values are normalized [0,1], indexed by pd\'s ParamId — the same',
        '// order PdSynthProcessor registers its parameters in.',
        '',
        '#pragma once',
        '',
        'namespace rtsynth::pd_presets {',
        '',
        f'inline constexpr int kNumValues = {width};',
        '',
        'struct FactoryPreset {',
        '    const char* name;',
        '    const double* values;  // kNumValues entries',
        '};',
        '',
    ]

    for index, (name, stem, values) in enumerate(presets):
        lines.append(f'// {stem}')
        lines.append(f'inline constexpr double kValues{index:02d}[kNumValues] = {{')
        for start in range(0, len(values), 6):
            row = ', '.join(repr(v) for v in values[start:start + 6])
            lines.append(f'    {row},')
        lines.append('};')
        lines.append('')

    lines.append('inline constexpr FactoryPreset kFactory[] = {')
    for index, (name, _stem, _values) in enumerate(presets):
        lines.append(f'    {{"{name}", kValues{index:02d}}},')
    lines.append('};')
    lines.append('')
    lines.append('inline constexpr int kFactoryCount ='
                 ' static_cast<int>(sizeof(kFactory) / sizeof(kFactory[0]));')
    lines.append('')
    lines.append('}  // namespace rtsynth::pd_presets')
    lines.append('')

    with open(out_path, 'w') as out:
        out.write('\n'.join(lines))
    print(f'{out_path}: {len(presets)} presets, {width} values each')
    for index, (name, stem, _values) in enumerate(presets):
        print(f'  {index:2d}  {name:<20} ({stem})')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
