"""Offline CLI with deterministic receipts and guarded output publication.

Generation finishes in memory before touching output. A sibling staging tree
preserves unowned files; directory publication rolls back on replacement failure.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""

import argparse
from dataclasses import asdict
import hashlib
import importlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import sys
import tempfile

from . import __version__
from .parser import InvalidDescriptor, Limits
from .schema import load_schema


MANIFEST = "manifest.json"
FORMAT = "telemetry-codegen-owned-output-v1"
TARGETS = ("cpp", "js", "ts", "qml")


def _json(value):
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True, allow_nan=False) + "\n"


def _path(name):
    if not isinstance(name, str) or not name or "\\" in name or ":" in name:
        raise ValueError("generated paths must be portable relative paths")
    path = PurePosixPath(name)
    if (path.is_absolute() or any(part in ("", ".", "..") for part in name.split("/"))
            or path.as_posix() != name or name.casefold() == MANIFEST.casefold()):
        raise ValueError("invalid generated output path: " + name)
    return name


def _targets(value):
    names = value.split(",") if isinstance(value, str) else list(value)
    if not names or any(name not in TARGETS for name in names):
        raise ValueError("targets must be a comma-separated selection of cpp,ts,js,qml")
    return tuple(sorted(set(names)))


def symbol_mapping(schema):
    """Include identities for nested members and duplicate enum labels as well."""
    types = []
    for item in schema.types:
        record = {"type_id": item["id"], "canonical_name": f"Type_{item['id']}"}
        if item["kind"] == 3:
            record["members"] = [
                {"index": index, "type_id": child, "original_name": name,
                 "cpp_name": schema.member_names[item["id"]][index], "js_name": name}
                for index, (child, name) in enumerate(item["members"])]
        if item["kind"] == 2:
            record["enum_entries"] = [
                {"index": index, "code": str(code), "original_name": name,
                 "generated_name": schema.enum_names[item["id"]][index]}
                for index, (code, name) in enumerate(item["entries"])]
        types.append(record)
    return {"endpoints": [asdict(endpoint) for endpoint in schema.endpoints], "types": types}


def _ux_extension():
    """Load only on explicit UX selection; no bundled alternate implementation."""
    from .emitter_api import API_VERSION, IR_VERSION, SDK_VERSION
    try:
        extension = importlib.import_module('telemetry_ux_codegen')
    except ModuleNotFoundError as error:
        if error.name == 'telemetry_ux_codegen':
            raise ImportError('UX generation requires telemetry-ux-codegen==' + SDK_VERSION +
                              '; install it in the generator environment') from error
        raise ImportError('Cannot import installed Telemetry UX: ' + str(error)) from error
    except ImportError as error:
        raise ImportError('Cannot import installed Telemetry UX: ' + str(error)) from error
    for name, expected in (('API_VERSION', API_VERSION), ('IR_VERSION', IR_VERSION),
                           ('SDK_VERSION', SDK_VERSION)):
        actual = getattr(extension, name, None)
        if type(actual) is not type(expected) or actual != expected:
            raise ValueError('Incompatible Telemetry UX ' + name + '; expected ' + str(expected))
    return extension


def _ux_target(extension, name, schema):
    from .emitter_api import GeneratedTarget
    from collections.abc import Mapping
    generate = getattr(extension, name, None)
    if not callable(generate):
        raise ValueError('Telemetry UX must provide ' + name + '(schema)')
    result = generate(schema)
    if (not isinstance(result, GeneratedTarget) or not isinstance(result.files, Mapping)
            or not isinstance(result.symbol_mapping, dict)):
        raise ValueError('Telemetry UX must return GeneratedTarget with a symbol_mapping dictionary')
    return result


def render(schema, targets, client_runtime=False, structural_metadata=False, ux_starter=None,
           wire_editing=False):
    """Import selected emitters lazily; no transport or compiler is required."""
    targets = _targets(targets)
    if client_runtime and not set(targets) & {"js", "ts"}:
        raise ValueError("--client-runtime requires a js or ts target")
    if ux_starter not in (None, 'web', 'qml'):
        raise ValueError('--ux-starter must be web or qml')
    if ux_starter == 'web' and ('js' not in targets or not structural_metadata):
        raise ValueError('--ux-starter web requires an explicit js target and --structural-metadata')
    if ux_starter == 'qml' and ('qml' not in targets or not structural_metadata):
        raise ValueError('--ux-starter qml requires an explicit qml target and --structural-metadata')
    cpp_wire_editing = wire_editing or ux_starter == 'qml'
    wire_editing = wire_editing or ux_starter is not None
    files, seen = {}, set()
    effective = tuple(sorted(set(targets) | ({"cpp"} if "qml" in targets else set())))

    def append(result):
        if not isinstance(result, dict):
            raise ValueError("generator must return a relative-path/text dictionary")
        for name, content in result.items():
            name = _path(name)
            if name.casefold() in seen:
                raise ValueError("colliding generated output paths: " + name)
            if not isinstance(content, str):
                raise ValueError("generator output must be text")
            seen.add(name.casefold())
            files[name] = content.encode("utf-8")

    qml_result = web_result = None
    if 'qml' in targets or ux_starter is not None:
        # Resolve only an explicitly requested extension. Incompatible installed
        # UX is an error; no bundled second implementation or silent fallback.
        extension = _ux_extension()
        if 'qml' in targets:
            qml_result = _ux_target(extension, 'generate_qml_configurator' if ux_starter == 'qml'
                                    else 'generate_qml', schema)
        if ux_starter == 'web':
            web_result = _ux_target(extension, 'generate_web_starter', schema)

    for target in effective:
        if target == 'qml':
            append(dict(qml_result.files))
            continue
        module = importlib.import_module(".generators." + (target if target in ("cpp", "qml") else "web"),
                                         __package__)
        generate = getattr(module, "generate" if target in ("cpp", "qml") else "generate_" + target)
        options = {}
        if target in ('js', 'ts'):
            if client_runtime:
                options['client_runtime'] = True
        if ((target == 'cpp' and cpp_wire_editing) or
                (target in ('js', 'ts') and wire_editing)):
            options['wire_editing'] = True
        result = generate(schema, **options)
        append(result)
    if structural_metadata:
        from .generators.metadata import generate
        append(generate(schema, effective))
    if web_result is not None:
        append(dict(web_result.files))
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in sorted(files.items())}
    manifest = dict(format=FORMAT, descriptor_sha256=schema.descriptor_sha256,
                    descriptor_fingerprint=f"0x{schema.fingerprint:016x}",
                    descriptor_wire_version=[3, 0], generator_version=__version__,
                    options={"namespace": schema.namespace, "targets": list(targets)},
                    generated_files=hashes, generated_symbol_mapping=symbol_mapping(schema))
    if client_runtime:
        manifest["options"]["client_runtime"] = True
    if structural_metadata:
        manifest["options"]["structural_metadata"] = True
    if wire_editing:
        manifest['options']['wire_editing'] = True
    if "qml" in targets:
        manifest["generated_symbol_mapping"]["qml"] = qml_result.symbol_mapping
    if ux_starter is not None:
        manifest['options']['ux_starter'] = ux_starter
    if web_result is not None:
        manifest['generated_symbol_mapping']['web_starter'] = web_result.symbol_mapping
    files[MANIFEST] = _json(manifest).encode("utf-8")
    return files


def _linked(path):
    if path.is_symlink():
        return True
    if hasattr(path, "is_junction"):
        return path.is_junction()
    # Path.is_junction() arrived in Python 3.12; Windows lstat has exposed
    # directory mount-point reparse tags since Python 3.8.
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    return getattr(info, "st_reparse_tag", 0) == getattr(stat, "IO_REPARSE_TAG_MOUNT_POINT", 0xa0000003)


def _safe_output(output):
    output = Path(output).absolute()
    if output == output.parent:
        raise ValueError("output cannot be a filesystem root")
    for path in (output, *output.parents):
        if _linked(path):
            raise ValueError("output paths cannot contain symlinks or junctions")
    if output.exists() and not output.is_dir():
        raise ValueError("output must be a directory")
    return output


def _inventory(output):
    files, folded = {}, set()
    if not output.exists():
        return files
    def unreadable(error):
        raise error
    for root, directories, names in os.walk(output, followlinks=False, onerror=unreadable):
        for name in directories + names:
            path = Path(root) / name
            if _linked(path):
                raise ValueError("output tree cannot contain symlinks or junctions")
        for name in names:
            path = Path(root) / name
            if not path.is_file():
                raise ValueError("output tree must contain regular files")
            relative = path.relative_to(output).as_posix()
            if relative.casefold() in folded:
                raise ValueError("output contains case-colliding file paths")
            folded.add(relative.casefold())
            files[relative] = path
    return files


def _digest_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _ownership(existing):
    if MANIFEST not in existing:
        return {}
    path = existing[MANIFEST]
    if path.stat().st_size > 64 * 1024 * 1024:
        raise ValueError("existing generation manifest is too large")
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (ValueError, UnicodeError) as error:
        raise ValueError("existing manifest is not a generation ownership receipt") from error
    if not isinstance(manifest, dict) or manifest.get("format") != FORMAT:
        raise ValueError("existing manifest is not a generation ownership receipt")
    owned = manifest.get("generated_files")
    if not isinstance(owned, dict):
        raise ValueError("existing manifest has invalid generated_files")
    folded = set()
    for name, digest in owned.items():
        _path(name)
        if name.casefold() in folded or not isinstance(digest, str) or not re.fullmatch("[0-9a-f]{64}", digest):
            raise ValueError("existing manifest has invalid output ownership")
        folded.add(name.casefold())
    return owned


def _output_layout(names):
    """Require one portable spelling and kind for every output component."""
    components = {}
    for name in names:
        parts = name.split("/")
        for index in range(len(parts)):
            prefix = "/".join(parts[:index + 1])
            directory = index + 1 < len(parts)
            previous = components.setdefault(prefix.casefold(), (prefix, directory))
            if previous[0] != prefix:
                raise ValueError("case-colliding output path components: " + prefix)
            if previous[1] != directory:
                raise ValueError("file/directory output path conflict: " + prefix)


def publish(files, output, check=False):
    """Keep modified/unowned application files intact, including stale outputs."""
    if MANIFEST not in files:
        raise ValueError("generation manifest is required")
    seen = set()
    for name, data in files.items():
        if name != MANIFEST:
            _path(name)
        if name.casefold() in seen or not isinstance(data, bytes):
            raise ValueError("invalid output publication plan")
        seen.add(name.casefold())
    output = _safe_output(output)
    existing = _inventory(output)
    owned = _ownership(existing)
    for name, digest in owned.items():
        if name in existing and _digest_file(existing[name]) != digest:
            raise ValueError("refusing to overwrite modified generated file: " + name)
    folded = {name.casefold(): name for name in existing}
    for name in files:
        other = folded.get(name.casefold())
        if other is not None and (other != name or (name not in owned and name != MANIFEST)):
            raise ValueError("refusing to overwrite unowned output file: " + other)
    stale = set(owned) - set(files)
    # Stale owned files are removed, while unrelated application paths retain
    # their spelling. Check the resulting tree before creating a staging tree.
    kept = (name for name in existing if name not in stale and name not in files)
    _output_layout((*kept, *files))
    if check:
        different = [name for name, data in files.items()
                     if name not in existing or existing[name].read_bytes() != data]
        if stale or different:
            raise ValueError("generated output is out of date: " + ", ".join(sorted(stale | set(different))))
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".telemetry-codegen-stage-", dir=output.parent))
    backup = None
    try:
        # Copy unrelated regular files without making them owned by the generator.
        for name, path in existing.items():
            if name in stale or name in files:
                continue
            destination = staging / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
        for name, data in files.items():
            destination = staging / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
        if output.exists():
            backup = Path(tempfile.mkdtemp(prefix=".telemetry-codegen-backup-", dir=output.parent))
            backup.rmdir()
            os.replace(output, backup)
        try:
            os.replace(staging, output)
        except BaseException:
            if backup is not None:
                os.replace(backup, output)
                backup = None
            raise
        if backup is not None:
            shutil.rmtree(backup)
            backup = None
    finally:
        if staging.exists():
            shutil.rmtree(staging)
        # Retain an unexpected backup rather than deleting the only old output.


def _read_descriptor(path):
    with Path(path).open("rb") as stream:
        data = stream.read(Limits().descriptor_bytes + 1)
    if len(data) > Limits().descriptor_bytes:
        raise InvalidDescriptor("descriptor budget")
    return data


def _write_descriptor(data, output):
    """Replace an explicitly requested binary file only after full validation."""
    output = Path(output).absolute()
    for path in (output, *output.parents):
        if _linked(path):
            raise ValueError("descriptor output paths cannot contain symlinks or junctions")
    if output.exists() and not output.is_file():
        raise ValueError("descriptor output must be a regular file")
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, staging = tempfile.mkstemp(prefix=".telemetry-descriptor-", dir=output.parent)
    staging = Path(staging)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
        os.replace(staging, output)
    finally:
        if staging.exists():
            staging.unlink()


def main(argv=None):
    parser = argparse.ArgumentParser(description="Offline Telemetry Descriptor v3 SDK generator")
    subcommands = parser.add_subparsers(dest="command", required=True)
    inspect = subcommands.add_parser("inspect", help="validate and describe a local binary descriptor")
    inspect.add_argument("descriptor")
    inspect.add_argument("--namespace", default="device_api")
    generate = subcommands.add_parser("generate", help="generate owned SDK output without a device")
    generate.add_argument("descriptor")
    generate.add_argument("--targets", default="cpp,ts,js")
    generate.add_argument("--client-runtime", action="store_true", help="include optional JS/TS asynchronous FIFO client")
    generate.add_argument("--structural-metadata", action="store_true",
                          help="include optional wire type/catalog/endpoint metadata; no UI hints")
    generate.add_argument('--ux-starter', choices=('web', 'qml'),
                          help='include an optional host-client-owned UX starter; requires its target and structural metadata')
    generate.add_argument('--wire-editing', action='store_true',
                          help='include optional C++/JS/TS codecs preserving original NaN wire bits; enabled by UX starters')
    generate.add_argument("--namespace", default="device_api")
    generate.add_argument("--output", required=True)
    generate.add_argument("--check", action="store_true", help="compare without modifying any files")
    fetch = subcommands.add_parser("fetch", help="download using direct Resource UDP datagrams")
    fetch.add_argument("--host", required=True)
    fetch.add_argument("--port", required=True, type=int)
    fetch.add_argument("--timeout", type=float, default=2.0)
    fetch.add_argument("--path", default="/telemetry/descriptor.bin")
    fetch.add_argument("--output", required=True, help="binary descriptor destination")
    args = parser.parse_args(argv)
    try:
        if args.command == "fetch":
            from .resource import UdpExchange, fetch as download
            with UdpExchange(args.host, args.port, args.timeout) as exchange:
                data = download(exchange, args.path)
            _write_descriptor(data, args.output)
            print("Fetched " + str(len(data)) + " descriptor bytes")
            return 0
        schema = load_schema(_read_descriptor(args.descriptor), args.namespace)
        if args.command == "inspect":
            print(_json(dict(descriptor_sha256=schema.descriptor_sha256,
                             descriptor_fingerprint=f"0x{schema.fingerprint:016x}",
                             descriptor_wire_version=[3, 0], bytes=schema.raw["total"],
                             type_count=len(schema.types), endpoint_count=len(schema.endpoints),
                             symbols=symbol_mapping(schema))), end="")
        else:
            files = render(schema, _targets(args.targets), client_runtime=args.client_runtime,
                           structural_metadata=args.structural_metadata, ux_starter=args.ux_starter,
                           wire_editing=args.wire_editing)
            publish(files, args.output, args.check)
            print(("Verified " if args.check else "Generated ") + str(len(files) - 1) + " files")
        return 0
    except (ValueError, TypeError, OSError, ImportError) as error:
        print("telemetry_codegen: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
