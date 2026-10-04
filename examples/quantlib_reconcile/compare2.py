names = [l.split()[0] for l in open('./cashflows2.txt') if l[0].isalpha() and l.split()[0] not in ('ois','proj') and len(l.split())==3]
def load(p):
    r=[l.split() for l in open(p)]; return [(float(r[i][0]),[float(x) for x in r[i+1]],[float(x) for x in r[i+2]]) for i in range(0,len(r),3)]
scan=load('./scan_risk.txt'); refs={e:load(f'./ql2_risk_{e}.txt') for e in ('0.0001','5e-05')}
print(f"{'instrument':26s} {'PV rel':>8s} | OIS risk: max rel(1e-4)  max rel(5e-5)  ratio | proj risk: max rel(1e-4)  max rel(5e-5)  ratio | zero buckets exact")
for i,nm in enumerate(names):
    s=scan[i]; out=[f"{nm:26s} {abs(s[0]-refs['0.0001'][i][0])/max(1e-300,abs(refs['0.0001'][i][0])):8.1e} |"]
    for col in (1,2):
        rel={}; sabs={}
        for e,ref in refs.items():
            r=ref[i]; sc=max(max(abs(x) for x in s[col]),1e-300)
            rel[e]=max(abs(a-b)/sc for a,b in zip(s[col],r[col])) ; sabs[e]=sum(abs(a-b) for a,b in zip(s[col],r[col]))
        ratio = sabs['0.0001']/sabs['5e-05'] if sabs['5e-05']>0 else float('nan')
        out.append(f" {rel['0.0001']:12.1e} {rel['5e-05']:14.1e} {ratio:6.2f} |")
    # buckets the scan says are exactly zero: is QL also ~0 there (relative to the instrument's largest sensitivity)?
    z=0; ok=True
    for col in (1,2):
        sc=max(max(abs(x) for x in s[col]),1e-300)
        for a,b in zip(s[col],refs['5e-05'][i][col]):
            if a==0.0: z+=1; ok = ok and abs(b)/sc < 1e-7
    out.append(f" {z:3d} {'yes' if ok else 'NO'}")
    print(''.join(out))
