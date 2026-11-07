#!/usr/bin/env python3
"""Installed core/optional UX/full codegen ownership and byte compatibility. MIT."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import zipfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PACKAGE_FILES = ('setup.py','pyproject.toml','MANIFEST.in','VERSION','LICENSE','README.md')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(directory):
    return {p.relative_to(directory).as_posix():digest(p) for p in sorted(directory.rglob('*')) if p.is_file()}


def requirements(metadata):
    return sorted(line.partition(':')[2].replace(' ','').replace('(','').replace(')','')
                  for line in metadata.splitlines() if line.startswith('Requires-Dist:'))


def inputs(ux):
    sdk_paths = [ROOT/name for name in PACKAGE_FILES]
    sdk_paths += [ROOT/'packaging/full'/name for name in PACKAGE_FILES]
    for folder in ('tools/telemetry_codegen','packaging/full','tests/ux_codegen_extension'):
        sdk_paths += [p for p in (ROOT/folder).rglob('*') if p.is_file() and '__pycache__' not in p.parts
                      and p.suffix in ('.py','.toml','.in','.json','.md')]
    sdk_paths += [ROOT/'lib/client/web'/name for name in ('Client.ts','Client.js','Client.d.ts')]
    sdk_paths += [ROOT/'tests/structured/descriptor'/(name+'.hex') for name in ('empty','mixed','edge')]
    ux_paths = [ux/name for name in (*PACKAGE_FILES,'MIGRATION.md')]
    ux_paths += [p for p in (ux/'python').rglob('*.py') if '__pycache__' not in p.parts]
    ux_paths += [ux/'tests/codegen_qml'/name for name in ('test_generator.py','descriptor_fixture.py')]
    return {**{'sdk/'+p.relative_to(ROOT).as_posix():digest(p) for p in sorted(set(sdk_paths))},
            **{'ux/'+p.relative_to(ux).as_posix():digest(p) for p in sorted(set(ux_paths))}}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,required=True)
    parser.add_argument('--ux-root',type=Path,required=True,help='Explicit sibling Telemetry UX source')
    args=parser.parse_args()
    ux=args.ux_root.resolve();out=args.build_dir.resolve()
    for source in (ROOT,ux):
        if out==source or source in out.parents or out in source.parents:
            parser.error('Use a fresh external output directory')
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        parser.error('Output must be fresh and empty; nothing is deleted')
    if not (ux/'python/telemetry_ux_codegen/__init__.py').is_file():
        parser.error('--ux-root must identify the optional UX generator source')
    out.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1',PYTHONNOUSERSITE='1',
        PIP_DISABLE_PIP_VERSION_CHECK='1',PIP_CACHE_DIR=str(out/'pip-cache'))
    env.pop('PYTHONPATH',None)
    report=dict(passed=False,hardware=False,started_utc=datetime.now(timezone.utc).isoformat(),
        source_sha256_before=inputs(ux),checks=0,commands=[],wheels={},golden_profiles=[])
    report['source_head']=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    baseline=json.loads((HERE/'legacy_output_hashes.json').read_text())
    report['baseline_commit']=baseline['baseline_commit']
    report['runtime_commit']=baseline['runtime_commit']
    version=(ROOT/'VERSION').read_text().strip()

    def save():
        (out/'summary.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf8')

    def check(condition,label):
        report['checks']+=1
        if not condition: raise RuntimeError(label)

    def run(name,command,success=True,cwd=out):
        command=list(map(str,command))
        result=subprocess.run(command,cwd=cwd,env=env,text=True,stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,timeout=600)
        log=out/(name+'.log');log.write_text(result.stdout,encoding='utf8')
        report['commands'].append(dict(name=name,command=command,returncode=result.returncode,
            log=log.name,sha256=digest(log)))
        save();check((result.returncode==0)==success,name+':\n'+result.stdout[-16000:])
        print(name+': exit='+str(result.returncode),flush=True)
        return result.stdout

    def prepare(name,source,core=False):
        destination=out/('prepared-'+name);destination.mkdir()
        for item in PACKAGE_FILES: shutil.copy2(source/item,destination/item)
        if core:
            shutil.copytree(source/'tools/telemetry_codegen',destination/'tools/telemetry_codegen',
                ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
            (destination/'lib/client/web').mkdir(parents=True)
            for item in ('Client.ts','Client.js','Client.d.ts'):
                shutil.copy2(source/'lib/client/web'/item,destination/'lib/client/web'/item)
        elif name=='ux':
            shutil.copy2(source/'MIGRATION.md',destination/'MIGRATION.md')
            shutil.copytree(source/'python',destination/'python',ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
        return destination

    def wheel(name,source):
        sd=out/('sdist-'+name);sd.mkdir()
        target_dist=out/'legacy-owner-dist' if name=='legacy-owner' else dist
        target_dist.mkdir(exist_ok=True)
        run(name+'-sdist',[sys.executable,'setup.py','sdist','--dist-dir',sd],cwd=source)
        archives=list(sd.glob('*.tar.gz'));check(len(archives)==1,'One '+name+' source archive')
        run(name+'-wheel',[sys.executable,'-m','pip','wheel','--no-deps','--no-index',
            '--no-build-isolation','--wheel-dir',target_dist,archives[0]])
        wheels=list(target_dist.glob({'core':'telemetry_codegen_core-','ux':'telemetry_ux_codegen-',
            'full':'telemetry_codegen-','legacy-owner':'telemetry_codegen-'}[name]+'*.whl'))
        check(len(wheels)==1,'One '+name+' wheel')
        path=wheels[0]
        with zipfile.ZipFile(path) as archive:
            check(archive.testzip() is None,'Wheel CRC '+name)
            metadata=archive.read(next(n for n in archive.namelist() if n.endswith('.dist-info/METADATA'))).decode()
            names=archive.namelist()
            entry=next((n for n in names if n.endswith('.dist-info/entry_points.txt')),None)
            entrypoints=archive.read(entry).decode() if entry else ''
        report['wheels'][name]=dict(path=path.relative_to(out).as_posix(),sha256=digest(path),
            metadata=metadata,entry_points=entrypoints,files=names)
        return path

    def venv(name):
        root=out/('venv-'+name)
        run('venv-'+name,[sys.executable,'-m','venv',root])
        return root,root/('Scripts/python.exe' if os.name=='nt' else 'bin/python'),\
            root/('Scripts/telemetry-codegen.exe' if os.name=='nt' else 'bin/telemetry-codegen')

    def generate(name,cli,fixture,profile,guard=False):
        label=fixture+'-'+profile;output=out/(name+'-'+label)
        targets='qml' if profile=='qml' else 'cpp,js,ts,qml' if profile.startswith('full') else 'cpp,js,ts'
        command=[cli] if not guard else [core_python,consumer]
        command+=['generate',fixtures/(fixture+'.bin'),'--targets',targets,'--output',output]
        if profile.endswith('options'): command+=['--client-runtime','--structural-metadata']
        run(name+'-'+label,command)
        expected=baseline['profiles'][label]
        check(digest(fixtures/(fixture+'.bin'))==expected['descriptor_sha256'],'Frozen Descriptor '+label)
        actual=inventory(output)
        check(actual==expected['generated'],'Exact output including explicit runtime baseline '+label)
        manifest=json.loads((output/'manifest.json').read_text(encoding='utf8'))
        check(manifest['generated_files']=={name:value for name,value in actual.items() if name!='manifest.json'},
            'Manifest owns the actual generated bytes '+label)
        if profile.endswith('options'):
            for generated,canonical in (('js/client.js','Client.js'),('js/client.d.ts','Client.d.ts'),
                                         ('ts/client.ts','Client.ts')):
                # The text emitter canonicalizes platform newlines to LF. The
                # installed wheel's raw assets are checked separately above.
                current=(ROOT/'lib/client/web'/canonical).read_text(encoding='utf8').encode('utf8')
                check((output/generated).read_bytes()==current,'Canonical current runtime '+label+' '+canonical)
                check(hashlib.sha256(current).hexdigest()==baseline['runtime_sha256'][canonical],
                    'Explicit runtime baseline '+label+' '+canonical)
        run(name+'-'+label+'-check',[*command,'--check'])
        report['golden_profiles'].append(label)
        return output

    try:
        run('python-version',[sys.executable,'--version'])
        sources={name:prepare(name,path,name=='core') for name,path in
            [('core',ROOT),('ux',ux),('full',ROOT/'packaging/full')]}
        dist=out/'dist';dist.mkdir()
        wheels={name:wheel(name,source) for name,source in sources.items()}
        core_meta=report['wheels']['core'];ux_meta=report['wheels']['ux'];full_meta=report['wheels']['full']
        check(not any(n.startswith('tools/telemetry_codegen/generators/qml') for n in core_meta['files']),
            'Minimal core wheel contains no QML implementation')
        check('telemetry-codegen = tools.telemetry_codegen.cli:main' in core_meta['entry_points'],
            'Core owns the existing CLI')
        check(not any(l.startswith('Requires-Dist:') for l in core_meta['metadata'].splitlines()),
            'Core has no mandatory UX or Qt dependency')
        check(not ux_meta['entry_points'] and not full_meta['entry_points'],'UX/full own no CLI')
        check(not any(n.startswith('tools/') for n in ux_meta['files']),'UX wheel never bundles SDK modules')
        check(not any(not n.split('/')[0].endswith('.dist-info') for n in full_meta['files']),
            'Legacy full metapackage owns no importable modules')
        check(requirements(ux_meta['metadata'])==['telemetry-codegen-core=='+version],
            'UX requires only the exact core version')
        check(requirements(full_meta['metadata'])==['telemetry-codegen-core=='+version,'telemetry-ux-codegen=='+version],
            'Full requires both exact real implementations')
        for name in ('Client.ts','Client.js','Client.d.ts'):
            with zipfile.ZipFile(wheels['core']) as archive:
                check(archive.read('tools/telemetry_codegen/_runtime/'+name)==(ROOT/'lib/client/web'/name).read_bytes(),
                    'One canonical runtime resource '+name)
        fixtures=out/'fixtures';fixtures.mkdir()
        for name in ('empty','mixed','edge'):
            (fixtures/(name+'.bin')).write_bytes(bytes.fromhex((ROOT/'tests/structured/descriptor'/(name+'.hex')).read_text()))
        consumer=out/'CoreConsumer.py';shutil.copy2(HERE/'CoreConsumer.py',consumer)
        for source in sources.values(): source.rename(source.with_name('unavailable-'+source.name))
        core_root,core_python,core_cli=venv('core')
        run('install-core',[core_python,'-m','pip','install','--no-index','--no-deps',wheels['core']])
        run('core-installed-identity',[core_python,'-c',
            "from importlib.metadata import distribution;import importlib.util;import tools.telemetry_codegen as c;"
            "from pathlib import Path;import sys;"
            "assert Path(c.__file__).resolve().is_relative_to(Path(sys.prefix).resolve());"
            "assert importlib.util.find_spec('telemetry_ux_codegen') is None;"
            "assert len([e for e in distribution('telemetry-codegen-core').entry_points if e.name=='telemetry-codegen'])==1;"
            "print(c.__file__)"])
        run('core-module-help',[core_python,'-m','tools.telemetry_codegen','--help'])
        run('installed-web-starter-controls',[core_python,HERE/'StarterControls.py',
            '--descriptor',fixtures/'mixed.bin','--output-dir',out/'web-starter-controls'])
        starter_controls=json.loads((out/'web-starter-controls/summary.json').read_text())
        check(starter_controls.get('passed') and starter_controls.get('checks',0)>=76,
            'Installed core optional web hook and atomic publication controls')
        report['web_starter_controls']=starter_controls
        for fixture in ('empty','mixed','edge'):
            for profile in ('core','core-options'): generate('minimal',core_cli,fixture,profile,guard=True)
        # Missing optional UI code must not publish a partial C++ SDK, even in a mixed target request.
        for target in ('qml','cpp,qml'):
            output=out/('missing-'+target.replace(',','-'))
            text=run('missing-'+target.replace(',','-'),[core_cli,'generate',fixtures/'mixed.bin',
                '--targets',target,'--output',output],success=False)
            check('telemetry-ux-codegen' in text,'Missing extension identifies the installable package')
            check(not output.exists(),'Missing extension creates no output')
        fault_root=out/'faults';fault_root.mkdir()
        preamble="from tools.telemetry_codegen.emitter_api import GeneratedTarget\nAPI_VERSION=1\nIR_VERSION=1\nSDK_VERSION="+repr(version)+'\n'
        faults={
            'api':("API_VERSION=99\n",'API_VERSION'),
            'api-bool':("API_VERSION=True\n",'API_VERSION'),
            'ir':("IR_VERSION=99\n",'IR_VERSION'),
            'sdk':("SDK_VERSION='9.9.9'\n",'SDK_VERSION'),
            'missing-generate':('generate_qml=None\n','generate_qml'),
            'wrong-target':('def generate_qml(schema): return {}\n','GeneratedTarget'),
            'bad-symbols':("def generate_qml(schema): return GeneratedTarget({},[])\n",'symbol_mapping'),
            'bad-files':("def generate_qml(schema): return GeneratedTarget([], {})\n",'GeneratedTarget'),
            'list-pairs':("def generate_qml(schema): return GeneratedTarget([('qml/Probe.hpp','bad')], {})\n",'GeneratedTarget'),
            'nan':("def generate_qml(schema): return GeneratedTarget({}, {'bad':float('nan')})\n",'Out of range'),
            'infinity':("def generate_qml(schema): return GeneratedTarget({}, {'bad':float('inf')})\n",'Out of range'),
            'nested-import':("import missing_ux_internal_dependency\n",'Cannot import installed Telemetry UX'),
            'escape':("def generate_qml(schema): return GeneratedTarget({'../escape.hpp':'bad'}, {})\n",'output path'),
            'manifest':("def generate_qml(schema): return GeneratedTarget({'manifest.json':'bad'}, {})\n",'output path'),
            'collision':("def generate_qml(schema): return GeneratedTarget({'CPP/DEVICE_API/TYPES.HPP':'bad'}, {})\n",'colliding'),
            'binary':("def generate_qml(schema): return GeneratedTarget({'qml/Probe.hpp':b'bad'}, {})\n",'must be text')}
        for name,(body,diagnostic) in faults.items():
            folder=fault_root/name;(folder/'telemetry_ux_codegen').mkdir(parents=True)
            (folder/'telemetry_ux_codegen/__init__.py').write_text(preamble+body)
            output=out/('fault-output-'+name);output.mkdir();(output/'application.txt').write_bytes(b'keep unchanged\x00\xff')
            before=inventory(output)
            script="import sys;sys.path.insert(0,"+repr(str(folder))+");from tools.telemetry_codegen.cli import main;raise SystemExit(main(sys.argv[1:]))"
            text=run('fault-'+name,[core_python,'-c',script,'generate',fixtures/'mixed.bin',
                '--targets','cpp,qml','--output',output],success=False)
            check(diagnostic.casefold() in text.casefold(),'Specific extension refusal '+name)
            check(inventory(output)==before,'Failure preserves application output '+name)
        # The public seam accepts any real Mapping, not only a mutable dictionary.
        proxy=fault_root/'mapping-proxy';(proxy/'telemetry_ux_codegen').mkdir(parents=True)
        (proxy/'telemetry_ux_codegen/__init__.py').write_text(preamble+
            "from types import MappingProxyType\ndef generate_qml(schema): return GeneratedTarget(MappingProxyType({'qml/Probe.hpp':'// probe\\n'}), {'probe':True})\n")
        proxy_output=out/'mapping-proxy-output'
        proxy_script="import sys;sys.path.insert(0,"+repr(str(proxy))+");from tools.telemetry_codegen.cli import main;raise SystemExit(main(sys.argv[1:]))"
        run('mapping-proxy',[core_python,'-c',proxy_script,'generate',fixtures/'mixed.bin',
            '--targets','qml','--output',proxy_output])
        check((proxy_output/'qml/Probe.hpp').read_text()=='// probe\n','MappingProxy source text')
        check(json.loads((proxy_output/'manifest.json').read_text())['generated_symbol_mapping']['qml']=={'probe':True},
            'MappingProxy symbol mapping reaches the unchanged manifest seam')
        full_root,full_python,full_cli=venv('full')
        run('install-full',[full_python,'-m','pip','install','--no-index','--find-links',dist,wheels['full']])
        run('full-installed-identity',[full_python,'-c',
            "from importlib.metadata import distribution;from pathlib import Path;import sys;"
            "import tools.telemetry_codegen as c;import telemetry_ux_codegen as u;"
            "assert Path(c.__file__).resolve().is_relative_to(Path(sys.prefix).resolve());"
            "assert Path(u.__file__).resolve().is_relative_to(Path(sys.prefix).resolve());"
            "assert not distribution('telemetry-codegen').entry_points;"
            "assert not distribution('telemetry-ux-codegen').entry_points;"
            "assert c.__version__==u.SDK_VERSION=="+repr(version)+";print(c.__file__,u.__file__)"])
        run('full-pip-check',[full_python,'-m','pip','check'])
        for fixture in ('empty','mixed','edge'):
            for profile in ('qml','full','full-options'): generate('legacy',full_cli,fixture,profile)
        web_output=out/'installed-web-starter'
        web_command=[full_cli,'generate',fixtures/'mixed.bin','--targets','js',
            '--structural-metadata','--ux-starter','web','--output',web_output]
        run('full-web-starter',web_command)
        run('full-web-starter-check',[*web_command,'--check'])
        check(set(inventory(web_output/'ui'))=={'DevicePage.js','DevicePage.d.ts','README.md'},
            'The real installed UX web starter owns only three thin files')
        # Run UX-owned QML unit/fixture source with the installed implementations.
        # Headless SDK golden profiles above remain an independent frozen oracle.
        units=out/'installed-legacy-units'
        unit_source=ux/'tests/codegen_qml'
        for name in ('test_generator.py','descriptor_fixture.py'):
            target=units/'tests/codegen_qml'/name
            target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(unit_source/name,target)
        script=("import tools.telemetry_codegen as c;import telemetry_ux_codegen as u;import sys,unittest;from pathlib import Path;"
            "assert Path(c.__file__).resolve().is_relative_to(Path(sys.prefix).resolve());"
            "assert Path(u.__file__).resolve().is_relative_to(Path(sys.prefix).resolve());"
            "suite=unittest.defaultTestLoader.discover("+repr(str(units/'tests/codegen_qml'))+",pattern='test_generator.py');"
            "count=suite.countTestCases();assert count==9;result=unittest.TextTestRunner(verbosity=2).run(suite);"
            "print('Installed legacy QML tests='+str(count));raise SystemExit(not result.wasSuccessful())")
        run('installed-legacy-qml-units',[full_python,'-c',script])
        report['legacy_qml_unit_tests']=9
        # Removing the module-free compatibility name must leave both real implementations usable.
        run('uninstall-full-metapackage',[full_python,'-m','pip','uninstall','-y','telemetry-codegen'])
        run('full-removal-keeps-qml',[full_cli,'generate',fixtures/'mixed.bin','--targets','qml',
            '--output',out/'after-full-uninstall'])
        check(inventory(out/'after-full-uninstall')==baseline['profiles']['mixed-qml']['generated'],
            'Removing the metapackage does not own modules or the CLI')
        # Removing the UI generator must not uninstall the still usable core's modules/CLI.
        run('uninstall-ux',[full_python,'-m','pip','uninstall','-y','telemetry-ux-codegen'])
        output=out/'after-ux-uninstall'
        run('core-survives-ux-uninstall',[full_cli,'generate',fixtures/'mixed.bin','--targets','cpp,js,ts','--output',output])
        check(inventory(output)==baseline['profiles']['mixed-core']['generated'],'Core output after UX removal')
        run('qml-missing-after-uninstall',[full_cli,'generate',fixtures/'mixed.bin','--targets','qml',
            '--output',out/'after-uninstall-qml'],success=False)
        check(not (out/'after-uninstall-qml').exists(),'No partial output after UX removal')
        # Ownership-only migration fixture: current modules/CLI under the historical owner name.
        # It intentionally does not claim to reproduce the old renderer implementation.
        legacy=out/'prepared-legacy-owner'
        shutil.copytree(sources['core'].with_name('unavailable-'+sources['core'].name),legacy,
            ignore=shutil.ignore_patterns('build','dist','*.egg-info','__pycache__','*.pyc'))
        setup=legacy/'setup.py';text=setup.read_text()
        check(text.count('name="telemetry-codegen-core"')==1,'Explicit legacy ownership fixture source')
        setup.write_text(text.replace('name="telemetry-codegen-core"','name="telemetry-codegen"'))
        old_wheel=wheel('legacy-owner',legacy)
        migration_root,migration_python,migration_cli=venv('migration')
        run('install-old-owner-fixture',[migration_python,'-m','pip','install','--no-index','--no-deps',old_wheel])
        run('old-owner-record',[migration_python,'-c',
            "from importlib.metadata import distribution;d=distribution('telemetry-codegen');"
            "assert any(str(f)=='tools/telemetry_codegen/cli.py' for f in d.files);"
            "assert any(e.name=='telemetry-codegen' for e in d.entry_points);print(d.metadata['Name'])"])
        run('uninstall-old-owner-first',[migration_python,'-m','pip','uninstall','-y','telemetry-codegen'])
        check(not migration_cli.exists(),'Historical owner CLI removed before installation')
        run('migrate-install-full',[migration_python,'-m','pip','install','--no-index','--find-links',dist,wheels['full']])
        run('migrate-pip-check',[migration_python,'-m','pip','check'])
        run('migrate-generate-qml',[migration_cli,'generate',fixtures/'mixed.bin','--targets','qml',
            '--output',out/'migration-qml'])
        check(inventory(out/'migration-qml')==baseline['profiles']['mixed-qml']['generated'],
            'Documented explicit ownership handoff restores the installed CLI and exact QML')
        report['migration_fixture']='Current module bytes under historical distribution owner; explicit uninstall before install, not automatic upgrade'
        report['source_sha256_after']=inputs(ux)
        check(report['source_sha256_before']==report['source_sha256_after'],'Source inputs changed during qualification')
        report['passed']=True
        print('UX codegen extension checks='+str(report['checks'])+' failures=0',flush=True)
    finally:
        report['finished_utc']=datetime.now(timezone.utc).isoformat();save()


if __name__=='__main__':
    main()
