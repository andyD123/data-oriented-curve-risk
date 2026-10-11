import re,glob,statistics as st
def grab(files,pat):
    v=[]
    for f in files:
        m=re.search(pat,open(f).read())
        if m: v.append(float(m.group(1)))
    return v
def line(name,v):
    v=sorted(v); print(f"{name:34s} n={len(v):2d} median {st.median(v):9.2f}  min {v[0]:9.2f}  max {v[-1]:9.2f}  spread {100*(v[-1]-v[0])/st.median(v):5.1f}%")
g=sorted(glob.glob('grp_*.txt')); f=sorted(glob.glob('full_*.txt'))
print("15 independent processes, 1 repetition each (ms)")
line("GRP date table + kernel (stream)",grab(g,r"GRP   date-table refresh \+ kernel\s+([\d.]+) ms"))
line("GRP kernel normal stores",grab(g,r"kernel, normal stores\s+([\d.]+) ms"))
line("GRP kernel streaming stores",grab(g,r"kernel, streaming stores \(2\)\s+([\d.]+) ms"))
line("SCAN replication + ladder",grab(g,r"SCAN  replication \+ scalar ladder\s+([\d.]+) ms"))
line("SCAN scalar ladder",grab(g,r"SCAN  scalar ladder\s+([\d.]+) ms"))
print("\n3 full runs (best of 3 inside each) (ms)")
for n,p in [("BASE",r"BASE  bump-and-reprice[^\n]*?\s([\d.]+) ms"),("SOA",r"SOA   same[^\n]*?\s([\d.]+) ms"),("TABLE",r"TABLE same[^\n]*?\s([\d.]+) ms"),("SCAN",r"SCAN  replication \+ scalar ladder\s+([\d.]+) ms"),("GRP",r"GRP   date-table refresh \+ kernel\s+([\d.]+) ms")]:
    line(n,grab(f,p))
for fn in ['write_floor.txt','copy_floor.txt']:
    print(fn, re.findall(r"([\d.]+) ms",open(fn).read()))
