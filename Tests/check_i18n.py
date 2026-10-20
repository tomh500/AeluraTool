"""Check translation coverage without launching the application."""
import json
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
zh = json.loads((root / "Strings/zh-CN.json").read_text(encoding="utf-8"))
en = json.loads((root / "Strings/en-US.json").read_text(encoding="utf-8"))
assert zh.keys() == en.keys(), "Language resource keys must match"
assert all(isinstance(v, str) and v for d in (zh, en) for v in d.values()), "Translations must not be empty"
files = [*root.glob("MainWindow*.cpp"), *root.glob("Integration/*.cpp"), *root.glob("Tools/ScreenShotPlus/*.cpp")]
keys = set()
for path in files:
    text = path.read_text(encoding="utf-8-sig")
    keys.update(re.findall(r'(?:tr|utf8)\("([\w.]+)"\)', text))
    keys.update(re.findall(r'"(record\.[a-z0-9_.]+)"', text))
for name in ("SearchCatalog.h", "ToolCatalog.h"):
    text = (root / "Integration" / name).read_text(encoding="utf-8-sig")
    keys.update(re.findall(r'"([a-z][a-z0-9_]*\.[a-z0-9_.]+)"', text))
assert not keys - zh.keys(), f"Missing keys: {keys - zh.keys()}"
print(f"PASS: {len(zh)} paired translations; {len(keys)} static resource references resolved")
