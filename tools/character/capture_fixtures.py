"""Capture the GW2 API responses the character tests run on.

Reads the "Main" key from api_keys.json at the repo root, fetches one real
character's data, scrubs everything account-identifying (key name/id,
character names), and writes pretty JSON to tests/data/character/. The key
itself is only ever sent in the Authorization header and never written out.

    python tools/character/capture_fixtures.py
"""

import json
import pathlib
import sys
import urllib.parse
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "tests" / "data" / "character"
API = "https://api.guildwars2.com"


def get(path, key):
    req = urllib.request.Request(API + path, headers={
        "Authorization": "Bearer " + key,
        "X-Schema-Version": "latest",
        "User-Agent": "castlemist-fixtures",
    })
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def batch(endpoint, ids, key):
    ids = sorted(set(ids))
    out = []
    for i in range(0, len(ids), 200):
        out += get(f"/v2/{endpoint}?ids=" + ",".join(map(str, ids[i:i + 200])), key)
    return out


def main():
    keys = json.loads((ROOT / "api_keys.json").read_text(encoding="utf-8"))["keys"]
    key = next(k["key"] for k in keys if k["name"] == "Main")

    token = get("/v2/tokeninfo", key)
    names = get("/v2/characters", key)

    chosen = None
    for name in names:
        enc = urllib.parse.quote(name, safe="")
        tabs = get(f"/v2/characters/{enc}/equipmenttabs?tabs=all", key)
        active = next((t for t in tabs if t.get("is_active")), None)
        if not active:
            continue
        eq = active.get("equipment", [])
        has_skin = any("skin" in e for e in eq)
        has_dye = any(any(d is not None for d in (e.get("dyes") or [])) for e in eq)
        if has_skin and has_dye:
            chosen = (name, enc, tabs, active)
            break
    if not chosen:
        sys.exit("no character has a transmuted and dyed piece in its active tab")
    name, enc, tabs, active = chosen
    core = get(f"/v2/characters/{enc}/core", key)

    eq = active["equipment"]
    items = batch("items", [e["id"] for e in eq], key)
    skin_ids = [e["skin"] for e in eq if "skin" in e]
    skin_ids += [i["default_skin"] for i in items if "default_skin" in i]
    skins = batch("skins", skin_ids, key)
    color_ids = [d for e in eq for d in (e.get("dyes") or []) if d is not None]
    for s in skins:
        for slot in ((s.get("details") or {}).get("dye_slots") or {}).get("default", []):
            if slot:
                color_ids.append(slot["color_id"])
    colors = batch("colors", color_ids, key)

    # Scrub. The chosen character becomes "Test Character", the rest
    # "Character 2..N"; any string equal to a real name (e.g. an item's
    # bound_to) is replaced the same way, wherever it appears.
    token["name"] = "Test Key"
    token["id"] = "00000000-0000-0000-0000-000000000000"
    others = [n for n in names if n != name]
    alias = {name: "Test Character"}
    alias.update({n: f"Character {i + 2}" for i, n in enumerate(others)})
    scrubbed_names = [alias[n] for n in names]

    def scrub(v):
        if isinstance(v, dict):
            return {k: scrub(x) for k, x in v.items()}
        if isinstance(v, list):
            return [scrub(x) for x in v]
        if isinstance(v, str) and v in alias:
            return alias[v]
        return v

    core, tabs = scrub(core), scrub(tabs)
    if core.get("guild"):
        core["guild"] = "00000000-0000-0000-0000-000000000000"

    OUT.mkdir(parents=True, exist_ok=True)
    for fname, data in [("tokeninfo.json", token), ("characters.json", scrubbed_names), ("core.json", core),
                        ("equipmenttabs.json", tabs), ("items.json", items), ("skins.json", skins),
                        ("colors.json", colors)]:
        text = json.dumps(data, indent=2, ensure_ascii=False) + "\n"
        assert key not in text and key[:8] not in text, "key leaked into " + fname
        for real in names:
            assert real not in text, "character name leaked into " + fname
        (OUT / fname).write_text(text, encoding="utf-8")
        print("wrote", fname)


if __name__ == "__main__":
    main()
