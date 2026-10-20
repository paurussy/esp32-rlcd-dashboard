# Teams calendar bridge

Small Windows program that reads today's meetings from Outlook on this PC and
pushes them to the dashboard over the local network. Teams meetings live in the
Outlook calendar, so they show up too.

Everything is read locally: no Azure app registration, OAuth, admin consent or
calendar publishing is needed, which matters with work (Microsoft 365) accounts
where those are usually locked down.

```
new Outlook  --offline cache (IndexedDB)--> teams_bridge.py --HTTP POST /calendar--> ESP32
classic Outlook --COM----------------------^
```

Pick the source with `source` in the `[outlook]` section of `config.ini`:

- **`new`** (default): the **new Outlook for Windows**. It is a web app (WebView2)
  that keeps an offline copy of your mailbox in a Chromium IndexedDB database
  under `%LOCALAPPDATA%\Microsoft\Olk`. The bridge opens it read-only with
  [ccl_chromium_reader](https://github.com/cclgroupltd/ccl_chromium_reader) and
  takes the `events` store, where recurring meetings are already expanded.
  - The cache is only refreshed while the new Outlook is running: keep it open
    (minimized is fine). The bridge logs a warning when it isn't running.
  - Outlook refreshes the cached **calendar** much less often than mail (hours
    apart have been seen). Until it does, meetings deleted or moved in the
    meantime are still in the cache, so the board shows them too. The bridge
    logs a warning when the cached calendar is more than 30 minutes old.
  - This is not an official interface. An Outlook update can change the format;
    `--dry-run --debug` shows what is read.
  - A read takes ~20 s (the database is tens of MB, parsed in pure Python).
- **`classic`**: the classic Outlook desktop app, through COM.

## What gets sent

- Every meeting that overlaps today, with all-day, cancelled and declined ones
  left out. Recurring meetings are expanded into their occurrences.
- At most one meeting per name: if several have the same subject (ignoring
  case and extra spaces), only the most recently modified one is sent. This
  also hides the old copy of a meeting that was deleted and created again.
  Meetings without a subject are never merged.
- Start/end (UTC), subject and location. Nothing else leaves the PC, and only
  to the board on your network.
- The scheduled task (`install_task.ps1`) pushes the whole day every 15 minutes
  and at sign-in. A meeting added or moved shows up on the board at the next run,
  within 15 minutes.

The board keeps the events in RAM, so it still beeps if the PC goes to sleep.
After 35 minutes without a push (`CALENDAR_STALE_MIN`), the footer says so.

## Setup

1. Set a token in the firmware's `secrets.h` and flash the board:
   ```c
   #define CALENDAR_TOKEN "some-long-random-string"
   ```
2. Install Python 3.11+ and the dependencies. Use `python -m pip` so they go
   into the same Python that runs the bridge (a bare `pip` may belong to
   another one):
   ```powershell
   python -m pip install -r requirements.txt
   ```
3. Copy `config.example.ini` to `config.ini` and set the same `token`.
   `host` defaults to `rlcd-dashboard.local`. If mDNS doesn't resolve on your
   network, use the board's IP instead; the dashboard footer shows it.
   If the new Outlook has several accounts (work and personal), set `mailbox`
   to your work address so only those meetings are sent.
4. Check that Outlook is read correctly:
   ```powershell
   python teams_bridge.py --dry-run
   ```
5. Push once and look at the board (KEY switches to the calendar):
   ```powershell
   python teams_bridge.py --once
   ```
6. Schedule it every 15 minutes (and at sign-in), hidden:
   ```powershell
   powershell -ExecutionPolicy Bypass -File install_task.ps1
   ```
   `-IntervalMinutes 5` changes the period; if you raise it above ~30, raise
   `CALENDAR_STALE_MIN` too. The task only runs while you are signed in, since
   Outlook is read from your session. Errors go to `teams_bridge.log` next to
   the script. To remove the task:
   `Unregister-ScheduledTask -TaskName "RLCD Teams calendar bridge"`.

   Running `python teams_bridge.py` without `--once` instead keeps it resident,
   reading Outlook every `interval_s` and pushing on changes (at least every
   `keepalive_s`).

## Testing without Outlook

Push a hand-written day with `curl` (times are UTC epochs, fields are
tab-separated):

```sh
now=$(date +%s); s=$((now + 6*60)); e=$((s + 30*60))
printf 'v1\t%s\n1\t%s\t%s\tTest meeting\tTeams\n' "$now" "$s" "$e" |
  curl -s -H "X-Token: some-long-random-string" --data-binary @- http://rlcd-dashboard.local/calendar
```

The board beeps twice a minute later (5 minutes before the start) and three
times when the meeting starts. `GET /calendar` (with the same `X-Token`
header) lists what the board holds, including which alerts it has played (`E`
early, `S` start).

## Troubleshooting

- **Wrong or no meetings**: run `python teams_bridge.py --dry-run --debug`. It
  lists every item of today and why it was kept or skipped (all-day, cancelled,
  declined, other mailbox).
  - New Outlook: make sure it is the app you actually use and that it is
    running, so its cache is current. `New Outlook data not found` means it was
    never opened on this Windows user.
  - Classic Outlook: `--debug` also shows which calendar is read, the other
    calendars in the profile and each date filter tried (Outlook parses them
    with the Windows locale, so several formats are tried).
- **`Classic Outlook error ... Server execution failed`**: classic Outlook is
  not installed or has no profile. If you use the new Outlook, set
  `source = new`.
- **`401 bad token`**: `token` in `config.ini` doesn't match `CALENDAR_TOKEN`.
- **`Could not reach the board`**: the PC and the board must be on the same
  network, the firmware must be built with `CALENDAR_ENABLED = true` (it can't
  be combined with `LOW_POWER_MODE`), and a work VPN can block local traffic.
- **Deleted meetings still show up** (new Outlook): Outlook hasn't refreshed its
  cached calendar yet; see the note above. The log tells when it last did.
- **`... is missing for <python>`**: the dependencies are installed in another
  Python. Run `python -m pip install -r requirements.txt`; `install_task.ps1`
  checks this too.
