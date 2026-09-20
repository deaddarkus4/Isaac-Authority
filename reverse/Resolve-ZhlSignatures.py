"""Read-only: resolve REPENTOGON's function signatures (libzhl/functions/*.zhl of a local checkout of
https://github.com/TeamREPENTOGON/REPENTOGON, written for Repentance+ J273) in the J460 image. Writes a TSV: class file,
declaration, number of matches, RVA of a unique match. A unique match is a lead, not a fact: a short signature can land on
another function of J460 (Game::RestoreState lands on a slot of the enemy class table) - check before use.

Resolve-ZhlSignatures.py ZHL_DIR OUTPUT.tsv     (the output belongs under Binaries/research/, which git ignores)"""
import re
import struct
import sys
from pathlib import Path

exe = next((Path(__file__).resolve().parents[1] / "Binaries").rglob("isaac-ng-J460.exe"))
data = exe.read_bytes()
pe = struct.unpack_from("<I", data, 0x3C)[0]
count = struct.unpack_from("<H", data, pe + 6)[0]; optional = struct.unpack_from("<H", data, pe + 20)[0]
sections = [struct.unpack_from("<8sIIII", data, pe + 24 + optional + n * 40) for n in range(count)]
text = next(s for s in sections if s[0].startswith(b".text")); code = data[text[4]:text[4] + text[3]]

rows = []; pair = re.compile(r'^\s*"([0-9a-fA-F?]+)"\s*:\s*\n\s*([^\n;]+;)', re.M)
for path in sorted(Path(sys.argv[1]).glob("*.zhl")):
    for signature, declaration in pair.findall(path.read_text(encoding="utf-8", errors="replace")):
        if len(signature) % 2: continue
        pattern = b"".join(b"." if signature[i:i + 2] == "??" else re.escape(bytes.fromhex(signature[i:i + 2])) for i in range(0, len(signature), 2))
        found = [m.start() for m in re.finditer(pattern, code, re.S)][:3]
        rows.append((path.stem, " ".join(declaration.split()), len(found), f"{text[2] + found[0]:#x}" if len(found) == 1 else ""))
Path(sys.argv[2]).write_text("file\tdeclaration\tmatches\trva\n" + "\n".join("\t".join(map(str, row)) for row in rows) + "\n", encoding="utf-8")
unique = sum(1 for row in rows if row[2] == 1)
print(f"{len(rows)} signatures: {unique} unique in J460, {sum(1 for row in rows if row[2] == 0)} not found, {sum(1 for row in rows if row[2] > 1)} ambiguous")
