"""Compile the shipped driver's FTM4 and HID routines against a deterministic FIFO."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
source = (here.parents[2] / "drivers/SwitchLiteTouch/src/switchtouch.c").read_text()

def routine(name):
    start = re.search(r"^static [A-Za-z]+ " + name + r"\([^;]*?\)\n\{", source, re.M).start()
    end = source.index("\n}", start) + 2
    return source[start:end]

with tempfile.TemporaryDirectory() as tmp:
    contents = source[source.index("#define TOUCH_MAX_CONTACTS"):source.index("#pragma pack(pop)") + len("#pragma pack(pop)")]
    contact = source[source.index("typedef struct _TOUCH_CONTACT {"):source.index("} TOUCH_CONTACT;") + len("} TOUCH_CONTACT;")]
    code = (here / "ftm4-test.c.in").read_text().replace("/* DRIVER_TYPES */", contents + "\n" + contact)
    code = code.replace("/* DRIVER_CODE */", "\n".join(routine(k) for k in ("SwTouchScale", "SwTouchParse",
                          "SwTouchSendReport", "SwTouchService", "SwTouchStartPanel")))
    c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
    c.write_text(code)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-o", exe, c], check=True)
    subprocess.run([exe], check=True)
