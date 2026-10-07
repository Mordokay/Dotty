#!/usr/bin/env python3
"""Checks the pet engine against the P1 ROM datamine's own simulator (timeline.py by
rhubarbtart, https://rhubarbtart.neocities.org/p1hackinglog), character by character.

  python3 tools/pet_sim/compare.py

Downloads timeline.py, utils.py and stats.txt to a temporary folder (they aren't ours to
commit), feeds timeline.py the published stats.txt instead of the ROM, builds sim.cpp with
the engine, runs both with perfect care from several start times and diffs the event lines,
then builds and runs tests.cpp (evolution branches, deaths, pause, food, game).
"""
import os, subprocess, sys, tempfile, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
BASE = "https://rhubarbtart.neocities.org/file/"
RIP = '''
def ripTamaStats():
    stats, cur = {}, None
    for line in open("stats.txt", encoding="utf8"):
        if not line.strip(): continue
        if not line.startswith(" "):
            cur = line.strip().rstrip(":"); stats[cur] = {}; continue
        k, v = line.strip().split(": ", 1)
        if k == "Initial Discipline": v = v.split(" ")[0]
        # The ROM has numbers where stats.txt says n/a: the baby's sleep times (timeline.py
        # replaces them) and "never calls for discipline".
        if v == "n/a": v = "0:00" if k in ("Wake Time", "Sleep Time") else "0 hungry/happy heart decrements"
        stats[cur][k] = v
        if k == "Base Countdown to Discipline": stats[cur]["Countdown to Discipline"] = v
    for name in list(stats):
        for part in name.split("/"): stats[part] = stats[name]
    return stats
'''
CHARACTERS = ["Babytchi", "Marutchi", "Tamatchi", "Tamatchi+", "Kuchitamatchi", "Kuchitamatchi+",
              "Mametchi", "Ginjirotchi", "Maskutchi", "Kuchipatchi", "Nyorotchi", "Tarakotchi"]
STARTS = ["9:00", "12:34", "18:59"]

def main():
    work = tempfile.mkdtemp(prefix="pet_sim_")
    for name in ("timeline.py", "utils.py", "stats.txt"):
        urllib.request.urlretrieve(BASE + name, os.path.join(work, name))
    open(os.path.join(work, "rip.py"), "w").write(RIP)
    sim = os.path.join(work, "sim")
    subprocess.run(["clang++", "-std=c++17", "-O2", "-Wall", "-o", sim, os.path.join(HERE, "sim.cpp"),
                    os.path.join(REPO, "cartridges/pet/pet_engine.cpp")], check=True)
    failed = 0
    for character in CHARACTERS:
        for start in STARTS:
            ref = subprocess.run([sys.executable, "timeline.py", character, start], cwd=work,
                                 capture_output=True, text=True, check=True).stdout.splitlines()
            ref = [l for l in ref if ": End," not in l]
            # The stage ends at its last line ("Time to Evolution reached", or an adult's third
            # sickness). timeline.py still counts the old character's hearts/poop in that same
            # minute; the engine has already evolved (or died) by then. One heart at a stage
            # boundary that the datamine doesn't settle, so the comparison stops there.
            ends = [i for i, l in enumerate(ref) if "Time to Evolution reached" in l or "Time to Sickness reached" in l]
            if ends:
                last = ends[-1]
                minute = ref[last][:6]
                ref = ref[:last + 1] + [l for l in ref[last + 1:] if not l.startswith(minute)]
            ours = subprocess.run([sim, character, start], capture_output=True, text=True, check=True).stdout.splitlines()
            if ref == ours:
                print(f"ok    {character:15} from {start:5}  {len(ref)} events")
                continue
            failed += 1
            print(f"DIFF  {character:15} from {start:5}")
            for i, (a, b) in enumerate(zip(ref, ours)):
                if a != b:
                    print(f"      first difference at line {i}: datamine {a!r} / ours {b!r}")
                    break
            else:
                print(f"      lengths differ: datamine {len(ref)}, ours {len(ours)}")
            if "--diff" in sys.argv:
                import difflib
                for line in list(difflib.unified_diff(ref, ours, "datamine", "ours", n=1, lineterm=""))[:12]:
                    print("      " + line)
    # The rules a perfect-care timeline never reaches: branches, deaths, pause, food, game.
    tests = os.path.join(work, "tests")
    subprocess.run(["clang++", "-std=c++17", "-O2", "-Wall", "-o", tests, os.path.join(HERE, "tests.cpp"),
                    os.path.join(REPO, "cartridges/pet/pet_engine.cpp")], check=True)
    failed += subprocess.run([tests]).returncode != 0
    print("ALL GOOD: timelines match the datamine, rule tests pass" if not failed else f"{failed} FAILED")
    return 1 if failed else 0

if __name__ == "__main__":
    sys.exit(main())
