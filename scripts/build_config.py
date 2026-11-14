"""Pinned source patching and explicit Meson target descriptions."""

import sys
from pathlib import Path

SOURCE_URL = 'https://github.com/helloooideeeeea/webrtc-audio-processing.git'
SOURCE_COMMIT = 'f8efa8486ac0dd463fb84e4e99be2e072c28adb8'
ANDROID_NDK = '27.2.12479018'
ANDROID_API = 28
IOS_MINIMUM = '14.0'
ANDROID_ABIS = {
    'arm64-v8a': ('aarch64', 'aarch64', 'aarch64-linux-android'),
    'armeabi-v7a': ('arm', 'armv7', 'armv7a-linux-androideabi'),
    'x86': ('x86', 'i686', 'i686-linux-android'),
}
C_EXPORTS = {
    'aozora_apm_create', 'aozora_apm_destroy', 'aozora_apm_process_reverse',
    'aozora_apm_process_capture', 'aozora_apm_set_stream_delay_ms',
    'aozora_apm_get_voice_detected', 'aozora_apm_frame_samples',
    'aozora_apm_get_build_revision', 'aozora_apm_get_statistics',
}
JNI_EXPORTS = {
    'Java_com_aozora_audio_AozoraWebRtcApm_' + method for method in (
        'nativeCreate', 'nativeRelease', 'nativeProcessReverse',
        'nativeProcessCapture', 'nativeSetStreamDelay', 'nativeGetVoiceDetected',
        'nativeGetBuildRevision', 'nativeGetStatistics')
}
HOST_TEST_TARGETS = ('apm_smoke', 'apm_statistics', 'apm_echo_regression')
HOST_TEST_SOURCES = ('native_smoke.cpp', 'native_statistics.cpp', 'native_echo_regression.cpp',
                     'native_echo_mutation_runner.py')


def select_abis(platform, selection):
    if platform not in ('android', 'ios', 'harmony', 'host'):
        raise ValueError(f'Unknown target platform: {platform}')
    if platform != 'android':
        return ['arm64-v8a']
    selected_abis = [selection] if isinstance(selection, str) else list(selection)
    if not selected_abis:
        raise ValueError('Select at least one Android ABI')
    if selected_abis == ['all']:
        return list(ANDROID_ABIS)
    for selected_abi in selected_abis:
        if selected_abi == 'all':
            raise ValueError('Android ABI all cannot be combined with explicit ABIs')
        if selected_abi not in ANDROID_ABIS:
            raise ValueError(f'Unknown Android ABI: {selected_abi}')
    return [abi for abi in ANDROID_ABIS if abi in selected_abis]


def quote(value):
    value = str(value).replace('\\', '/')
    if any(character in value for character in ("'", '\n', '\r', '\x00')):
        raise ValueError('Unsafe path in Meson machine file')
    return "'" + value + "'"


def array(values):
    return '[' + ', '.join(quote(value) for value in values) + ']'


def cross_file(system, family, cpu, compiler, cpp_compiler, tools, sysroot,
               compile_flags, link_flags):
    return '\n'.join([
        '[binaries]', 'c = ' + array(compiler), 'cpp = ' + array(cpp_compiler),
        'ar = ' + quote(tools['ar']), 'strip = ' + quote(tools['strip']),
        'pkg-config = ' + quote('pkg-config'), '', '[host_machine]',
        'system = ' + quote(system), 'cpu_family = ' + quote(family),
        'cpu = ' + quote(cpu), "endian = 'little'", '', '[properties]',
        'needs_exe_wrapper = true', 'sys_root = ' + quote(sysroot),
        'pkg_config_libdir = []', '', '[built-in options]',
        'c_args = ' + array(compile_flags), 'cpp_args = ' + array(compile_flags),
        'c_link_args = ' + array(link_flags), 'cpp_link_args = ' + array(link_flags),
        "default_library = 'static'", 'b_staticpic = true', '',
    ])


def ndk_tools(ndk):
    host = {'win32': 'windows-x86_64', 'darwin': 'darwin-x86_64'}.get(sys.platform, 'linux-x86_64')
    return Path(ndk) / 'toolchains/llvm/prebuilt' / host


def android_cross_file(ndk, abi, api=ANDROID_API):
    family, cpu, target = ANDROID_ABIS[abi]
    root = ndk_tools(ndk)
    executable = '.exe' if sys.platform == 'win32' else ''
    # Direct clang invocation also works on Windows without .cmd wrapper quoting.
    if sys.platform == 'win32':
        compiler = [root / 'bin/clang.exe', f'--target={target}{api}']
        cpp_compiler = [root / 'bin/clang++.exe', f'--target={target}{api}']
    else:
        compiler = [root / f'bin/{target}{api}-clang']
        cpp_compiler = [root / f'bin/{target}{api}-clang++']
    link = ['-Wl,--gc-sections', '-Wl,--no-undefined',
            '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384']
    return cross_file('android', family, cpu, compiler, cpp_compiler,
                      {'ar': root / f'bin/llvm-ar{executable}',
                       'strip': root / f'bin/llvm-strip{executable}'}, root / 'sysroot',
                      ['-ffunction-sections', '-fdata-sections'], link)


