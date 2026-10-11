"""Replay the projection-pass reconciliation: run the C++ scan on the recorded discount unit cashflows and projection
terms of term-rate swaps, then check it against finite differences through the independent full valuation.
Standard library only; all outputs go to a temporary directory."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    scan, base = [Path(s).resolve() for s in sys.argv[1:3]]
    fixture = base / 'projection'
    names = ['wave_buckets.txt', 'unit_cashflows_wave.txt'] + [p.name for p in fixture.glob('wave_risk_*.txt')]
    with tempfile.TemporaryDirectory(prefix='ladder-proj-') as td:
        work = Path(td)
        for n in names:
            shutil.copyfile(fixture / n, work / n)
        for n in ('rfr_reference.py', 'rfr_projection_check.py'):
            shutil.copyfile(base / n, work / n)
        for cmd in ([scan, work], [sys.executable, work / 'rfr_projection_check.py', 'check', work]):
            p = subprocess.run([str(c) for c in cmd], cwd=work, capture_output=True, text=True, timeout=120)
            print(p.stdout.strip())
            if p.returncode != 0:
                raise AssertionError(f'{cmd[0]} failed ({p.returncode})\n{p.stderr}')


if __name__ == '__main__':
    main()
