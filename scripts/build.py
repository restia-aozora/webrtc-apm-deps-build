"""Build and verify WebRTC APM native artifacts without AAR/HAR packaging."""

import argparse
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from build_config import (ANDROID_API, ANDROID_NDK, IOS_MINIMUM, SOURCE_COMMIT,
                          SOURCE_URL, android_cross_file, harmony_cross_file,
                          ios_cross_file, ndk_tools, patch_crc_dependency,
                          select_abis, wrapper_meson)
from support import (LOCK, ROOT, collect_licenses, copy_file, digest, package_output,
                     prepare_harmony_sdk, run, use_abseil_release_mirror, verify_abseil_lock)
from verify import elf_dependencies, verify_elf, verify_ios


def prepare_source(work, platform, source_repository):
    source = work / 'source'
    run('git', 'clone', '--no-checkout', '--no-hardlinks', source_repository, source)
    run('git', '-C', source, 'checkout', '--detach', SOURCE_COMMIT)
    actual = run('git', '-C', source, 'rev-parse', 'HEAD', capture=True).strip()
    if actual != SOURCE_COMMIT or LOCK['source']['commit'] != SOURCE_COMMIT:
        raise ValueError('Source commit does not match the reviewed dependency lock')
    verify_abseil_lock(source)
    use_abseil_release_mirror(source)
    patch_crc_dependency(source)
    wrapper = source / 'aozora-wrapper'
    shutil.copytree(ROOT / 'native', wrapper)
    if platform == 'host':
        copy_file(ROOT / 'tests/native_smoke.cpp', wrapper / 'native_smoke.cpp')
    build_definition, export_map = wrapper_meson(platform)
    (wrapper / 'meson.build').write_text(build_definition, encoding='utf-8')
    (wrapper / 'exports.map').write_text(export_map, encoding='utf-8')
    with (source / 'meson.build').open('a', encoding='utf-8') as stream:
        stream.write("\nsubdir('aozora-wrapper')\n")
    return source


def configure_build(source, build, cross, jobs, environment, platform):
    command = [sys.executable, '-m', 'mesonbuild.mesonmain', 'setup', build, source,
               '--buildtype=minsize', '--wrap-mode=forcefallback',
               '-Ddefault_library=static', '-Db_staticpic=true', '-Db_ndebug=true',
               '-Dgnustl=disabled']
    if cross is not None:
        command += ['--cross-file', cross]
    run(*command, environment=environment)
    target = 'apm_smoke' if platform == 'host' else 'aozora_apm'
    run(sys.executable, '-m', 'mesonbuild.mesonmain', 'compile', '-C', build,
        '-j', jobs, target, environment=environment)


def package_ios(build, work, output, sdk):
    framework = work / 'AozoraApm.framework'
    binary = framework / 'AozoraApm'
    copy_file(build / 'aozora-wrapper/libaozora_apm.dylib', binary)
    copy_file(ROOT / 'native/aozora_apm.h', framework / 'Headers/aozora_apm.h')
    (framework / 'Modules').mkdir()
    (framework / 'Modules/module.modulemap').write_text(
        'framework module AozoraApm {\n  umbrella header "aozora_apm.h"\n  export *\n}\n', encoding='utf-8')
    information = {
        'CFBundleIdentifier': 'com.aozora.audio.apm', 'CFBundleName': 'AozoraApm',
        'CFBundleExecutable': 'AozoraApm', 'CFBundlePackageType': 'FMWK',
        'CFBundleVersion': '1', 'CFBundleShortVersionString': '1.0.0',
        'MinimumOSVersion': IOS_MINIMUM, 'CFBundleSupportedPlatforms': ['iPhoneOS'],
    }
    with (framework / 'Info.plist').open('wb') as stream:
        plistlib.dump(information, stream)
    run('xcrun', 'strip', '-x', binary)
    run('xcrun', 'install_name_tool', '-id', '@rpath/AozoraApm.framework/AozoraApm', binary)
    checks = verify_ios(binary, IOS_MINIMUM)
    run('codesign', '--force', '--sign', '-', framework)
    smoke = work / 'module-smoke.swift'
    smoke.write_text('import AozoraApm\nlet processor = aozora_apm_create(16000, 1, 1, 0)\naozora_apm_destroy(processor)\n', encoding='utf-8')
    run('xcrun', 'swiftc', '-typecheck', '-sdk', sdk, '-target', f'arm64-apple-ios{IOS_MINIMUM}',
        '-F', framework.parent, smoke)
    destination = output / 'app-ios/Frameworks/AozoraApm.xcframework'
    destination.parent.mkdir(parents=True)
    run('xcodebuild', '-create-xcframework', '-framework', framework, '-output', destination)
    checks['swiftModuleImport'] = 'PASSED'
    return checks


