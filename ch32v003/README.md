# nano-lite: the CH32V003 deck engine

freedj-nano's deck engine in plain C for the CH32V003 (RV32EC, 48 MHz, no multiply, 16 KB flash,
2 KB RAM): varispeed and keylock on a mono 22.05 kHz mu-law track streamed through two small
rings. The plan, the budgets and the phase 0 results: [../docs/DECK10C.md](../docs/DECK10C.md).

| | |
|---|---|
| `lite/` | the engine (`lite.c`, `lite.h`) and the mu-law table and encoder (`ulaw.c`) |
| `tests/test_lite.cpp` | host tests, some against the C6 engine (`components/nano`) |
| `bench/` | RV32EC bench: a freestanding program, the runtime bits the toolchain lacks (`rt.c`), and the instruction counter (`count.py`) |

```
make test     # host tests; listening WAVs in build/out/
make bench    # Docker (espressif/idf:release-v5.5 for its RISC-V GCC) + qemu-riscv32 on the host
```

`make bench` prints the engine's size, its RAM (`lite_deck`), and instructions per sample for
varispeed and keylock, with the worst 64-sample block. CPU percentages assume 1.5 cycles per
instruction until phase 1 measures it on the chip.

No firmware yet: that's phase 1 (ch32fun, a WCH-LinkE, PWM out).
