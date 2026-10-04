# Adding an expansion card

An expansion card (speech synthesis, MIDI, modem, controller…) is added with
**one file and one line**:

1. write `src/cards/card_<id>.c`;
2. add `X(<id>, <tick>)` to `CARD_MODULE_LIST` in `include/cards_list.h`.

Nothing else: the Makefile compiles `src/cards/card_*.c` by pattern, and the core
derives from the list the launch options, the help, the F1 menu, `phosphoric.cfg`, the
I/O bus table, the ticks, the save states and the audio mixing. The sample card
[`docs/examples/card_demo.c`](examples/card_demo.c) is the template to copy;
`make test-card-template` adds it to a copy of the tree and checks that it works end
to end.

Decision: [ADR 0006](adr/0006-cartes-en-modules.md). Plan and measurements:
[`docs/specs/CARD_MODULES.md`](specs/CARD_MODULES.md). Contract:
[`include/card_module.h`](../include/card_module.h).

## The descriptor

The file defines a `const card_module_t card_<id>`:

| Field | Role |
|---|---|
| `descs`, `ndescs` | F1 menu entry (or entries) (`card_desc_t`, `include/cards.h`): id, name, role, exclusive group, enabling option, explained parameters. One chip can carry several cards (Mageco and ORICON). |
| `opts`, `nopts` | Launch options: name without `--`, `no_argument` / `required_argument` / `optional_argument`, function that stores the value in the card's configuration. |
| `helps`, `nhelps` | Help blocks (`card_help_t`), exact text printed by `--help`. |
| `cfg_size`, `cfg_defaults` | The card's own configuration, allocated and set to its defaults by the core. |
| `init` | Called at startup, whether or not the card is present (idle state, wired interrupts). |
| `stage`, `setup` | Setup, at a given stage (`card_stage_t`): reads the card's configuration and the core's, checks for conflicts, turns the card on (`emu->card_on[CARD_IDX_<id>] = true`). Returns 0, or 1 after logging the error. |
| `teardown` | Shutdown (transports, files). |
| `bus` | Existing I/O contract (`io_device_t`, `include/io/io_device.h`): `claims`, `read`, `write`, `peek` (side-effect-free read), `save`/`load` of a `.ost` section, `tick`. |

## Presence, state, tick

- **Presence**: `emu->card_on[CARD_IDX_<id>]`, set by `setup`. `claims`, `save` and
  the tick test it.
- **State**: private to the module, in a `static` variable (there is only one machine
  per process). `emulator_t` does not need to know about it.
- **Tick**: with `X(<id>, 1)`, the core calls `card_<id>_tick(emu, cycles)` on every
  CPU cycle if the card is present. The call is **direct**, generated from the list
  (`include/card_ticks.h`): a table of pointers cost 5 % in instructions.
  The order of the list is the order of the ticks. Without a tick: `X(<id>, 0)`.

## Sound

A card that produces sound registers, in `setup`, a mono source:
`audio_add_source(fn, ctx)` (`include/audio/audio.h`). It is mixed with the PSG by the
SDL callback as well as by capture (`--audio-wav`, AVI, cast). Returning `false` means
"nothing produced" (no mixing).

## Save states

`bus->save_tag` (4 bytes, e.g. `"DMO\0"`) names the section; `save` returns `false`
when the card is absent, so that the `.ost` stays unchanged without it. Prefer
field-by-field serialisation to a raw write of a structure that contains pointers
(addresses differ on every launch: `.ost` not reproducible).

## F1 menu and `phosphoric.cfg`

The menu entry is enough: the menu lists the card, its parameters and their
explanation; the configuration stores it as `carte.<id>=oui` and `<id>.<key>=value`
(the key is the parameter's key in the menu entry). Changing cards restarts the
emulator (ADR 0004).

Parameter kinds (`card_param_kind_t`): file, directory, text, hexadecimal
address, yes/no, and **choice** (`CARD_P_CHOICE`, 2.21.0): `def` gives the
possible values separated by `|`, the first being the default
(`"aucun|simulé|réel"`); Enter moves to the next one. A parameter without an
option (`cli` set to `NULL`) is translated separately in `cards_build_argv`: this
is the case of the LOCI card's modem, which becomes `--serial picowifi` or
`--serial com:115200,8,N,1,<port>`.

## Anchors: keeping a place

Without an anchor (`*_before` fields set to `NULL`), a card goes **at the end** of the
option, help, menu and bus tables. The historical cards keep their exact place thanks
to anchors: `opts_before` (option they precede in the getopt table), the `before` of
each help block, `desc_before` (menu entry), `bus_before` (bus device). An anchor can
name another card. The bus order is also the order of access priority and of the
`.ost` sections.

## Checking

- `make test-card-template` (the template) and `make tests-strict`.
- Behaviour unchanged for anyone not using the card: `tools/cli_golden.sh` between the
  binary before and the one after (help, messages, files produced).
- Performance: `tools/instr_count.sh BINARY [options]` counts the executed
  instructions (stable, unlike timing on a throttled machine).
- Add to `tests/cli_golden/cases.txt` a few lines that exercise the card.

## Current cards

`microdisc`, `jasmin`, `loci`, `acia`, `dtl2000`, `mageco` (+ ORICON), `sp0256`,
`mea8000`, `loci_emu` (menu entry only), `ula_ng`. For some of them, a part stays in
the core because other parts of the emulator read it: options, state or tick of
Microdisc, Jasmin and LOCI; state of the ACIA and of the ULA-NG (details in
`docs/specs/CARD_MODULES.md`).
