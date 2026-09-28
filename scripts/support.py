"""Filesystem, dependency and SDK checks shared by native build entrypoints."""

import configparser
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import zipfile
from pathlib import Path, PurePosixPath
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / 'dependencies.json').read_text(encoding='utf-8'))


def run(*arguments, capture=False, cwd=None, environment=None, quiet=False):
    command = [str(argument) for argument in arguments]
    if not quiet:
        print('+ ' + ' '.join(command), flush=True)
    result = subprocess.run(command, check=True, cwd=cwd, env=environment,
                            text=True, encoding='utf-8', errors='replace',
                            stdout=subprocess.PIPE if capture or quiet else None,
                            stderr=subprocess.PIPE if quiet else None)
    return result.stdout if capture else None


def digest(filename):
    with Path(filename).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def copy_file(source, destination):
    source, destination = Path(source), Path(destination)
    if destination.exists() and source.samefile(destination):
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def download_verified(url, expected, destination):
    if urlsplit(url).scheme != 'https' or not re.fullmatch('[0-9a-f]{64}', expected):
        raise ValueError('SDK requires an HTTPS URL and lowercase SHA-256')
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        if digest(destination) != expected:
            raise ValueError('Cached SDK does not match the configured checksum')
        return destination
    temporary = destination.with_suffix('.partial')
    try:
        # Signed URLs must never be printed by this wrapper, including failures.
        try:
            run('curl', '--fail', '--location', '--proto', '=https', '--proto-redir', '=https',
                '--retry', '3', '--connect-timeout', '30', '--max-time', '1800',
                '--output', temporary, url, quiet=True)
        except subprocess.CalledProcessError:
            raise ValueError('SDK download failed; verify the repository secret URL and its expiry') from None
        if digest(temporary) != expected:
            raise ValueError('Downloaded SDK checksum mismatch')
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    return destination


def extract_native_sdk(archive, destination):
    if destination.exists():
        raise ValueError('SDK extraction destination must be new')
    with zipfile.ZipFile(archive) as bundle:
        toolchains = [PurePosixPath(entry.filename) for entry in bundle.infolist()
                      if not entry.is_dir() and PurePosixPath(entry.filename).parts[-3:]
                      == ('build', 'cmake', 'ohos.toolchain.cmake')]
        if len(toolchains) != 1:
            raise ValueError('Expected exactly one unpacked Harmony Native SDK')
        prefix = toolchains[0].parents[2]
        if prefix.is_absolute() or '..' in prefix.parts or ':' in str(prefix) or '\\' in str(prefix):
            raise ValueError('Unsafe Native SDK archive prefix')
        destination.mkdir(parents=True)
        boundary = destination.resolve().joinpath(*prefix.parts)
        links = []
        for entry in bundle.infolist():
            relative = PurePosixPath(entry.filename)
            if relative.is_absolute() or '..' in relative.parts or ':' in str(relative) or '\\' in str(relative):
                raise ValueError('Unsafe ZIP path')
            if not relative.is_relative_to(prefix):
                continue
            target = destination.resolve().joinpath(*relative.parts)
            if not target.resolve().is_relative_to(boundary):
                raise ValueError('ZIP path escapes Native SDK')
            mode = entry.external_attr >> 16
            if stat.S_ISLNK(mode):
                link = bundle.read(entry).decode('utf-8')
                if '\\' in link or ':' in link or Path(link).is_absolute() or not (target.parent / link).resolve().is_relative_to(boundary):
                    raise ValueError('Unsafe ZIP symlink')
                links.append((target, link))
            elif entry.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with bundle.open(entry) as source, target.open('xb') as output:
                    shutil.copyfileobj(source, output)
                if mode & 0o777:
                    target.chmod(mode & 0o777)
        for target, link in links:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.symlink_to(link)
        # Reject symlink chains that escape the selected subtree.
        for target, _ in links:
            if not target.resolve().is_relative_to(boundary):
                raise ValueError('ZIP symlink chain escapes Native SDK')
    return boundary


