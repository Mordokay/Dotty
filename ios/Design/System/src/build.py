"""Builds project/components/bundle.js from bundle.src.js, embedding the firefly mark.

Run from anywhere: python3 Design/System/src/build.py
"""
from pathlib import Path
from urllib.parse import quote

here = Path(__file__).resolve().parent
mark = (here.parents[1] / "Logo" / "dotty-mark.svg").read_text()
src = (here / "bundle.src.js").read_text()
data_uri = "data:image/svg+xml," + quote(mark, safe="")
out = src.replace("__MARK_SRC__", data_uri)
assert "</script" not in out.lower() and "<!--" not in out, "bundle must not contain </script or <!--"
(here.parent / "project" / "components" / "bundle.js").write_text(out)
print(f"bundle.js written ({len(out):,} bytes)")
