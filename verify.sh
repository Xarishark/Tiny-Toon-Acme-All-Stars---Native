#!/usr/bin/env bash
#
# The checks you can run with nothing but Python and your ROM.
#
#   ./verify.sh [rom.bin]
#
# The project's full set of gates compares against independent implementations —
# vasm rebuilding the ROM, Musashi executing alongside the generated C,
# Nuked-OPN2 against the FM synthesis, published CRCs against the Z80. Those
# need reference code that is not vendored here; see CREDITS.md and
# docs/22-differential-testing.md.
#
# What runs here needs nothing extra, and it is not nothing: it re-derives the
# whole static analysis from your ROM and checks that the flow closes.
#
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$RAIZ"

ROM="${1:-rom.bin}"
PY=""
for c in python3 python; do command -v "$c" >/dev/null 2>&1 && { PY="$c"; break; }; done
[ -n "$PY" ] || { echo "Python 3 not found." >&2; exit 1; }

# Checked up front, like build.sh does. Without this the first check passes
# (it only reads the header) and the next two fail with the same message twice,
# which reads like two separate problems instead of one missing package.
"$PY" -c 'import capstone' 2>/dev/null || {
    echo "ERROR: the Python module 'capstone' is missing." >&2
    echo >&2
    echo "It is the disassembler the analysis is built on." >&2
    echo "  pip install capstone" >&2
    echo >&2
    echo "Or run this inside the build container, which already has it:" >&2
    echo "  docker run --rm -v \"\$PWD:/work\" -w /work tinytoon-build ./verify.sh" >&2
    exit 1
}

[ -f "$ROM" ] || { echo "ROM not found at '$ROM'. See LEGAL.md." >&2; exit 1; }

falhas=0
passo() {
    echo
    echo "=== $1"
    shift
    if "$@"; then echo "    OK"; else echo "    FAILED"; falhas=$((falhas + 1)); fi
}

# 1. Is this the ROM the analysis was built against? Every claim below assumes
#    yes, so it is checked first.
passo "ROM identity against the recorded baseline" \
    "$PY" tools/rom_info.py --check --rom "$ROM"

# 2. Re-derive the disassembly from your ROM. The number that matters is
#    "Fluxo nao resolvido: 0" — every indirect jump and call in the traced code
#    lands on a target the analysis validated. A non-zero value means the game
#    can reach code this build does not contain, and the port would abort there
#    rather than execute the wrong thing.
passo "static trace: recursive scan and indirect flow" \
    "$PY" tools/trace_code.py --dry-run --rom "$ROM"

# 3. How much of the ROM is classified, and by what evidence. Informational —
#    it does not fail — but it is the honest measure of how much is still opaque.
passo "ROM coverage" \
    "$PY" tools/rom_coverage.py --blocos 5

echo
if [ "$falhas" -eq 0 ]; then
    echo "all checks passed."
else
    echo "$falhas check(s) FAILED."
    exit 1
fi
