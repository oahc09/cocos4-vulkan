#!/usr/bin/env python3
"""Exercise the selected external root using the actual production CMake blocks."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--cmake', default='cmake')
parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[3])
args = parser.parse_args()
source = (args.source_root / 'native/CMakeLists.txt').read_text()
selection = source[source.index('set(EXTERNAL_ROOT '):source.index('\ninclude_directories(${EXTERNAL_ROOT}/sources)')]
start = source.index('if(MSVC)\n    set(V8_DIR') + len('if(MSVC)\n')
runtime = source[start:source.index('    target_compile_options', start)]
with tempfile.TemporaryDirectory(prefix='vulkan external roots ') as directory:
    root = Path(directory)
    for builtin in (False, True):
        case = root / ('builtin fallback' if builtin else 'local external')
        custom, installed = case / 'custom native', case / 'installed native'
        custom.mkdir(parents=True)
        selected = installed if builtin else custom
        external = selected / 'external'
        (external / 'win64/libs/v8/Release').mkdir(parents=True)
        (external / 'CMakeLists.txt').write_text('# fixture external root\n')
        (external / 'win64/libs/v8/Release/v8.dll').write_text('fixture')
        (external / 'win64/libs/codec.dll').write_text('fixture')
        (external / 'win64/windows-runtime-libs.cmake').write_text('''
function(cc_windows_collect_runtime_dlls output)
    set(${output} "${EXTERNAL_ROOT}/toolchain-runtime.dll" PARENT_SCOPE)
endfunction()
''')
        (custom / 'probe.cmake').write_text(f'''
set(CWD "{custom.as_posix()}")
set(BUILTIN_COCOS_X_PATH "{installed.as_posix()}")
{selection}
{runtime}
if(NOT EXTERNAL_ROOT STREQUAL "{external.as_posix()}")
    message(FATAL_ERROR "Wrong external root: ${{EXTERNAL_ROOT}}")
endif()
if(NOT V8_DLLS STREQUAL "{external.as_posix()}/win64/libs/v8/Release/v8.dll")
    message(FATAL_ERROR "V8 runtime lost the selected root: ${{V8_DLLS}}")
endif()
list(LENGTH WINDOWS_DLLS count)
if(NOT count EQUAL 2)
    message(FATAL_ERROR "Runtime dependencies missing: ${{WINDOWS_DLLS}}")
endif()
message(STATUS "PASS: {'builtin fallback' if builtin else 'local external'}")
''')
        subprocess.run([args.cmake, '-P', str(custom / 'probe.cmake')], check=True)
