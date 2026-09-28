"""Fail-closed artifact checks; these do not claim device runtime validation."""

import re

from build_config import C_EXPORTS, JNI_EXPORTS
from support import run


def version_components(version):
    if not re.fullmatch(r'[0-9]+(?:\.[0-9]+){0,2}', version):
        raise ValueError(f'Invalid OS version: {version}')
    components = tuple(map(int, version.split('.')))
    return components + (0,) * (3 - len(components))


def elf_dependencies(library, readelf):
    text = run(readelf, '-d', library, capture=True)
    return set(re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', text))


def verify_elf(library, platform, abi, tools, packaged=()):
    suffix = '.exe' if (tools / 'llvm-readelf.exe').is_file() else ''
    readelf = tools / ('llvm-readelf' + suffix)
    header = run(readelf, '-h', library, capture=True)
    machine = {'arm64-v8a': 'AArch64', 'armeabi-v7a': 'ARM', 'x86': 'Intel 80386'}[abi]
    if not re.search(r'Machine:\s+' + re.escape(machine) + r'\s*$', header, re.M):
        raise ValueError(f'Wrong ELF architecture for {abi}: {library.name}')
    needed = elf_dependencies(library, readelf)
    allowed = {'libc.so', 'libm.so', 'libdl.so'}
    allowed |= {'liblog.so'} if platform == 'android' else {'libace_napi.z.so'}
    unexpected = needed - allowed - set(packaged)
    if unexpected:
        raise ValueError(f'Unbundled or unexpected dynamic dependencies: {sorted(unexpected)}')
    if platform == 'android':
        segments = run(readelf, '-lW', library, capture=True)
        alignments = [int(line.split()[-1], 16) for line in segments.splitlines()
                      if line.strip().startswith('LOAD ')]
        if not alignments or any(alignment < 16384 for alignment in alignments):
            raise ValueError('Android ELF LOAD alignment is below 16 KB')
    if library.name == 'libaozora_apm.so':
        symbol_text = run(tools / ('llvm-nm' + suffix), '-D', '--defined-only', library, capture=True)
        symbols = {line.split()[-1] for line in symbol_text.splitlines() if line.strip()}
        required = C_EXPORTS | (JNI_EXPORTS if platform == 'android' else set())
        if not required <= symbols:
            raise ValueError(f'Missing exported APIs: {sorted(required - symbols)}')
        undefined = run(tools / ('llvm-nm' + suffix), '-D', '--undefined-only', library, capture=True)
        if 'absl' in undefined or 'webrtc' in undefined:
            raise ValueError('Unresolved WebRTC or Abseil symbols')
        if platform == 'harmony' and 'napi_module_register' not in undefined:
            raise ValueError('Harmony N-API module registration is missing')
    return {'machine': machine, 'needed': sorted(needed), 'linkage': 'PASSED'}


def verify_ios(binary, minimum):
    architectures = run('xcrun', 'lipo', '-archs', binary, capture=True).strip()
    if architectures != 'arm64':
        raise ValueError(f'Unexpected iOS architecture: {architectures}')
    load_commands = run('xcrun', 'vtool', '-show-build', binary, capture=True)
    if not re.search(r'platform\s+(?:IOS|2)\b', load_commands):
        raise ValueError('Framework must target an iPhoneOS device, not a simulator')
    minimums = re.findall(r'minos\s+([0-9.]+)', load_commands)
    if not minimums or any(version_components(version) > version_components(minimum)
                           for version in minimums):
        raise ValueError('Framework deployment target exceeds the configured iOS minimum')
    dependencies = run('xcrun', 'otool', '-L', binary, capture=True)
    for line in dependencies.splitlines()[1:]:
        library = line.strip().split(' ', 1)[0]
        if not library.startswith(('/usr/lib/', '/System/Library/', '@rpath/AozoraApm.framework/AozoraApm')):
            raise ValueError(f'Unexpected framework dependency: {library}')
    exports = set(run('xcrun', 'nm', '-gjU', binary, capture=True).split())
    required = {'_' + symbol for symbol in C_EXPORTS}
    if not required <= exports:
        raise ValueError(f'Missing iOS C APIs: {sorted(required - exports)}')
    return {'architecture': architectures, 'loadCommands': load_commands, 'dependencies': dependencies}
