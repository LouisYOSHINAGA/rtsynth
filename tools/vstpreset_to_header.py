#!/usr/bin/env python3
"""Turn the pd plugin's .vstpreset files into rtsynth's factory bank.

The plugin and rtsynth share pd's ParamId enumeration (rtsynth's
kNumPdParams *is* Steinberg::Vst::kNumParams), so a preset saved from the
VST3 build is already in rtsynth's parameter order — the conversion is a
container unwrap, not a remapping.

Usage:
    tools/vstpreset_to_header.py src/synth/PdPresets.hpp \
        Factory=presets/cz101 User=presets/user

Each <Name>=<dir> becomes one table in the header (k<Name> and
k<Name>Count), in the order given — the instrument registers them in that
order, so the user slots stay at the end of the bank.

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


def read_group(source_dir):
    """Every preset in one directory, in file-name order."""
    files = sorted((f for f in os.listdir(source_dir) if f.endswith('.vstpreset')),
                   key=lambda f: (slot_number(os.path.splitext(f)[0]), f))
    if not files:
        raise ValueError(f'no .vstpreset files in {source_dir}')
    presets = []
    for name in files:
        stem = os.path.splitext(name)[0]
        values = read_values(os.path.join(source_dir, name))
        presets.append((display_name(stem), stem, values))
    return presets


def main(argv):
    if len(argv) < 3 or any('=' not in arg for arg in argv[2:]):
        print(__doc__)
        return 2
    out_path = argv[1]

    groups = []
    width = None
    for arg in argv[2:]:
        group_name, source_dir = arg.split('=', 1)
        presets = read_group(source_dir)
        for _display, stem, values in presets:
            if width is None:
                width = len(values)
            elif len(values) != width:
                raise ValueError(f'{stem}: {len(values)} values, expected {width}')
        groups.append((group_name, source_dir, presets))

    lines = [
        '// GENERATED FILE — do not edit.',
        '//',
        '// Built by tools/vstpreset_to_header.py from the .vstpreset files',
        '// under presets/, which are saved straight out of the pd VST3 plugin.',
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
        'struct Preset {',
        '    const char* name;',
        '    const double* values;  // kNumValues entries',
        '};',
        '',
    ]

    for group_name, source_dir, presets in groups:
        lines.append(f'// --- {group_name}: {source_dir} '
                     + '-' * max(0, 46 - len(group_name) - len(source_dir)))
        lines.append('')
        for index, (_name, stem, values) in enumerate(presets):
            lines.append(f'// {stem}')
            lines.append(f'inline constexpr double k{group_name}Values'
                         f'{index:02d}[kNumValues] = {{')
            for start in range(0, len(values), 6):
                row = ', '.join(repr(v) for v in values[start:start + 6])
                lines.append(f'    {row},')
            lines.append('};')
            lines.append('')
        lines.append(f'inline constexpr Preset k{group_name}[] = {{')
        for index, (name, _stem, _values) in enumerate(presets):
            lines.append(f'    {{"{name}", k{group_name}Values{index:02d}}},')
        lines.append('};')
        lines.append(f'inline constexpr int k{group_name}Count ='
                     f' static_cast<int>(sizeof(k{group_name}) / sizeof(k{group_name}[0]));')
        lines.append('')

    lines.append('}  // namespace rtsynth::pd_presets')
    lines.append('')

    with open(out_path, 'w') as out:
        out.write('\n'.join(lines))
    print(f'{out_path}: {width} values each')
    slot = 0
    for group_name, _source_dir, presets in groups:
        for name, stem, _values in presets:
            print(f'  {slot:2d}  {name:<20} ({group_name}: {stem})')
            slot += 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
