#!/usr/bin/env python3
from pathlib import Path
import subprocess,sys,tempfile
def main():
    checker=Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as td:
        w=Path(td)
        with (w/"scan_risk.txt").open("w") as out:
            for i in range(1,8): out.write(f"{i}.0\n{i}.0\n{2*i}.0\n")
        exact=[("eonia",0,224.0,[2.0,3.0]),("euribor",1,196.0,[-1.0,4.0])]
        for name,h2 in (("quote_risk_1e-06.txt",1.0),("quote_risk_5e-07.txt",0.25)):
            with (w/name).open("w") as out:
                for curve,index,direct,jac in exact:
                    nj=[v+(j+1)*0.3*h2 for j,v in enumerate(jac)]
                    out.write(f"{curve} {index} {direct+9*h2} {' '.join(map(str,nj))}\n")
        good=subprocess.run([sys.executable,str(checker),"--directory",str(w),"--expected-cross","1","--max-relative","1e-12"],capture_output=True,text=True)
        if good.returncode or "PASS" not in good.stdout: raise AssertionError(good.stdout+good.stderr)
        lines=(w/"quote_risk_5e-07.txt").read_text().splitlines(); f=lines[0].split(); f[2]=str(float(f[2])+1e6); lines[0]=" ".join(f); (w/"quote_risk_5e-07.txt").write_text("\n".join(lines)+"\n")
        bad=subprocess.run([sys.executable,str(checker),"--directory",str(w),"--expected-cross","1","--max-relative","1e-12"],capture_output=True,text=True)
        if bad.returncode==0: raise AssertionError("corruption passed")
    print("quote-risk checker synthetic gate: PASS"); return 0
if __name__=="__main__": raise SystemExit(main())
