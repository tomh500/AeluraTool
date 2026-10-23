"""Check an extracted release and reject incomplete/unsigned fixtures without installing."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import zipfile

powershell = str(Path(os.environ["WINDIR"]) / "System32/WindowsPowerShell/v1.0/powershell.exe")
environment = {key: value for key, value in os.environ.items() if key.upper() != "PSMODULEPATH"}
with tempfile.TemporaryDirectory(prefix="Aelura distribution ") as directory:
    folder = Path(directory)
    with zipfile.ZipFile(sys.argv[1]) as archive:
        archive.extractall(folder)
    installers = list(folder.rglob("Install.ps1"))
    assert len(installers) == 1, installers
    installer = installers[0]
    assert (installer.parent / "Install.bat").is_file()
    assert (installer.parent / "Install.cmd").is_file()

    def check(expected_success):
        result = subprocess.run([powershell, "-NoProfile", "-ExecutionPolicy", "Bypass",
                                 "-File", str(installer), "-CheckOnly"],
                                env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        assert (result.returncode == 0) == expected_success, result.stdout

    check(True)
    for launcher in ("Install.bat", "Install.cmd"):
        result = subprocess.run([os.environ["COMSPEC"], "/d", "/c", f"call {launcher} -CheckOnly"],
                                cwd=installer.parent, input=b"\r\n", stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=60)
        assert result.returncode == 0, result.stdout
    dependencies = installer.parent / "Dependencies"
    dependencies.rename(installer.parent / "Dependencies.saved")
    check(False)
    (installer.parent / "Dependencies.saved").rename(dependencies)
    # A directory with some files is not enough: every declared dependency must
    # be present, even if the host already has the missing runtime installed.
    dependency = next((dependencies / "x64").glob("*.msix"))
    dependency.rename(dependency.with_suffix(".saved"))
    check(False)
    dependency.with_suffix(".saved").rename(dependency)
    certificate = installer.parent / "AeluraTool.cer"
    certificate.rename(certificate.with_suffix(".saved"))
    check(False)
    certificate.with_suffix(".saved").rename(certificate)
    original = installer.read_text(encoding="utf-8-sig")
    installer.write_text(original.replace("$expected = '", "$expected = 'BAD"), encoding="utf-8-sig")
    check(False)
    installer.write_text(original, encoding="utf-8-sig")
    package, = installer.parent.glob("*.msix")
    package.unlink()
    with zipfile.ZipFile(package, "w") as archive:
        archive.writestr("AppxManifest.xml", "<Package/>")
    check(False)
print("PASS: extracted signed ZIP preflight; missing dependencies/certificate, wrong signer pin and unsigned package rejected")
