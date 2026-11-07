"""Build the pinned PDFium text-object frontend and bundle its runtime.

Run: python build_scripts/build_pdfium.py --fetch
Use --source to reuse an existing pinned PDFium checkout. Requires the platform
C++ toolchain; release builds use ThinLTO without profile training.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

from validate_pdfium import BRIDGE_ABI

ROOT=Path(__file__).resolve().parents[1]
HERE=ROOT/'c/vendor/pdfium'
SOURCE=ROOT/'build/pdfium'
PDFIUM_REVISION='d5960c8270a7d06203316c2d6f0ba606547fdb07'
DEPOT_REVISION='3218cfc968b246bf2498fd13f707851a0b1cd408'


def fetch():
    workspace=SOURCE.parent
    workspace.mkdir(parents=True,exist_ok=True)
    depot=workspace/'depot_tools'
    env=dict(os.environ,DEPOT_TOOLS_WIN_TOOLCHAIN='0',DEPOT_TOOLS_UPDATE='0',
             CIPD_CACHE_DIR=str(workspace/'cipd-cache'))
    env['PATH']=str(depot)+os.pathsep+str(Path(sys.executable).parent)+os.pathsep+env['PATH']
    for path,url,revision in [
        (depot,'https://chromium.googlesource.com/chromium/tools/depot_tools.git',DEPOT_REVISION),
        (SOURCE,'https://pdfium.googlesource.com/pdfium.git',PDFIUM_REVISION),
    ]:
        if not (path/'.git').exists():
            subprocess.run(['git','init',str(path)],check=True,env=env)
            subprocess.run(['git','-C',str(path),'remote','add','origin',url],check=True,env=env)
            subprocess.run(['git','-C',str(path),'fetch','--depth','1','origin',revision],check=True,env=env)
            subprocess.run(['git','-C',str(path),'checkout','--detach','FETCH_HEAD'],check=True,env=env)
        actual=subprocess.check_output(['git','-C',str(path),'rev-parse','HEAD'],text=True,env=env).strip()
        if actual!=revision: raise RuntimeError('Unexpected source revision in '+str(path)+': '+actual)
    solution=dict(name=SOURCE.name,url='https://pdfium.googlesource.com/pdfium.git',managed=False,
                  custom_vars={'checkout_configuration':'minimal'})
    (workspace/'.gclient').write_text('solutions = '+repr([solution])+'\ntarget_os=[]\n')
    subprocess.run([sys.executable,str(depot/'gclient.py'),'sync','--no-history','--nohooks','-j','8'],
                   cwd=workspace,env=env,check=True)
    # Minimal checkout omits this dependency, although GN still loads its test target.
    simd=SOURCE/'third_party/simdutf'
    if not (simd/'.git').exists():
        subprocess.run(['git','init',str(simd)],check=True,env=env)
        subprocess.run(['git','-C',str(simd),'fetch','--depth','1',
            'https://chromium.googlesource.com/chromium/src/third_party/simdutf',
            'f7356eed293f8208c40b3c1b344a50bd70971983'],check=True,env=env)
        subprocess.run(['git','-C',str(simd),'checkout','--detach','FETCH_HEAD'],check=True,env=env)


def replace(path, old, new):
    text=path.read_text()
    if new in text:
        return
    if old not in text:
        raise RuntimeError('Pinned PDFium build input changed unexpectedly: '+str(path))
    path.write_text(text.replace(old,new,1))


def patch_text_extraction():
    patch=HERE/'text_extraction.patch'
    command=['git','apply','--ignore-space-change','--whitespace=nowarn']
    check=subprocess.run([*command,'--check',str(patch)],cwd=SOURCE,capture_output=True,text=True)
    if check.returncode==0:
        subprocess.run([*command,str(patch)],cwd=SOURCE,check=True)
    else:
        applied=subprocess.run([*command,'--reverse','--check',str(patch)],
                               cwd=SOURCE,capture_output=True,text=True)
        if applied.returncode:
            raise RuntimeError('Cannot apply the text extraction patch; use a clean checkout of '
                               'the pinned PDFium revision.\n'+check.stderr)
    target=SOURCE/'core/fpdfapi/page/doc2struct_text_mode.h'
    contents=(HERE/'text_mode.h').read_text(encoding='utf-8')
    if not target.exists() or target.read_text(encoding='utf-8')!=contents:
        target.write_text(contents,encoding='utf-8')
    replace(SOURCE/'core/fpdfapi/page/BUILD.gn','source_set("page") {\n  sources = [',
        'source_set("page") {\n  sources = [\n    "doc2struct_text_mode.h",')


def runtime_key():
    arch=platform.machine().lower()
    arch={'amd64':'x86_64','aarch64':'arm64'}.get(arch,arch)
    if sys.platform=='win32':
        return 'win_'+('amd64' if sys.maxsize>2**32 and arch=='x86_64' else 'arm64' if arch=='arm64' else 'x86')
    return ('macos' if sys.platform=='darwin' else 'linux')+'_'+arch


def bundle(binary, args, key, *, cross=False):
    if not cross:
        dll=ctypes.CDLL(str(binary.resolve()))
        dll.DS_PDF_BridgeVersion.restype=ctypes.c_uint
        if dll.DS_PDF_BridgeVersion()!=BRIDGE_ABI:
            raise RuntimeError('Unexpected PDFium bridge ABI')
    target=HERE/'runtime'/key
    target.mkdir(parents=True,exist_ok=True)
    shutil.copy2(binary,target/('doc2struct_pdfium'+binary.suffix))
    notices=target/'licenses'
    notices.mkdir(exist_ok=True)
    shutil.copy2(SOURCE/'LICENSE',notices/'pdfium.txt')
    # Preserve notices from source dependencies rather than a different vendor's
    # binary build. Includes optional dependencies as well as linked libraries.
    parts=[]
    dependencies=['abseil-cpp','agg23','bigint','brotli','cpu_features','dragonbox',
        'fast_float','fp16','freetype','harfbuzz','highway','icu','lcms','libc++',
        'libc++abi','libjpeg_turbo','libopenjpeg','libpng','libtiff','libunwind','llvm-libc',
        'NotoSansBengali2','NotoSansBengali3','NotoSansCJK','simdutf','zlib']
    for name in dependencies:
        for path in sorted((SOURCE/'third_party'/name).rglob('*')):
            if path.is_file() and path.name.lower().startswith(('license','copying','notice','ftl.txt','readme.ijg')):
                parts.append(str(path.relative_to(SOURCE))+'\n\n'+path.read_text(encoding='utf-8',errors='replace'))
    (notices/'third_party.txt').write_text('\n\n'.join(parts),encoding='utf-8')
    metadata=dict(origin='doc2struct',major=149,minor=0,build=7825,patch=0,
        revision=PDFIUM_REVISION,bridge_abi=BRIDGE_ABI,platform=key,
        text_patch_sha256=hashlib.sha256((HERE/'text_extraction.patch').read_bytes()).hexdigest(),
        sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),gn_args=args)
    (target/'version.json').write_text(json.dumps(metadata,indent=2)+'\n')
    print('Bundled '+str(target),flush=True)


def main():
    global SOURCE
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=SOURCE)
    parser.add_argument('--fetch',action='store_true')
    parser.add_argument('--target-cpu',choices=('x64','arm64'),help='Linux target; ARM is cross-compiled with the pinned Debian sysroot')
    parser.add_argument('--jobs',type=int,default=4)
    options=parser.parse_args()
    SOURCE=options.source.resolve()
    if options.jobs < 1: parser.error('--jobs must be positive')
    if options.target_cpu and sys.platform!='linux': parser.error('--target-cpu is supported only on Linux')
    if sys.platform=='linux' and platform.machine().lower() not in ('x86_64','amd64'):
        parser.error('The pinned Chromium Linux toolchain runs on x86-64; cross-compile ARM using --target-cpu arm64')
    if options.fetch: fetch()
    revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=SOURCE,text=True).strip()
    if revision!=PDFIUM_REVISION: raise RuntimeError('Unexpected PDFium source revision: '+revision)
    patch_text_extraction()
    replace(SOURCE/'.gn','script_executable = "python3"','script_executable = "'+Path(sys.executable).as_posix()+'"')
    for name in ['bridge.cpp','bridge.h']:
        contents=(HERE/name).read_text()
        if name=='bridge.h': contents=contents.replace('"fpdfview.h"','"public/fpdfview.h"')
        target=SOURCE/'fpdfsdk'/name
        if not target.exists() or target.read_text()!=contents: target.write_text(contents)
    replace(SOURCE/'fpdfsdk/BUILD.gn','source_set("fpdfsdk") {\n  sources = [',
        'source_set("fpdfsdk") {\n  sources = [\n    "bridge.cpp",\n    "bridge.h",')
    replace(SOURCE/'BUILD.gn','component("pdfium") {','shared_library("pdfium") {')
    replace(SOURCE/'BUILD.gn','config("pdfium_public_config") {\n  defines = []',
        'config("pdfium_public_config") {\n  defines = [ "FPDF_SHARED_LIBRARY" ]')
    replace(SOURCE/'public/fpdfview.h','#if defined(COMPONENT_BUILD)\n',
        '#if defined(COMPONENT_BUILD) || defined(FPDF_SHARED_LIBRARY)\n')
    args='''is_debug = false
is_component_build = false
is_official_build = true
use_thin_lto = true
chrome_pgo_phase = 0
pdf_enable_v8 = false
pdf_enable_xfa = false
pdf_use_skia = false
pdf_use_partition_alloc = false
pdf_enable_fontations = false
pdf_enable_rust_png = false
enable_rust = false
use_custom_libcxx = false
clang_use_chrome_plugins = false
treat_warnings_as_errors = false
symbol_level = 0
icu_use_data_file = false
compute_build_timestamp = "//build/doc2struct_timestamp.py"
'''
    (SOURCE/'build/doc2struct_timestamp.py').write_text('from pathlib import Path\nprint((Path(__file__).parent / "util/LASTCHANGE.committime").read_text().strip())\n')
    env=dict(os.environ,DEPOT_TOOLS_WIN_TOOLCHAIN='0',DEPOT_TOOLS_UPDATE='0')
    env['PATH']=str(SOURCE.parent/'depot_tools')+os.pathsep+str(Path(sys.executable).parent)+os.pathsep+env['PATH']
    key=runtime_key()
    cross=False
    if sys.platform=='linux':
        cpu=options.target_cpu or 'x64'
        cross=cpu=='arm64'
        key='linux_'+('arm64' if cross else 'x86_64')
        # Statically link Chromium's libc++ instead of inheriting the build
        # host's libstdc++ ABI. Bullseye's glibc is below the 2.34 release floor.
        args=args.replace('use_custom_libcxx = false','use_custom_libcxx = true')
        args+=f'target_cpu = "{cpu}"\nuse_sysroot = true\n'
        # Official x64 builds enable CFI by default, but standalone PDFium
        # does not include Chromium's tools/cfi/ignores.txt.
        args+='is_cfi = false\nuse_cfi_icall = false\n'
        for arch in sorted({'amd64','arm64' if cross else 'amd64'}):
            subprocess.run([sys.executable,'build/linux/sysroot_scripts/install-sysroot.py','--arch='+arch],cwd=SOURCE,env=env,check=True)
    elif sys.platform=='darwin':
        args+='mac_deployment_target = "14.0"\n'
        # Official builds request dSYMs even with symbol_level=0. Avoid the
        # dsymutil hook dependency when building this stripped runtime.
        args+='enable_dsyms = false\n'
    if sys.platform=='win32':
        vswhere=Path(os.environ.get('ProgramFiles(x86)','C:/Program Files (x86)'))/'Microsoft Visual Studio/Installer/vswhere.exe'
        vs=Path(subprocess.check_output([str(vswhere),'-latest','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath'],text=True).strip())
        version='2026' if '/18/' in vs.as_posix() else '2022'
        sdk=Path(os.environ.get('ProgramFiles(x86)','C:/Program Files (x86)'))/'Windows Kits/10'
        sdk_version=max((p.name for p in (sdk/'Include').iterdir() if (p/'um/windows.h').exists()),key=lambda s:tuple(map(int,s.split('.'))))
        runtimes=sorted((vs/'VC/Redist/MSVC').glob('*/x64/Microsoft.VC*.CRT'))
        if not runtimes: raise RuntimeError('Visual C++ runtime directory not found')
        toolchain=SOURCE/'build/config/win/visual_studio_version.gni'
        contents=toolchain.read_text().replace('visual_studio_runtime_dirs = []','visual_studio_runtime_dirs = "'+runtimes[-1].as_posix()+'"')
        toolchain.write_text(contents)
        for indent in ['    ','  ']:
            script=SOURCE/'build/vs_toolchain.py'
            script.write_text(script.read_text().replace('\n'+indent+'_CopyDebugger(target_dir, target_cpu)',
                '\n'+indent+'# Standalone release build does not package Windows debugging tools.'))
        args+=f'visual_studio_path = "{vs.as_posix()}"\nvisual_studio_version = "{version}"\nwindows_sdk_path = "{sdk.as_posix()}"\nwdk_path = "{sdk.as_posix()}"\nwindows_sdk_version = "{sdk_version}"\n'
        env['vs'+version+'_install']=str(vs)
        env['GYP_MSVS_OVERRIDE_PATH']=str(vs)
        env['GYP_MSVS_VERSION']=version
    output='out/doc2struct_'+key
    destination=SOURCE/output
    destination.mkdir(parents=True,exist_ok=True)
    (destination/'args.gn').write_text(args)
    host='win' if sys.platform=='win32' else 'mac' if sys.platform=='darwin' else 'linux64'
    suffix='.exe' if sys.platform=='win32' else ''
    subprocess.run([sys.executable,'build/util/lastchange.py','-o','build/util/LASTCHANGE'],cwd=SOURCE,env=env,check=True)
    subprocess.run([str(SOURCE/f'buildtools/{host}/gn{suffix}'),'gen',output],cwd=SOURCE,env=env,check=True)
    subprocess.run([str(SOURCE/f'third_party/ninja/ninja{suffix}'),'-C',output,'pdfium','-j',str(options.jobs)],cwd=SOURCE,env=env,check=True)
    library='pdfium.dll' if sys.platform=='win32' else 'libpdfium.dylib' if sys.platform=='darwin' else 'libpdfium.so'
    bundle(destination/library,args,key,cross=cross)


if __name__=='__main__': main()


