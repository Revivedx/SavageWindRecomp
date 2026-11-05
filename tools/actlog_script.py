#!/usr/bin/env python3
"""A SAVAGEWIND_INPUT script for the action-naming run (docs/ACTIONS.md): one Switch button at a time, each held for a
while with a gap after, optionally while the left stick or ZL is held. With SAVAGEWIND_ACTLOG=1 the runtime logs which
actions the game itself answers true to each press, which names the actions and checks the mask-to-button table.

usage: actlog_script.py [--start FRAME] [--state stand|run|aim] [--hold N] [--gap N]
  --start  first frame of the first press (the script's clock: frames the game has polled)  [600]
  --state  stand: nothing else held; run: the left stick held up throughout; aim: ZL held throughout (ZL itself is
           then left out)                                                                    [stand]
  --hold / --gap  frames a button is held / frames before the next                           [40 / 80]
Prints the string for SAVAGEWIND_INPUT and, on stderr, which frame each button is pressed at.
"""
import argparse
import sys

BUTTONS = ["A", "B", "X", "Y", "L", "R", "ZL", "ZR", "UP", "DOWN", "LEFT", "RIGHT", "LSTICK", "RSTICK"]


def script(start, state, hold, gap):
    toks, plan, frame = [], [], start
    for b in BUTTONS:
        if state == "aim" and b == "ZL":
            continue
        toks.append(f"{frame}:{b}:{hold}")
        plan.append((frame, b))
        frame += hold + gap
    end = frame
    if state == "run":
        toks.append(f"{start - 60}:STICK_UP:{end - start + 120}")
    elif state == "aim":
        toks.append(f"{start - 60}:ZL:{end - start + 120}")
    return ",".join(toks), plan, end


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--start", type=int, default=600)
    ap.add_argument("--state", choices=("stand", "run", "aim"), default="stand")
    ap.add_argument("--hold", type=int, default=40)
    ap.add_argument("--gap", type=int, default=80)
    a = ap.parse_args()
    if a.start < 60 or a.hold < 1 or a.gap < 0:
        sys.exit("start must be at least 60, hold at least 1")
    s, plan, end = script(a.start, a.state, a.hold, a.gap)
    for frame, b in plan:
        print(f"frame {frame}: {b}", file=sys.stderr)
    print(f"ends at frame {end}", file=sys.stderr)
    print(s)


if __name__ == "__main__":
    main()
