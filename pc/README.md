# PC programs

Two small Python programs that run on your PC and feed the board over the local
network. Each one is optional: it is turned on by its token in the firmware's
`secrets.h`, and the board ignores a feature whose token is missing.

| Program | What it does | Board token | Details |
| ------- | ------------ | ----------- | ------- |
| [`calendar_bridge`](calendar_bridge/README.md) | Reads today's Outlook (Teams) meetings and pushes them to the calendar screen | `CALENDAR_TOKEN` | `python -m calendar_bridge` |
| [`tasks_server`](tasks_server/README.md) | Web page to manage pending tasks, synced with the tasks screen | `TASKS_TOKEN` | `python -m tasks_server` |

```
pc/
├── config.example.ini    # Copy to config.ini (git-ignored): host, tokens, settings
├── requirements.txt
├── install_tasks.ps1     # Registers both programs as Windows scheduled tasks
├── rlcd/                 # Shared: pushing to the board and its WiFi windows, config, logging
├── calendar_bridge/      # Outlook -> board
└── tasks_server/         # Task page <-> board
```

Everything runs from this folder (`python -m <program>`), and `config.ini`, `logs/`
and `data/` (the tasks) live here too; none of them is committed.

## How the board is reached

To save power the board keeps WiFi off and only listens in a window that opens at
minute :59, :14, :29 and :44 and closes at :01, :16, :31 and :46 (`WIFI_WINDOW_*`
in the firmware's `config.h`; `WINDOW_*` at the top of `rlcd/board.py` must match).
Both programs push inside it: they retry every `retry_every_s` until the board
answers or the window is about to close. The board switches WiFi off once it has
received everything it is waiting for, so run by hand outside a window they try
once and the board will most likely not answer.

## Setup

1. Set the tokens in the firmware's `secrets.h` and flash the board:
   ```c
   #define CALENDAR_TOKEN "some-long-random-string"
   #define TASKS_TOKEN    "another-long-random-string"
   ```
   Leave one out to turn that feature off.
2. Install Python 3.11+ and the dependencies. Use `python -m pip` so they go into
   the same Python that runs the programs (a bare `pip` may belong to another one):
   ```powershell
   python -m pip install -r requirements.txt
   ```
3. Copy `config.example.ini` to `config.ini` and set the same tokens: `[esp32] token`
   for the calendar and `[tasks] token` for the tasks. `host` defaults to
   `rlcd-dashboard.local`. If mDNS doesn't resolve on your network, use the board's
   IP instead; the dashboard footer shows it.
4. Try each program once (see its README), then schedule them:
   ```powershell
   powershell -ExecutionPolicy Bypass -File install_tasks.ps1
   ```
   It installs "RLCD Teams calendar bridge" (every 15 minutes, aligned with the
   board's WiFi windows) and, if `[tasks] token` is set, "RLCD tasks server" (started
   at sign-in and kept running). Both run hidden and only while you are signed in.
   Errors go to `logs/`. To remove them:
   ```powershell
   Unregister-ScheduledTask -TaskName "RLCD Teams calendar bridge"
   Unregister-ScheduledTask -TaskName "RLCD tasks server"
   ```

## Troubleshooting

- **`401 bad token`**: the token in `config.ini` doesn't match the one in `secrets.h`.
- **`Could not reach the board`**: the PC and the board must be on the same network,
  the firmware must have the feature's token, the push must happen inside the
  board's WiFi window (see above), and a work VPN can block local traffic. If
  `rlcd-dashboard.local` doesn't resolve in time, set `host` to the board's IP
  (give it a DHCP reservation in the router).
- **`... is missing for <python>`**: the dependencies are installed in another
  Python. Run `python -m pip install -r requirements.txt`; `install_tasks.ps1`
  checks this too.
