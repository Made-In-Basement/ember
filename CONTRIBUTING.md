# Contributing to Ember

Thanks for looking. Bug reports, hardware reports, programs and fixes are all
welcome. This page says how to build and test, and what a pull request needs
before it can be merged.

## Before you start

- **Open an issue first for anything bigger than a fix.** A new program, a
  driver or a change to the kernel is easier to agree on before the work is
  done than after.
- **Programs and games you only want to show off** can go in
  [Discussions](https://github.com/Made-In-Basement/ember/discussions), under
  *Show and tell*. No pull request is needed.
- **One feature per pull request.** A game and an unrelated `input.c` fix are
  two pull requests.

## Licenses

Ember is MIT (see `LICENSE`). By opening a pull request you agree that your
contribution is released under MIT too.

- **GPL or other copyleft code must be discussed before any work starts.**
  It can't simply be mixed into the MIT tree. The precedent is NDOOM: id's GPL
  source is downloaded by `tools/build_doom.py` at build time and is never
  kept in the repository. Ask in an issue before following that route.
- **Code ported from another project** needs that project's license named in
  the pull request, and a link to it.
- **No game data or commercial binaries.** No WADs, no `.EXE`s from
  commercial games, no vendor drivers, no ROMs. Programs are built from source.
  Test data that you made yourself is fine.
- **No personal information** in commits. GitHub's
  [no-reply email address](https://docs.github.com/en/account-and-profile/setting-up-and-managing-your-personal-account-on-github/managing-email-preferences/setting-your-commit-email-address)
  keeps your own address out of the history.

## Building

Python 3 and NASM (a Windows copy is in `tools/nasm/`). The 32-bit programs
and Ember 2.0 also need [Zig](https://ziglang.org).

```bash
python build.py                      # build/ember.img, the BIOS disk image
python tools/build_doom.py --gui     # EMBER.N32, the desktop
python tools/build_ember2.py         # Ember 2.0, the UEFI stick
```

The README has the full list.

## Testing

`tools/qemu_test.py` boots the image in headless QEMU. It types keys, moves
the mouse, and prints the text screen or saves a screenshot:

```bash
python tools/qemu_test.py --keys "dir{ret}"
python tools/qemu_test.py --keys "win{ret}" --mouse 300,200,click --png build/gui.png
```

`tools/ember2_test.py` does the same for Ember 2.0.

QEMU is more forgiving than real hardware: it ignores segment limits, for
example. A pull request should say what it was tested on. If that was only
QEMU, say so; that's fine, and someone with the hardware can try it.

## Pull requests

- Build from a clean checkout of `main`, and say in the pull request what you
  ran.
- Commit messages are a sentence describing the change, with a prose body
  saying why. `git log` shows the style.
- Don't commit build output. `build/` and `root/*.N32` are ignored for that
  reason.
- Screenshots help for anything on screen.

## Reporting bugs

Use the *Bug report* issue form. The most useful things in a report:

- the machine, or the QEMU command line;
- how Ember was booted: BIOS/CSM, or UEFI with Ember 2.0;
- what you typed, and what happened;
- a photo of the screen if it stopped, and `EMBER.LOG` from the stick if
  there is one.
