#!/usr/bin/env python3
import ast
import hashlib
from pathlib import Path

source = Path('ps5/tools/package-public-release.py').read_text()
tree = ast.parse(source)
functions = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in ('digest', 'check_font_input')]
namespace = {'hashlib': hashlib, 'Path': Path}
exec(compile(ast.Module(body=functions, type_ignores=[]), '<guard>', 'exec'), namespace)
check = namespace['check_font_input']
check('PPSA90212/FiraSans-SemiBold.ttf', Path('third_party/fonts/FiraSans/FiraSans-SemiBold.ttf').read_bytes())
check('PPSA90212/fonts/OpenSans.ttf', Path('.deps/ps5/opensans/OpenSans.ttf').read_bytes())
for name, data in [('PPSA90212/unapproved.otf', b'font'), ('PPSA90212/fonts/OpenSans.ttf', b'changed'),
                   ('PPSA90212/cache/glyphs/sheet.bin', b'cache'), ('PPSA90212/cache/fonts/sheet.bin', b'cache'),
                   ('PPSA90212/other.woff3', b'font')]:
    try:
        check(name, data)
    except SystemExit:
        continue
    raise AssertionError(f'{name} was allowed')
print('release font guard passed')
