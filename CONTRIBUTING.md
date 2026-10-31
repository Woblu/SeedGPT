# Contributing

Bug reports and pull requests are welcome.

## Reporting a wrong result

Seedsmith's whole value is being right, so a wrong result is the most useful bug
report you can send. Please include the seed, the Minecraft version, the search
(the **share** link works), and what you found in the game. The
[bug report form](https://github.com/Woblu/Seedsmith/issues/new/choose) asks for all of it.

Before reporting a missing structure, check
[what can and cannot be verified](docs/accuracy.md): ruined portals, for example,
can be buried, and world spawn is an estimate.

## Building

You need `clang`, `git`, `bash` and Python 3. On Windows, use Git Bash.

```sh
./setup.sh           # fetch the pinned engine and apply patches/
./build.sh all       # build every tool into build/
python serve.py      # run the app from source
```

## Testing

```sh
python tests/test_nlparse.py   # the offline prompt, under a minute
python tests/golden.py         # same worlds as every other platform, half a minute
./test.sh --quick              # most of the suite, several minutes on every core
./test.sh                      # everything; needs a JDK
```

GitHub Actions runs all of these on Windows, Linux and macOS for every pull
request, so you do not have to tie up your own machine.

## Rules this project keeps

- **A wrong seed is the worst bug.** It fails silently. Anything that affects what
  is generated needs a check against something independent: the JDK reference in
  `tools/`, or a real server through `tier3-java/`.
- **`tests/golden.py` should not change by accident.** If your change moves those
  hashes, say why in the pull request, then regenerate with `--write`.
- **Say what is not known.** A result that might be wrong should say so. An
  unverified claim does not go in the README.
- **Keep code warning-free.** The suite fails on compiler warnings.
- **Do not add GPL code, or anything of Mojang's.** cubiomes-viewer is GPL-3.0, and
  Minecraft's jars and decompiled source cannot be redistributed.

## Updating the engine

`setup.sh` pins cubiomes to a commit on purpose, so results can only change
deliberately. To support a new Minecraft version, bump the pin, apply `patches/`,
run the suite, and explain any change to `tests/golden.json`.
