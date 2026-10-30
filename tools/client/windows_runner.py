"""Launch one interactive client within the existing resource-guard job."""
import json
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
config = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
name = config["name"]
if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", name):
    raise SystemExit("invalid client window name")
arguments = [config["binary"]]
if config["editor"]:
    arguments.extend([str(root / "MRFPS.uproject"), "-game"])
arguments.extend(["/Game/Maps/MRFPSArena", "-MRFPSDesktop", "-nohmd", "-windowed", "-NoLiveCoding",
                  f"-MRFPSWindowName={name}"])
log_path = Path(config["report"]).parent / "engine.log"
log_path.parent.mkdir(parents=True, exist_ok=True)
# FParse needs the quote immediately after '=' even when the path has spaces.
commandline = subprocess.list2cmdline(arguments) + f' -abslog="{log_path}" -stdout -FullStdOutLogOutput'
result = subprocess.run(commandline, cwd=root, close_fds=True)
raise SystemExit(result.returncode)
