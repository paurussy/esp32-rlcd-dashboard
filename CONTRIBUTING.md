# Contributing

Thanks for your interest in improving ESP32 RLCD Dashboard! Bug reports, ideas,
documentation fixes and pull requests are all welcome.

## Ways to contribute

- **Report a bug** or **suggest a feature** by opening an
  [issue](../../issues). Check first that it hasn't been reported already.
- **Fix something or add a feature** with a pull request (see below).
- **Improve the docs**: README, `pc/README.md` and the per-program READMEs.

For anything bigger than a small fix, please open an issue first so we can agree
on the approach before you spend time on it.

## Reporting bugs

Include as much of this as you can:

- What you expected and what happened instead
- Firmware: commit or release, board (Waveshare ESP32-S3-RLCD-4.2), `esp32`
  core version and the Arduino IDE / `arduino-cli` settings you used
- For PC programs: Windows version, Python version, Outlook flavour (classic or
  new) and the command you ran
- Serial monitor output or PC program logs. **Remove WiFi credentials, tokens,
  IPs and meeting titles** before pasting anything

## Project layout

See [Project layout](README.md#project-layout) in the README. In short:

- `esp32-rlcd-dashboard.ino` and `config.h`: entry point and settings
- `src/drivers/`: hardware, one driver per chip
- `src/net/`: WiFi, NTP and the HTTP endpoints the PC pushes to
- `src/model/`: plain data and logic, **no hardware access**
- `src/ui/`: screen rendering, **no hardware access**
- `pc/`: Python programs (`calendar_bridge`, `tasks_server`) that run on the PC
- `tools/screenshots/`: renders the README images on the PC

Keep the layering: `model/` and `ui/` must not touch hardware, because the
screenshot tool builds them on the host.

## Development setup

### Firmware

1. Copy `secrets.example.h` to `secrets.h` and fill it in. It is git-ignored,
   never commit it.
2. Install the **U8g2** library and the `esp32` core (>= 3.3.0).
3. Build and upload as described in
   [Getting started](README.md#getting-started).

There is no automated test suite for the firmware. Before opening a PR, make
sure it **compiles without new warnings** and, if you have the board, that you
tried your change on it.

### PC programs

Create a virtual environment and install the requirements from `pc/`:

```sh
python -m venv .venv
.venv\Scripts\activate
pip install -r pc/requirements.txt
```

Copy `pc/config.example.ini` to your own config and never commit real tokens or
addresses.

### Screenshots

If you change how a screen looks, regenerate the images in `docs/images/` and
include them in your PR:

```sh
tools/screenshots/build.sh
```

It needs `gcc`, `g++` and `git` (Linux, macOS or WSL). Set `U8G2_DIR` to an
existing U8g2 library to skip the download.

## Code style

Match the surrounding code: naming, comment density and idioms.

- **C++**: follow the existing style in `src/`. Put pins, timings and thresholds
  in `config.h` instead of hard-coding them. Be careful with RAM and with
  anything that keeps WiFi or the CPU awake longer, since low power is a design
  goal.
- **Python**: keep to the standard library where possible, add type hints
  like the existing modules do, and keep the programs runnable with
  `python -m calendar_bridge` / `python -m tasks_server`.
- **Docs**: if you add a setting, button action, endpoint or feature, update the
  README (and the relevant `pc/` README) in the same PR.

## Commits and pull requests

1. Fork the repository and create a branch from `main`
   (for example `fix/battery-icon` or `feat/weather-screen`).
2. Keep each PR focused on one thing. Unrelated cleanups go in their own PR.
3. Write commit messages in the imperative, short summary first, matching the
   history, for example `Fix battery icon rendering` or
   `Add tasks screen with a web page`.
4. Open the PR against `main` and describe **what** changed and **why**. Link
   the issue it closes (`Closes #123`).
5. Mention how you tested it (board, PC program, screenshots) and attach
   updated screenshots for UI changes.

PRs are squash-merged, and the PR number is added to the title.

## Security

Do not open a public issue for something that exposes credentials or lets
someone on the network control the board. Contact the maintainer privately
through GitHub instead. Never include tokens, WiFi passwords or `secrets.h` in
issues, PRs or screenshots.

## License

By contributing you agree that your contributions are licensed under the
[MIT License](LICENSE) that covers the project.
