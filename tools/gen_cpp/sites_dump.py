#!/usr/bin/env python3
"""Reference site resolution dump for check_sites.sh."""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import game_sites
from nso import Nso

nso = Nso(sys.argv[1])
logs = []
version, sites = game_sites.for_module(nso, game_sites.MAIN_BASE, log=logs.append)
fallback = {line.split(': ')[1] for line in logs if ' from the table' in line}
kinds = {site.name: site.kind for site in game_sites.SITES}
print('version', version)
for name, value in sorted(sites.items()):
    print('site', name, kinds[name], f'{value:x}', int(name in fallback))
for index, group in enumerate(game_sites.OPTIONAL_GROUPS):
    print('group', index, int(all(name in sites for name in group)))