def harmony_cross_file(native):
    native = Path(native)
    target = '--target=aarch64-linux-ohos'
    sysroot_flag = '--sysroot=' + native.joinpath('sysroot').as_posix()
    compile_flags = [target, sysroot_flag, '-D__MUSL__', '-ffunction-sections', '-fdata-sections']
    return cross_file('linux', 'aarch64', 'aarch64',
                      [native / 'llvm/bin/clang'], [native / 'llvm/bin/clang++'],
                      {'ar': native / 'llvm/bin/llvm-ar', 'strip': native / 'llvm/bin/llvm-strip'},
                      native / 'sysroot', compile_flags,
                      [target, sysroot_flag, '-Wl,--gc-sections', '-Wl,--no-undefined'])


def ios_cross_file(sdk, minimum=IOS_MINIMUM):
    flags = ['-target', f'arm64-apple-ios{minimum}', '-isysroot', str(sdk)]
    return cross_file('darwin', 'aarch64', 'aarch64', ['xcrun', '--sdk', 'iphoneos', 'clang'],
                      ['xcrun', '--sdk', 'iphoneos', 'clang++'],
                      {'ar': 'ar', 'strip': 'strip'}, sdk,
                      flags + ['-ffunction-sections', '-fdata-sections'],
                      flags + ['-Wl,-dead_strip', '-Wl,-headerpad_max_install_names'])


def patch_crc_dependency(source):
    manifest = Path(source) / 'meson.build'
    text = manifest.read_text(encoding='utf-8')
    anchor = "  dependency('absl_bad_optional_access'),\n"
    if text.count(anchor) != 1 or "dependency('absl_crc')" in text:
        raise ValueError('Unexpected upstream Abseil dependency block; refusing to guess')
    manifest.write_text(text.replace(anchor, anchor + "  dependency('absl_crc'),\n"), encoding='utf-8')


def wrapper_meson(platform):
    sources = ['aozora_apm.cpp']
    if platform == 'android':
        sources.append('aozora_apm_jni.cpp')
    elif platform == 'harmony':
        sources.append('aozora_apm_napi.cpp')
    exports = sorted(C_EXPORTS | (JNI_EXPORTS if platform == 'android' else set()))
    statements = [
        "wrapper_deps = common_deps",
        "wrapper_links = []",
    ]
    if platform == 'harmony':
        statements.append("wrapper_deps += [cpp.find_library('ace_napi.z')]")
    if platform in ('android', 'harmony', 'host'):
        statements.append("wrapper_links += ['-Wl,--version-script=' + (meson.current_source_dir() / 'exports.map')]")
        if platform == 'android':
            statements.append("wrapper_links += ['-static-libstdc++']")
    statements += [
        "wrapper = shared_library('aozora_apm', " + array(sources) + ',',
        "  include_directories: [webrtc_inc],",
        "  dependencies: wrapper_deps, link_with: [libwebrtc_audio_processing],",
        "  c_args: common_cflags + ['-DWEBRTC_APM_DEBUG_DUMP=0'],",
        "  cpp_args: common_cxxflags + ['-DWEBRTC_APM_DEBUG_DUMP=0'],",
        "  gnu_symbol_visibility: 'hidden', link_args: wrapper_links, install: false)",
    ]
    if platform == 'host':
        statements += [
            "smoke = executable('apm_smoke', 'native_smoke.cpp', link_with: wrapper)",
            "test('native-api-smoke', smoke, timeout: 120)",
            "statistics = executable('apm_statistics', 'native_statistics.cpp', link_with: wrapper)",
            "test('native-statistics-contract', statistics, timeout: 120)",
            "echo_regression = executable('apm_echo_regression', 'native_echo_regression.cpp', link_with: wrapper)",
            "test('native-echo-regression', echo_regression, timeout: 180)",
            "mutation_python = import('python').find_installation()",
            "mutation_runner = files('native_echo_mutation_runner.py')[0]",
            "test('native-echo-disconnect-reference', mutation_python, args: [mutation_runner, echo_regression, 'disconnect-reference'], timeout: 400)",
            "test('native-echo-zero-output', mutation_python, args: [mutation_runner, echo_regression, 'zero-output'], timeout: 400)",
        ]
    export_map = '{\n  global:\n' + ''.join(f'    {symbol};\n' for symbol in exports) + '  local: *;\n};\n'
    return '\n'.join(statements) + '\n', export_map