def prepare_harmony_sdk(cache, work):
    configured = os.environ.get('OHOS_SDK_NATIVE_DIR')
    if configured:
        native = Path(configured).resolve()
    else:
        url = os.environ.get('HARMONY_SDK_URL', '')
        expected = os.environ.get('HARMONY_SDK_SHA256', '')
        if not url or not expected:
            raise ValueError('Configure HARMONY_SDK_URL secret and HARMONY_SDK_SHA256 variable in THIS repository')
        if expected != LOCK['harmony']['sdkSha256']:
            raise ValueError('SDK checksum differs from dependencies.json; review the toolchain before updating the lock')
        archive = download_verified(url, expected, cache / f'harmony-{expected}.zip')
        native = extract_native_sdk(archive, work / 'sdk')
    for relative in ('llvm/bin/clang', 'llvm/bin/clang++', 'llvm/bin/llvm-ar',
                     'llvm/bin/llvm-strip', 'llvm/bin/llvm-readelf', 'llvm/bin/llvm-nm',
                     'build/cmake/ohos.toolchain.cmake'):
        if not (native / relative).is_file():
            raise ValueError(f'Incomplete Linux Harmony SDK: missing {relative}')
    if not (native / 'sysroot').is_dir():
        raise ValueError('Harmony Native SDK sysroot is missing')
    return native


def verify_abseil_lock(source):
    configuration = configparser.ConfigParser(interpolation=None)
    configuration.read(source / 'subprojects/abseil-cpp.wrap', encoding='utf-8')
    for key, expected in LOCK['abseil'].items():
        if configuration['wrap-file'].get(key) != expected:
            raise ValueError(f'Pinned Abseil wrap mismatch: {key}')


def use_abseil_release_mirror(source):
    manifest = source / 'subprojects/abseil-cpp.wrap'
    text = manifest.read_text(encoding='utf-8')
    original = 'patch_url = https://wrapdb.mesonbuild.com/v2/abseil-cpp_20230125.1-4/get_patch'
    mirror = ('patch_url = https://github.com/mesonbuild/wrapdb/releases/download/'
              'abseil-cpp_20230125.1-4/abseil-cpp_20230125.1-4_patch.zip')
    if text.count(original) != 1:
        raise ValueError('Unexpected Abseil patch URL; refusing to change the dependency')
    # Same official wrapdb release and SHA-256; avoids a redirect through WrapDB.
    manifest.write_text(text.replace(original, mirror), encoding='utf-8')


def collect_licenses(source, output):
    collected = []
    for filename in sorted(source.rglob('*')):
        relative = filename.relative_to(source)
        if '.git' in relative.parts or not filename.is_file() or filename.is_symlink():
            continue
        if filename.name.lower().startswith(('license', 'copying', 'notice', 'patents', 'authors')):
            copy_file(filename, output / 'licenses' / relative)
            collected.append(relative.as_posix())
    if not any('abseil-cpp' in name and name.endswith('LICENSE') for name in collected):
        raise ValueError('Abseil license was not collected')
    if not any('/' not in name and name.lower().startswith(('copying', 'license')) for name in collected):
        raise ValueError('WebRTC license was not collected')
    return collected


def package_output(output, platform, manifest):
    manifest['files'] = {
        filename.relative_to(output).as_posix(): {'bytes': filename.stat().st_size, 'sha256': digest(filename)}
        for filename in sorted(output.rglob('*')) if filename.is_file()
    }
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    distribution = ROOT / 'dist'
    distribution.mkdir(exist_ok=True)
    archive = distribution / f'webrtc-apm-{platform}.zip'
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for filename in sorted(output.rglob('*')):
            if filename.is_file():
                bundle.write(filename, filename.relative_to(output))
    archive.with_suffix('.zip.sha256').write_text(f'{digest(archive)}  {archive.name}\n', encoding='ascii')
    print(f'PACKAGE: {archive.name}, {archive.stat().st_size / 1024 / 1024:.2f} MiB', flush=True)
    return archive
