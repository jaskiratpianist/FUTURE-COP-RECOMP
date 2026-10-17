"""Where the user's game files are: $FCOP_GAME_DIR, or the folder under game_files/ that
holds FCopLAPD.exe (any depth, any letter case)."""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def game_dir():
    env = os.environ.get("FCOP_GAME_DIR")
    if env:
        return os.path.abspath(env)
    for d, _, files in sorted(os.walk(os.path.join(ROOT, "game_files"))):
        if any(f.lower() == "fcoplapd.exe" for f in files):
            return d
    raise SystemExit("FCopLAPD.exe not found under game_files/ (extract the game there, or set FCOP_GAME_DIR)")


def exe():
    d = game_dir()
    return os.path.join(d, next(f for f in os.listdir(d) if f.lower() == "fcoplapd.exe"))
