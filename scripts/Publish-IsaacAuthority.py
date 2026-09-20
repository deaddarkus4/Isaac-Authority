"""Publishes this project's own work to its public repository - and nothing else.

The working repository began as a clone of another author's repository: its master and the folders that came with it (a
patcher and third-party mods, without a licence) are not ours to publish. This script rewrites, in a throw-away clone,
the commits made on top of that master so that they hold only this project's paths, cuts them loose from the foreign
history, takes players' names out of every version of every text file, and pushes the result as one branch. The rewrite
is deterministic, so publishing again after new commits is an ordinary fast-forward push.

Publish-IsaacAuthority.py [--branch authority-prototype] [--base master] [--to URL] [--as main] [--dry-run]

Players' names are read from Binaries/publish-names.txt (not tracked; one name per line): each becomes "Игрок N". Without
that file nothing is published.
--scrub is what the rewrite calls for every commit, inside that commit's checked-out tree."""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FOREIGN = ["Afterbirth Music+++", "External Item Descriptions", "IsaacOnlineModded", "Mod Config Menu Pure", ".github"]
OURS_MARK = "<!-- isaac-authority -->"   # the README that is this project's own carries it; the one that came with the clone does not
TEXT = {".md", ".txt", ".py", ".ps1", ".cpp", ".hpp", ".h", ".json", ".java", ".def", ".cmake", ".tsv", ".cmd"}


def names():
    path = ROOT / "Binaries/publish-names.txt"
    if not path.exists():
        raise SystemExit(f"{path} is missing: list there the players' names that must not be published, one per line.")
    return [line.strip() for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def scrub():
    tree = Path.cwd()
    for folder in FOREIGN:
        shutil.rmtree(tree / folder, ignore_errors=True)
    readme = tree / "README.md"
    if readme.exists() and OURS_MARK not in readme.read_text(encoding="utf-8", errors="ignore"):
        readme.unlink()
    patterns = [(re.compile(re.escape(name), re.I), f"Игрок {n + 1}") for n, name in enumerate(names())]
    for path in tree.rglob("*"):
        if not path.is_file() or ".git" in path.parts or (path.suffix.lower() not in TEXT and path.name != "CMakeLists.txt"):
            continue
        data = path.read_bytes()
        try: text = data.decode("utf-8")
        except UnicodeDecodeError: continue
        clean = text
        for pattern, plain in patterns: clean = pattern.sub(plain, clean)
        if clean != text: path.write_bytes(clean.encode("utf-8"))


def git(*arguments, cwd):
    done = subprocess.run(["git", *arguments], cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if done.returncode:
        raise SystemExit(f"git {' '.join(arguments[:3])}... failed:\n{done.stdout[-2000:]}\n{done.stderr[-2000:]}")
    return done.stdout.strip()


parser = argparse.ArgumentParser()
parser.add_argument("--scrub", action="store_true"); parser.add_argument("--branch", default="authority-prototype"); parser.add_argument("--base", default="master")
parser.add_argument("--to", default="https://github.com/deaddarkus4/Isaac-Authority.git"); parser.add_argument("--as", dest="name", default="main")
parser.add_argument("--dry-run", action="store_true", help="rewrite and report, push nothing")
args = parser.parse_args()
if args.scrub:
    scrub(); sys.exit(0)
hidden = names()
if git("status", "--porcelain", "--untracked-files=no", cwd=ROOT):
    raise SystemExit("There are uncommitted changes: what is published is what is committed.")
base = git("rev-parse", args.base, cwd=ROOT)
with tempfile.TemporaryDirectory(prefix="isaac-authority-publish-") as temporary:
    clone = Path(temporary) / "clone"
    git("clone", "--quiet", "--no-hardlinks", "--single-branch", "--branch", args.branch, str(ROOT), str(clone), cwd=ROOT)
    script = str(Path(__file__).resolve()).replace("\\", "/")
    subprocess.run(["git", "filter-branch", "-f", "--parent-filter", f"sed 's/-p {base}//'", "--tree-filter", f'python "{script}" --scrub',
                    "--prune-empty", "--", f"{base}..{args.branch}"], cwd=clone, check=True, env={**__import__("os").environ, "FILTER_BRANCH_SQUELCH_WARNING": "1"})
    # What goes out: nothing of the foreign folders, none of the names, in any commit.
    commits = git("rev-list", args.branch, cwd=clone).split()
    for folder in FOREIGN:
        if git("log", "--oneline", args.branch, "--", folder, cwd=clone): raise SystemExit(f"{folder} is still in the rewritten history")
    for name in hidden:
        found = subprocess.run(["git", "grep", "-I", "-i", "-l", "-F", name, *commits], cwd=clone, capture_output=True, text=True).stdout.strip()
        if found: raise SystemExit("a hidden name is still in the rewritten history: " + ", ".join(sorted({line.split(':', 1)[1] for line in found.splitlines()})[:5]))
    if base in git("rev-list", args.branch, cwd=clone).split(): raise SystemExit("the foreign history is still attached")
    print(f"{len(commits)} commits, tip {commits[0][:7]}, tree: {git('ls-tree', '--name-only', args.branch, cwd=clone).split()}")
    if args.dry_run:
        print("dry run: nothing pushed")
    else:
        git("push", args.to, f"{args.branch}:refs/heads/{args.name}", cwd=clone)
        print(f"pushed to {args.to} as {args.name}")
