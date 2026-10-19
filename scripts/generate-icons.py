"""Regenerate Windows icon assets from the supplied artwork (requires Pillow)."""
from pathlib import Path
from PIL import Image, ImageOps

assets = Path(__file__).resolve().parents[1] / "Assets"
source = Image.open(assets / "AppSource.jpg").convert("RGBA")

def canvas(width, height, artwork_size):
    result = Image.new("RGBA", (width, height))
    artwork = ImageOps.contain(source, (artwork_size, artwork_size), Image.Resampling.LANCZOS)
    result.alpha_composite(artwork, ((width - artwork.width) // 2, (height - artwork.height) // 2))
    return result

canvas(256, 256, 256).save(assets / "App.ico", sizes=[(n, n) for n in (16, 24, 32, 48, 64, 128, 256)])
for name, width, height, size in [
    ("StoreLogo.png", 50, 50, 50),
    ("Square44x44Logo.scale-200.png", 88, 88, 88),
    ("Square44x44Logo.targetsize-24_altform-unplated.png", 24, 24, 24),
    ("Square150x150Logo.scale-200.png", 300, 300, 300),
    ("Wide310x150Logo.scale-200.png", 620, 300, 300),
    ("SplashScreen.scale-200.png", 1240, 600, 400),
    ("LockScreenLogo.scale-200.png", 48, 48, 48),
]:
    canvas(width, height, size).save(assets / name)
