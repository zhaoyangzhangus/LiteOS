# OS Implementation Specification

Start with:

1. `OS_IMPLEMENTATION_SPECIFICATION.md` — master specification.
2. `docs/02_CONFLICT_RESOLUTION.md` — old design decisions that are explicitly retired.
3. `docs/03_REPOSITORY_TREE.md` — unique source tree.
4. `include/` — authoritative core struct/API/UAPI skeleton.
5. `docs/08_INITIALIZATION_ORDER.md` — boot/init sequence.
6. `docs/11_IMPLEMENTATION_ORDER.md` — actual coding order.
7. `docs/12_ACCEPTANCE_GATES.md` — gates before moving to the next phase.

`tree/os/` contains a physical directory skeleton matching the final source tree and a copy of the authoritative `include/` tree.
