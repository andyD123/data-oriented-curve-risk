"""Replay the RFR reconciliation fixtures: run the C++ scan on the recorded unit cashflows, then check it against
the independent direct-overlap oracle, explicit adjoint, finite-difference convergence, quote risk and hedge solve.
Standard library only; all outputs go to a temporary directory."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    scan, fixture = [Path(s).resolve() for s in sys.argv[1:3]]
    names = ['quotes.txt', 'wave_buckets.txt', 'unit_cashflows_wave.txt', 'rfr_reference.py'] + \
            [p.name for p in fixture.glob('wave_risk_*.txt')]
    with tempfile.TemporaryDirectory(prefix='ladder-rfr-') as td:
        work = Path(td)
        for n in names:
            shutil.copyfile(fixture / n, work / n)
        for cmd in ([scan, work], [sys.executable, work / 'rfr_reference.py', 'check', work]):
            p = subprocess.run([str(c) for c in cmd], cwd=work, capture_output=True, text=True, timeout=120)
            print(p.stdout.strip())
            if p.returncode != 0:
                raise AssertionError(f'{cmd[0]} failed ({p.returncode})\n{p.stderr}')


if __name__ == '__main__':
    main()
