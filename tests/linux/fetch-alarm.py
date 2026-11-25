#!/usr/bin/env python3
"""Arch Linux ARM's smallest root, from its own repositories (no 830 MB tarball): the
packages pacman needs and their dependencies, each checked against the SHA-256 in the
repository database (over HTTPS) and unpacked into ROOT. pacman itself then reinstalls
them with its signature checks (tests/linux/run_pacman.sh).
usage: tests/linux/fetch-alarm.py ROOT [PACKAGE...]"""
import hashlib, io, os, re, sys, tarfile, urllib.request

# official mirrors whose HTTPS certificates are valid (mirror.archlinuxarm.org's redirector's is not)
MIRRORS = ["https://de3.mirror.archlinuxarm.org/aarch64", "https://fl.us.mirror.archlinuxarm.org/aarch64",
           "https://ca.us.mirror.archlinuxarm.org/aarch64"]
REPOS = ["core", "extra", "alarm"]
WANT = ["filesystem", "bash", "coreutils", "pacman", "pacman-mirrorlist", "archlinuxarm-keyring",
        "ca-certificates", "sed", "grep", "gawk", "findutils", "tar", "gzip", "xz", "which", "less"]

def get(path):
    err = None
    for m in MIRRORS:
        try:
            with urllib.request.urlopen(urllib.request.Request(f"{m}/{path}", headers={"User-Agent": "lxterm-ci"}), timeout=300) as r:
                return r.read()
        except Exception as e:
            err = e
    raise err

def parse_db(repo):
    pkgs = {}
    t = tarfile.open(fileobj=io.BytesIO(get(f"{repo}/{repo}.db")))
    entries = {}                                   # NAME-VERSION/desc and (older format) /depends
    for m in t.getmembers():
        if not m.isfile() or "/" not in m.name:
            continue
        fields, key = entries.setdefault(m.name.split("/")[0], {}), None
        for line in t.extractfile(m).read().decode().splitlines():
            if line.startswith("%") and line.endswith("%"):
                key = line.strip("%"); fields.setdefault(key, [])
            elif line and key:
                fields[key].append(line)
    for fields in entries.values():
        if "NAME" in fields:
            fields["REPO"] = repo
            pkgs[fields["NAME"][0]] = fields
    return pkgs

def main():
    root = sys.argv[1]
    want = sys.argv[2:] or WANT
    db = {}
    for r in REPOS:
        try:
            db.update({k: v for k, v in parse_db(r).items() if k not in db})
        except Exception as e:
            print(f"(no {r}: {e})")
    provides = {}
    for name, f in db.items():
        for p in f.get("PROVIDES", []):
            provides.setdefault(re.split(r"[<>=]", p)[0], name)
    def resolve(dep):
        n = re.split(r"[<>=]", dep)[0]
        return n if n in db else provides.get(n)
    todo, done = list(want), []
    seen = set()
    while todo:
        d = todo.pop(0)
        n = resolve(d)
        if not n:
            print(f"unresolved: {d}"); continue
        if n in seen:
            continue
        seen.add(n); done.append(n)
        todo += db[n].get("DEPENDS", [])
    os.makedirs(root, exist_ok=True)
    total = 0
    for n in done:
        f = db[n]
        fn = f["FILENAME"][0]
        data = get(f"{f['REPO']}/{fn}")
        got = hashlib.sha256(data).hexdigest()
        if got != f["SHA256SUM"][0]:
            sys.exit(f"SHA-256 mismatch for {fn}: {got}")
        total += len(data)
        with tarfile.open(fileobj=io.BytesIO(data)) as t:
            members = [m for m in t.getmembers() if not m.name.startswith(".") and not m.isdev()]
            t.extractall(root, members=members, filter="tar") if hasattr(tarfile, "data_filter") else t.extractall(root, members=members)
    os.system(f'chmod -R u+rwX "{root}"')
    with open(os.path.join(root, ".lxrun-image"), "w") as o:
        o.write("archlinuxarm " + " ".join(done) + "\n")
    print(f"verified and unpacked {len(done)} packages, {total // 1048576} MB: {' '.join(done)}")

main()
