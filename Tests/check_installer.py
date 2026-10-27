"""Exercise the real batch launchers with isolated installer fixtures, no system changes."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="Aelura installer ") as directory:
    folder = Path(directory) / "安装 with spaces"
    folder.mkdir()
    for name in ("Install.bat", "Install.cmd"):
        shutil.copy2(root / "scripts" / name, folder / name)
    for launcher in ("Install.bat", "Install.cmd"):
        for code in (0, 23, 3010):
            (folder / "Install.ps1").write_text(
                "param([switch]$NoPause, [switch]$CheckOnly)\n"
                "if (!$NoPause -or !$CheckOnly -or ![Environment]::Is64BitProcess) { exit 99 }\n"
                f"Write-Output 'FIXTURE_STARTED'; exit {code}\n", encoding="utf-8-sig")
            result = subprocess.run(
                [os.environ["COMSPEC"], "/d", "/c", f"call {launcher} -CheckOnly"], cwd=folder,
                input=b"\r\n", stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
            assert result.returncode == code, (launcher, code, result.stdout)
            assert b"FIXTURE_STARTED" in result.stdout, result.stdout
            # A successful fixture emits one line; failure must also emit PAUSE's prompt.
            lines = [line for line in result.stdout.splitlines() if line.strip()]
            assert (len(lines) > 1) == (code != 0), result.stdout
        (folder / "Install.ps1").unlink()
        result = subprocess.run(
            [os.environ["COMSPEC"], "/d", "/c", f"call {launcher}"], cwd=folder,
            input=b"\r\n", stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        assert result.returncode != 0 and result.stdout.strip(), result.stdout
print("PASS: BAT/CMD success, failure pause, missing script, argument forwarding and spaced Unicode paths")
