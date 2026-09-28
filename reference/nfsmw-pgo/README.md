# Profiles for profile guided optimization

This folder has the profiles the released NROs were built with, one folder per [edition](../docs/glossary.md#edition)
of the game. [PGO](../docs/glossary.md#pgo) (profile guided optimization) builds the program with a record of which
code runs most, so the compiler can optimise for it. You need these files to build an NRO like the released ones
(`-DNFSMW_PGO=usar`, see [docs/building.md](../docs/building.md)).

Each folder has the GCC profile, as `.gcda` files (the files where GCC keeps its counters):

| Folder | Edition |
|---|---|
| `pal_es` | PAL Spanish (the default tree `app/`) |
| `pal_en` | PAL English |
| `pal_de` | PAL German |
| `pal_it` | PAL Italian |
| `usa` | NTSC-U |
| `jpn` | NTSC-J |

## Where the profiles come from

`pal_es` was recorded on the console while playing, with an instrumented build (`-DNFSMW_PGO=generar`): a special
build that counts how often each piece of code runs. It is not a complete profile. It comes from a small test, done
only to check that PGO improves the port's performance, not from playing through the whole game.

The others are that same profile, translated to the addresses of each edition by
`tools/editions/pgo/traducir_perfil.py` ("translate profile"). This is needed because each recompiled function is
named after its [guest](../docs/glossary.md#guest) address (for example `__imp__sub_823B5A40`), and in other editions
the same function is often at another address. GCC finds each function in the profile by a hash of its name, so that
hash changes from one edition to another.

## What the files contain

The files contain counters and hashes of function names, nothing from the game. Each file name is the path of an
object file relative to the build folder, with `#` instead of `/`. So GCC finds them as long as the build folder has
the same layout.

How to use a profile and how to record a new one is in [docs/building.md](../docs/building.md). The traps of PGO with
GCC on the Switch are in [docs/toolchain.md](../docs/toolchain.md).