def package_elf(build, output, platform, abi, tools, native=None):
    libraries = output / f'app-{platform}/libs' / abi
    library = libraries / 'libaozora_apm.so'
    copy_file(build / 'aozora-wrapper/libaozora_apm.so', library)
    suffix = '.exe' if (tools / 'llvm-readelf.exe').is_file() else ''
    if platform == 'harmony':
        needed = elf_dependencies(library, tools / 'llvm-readelf')
        if 'libc++_shared.so' in needed:
            candidates = [filename for filename in native.rglob('libc++_shared.so')
                          if 'aarch64' in filename.as_posix()]
            if not candidates or len({digest(filename) for filename in candidates}) != 1:
                raise ValueError('Cannot select an unambiguous ARM64 Harmony libc++_shared.so')
            copy_file(candidates[0], libraries / 'libc++_shared.so')
            notices = [filename for filename in native.rglob('NOTICE*') if filename.is_file()]
            if not notices:
                raise ValueError('Harmony SDK runtime notices are missing; do not redistribute without them')
            for filename in notices:
                copy_file(filename, output / 'licenses/harmony-sdk' / filename.relative_to(native))
        shutil.copytree(ROOT / 'native/harmony', output / 'app-harmony/types/libaozora_apm')
    checks = {}
    for filename in sorted(libraries.glob('*.so')):
        run(tools / ('llvm-strip' + suffix), '--strip-unneeded', filename)
        checks[filename.name] = verify_elf(filename, platform, abi, tools,
                                          packaged={item.name for item in libraries.glob('*.so')})
    copy_file(ROOT / 'native/aozora_apm.h', output / f'app-{platform}/include/aozora_apm.h')
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('platform', choices=('android', 'ios', 'harmony', 'host'))
    parser.add_argument('--android-abis', choices=('arm64-v8a', 'armeabi-v7a', 'x86', 'all'), default='arm64-v8a')
    parser.add_argument('--jobs', type=int, default=3)
    parser.add_argument('--source-repository', default=SOURCE_URL,
                        help='Optional local clone, read-only; the pinned commit is still required')
    arguments = parser.parse_args()
    if arguments.jobs < 1:
        parser.error('jobs must be positive')
    if arguments.platform == 'ios' and sys.platform != 'darwin':
        parser.error('iOS requires macOS and Xcode; use the GitHub macOS job')
    if arguments.platform in ('host', 'harmony') and sys.platform != 'linux':
        parser.error('Harmony and the host smoke test require Linux; use the GitHub jobs')
    work_root = ROOT / '.work'
    work_root.mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=arguments.platform + '-', dir=work_root))
    print('BUILD DIRECTORY: ' + str(work), flush=True)
    output = work / 'package'
    output.mkdir()
    environment = os.environ.copy()
    for variable in ('CFLAGS', 'CXXFLAGS', 'LDFLAGS', 'CPPFLAGS', 'PKG_CONFIG_PATH', 'PKG_CONFIG_LIBDIR'):
        environment.pop(variable, None)
    # Ensure the Ninja installed into the same Python environment is visible.
    import ninja
    environment['PATH'] = str(ninja.BIN_DIR) + os.pathsep + environment.get('PATH', '')
    native = sdk = ndk = None
    if arguments.platform == 'harmony':
        native = prepare_harmony_sdk(work_root / 'downloads', work)
    elif arguments.platform == 'android':
        configured = environment.get('ANDROID_NDK_HOME', '')
        if not configured:
            raise ValueError('Set ANDROID_NDK_HOME to Android NDK ' + ANDROID_NDK)
        ndk = Path(configured).resolve()
        properties = (ndk / 'source.properties').read_text(encoding='utf-8')
        if f'Pkg.Revision = {ANDROID_NDK}' not in properties:
            raise ValueError('Android NDK version differs from the dependency lock')
    elif arguments.platform == 'ios':
        sdk = run('xcrun', '--sdk', 'iphoneos', '--show-sdk-path', capture=True).strip()
    source = prepare_source(work, arguments.platform, arguments.source_repository)
    manifest = {'platform': arguments.platform, 'source': LOCK['source'],
                'dependencyLock': LOCK, 'features': ['AEC3', 'NS', 'AGC2', 'voice-detection'],
                'runtimeValidation': 'NOT RUN', 'utsValidation': 'NOT RUN', 'checks': {}}
    for abi in select_abis(arguments.platform, arguments.android_abis):
        build = work / ('build-' + abi)
        cross = work / (abi + '.ini')
        if arguments.platform == 'android':
            cross.write_text(android_cross_file(ndk, abi, ANDROID_API), encoding='utf-8')
        elif arguments.platform == 'harmony':
            cross.write_text(harmony_cross_file(native), encoding='utf-8')
        elif arguments.platform == 'ios':
            cross.write_text(ios_cross_file(sdk), encoding='utf-8')
        else:
            cross = None
        configure_build(source, build, cross, arguments.jobs, environment, arguments.platform)
        if arguments.platform == 'host':
            run(sys.executable, '-m', 'mesonbuild.mesonmain', 'test', '-C', build,
                '--print-errorlogs', environment=environment)
            print('Host API smoke PASSED; this is not device or acoustic validation.', flush=True)
            return
        if arguments.platform == 'ios':
            manifest['checks'][abi] = package_ios(build, work, output, sdk)
        else:
            tools = ndk_tools(ndk) / 'bin' if ndk else native / 'llvm/bin'
            manifest['checks'][abi] = package_elf(build, output, arguments.platform, abi, tools, native)
    manifest['licenses'] = collect_licenses(source, output)
    if ndk is not None:
        for name in ('NOTICE', 'NOTICE.toolchain'):
            copy_file(ndk / name, output / 'licenses/android-ndk' / name)
    copy_file(ROOT / 'README.md', output / 'README.md')
    copy_file(ROOT / 'LICENSE', output / 'licenses/wrapper-LICENSE')
    copy_file(ROOT / 'THIRD_PARTY_NOTICES.md', output / 'THIRD_PARTY_NOTICES.md')
    package_output(output, arguments.platform, manifest)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'BUILD BLOCKED: {error}', file=sys.stderr)
        sys.exit(1)
