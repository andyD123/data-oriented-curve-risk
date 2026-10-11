#!/usr/bin/env python3
"""Build a benchmark curve file from the Bank of England published SONIA-OIS spot curve.

The data are NOT redistributed with this repository: the Bank's website terms (bankofengland.co.uk/legal)
allow personal / internal non-commercial use, and re-use needs the Bank's permission. Download the archive
yourself and point this script at it:

    https://www.bankofengland.co.uk/-/media/boe/files/statistics/yield-curves/oisddata.zip

Pure standard library (zipfile + xml parsing, no openpyxl). Reads the "spot curve" sheet (not "spot, short end") of the newest
workbook that contains the requested date (spot rates in per cent, 0.5y..25y in 0.5y steps), converts
r(t) to discount factors exp(-r t), interpolates log-linearly in the discount factor between pillars with
DF(0)=1, and extends flat-forward beyond 25y. The benchmark's 30 + 36 node times (GRIDS below) are used for
the two blocks; the "proj" block carries the same curve (single-curve RFR market).

With --flat RATE no download is needed: it writes a synthetic flat curve (continuous rate RATE) on the same grids,
which is what the build uses so the tests and smoke runs work without third-party data.

Usage: python3 make_rfr_curves_boe.py oisddata.zip --date YYYY-MM-DD [--out rfr_curves_boe.txt]
       python3 make_rfr_curves_boe.py --flat 0.03 --out rfr_curves.txt

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



# Node times (years) of the benchmark grids: 30 discount ("ois") and 36 projection ("proj") forward buckets,
# 0 included. In an RFR market both blocks carry the same curve on their own node grid.
GRIDS = {
    "ois": [
    0.0, 0.0027397260273972603, 0.005479452054794521, 0.00821917808219178, 0.024657534246575342,
    0.043835616438356165, 0.06301369863013699, 0.09315068493150686, 0.17534246575342466, 0.25205479452054796,
    0.3287671232876712, 0.4054794520547945, 0.5013698630136987, 1.252054794520548, 1.5041095890410958,
    1.7616438356164383, 2.010958904109589, 3.0082191780821916, 4.008219178082192, 5.008219178082192,
    6.008219178082192, 7.008219178082192, 8.013698630136986, 9.01095890410959, 10.01095890410959,
    11.01095890410959, 12.013698630136986, 15.013698630136986, 20.019178082191782, 25.024657534246575,
    30.03013698630137
    ],
    "proj": [
    0.005479452054794521, 0.5068493150684932, 0.5917808219178082, 0.6712328767123288, 0.7561643835616438,
    0.8438356164383561, 0.9232876712328767, 1.0054794520547945, 1.095890410958904, 1.1753424657534246,
    1.252054794520548, 1.3397260273972602, 1.4191780821917808, 1.5041095890410958, 1.5890410958904109,
    1.6712328767123288, 1.7616438356164383, 1.841095890410959, 1.9232876712328768, 2.010958904109589,
    3.0082191780821916, 4.008219178082192, 5.008219178082192, 6.008219178082192, 7.008219178082192,
    8.013698630136986, 9.01095890410959, 10.01095890410959, 12.013698630136986, 15.013698630136986,
    20.019178082191782, 25.024657534246575, 30.03013698630137, 35.02739726027397, 40.032876712328765,
    50.038356164383565, 60.04657534246575
    ],
}


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
    ap.add_argument("zip", nargs="?")
    ap.add_argument("--date", help="curve date YYYY-MM-DD (a published business day)")
    ap.add_argument("--flat", type=float, help="write a synthetic flat curve at this continuous rate instead")
    here = Path(__file__).resolve().parent
    ap.add_argument("--out", default=str(here / "rfr_curves_boe.txt"))
    a = ap.parse_args()

    if a.flat is not None:
        df = lambda t: math.exp(-a.flat * t)
        what = "synthetic flat curve"
    else:
        if not a.zip or not a.date:
            ap.error("give the zip and --date, or --flat RATE")
        want = dt.date.fromisoformat(a.date)
        outer = zipfile.ZipFile(a.zip)
        for xlsx in sorted(n for n in outer.namelist() if n.endswith(".xlsx")):
            mats, table = read_spot_rows(outer.read(xlsx))
            if want in table:
                break
        else:
            raise SystemExit(f"{want} not found in {a.zip} (not a published business day?)")
        curve = Curve(mats, table[want])
        df = curve.df
        what = f"BoE UK OIS spot curve {want} ({xlsx}), {len(mats)} pillars {mats[0]}y-{mats[-1]}y"

    lines = [str(len(GRIDS))]
    for name, times in GRIDS.items():
        lines.append(f"{name} {len(times)} " + " ".join(f"{t:.17g} {df(t):.17g}" for t in times))
    Path(a.out).write_text("\n".join(lines) + "\n")
    print(f"wrote {a.out}: {what}")


if __name__ == "__main__":
    main()
