"""Replay immutable fixtures in a temporary directory; failures are process failures.
Only the Python standard library is required. Fully standalone test suite.
"""
from pathlib import Path
import math
import shutil
import subprocess
import sys
import tempfile


def run(args, cwd, success=True):
    p = subprocess.run([str(a) for a in args], cwd=cwd, capture_output=True, text=True, timeout=30)
    if (p.returncode == 0) != success or p.returncode < 0:
        raise AssertionError(f"unexpected exit {p.returncode}: {args}\n{p.stdout}\n{p.stderr}")
    return p


def require(condition):
    if not condition:
        raise AssertionError("negative-test gate failed")


def matrix(path):
    return [[float(v) for v in line.split()] for line in path.read_text().splitlines() if line.strip()]


def agree(actual, expected, rel=1e-11):
    if len(actual) != len(expected):
        return False
    for a, e in zip(actual, expected):
        if len(a) != len(e) or not all(math.isfinite(v) for v in a + e):
            return False
        scale = max([1.0] + [abs(v) for v in e])
        if any(abs(x-y) > 1e-9 + rel*scale for x, y in zip(a, e)):
            return False
    return True


def direct(fixture):
    grids = matrix(fixture / 'wave_buckets.txt')
    def overlap(b, k, t):
        if t <= b[k]:
            return 0.0
        if k == len(b)-2:
            return t-b[k]
        return min(t, b[k+1])-b[k]
    tokens = iter((fixture / 'unit_cashflows_wave.txt').read_text().split())
    n = int(next(tokens)); result = []
    for _ in range(n):
        next(tokens); ne, np = int(next(tokens)), int(next(tokens))
        cash = [(float(next(tokens)), float(next(tokens))) for _ in range(ne)]
        proj = [tuple(float(next(tokens)) for _ in range(4)) for _ in range(np)]
        bd, bp = grids
        d = [math.fsum(-x*overlap(bd,k,t) for t,x in cash) for k in range(len(bd)-1)]
        p = [math.fsum(w*(overlap(bp,k,b)-overlap(bp,k,a)) for _,a,b,w in proj) for k in range(len(bp)-1)]
        result.append(d+p)
    if next(tokens, None) is not None:
        raise AssertionError('trailing fixture input')
    return result


def main():
    scan, bench, fixture = [Path(s).resolve() for s in sys.argv[1:]]
    with tempfile.TemporaryDirectory(prefix='ladder-replay-') as td:
        work = Path(td)
        for name in ('wave_buckets.txt', 'unit_cashflows_wave.txt'):
            shutil.copyfile(fixture/name, work/name)
        print(run([scan,work],work).stdout.strip())
        actual = matrix(work/'scan_wave_risk.txt')
        expected = direct(fixture)
        if not agree(actual,expected):
            raise AssertionError('scan differs from independent direct-overlap oracle')
        reference = matrix(fixture/'wave_risk_5e-05.txt')
        if not agree(actual,reference,rel=2e-7):
            raise AssertionError('scan differs from recorded reference finite differences')
        worst = max(abs(a-b)/max(1.,max(map(abs,e))) for row,e in zip(actual,expected) for a,b in zip(row,e))
        print(f'replayed {len(actual)} instruments; max scaled direct-oracle error {worst:.3e}')
        # Check that the gate itself rejects NaNs, wrong values and truncated rows.
        bad=[r[:] for r in actual]; bad[0][0]=math.nan
        require(not agree(bad,expected))
        bad=[r[:] for r in actual]; bad[0][0]+=1e6
        require(not agree(bad,expected))
        require(not agree(actual[:-1],expected))
        good_buckets=(work/'wave_buckets.txt').read_text()
        for text in ('', '0 0\n0 1\n', '0 1 broken\n0 1\n'):
            (work/'wave_buckets.txt').write_text(text); run([scan,work],work,False)
        (work/'wave_buckets.txt').unlink(); run([scan,work],work,False)
        (work/'wave_buckets.txt').write_text(good_buckets)
        for text in ('', '-1\n', '1\nbad -1 0\n', '1\nbad 1 0\n0\n',
                     '1\nbad 1 0\nnan 100\n', '1\nbad 0 1\n3 2 1 10\n', '0\njunk\n'):
            (work/'unit_cashflows_wave.txt').write_text(text); run([scan,work],work,False)
        (work/'unit_cashflows_wave.txt').unlink(); run([scan,work],work,False)
        (work/'unit_cashflows_wave.txt').write_text('0\n'); run([scan,work],work)
        require((work/'scan_wave_risk.txt').read_text()=='')
        for args in (['0'], ['-1'], ['junk'], ['1','2'], ['1','0','0'], ['1','0','1',work/'missing.txt']):
            run([bench,*args],work,False)
        bad_curve=work/'bad_curve.txt'
        for text in ('', '2\nois -1\n', '2\nois 2 0 1 1 0\n', '2\nois 2 1 1 0 1\n'):
            bad_curve.write_text(text); run([bench,'1','0','1',bad_curve],work,False)
        print('missing/malformed-input and comparison-gate negative tests: PASS')


if __name__ == '__main__':
    main()
