#!/usr/bin/env python3
"""scenario_bench: a small gated run, then malformed arguments, curve files and LRU_CAPS values.

Every rejected input must exit with status 1 and a message; a crash or a silent success fails the test.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def run(exe, args, env_extra=None):
    env = dict(os.environ)
    env.pop('LRU_CAPS', None)
    env.pop('LRU_ALL', None)
    env.update(env_extra or {})
    return subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True, env=env)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    exe = Path(sys.argv[1]).resolve()

    # a small run with the baseline: all orders at two cache sizes, every run gated against the scalar scan
    p = run(exe, ['2000', '1', '1'], {'LRU_CAPS': '16,64'})
    require(p.returncode == 0, f'gated run failed (exit {p.returncode})\n{p.stdout}\n{p.stderr}')
    require('correctness gate: PASS' in p.stdout, 'gate line missing')
    require('at 64 columns: grouped is' in p.stdout and 'at the miss floor' in p.stdout, 'grouped order did not reach the floor')
    require('largest difference over 9 runs' in p.stdout, 'expected 9 checked runs (2 full table + 6 cache + BASE)')
    print('gated run: PASS')

    with tempfile.TemporaryDirectory(prefix='scenario-bench-') as td:
        work = Path(td)
        curves = {
            'empty.txt': '',
            'bad_header.txt': '2\nois -1\n',
            'bad_factor.txt': '2\nois 2 0 1 1 0\n',
            'bad_order.txt': '2\nois 2 1 1 0 1\n',
        }
        too_many = ['2']
        for name in ('ois', 'ibor'):           # 69 + 69 = 138 waves: more than the 128 the accumulator holds
            too_many.append(f'{name} 70')
            too_many += [f'{0.5 * i} {1.0 / (1.0 + 0.015 * i)}' for i in range(70)]
        curves['too_many_nodes.txt'] = '\n'.join(too_many) + '\n'
        for name, text in curves.items():
            (work / name).write_text(text)

        bad = [
            (['0'], {}), (['-1'], {}), (['junk'], {}), (['1', '2'], {}), (['1', '0', '0'], {}),
            (['100', '0', '1', 'x', 'extra'], {}),
            (['1', '0', '1', work / 'missing.txt'], {}),
            *[(['1', '0', '1', work / name], {}) for name in curves],
            (['100', '0', '1'], {'LRU_CAPS': '0'}), (['100', '0', '1'], {'LRU_CAPS': 'abc'}),
            (['100', '0', '1'], {'LRU_CAPS': '64,'}), (['100', '0', '1'], {'LRU_CAPS': ''}),
        ]
        for args, env in bad:
            p = run(exe, args, env)
            require(p.returncode == 1 and p.stderr.startswith('scenario_bench: '),
                    f'expected exit 1 with a message for {args} {env}; got exit {p.returncode}\n{p.stderr}')
    print(f'{len(bad)} malformed-input cases rejected with exit 1: PASS')


if __name__ == '__main__':
    main()
