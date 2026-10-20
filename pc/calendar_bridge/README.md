# Calendar bridge

Small Windows program that reads today's meetings from Outlook on this PC and
pushes them to the dashboard over the local network. Teams meetings live in the
Outlook calendar, so they show up too. Setup, scheduling and the board's WiFi
windows are described in the [pc/ README](../README.md).

Everything is read locally: no Azure app registration, OAuth, admin consent or
calendar publishing is needed, which matters with work (Microsoft 365) accounts
where those are usually locked down.

```
new Outlook  --offline cache (IndexedDB)--> calendar_bridge --HTTP POST /calendar--> ESP32
classic Outlook --COM----------------------^
```

```
calendar_bridge/
├── cli.py                # Arguments, and the push loop
├── events.py             # A meeting, and the body the board receives
├── errors.py
└── outlook/
    ├── __init__.py       # Picks the source, drops duplicates
    ├── new.py            # New Outlook: its IndexedDB offline cache
    └── classic.py        # Classic Outlook: COM
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
- The scheduled task pushes the whole day every 15 minutes, timed to the board's
  WiFi windows. A meeting added or moved shows up on the board at the next
  window, within 15 minutes.

The scheduled task starts at :58:40, reads Outlook (~20 s) and waits for the
window to open. The board keeps the events in RAM, so it still beeps if the PC
goes to sleep. After 35 minutes without a push (`CALENDAR_STALE_MIN`), the footer
says so.

## Usage

From the `pc/` folder:

```powershell
python -m calendar_bridge --dry-run   # Print today's meetings, send nothing
python -m calendar_bridge --once      # Push once and exit (what the scheduled task runs)
python -m calendar_bridge             # Stay running, reading Outlook shortly before each window (read_lead_s)
```

Run `--once` at :58:40 or later, until :01: outside the board's window it tries
once and the board will most likely not answer.

## Testing without Outlook

Push a hand-written day with `curl` (times are UTC epochs, fields are
tab-separated). The board only answers during its WiFi window (:59-:01, :14-:16,
:29-:31, :44-:46):

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

- **Wrong or no meetings**: run `python -m calendar_bridge --dry-run --debug`. It
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
- **Deleted meetings still show up** (new Outlook): Outlook hasn't refreshed its
  cached calendar yet; see the note above. The log (`logs/calendar_bridge.log`)
  tells when it last did.

Connection problems (`401 bad token`, `Could not reach the board`) are covered in
the [pc/ README](../README.md#troubleshooting).
