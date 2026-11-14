#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else '.').resolve()
libmk = root / 'lib' / 'NTMakefile'


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding='utf-8')
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{path}: expected exactly one match, found {count}: {old!r}')
    path.write_text(text.replace(old, new), encoding='utf-8')

replace_once(
    libmk,
    'libsasl_res = libsasl.res\n'
    'libsasl_out = libsasl.dll libsasl.exp libsasl.lib $(libsasl_res)\n',
    'libsasl_res = libsasl.res\n'
    '\n'
    '!IF "$(STATIC_BUNDLE)" == "1"\n'
    'static_plugin_objs = plain.obj scram.obj\n'
    'libsasl_out = libsasl2-static.lib\n'
    'STATIC_PLUGIN_FLAGS = /D "STATIC_PLAIN" /D "STATIC_SCRAM" /D "HAVE_SHA512=1" /D "SASL_STATIC_ONLY"\n'
    '!ELSE\n'
    'static_plugin_objs =\n'
    'libsasl_out = libsasl.dll libsasl.exp libsasl.lib $(libsasl_res)\n'
    'STATIC_PLUGIN_FLAGS =\n'
    '!ENDIF\n'
)

replace_once(
    libmk,
    'CPPFLAGS = /wd4996 /D NEED_GETOPT /I "..\\win32\\include" /I "." /I "..\\include" /I "..\\common" /D "LIBSASL_EXPORTS" $(OPENSSL_FLAGS) $(COMMON_CPPFLAGS)\n',
    'CPPFLAGS = /wd4996 /D NEED_GETOPT /I "..\\win32\\include" /I "." /I "..\\include" /I "..\\plugins" /I "..\\common" /D "LIBSASL_EXPORTS" $(OPENSSL_FLAGS) $(STATIC_PLUGIN_FLAGS) $(COMMON_CPPFLAGS)\n'
)

replace_once(
    libmk,
    'all_objs = $(libsasl_objs)\n',
    'all_objs = $(libsasl_objs) $(static_plugin_objs)\n'
)

replace_once(
    libmk,
    'install: libsasl.dll\n'
    '\t@echo libsasl.exp > $(exclude_list)\n',
    '!IF "$(STATIC_BUNDLE)" == "1"\n'
    'install: libsasl2-static.lib\n'
    '\t@if not exist "$(libdir)" mkdir "$(libdir)"\n'
    '\t@copy /Y libsasl2-static.lib "$(libdir)\\libsasl2-static.lib"\n'
    '!ELSE\n'
    'install: libsasl.dll\n'
    '\t@echo libsasl.exp > $(exclude_list)\n'
)

replace_once(
    libmk,
    '\t@xcopy libsasl.l* "$(libdir)" /I /F /Y\n\n'
    'all-recursive: libsasl.dll\n\n'
    'libsasl.dll: $(libsasl_objs) $(libsasl_res)\n',
    '\t@xcopy libsasl.l* "$(libdir)" /I /F /Y\n'
    '!ENDIF\n\n'
    '!IF "$(STATIC_BUNDLE)" == "1"\n'
    'all-recursive: libsasl2-static.lib\n\n'
    'libsasl2-static.lib: $(all_objs)\n'
    '\tlib /NOLOGO /OUT:"libsasl2-static.lib" $(all_objs)\n'
    '!ELSE\n'
    'all-recursive: libsasl.dll\n\n'
    'libsasl.dll: $(libsasl_objs) $(libsasl_res)\n'
)

replace_once(
    libmk,
    '\tIF EXIST $@.manifest mt -manifest $@.manifest -outputresource:$@;2\n\n'
    'plugin_common.c:',
    '\tIF EXIST $@.manifest mt -manifest $@.manifest -outputresource:$@;2\n'
    '!ENDIF\n\n'
    'plugin_common.c:'
)

replace_once(
    libmk,
    '.c.obj::\n'
    '   $(CPP) @<<\n'
    '   $(CPP_PROJ) $< \n'
    '<<\n',
    '.c.obj::\n'
    '   $(CPP) @<<\n'
    '   $(CPP_PROJ) $< \n'
    '<<\n'
    '\n'
    '{..\\plugins}.c.obj::\n'
    '   $(CPP) @<<\n'
    '   $(CPP_PROJ) $<\n'
    '<<\n'
)

# The Windows loader otherwise scans registry-configured directories even when
# all mechanisms are embedded. Keep that behavior for ordinary Cyrus builds.
loader = root / 'lib' / 'windlopen.c'
replace_once(
    loader,
    '    int result;\n'
    '    TCHAR cur_dir[PATH_MAX], full_name[PATH_MAX+2], prefix[PATH_MAX+2];\n',
    '    int result;\n'
    '    const add_plugin_list_t *cur_ep;\n'
    '#ifndef SASL_STATIC_ONLY\n'
    '    TCHAR cur_dir[PATH_MAX], full_name[PATH_MAX+2], prefix[PATH_MAX+2];\n'
)
replace_once(
    loader,
    '    int position;\n'
    '    const add_plugin_list_t *cur_ep;\n',
    '    int position;\n'
)
replace_once(
    loader,
    '    size_t prefix_len;\n',
    '    size_t prefix_len;\n'
    '#endif\n'
)
replace_once(
    loader,
    '    /* get the path to the plugins */\n',
    '#ifdef SASL_STATIC_ONLY\n'
    '    return SASL_OK;\n'
    '#else\n'
    '    /* get the path to the plugins */\n'
)
replace_once(
    loader,
    '    return retCode;\n'
    '}\n\n'
    'int\n'
    '_sasl_done_with_plugins(void)\n',
    '    return retCode;\n'
    '#endif\n'
    '}\n\n'
    'int\n'
    '_sasl_done_with_plugins(void)\n'
)

print(f'Patched {libmk} and {loader}')
