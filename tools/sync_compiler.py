#!/usr/bin/env python3
"""Re-import the Cypher compiler (compiler/) from an upstream NeuG checkout.

compiler/ started as the parser, binder, index rewriters, planner and gopt of
NeuG (IncGQ) and is now maintained as part of CICO.  This tool is only needed
to pull later upstream NeuG changes: it copies the translation units listed in
``compiler/upstream/sources.txt`` plus their header closure from a *built*
NeuG checkout into ``compiler/``, refreshes the small vendored libraries in
``third_party/``, re-applies the CICO changes in ``compiler/upstream/patches``
(order in ``series``) and rewrites ``compiler/upstream/provenance.json``.

Local edits made directly in compiler/ are overwritten: turn them into a patch
under compiler/upstream/patches first (or review the result with git diff).

The header closure is the union of the NeuG build's dependency files
(``*.o.d``) and a fixpoint scan of ``#include "..."`` directives in the copied
files, the engine and the execution stubs.

Usage (from the CICO root):
    python3 tools/sync_compiler.py --neug-source /path/to/IncGQ [--neug-build DIR] [--clean]
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
COMPILER = ROOT / 'compiler'
UPSTREAM = COMPILER / 'upstream'
THIRD_PARTY = ROOT / 'third_party'

# Libraries compiled from vendored sources (copied whole, minus VCS data).
SOURCE_LIBS = {
    'antlr4_runtime': ['CMakeLists.txt', 'LICENSE', 'src'],
    'antlr4_cypher': ['CMakeLists.txt', 'cypher_lexer.cpp', 'cypher_parser.cpp', 'include'],
    're2': None,          # whole directory
    'utf8proc': None,
}
# Header-only libraries: (source path, destination, entries to copy).
HEADER_LIBS = [
    ('rapidjson', 'rapidjson', ['include', 'license.txt']),
    ('HowardHinnant/date', 'date', ['include', 'LICENSE.txt']),
    ('expected', 'expected', ['tl']),
    ('flat_hash_map', 'flat_hash_map', ['flat_hash_map.hpp', 'bytell_hash_map.hpp',
                                        'unordered_map.hpp', 'LICENSE_1_0.txt']),
    ('pcg', 'pcg', None),
    ('fast_float', 'fast_float', ['include']),
    ('parallel-hashmap', 'parallel_hashmap', ['parallel_hashmap', 'LICENSE']),
]
# Configured headers / generated sources from the NeuG build that are plain
# constants for the build options NeuG uses; copied verbatim.
GENERATED = {
    'include/neug/config.h': 'generated/include/neug/config.h',
    'include/neug/version.h': 'generated/include/neug/version.h',
    'src/compiler/neug/compiler/common/system_config.h':
        'generated/include/neug/compiler/common/system_config.h',
    'src/compiler/extension/codegen/include/generated_extension_loader.h':
        'generated/include/generated_extension_loader.h',
    'src/compiler/extension/codegen/generated_extension_loader.cpp':
        'generated/src/generated_extension_loader.cpp',
}
PROTO_EXCLUDE = {'http_svc.proto'}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def git_blob(data):
    return hashlib.sha1(f'blob {len(data)}\0'.encode() + data).hexdigest()


def run(*cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, check=True, capture_output=True, text=True).stdout.rstrip('\n')


def copy(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if not dst.exists() or dst.read_bytes() != src.read_bytes():
        shutil.copy2(src, dst)


def copy_tree(src, dst, entries=None):
    names = entries if entries is not None else [p.name for p in src.iterdir() if p.name != '.git']
    for name in names:
        s, d = src / name, dst / name
        if s.is_dir():
            shutil.copytree(s, d, dirs_exist_ok=True, ignore=shutil.ignore_patterns('.git', '.github'))
        elif s.exists():
            copy(s, d)
        else:
            raise FileNotFoundError(s)


def depfile_paths(depfile):
    text = depfile.read_text().replace('\\\n', ' ')
    _, _, deps = text.partition(':')
    return [Path(p) for p in deps.split() if p.startswith('/')]


def object_index(build):
    commands = json.loads((build / 'compile_commands.json').read_text())
    index = {}
    for entry in commands:
        out = entry.get('output')
        if out is None:
            out = re.search(r'\s-o\s+(\S+)', entry['command']).group(1)
        index[Path(entry['file']).resolve()] = (Path(entry['directory']) / out).resolve()
    return index


def resolve_include(name, including, neug):
    for base in (including.parent, neug / 'include', neug / 'src' / 'compiler'):
        candidate = (base / name).resolve()
        if candidate.is_file():
            return candidate
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--neug-source', required=True, type=Path)
    parser.add_argument('--neug-build', type=Path)
    parser.add_argument('--engine-source', type=Path, action='append',
                        default=[ROOT / 'engine/rewrite_engine.cc', ROOT / 'compiler/stubs/execution_stubs.cc'])
    parser.add_argument('--clean', action='store_true')
    args = parser.parse_args()
    source = args.neug_source.resolve()
    build = (args.neug_build or source / 'build').resolve()
    manifest = [l.strip() for l in (UPSTREAM / 'sources.txt').read_text().splitlines()
                if l.strip() and not l.startswith('#')]

    if args.clean:
        for sub in ('include', 'src', 'proto', 'generated', 'grammar'):
            shutil.rmtree(COMPILER / sub, ignore_errors=True)
        for name in list(SOURCE_LIBS) + [d for _, d, _ in HEADER_LIBS]:
            shutil.rmtree(THIRD_PARTY / name, ignore_errors=True)

    objects = object_index(build)
    wanted = set()          # files under the NeuG source tree
    third_party = set()
    generated = set()
    for rel in manifest:
        if rel.startswith('@build/'):
            src = build / rel[len('@build/'):]
        else:
            src = source / rel
            wanted.add(src.resolve())
        obj = objects.get(src.resolve())
        if obj is None:
            sys.exit(f'{rel}: not in compile_commands.json')
        depfile = Path(str(obj) + '.d')
        if not depfile.exists():
            sys.exit(f'{depfile}: missing (build NeuG first)')
        for dep in depfile_paths(depfile):
            dep = dep.resolve()
            if dep.is_relative_to(build):
                generated.add(dep.relative_to(build).as_posix())
            elif dep.is_relative_to(source / 'third_party'):
                third_party.add(dep.relative_to(source / 'third_party').parts[0])
            elif dep.is_relative_to(source):
                wanted.add(dep)

    # Fixpoint over quoted includes (patched sources, engine, boundary).
    queue = list(wanted) + [p.resolve() for p in args.engine_source if p.exists()]
    seen = set()
    while queue:
        path = queue.pop()
        if path in seen:
            continue
        seen.add(path)
        text = path.read_text(errors='replace')
        for name in INCLUDE_RE.findall(text):
            hit = resolve_include(name, path, source)
            if hit and hit.is_relative_to(source) and not hit.is_relative_to(source / 'third_party') \
                    and not hit.is_relative_to(build):
                if hit not in wanted:
                    wanted.add(hit)
                    queue.append(hit)

    copied = {}
    for path in sorted(wanted):
        rel = path.relative_to(source).as_posix()
        if not (rel.startswith('src/') or rel.startswith('include/')):
            continue
        copy(path, COMPILER / rel)
        copied[rel] = sha256(path)

    gen_meta = {}
    for rel, dst in GENERATED.items():
        src = build / rel
        if src.exists():
            copy(src, COMPILER / dst)
            gen_meta[dst] = {'from': f'build/{rel}', 'sha256': sha256(src)}
    unknown = sorted(g for g in generated if not any(g == k for k in GENERATED)
                     and '/generated/proto/' not in g and not g.startswith('third_party/')
                     and not g.startswith('_deps/'))
    if unknown:
        sys.exit(f'Unhandled generated dependencies: {unknown}')

    protos = sorted(p for p in (source / 'proto').glob('*.proto') if p.name not in PROTO_EXCLUDE)
    for proto in protos:
        copy(proto, COMPILER / 'proto' / proto.name)
    copy(source / 'LICENSE', COMPILER / 'LICENSE')
    copy(source / 'src/compiler/antlr4/Cypher.g4', COMPILER / 'grammar/Cypher.g4')
    copy(source / 'src/compiler/antlr4/keywords.txt', COMPILER / 'grammar/keywords.txt')

    for lib, entries in SOURCE_LIBS.items():
        copy_tree(source / 'third_party' / lib, THIRD_PARTY / lib, entries)
    for src_rel, dst, entries in HEADER_LIBS:
        copy_tree(source / 'third_party' / src_rel, THIRD_PARTY / dst, entries)

    # Apply CICO kernel fixes in series order, one file section at a time.
    applied = {}
    series = UPSTREAM / 'patches' / 'series'
    for name in [l.strip() for l in series.read_text().splitlines() if l.strip() and not l.startswith('#')]:
        patch = UPSTREAM / 'patches' / name
        sections = patch.read_text().split('diff --git ')[1:]
        for section in sections:
            target = section.split()[0][2:]
            if not (COMPILER / target).exists():
                continue          # e.g. NeuG unit tests touched by the same patch
            subprocess.run(['patch', '--batch', '--forward', '--fuzz=0', '-p1', '-d', str(COMPILER)],
                           input=('diff --git ' + section).encode(), check=True, capture_output=True)
            applied.setdefault(target, []).append(name)
    patched = {t: {'patches': p, 'git_blob': git_blob((COMPILER / t).read_bytes())} for t, p in applied.items()}

    try:
        head = run('git', 'rev-parse', 'HEAD', cwd=source)
        dirty = run('git', 'status', '--porcelain', '--untracked-files=no', '--', 'src', 'include', 'proto', cwd=source)
        diff = subprocess.run(['git', 'diff', 'HEAD', '--', 'src', 'include', 'proto'], cwd=source,
                              check=True, capture_output=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        head, dirty, diff = None, '', b''
    provenance = {
        'neug_source': str(source),
        'neug_commit': head,
        'neug_dirty_files': [l[3:] for l in dirty.splitlines()],
        'neug_worktree_diff_sha256': hashlib.sha256(diff).hexdigest() if diff else None,
        'files': len(copied),
        'sources': copied,
        'generated': gen_meta,
        'protos': [p.name for p in protos],
        'patched': patched,
        'third_party': sorted(list(SOURCE_LIBS) + [d for _, d, _ in HEADER_LIBS]),
        'third_party_seen': sorted(third_party),
    }
    (UPSTREAM / 'provenance.json').write_text(json.dumps(provenance, indent=1, sort_keys=True) + '\n')
    print(f'copied {len(copied)} NeuG files, {len(gen_meta)} generated, {len(protos)} protos; '
          f'patched {sorted(patched)}')


if __name__ == '__main__':
    main()
