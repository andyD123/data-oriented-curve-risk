#!/usr/bin/env python3
"""Build a benchmark curve file from the Bank of England published SONIA-OIS spot curve.

The data are NOT redistributed with this repository: the Bank's website terms (bankofengland.co.uk/legal)
allow personal / internal non-commercial use, and re-use needs the Bank's permission. Download the archive
yourself and point this script at it:

    https://www.bankofengland.co.uk/-/media/boe/files/statistics/yield-curves/oisddata.zip

Pure standard library (zipfile + xml parsing, no openpyxl). Reads the "spot curve" sheet (not "spot, short end") of the newest
workbook that contains the requested date (spot rates in per cent, 0.5y..25y in 0.5y steps), converts
r(t) to discount factors exp(-r t), interpolates log-linearly in the discount factor between pillars with
DF(0)=1, and extends flat-forward beyond 25y. The 30 + 36 node times of the existing rfr_curves.txt are
kept, so the bucket structure is unchanged; the trailing instrument examples are preserved. The "proj" block
carries the same curve (single-curve RFR market).

Usage: python3 make_rfr_curves_boe.py oisddata.zip --date 2026-10-01 [--template rfr_curves.txt] [--out rfr_curves_boe.txt]

Attribution if you publish numbers derived from the output (Bank of England requirement for SONIA data;
the OIS curve data are the Bank's copyright): source "Bank of England, UK OIS spot curve (Bloomberg Finance
L.P. and Bank calculations)", with the curve date and the download date.
"""
import argparse
import datetime as dt
import io
import math
import re
import zipfile
from pathlib import Path



def _strings(z):
    xml = z.read("xl/sharedStrings.xml").decode()
    return ["".join(re.findall(r"<t[^>]*>([^<]*)</t>", s)) for s in re.findall(r"<si>(.*?)</si>", xml, re.S)]


def _sheet_path(z):
    wb = z.read("xl/workbook.xml").decode()
    rid = next(r for n, r in re.findall(r'<sheet name="([^"]*)"[^>]*r:id="(\w+)"', wb)
               if "spot" in n.lower() and "short" not in n.lower())
    rels = z.read("xl/_rels/workbook.xml.rels").decode()
    tgt = re.search(r'<Relationship [^>]*Id="%s"[^>]*Target="([^"]+)"' % rid, rels) or \
        re.search(r'<Relationship [^>]*Target="([^"]+)"[^>]*Id="%s"' % rid, rels)
    return "xl/" + tgt.group(1).lstrip("/").replace("xl/", "")


def _col(ref):
    n = 0
    for ch in re.match(r"[A-Z]+", ref).group(0):
        n = n * 26 + ord(ch) - 64
    return n


def read_spot_rows(xlsx_bytes):
    """Return (maturities in years, {date: [rate_percent...]})."""
    z = zipfile.ZipFile(io.BytesIO(xlsx_bytes))
    ss, xml = _strings(z), z.read(_sheet_path(z)).decode()
    mats, table = None, {}
    for _, body in re.findall(r'<row [^>]*r="(\d+)"[^>]*>(.*?)</row>', xml, re.S):
        vals = {}
        for m in re.finditer(r'<c r="([A-Z]+\d+)"([^>]*?)(?:/>|>(.*?)</c>)', body, re.S):
            v = re.search(r"<v>([^<]*)</v>", m.group(3) or "")
            if v:
                vals[_col(m.group(1))] = ss[int(v.group(1))] if 't="s"' in m.group(2) else v.group(1)
        if not vals:
            continue
        if vals.get(1) == "years:":
            mats = [float(vals[c]) for c in sorted(vals) if c > 1]
        elif mats and len(vals) > 1:
            try:
                d = dt.date(1899, 12, 30) + dt.timedelta(days=int(float(vals[1])))
                table[d] = [float(vals[c]) for c in sorted(vals) if c > 1]
            except ValueError:
                pass
    return mats, table


class Curve:
    def __init__(self, mats, rates_pct):
        self.t = [0.0] + mats
        self.ld = [0.0] + [-(r / 100.0) * t for t, r in zip(mats, rates_pct)]  # log discount factors

    def df(self, t):
        ts, ld = self.t, self.ld
        if t <= 0:
            return 1.0
        if t >= ts[-1]:  # flat forward beyond the last pillar
            f = (ld[-1] - ld[-2]) / (ts[-1] - ts[-2])
            return math.exp(ld[-1] + f * (t - ts[-1]))
        i = max(k for k in range(len(ts) - 1) if ts[k] <= t)
        w = (t - ts[i]) / (ts[i + 1] - ts[i])
        return math.exp(ld[i] + w * (ld[i + 1] - ld[i]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("zip")
    ap.add_argument("--date", required=True, help="curve date YYYY-MM-DD (a published business day)")
    here = Path(__file__).resolve().parent
    ap.add_argument("--template", default=str(here / "rfr_curves.txt"))
    ap.add_argument("--out", default=str(here / "rfr_curves_boe.txt"))
    a = ap.parse_args()
    want = dt.date.fromisoformat(a.date)

    outer = zipfile.ZipFile(a.zip)
    for xlsx in sorted(n for n in outer.namelist() if n.endswith(".xlsx")):
        mats, table = read_spot_rows(outer.read(xlsx))
        if want in table:
            break
    else:
        raise SystemExit(f"{want} not found in {a.zip} (not a published business day?)")
    rates = table[want]
    curve = Curve(mats, rates)

    tok = Path(a.template).read_text().split()
    pos, nc, blocks = 1, int(tok[0]), []
    for _ in range(nc):
        n = int(tok[pos + 1])
        blocks.append((tok[pos], [float(x) for x in tok[pos + 2:pos + 2 + 2 * n:2]]))
        pos += 2 + 2 * n
    lines = [str(nc)]
    for name, times in blocks:
        lines.append(f"{name} {len(times)} " + " ".join(f"{t:.17g} {curve.df(t):.17g}" for t in times))
    lines.append(" ".join(tok[pos:]))
    Path(a.out).write_text("\n".join(lines) + "\n")
    print(f"wrote {a.out}: BoE UK OIS spot curve {want} ({xlsx}), {len(mats)} pillars "
          f"{mats[0]}y-{mats[-1]}y, 1y {rates[1]:.4f}% 10y {rates[mats.index(10.0)]:.4f}%")


if __name__ == "__main__":
    main()
