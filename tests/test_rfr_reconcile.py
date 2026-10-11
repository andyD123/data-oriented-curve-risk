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
<<<<<<< HEAD
        work = Path(td)
        for n in names:
            shutil.copyfile(fixture / n, work / n)
        for cmd in ([scan, work], [sys.executable, work / 'rfr_reference.py', 'check', work]):
            p = subprocess.run([str(c) for c in cmd], cwd=work, capture_output=True, text=True, timeout=120)
            print(p.stdout.strip())
            if p.returncode != 0:
                raise AssertionError(f'{cmd[0]} failed ({p.returncode})\n{p.stderr}')
=======
        root = Path(td)

        def run(*args, cwd):
            proc = subprocess.run([str(x) for x in args], cwd=cwd,
                                  capture_output=True, text=True, timeout=120)
            if proc.stdout.strip():
                print(proc.stdout.strip())
            if proc.returncode:
                raise AssertionError(
                    f'{args[0]} failed ({proc.returncode})\n{proc.stderr}')

        # Reconcile immutable recorded examples, without modifying the checkout.
        recorded = root / 'recorded'
        recorded.mkdir()
        for name in names:
            shutil.copyfile(fixture / name, recorded / name)
        run(scan, recorded, cwd=recorded)
        run(sys.executable, recorded / 'rfr_reference.py', 'check', recorded,
            cwd=recorded)

        # Generate a *fresh* book/curve/risk fixture, rerun C++, then
        # independently check wave overlaps, adjoints, quote Jacobian,
        # bump-step convergence, hedge and cubic-spline negative control.
        fresh = root / 'generated'
        fresh.mkdir()
        run(sys.executable, fixture / 'rfr_reference.py', 'generate', fresh,
            cwd=fresh)
        run(scan, fresh, cwd=fresh)
        run(sys.executable, fixture / 'rfr_reference.py', 'check', fresh,
            cwd=fresh)
        print('PASS: recorded and regenerated RFR reconciliation')
>>>>>>> origin/main


if __name__ == '__main__':
    main()
