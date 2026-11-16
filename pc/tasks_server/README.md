# Tasks server

A small web page to keep a list of pending tasks, so they don't get forgotten, and
the board's tasks screen to see them at a glance. Setup, scheduling and the board's
WiFi windows are described in the [pc/ README](../README.md).

The page lives on the PC, not on the board: the board only has WiFi on for ~2
minutes every 15 (to save power), so it could not serve a page you open whenever
you like. Instead the PC keeps the tasks and pushes them to the board in each of
its WiFi windows.

```
phone / PC browser --HTTP--> tasks_server --POST /tasks--> ESP32
```

Sync is one way, PC => board: the board only displays the tasks, and finishing or
deleting them is done on the page.

```
tasks_server/
├── cli.py          # Arguments, starts the server and the push thread
├── web.py          # The page and its JSON API
├── store.py        # The tasks (data/tasks.json) and the body the board receives
├── delivery.py     # Pushes in every WiFi window
└── static/index.html
```

## Using it

Open `http://localhost:8765` on the PC, or `http://<the PC's IP>:8765` from the
phone (same network). Each task has a text and a priority:

- Drag the handle on the left of a task (or focus it and press the up / down
  arrows) to reorder them: the board keeps exactly that order. New tasks go at the
  end. The flag only marks a task as high priority (a bar on the board), it does
  not move it. The board shows up to 20 tasks (`TASKS_MAX_ITEMS`); the page marks
  the ones that don't fit yet.
- Click a task to open its details: edit the title, add a description (stays on
  the PC, it is not sent to the board) and see when it was created and, if done,
  the day it was marked as done (also shown under the task in the Done list).
- Marking a task as done, moving it back to pending, deleting it and "Clear all"
  ask for confirmation first (and can still be undone from the message after).
- Changes reach the board in its next WiFi window, within 15 minutes. The page
  shows when the last delivery was and when the next window is. With the board's
  power saving off (`[esp32] power_saving = false`) they reach it within a second or
  two, and a board that restarted (it keeps the tasks in RAM) gets them back within
  a minute: the server asks the board every minute what it holds (`GET /status`
  reports a hash of it) and pushes only when that differs.
- **Export** downloads every task (pending and done) as a JSON file, in the same
  layout as `data/tasks.json`. **Import** loads such a file (or a copy of
  `tasks.json`) and asks what to do: **Merge** adds the imported tasks after the
  current ones, skipping those already there with the same text and state;
  **Replace** swaps the whole list. Either one can be undone from the message that
  follows, and the previous file is kept as `data/tasks.json.bak` anyway (a damaged
  `tasks.json` is put aside and the backup loaded, instead of refusing to start).

On the board's tasks screen (KEY switches screens), **BOOT** selects the next task,
which scrolls the list when there are more than fit. Marking tasks as done or
deleting them is only possible on the page; the board drops them at the next push.

## Usage

From the `pc/` folder:

```powershell
python -m tasks_server              # Serve the page and push to the board
python -m tasks_server --no-push    # Only serve the page (no board needed)
```

Settings are in the `[tasks]` section of `config.ini`:

- `token`: must match `TASKS_TOKEN` in the board's `secrets.h`. Without it the
  program refuses to start (except with `--no-push`) and the board ignores tasks.
- `bind` / `port`: where the page listens (default `0.0.0.0:8765`, reachable from
  other devices; Windows asks to allow it in the firewall the first time).
  `127.0.0.1` keeps it to this PC.
- `web_token`: optional password for the page, asked once per browser. Without
  it anyone on your network can edit the tasks.
- `allowed_hosts`: the page only answers when opened by an IP address, `localhost`
  or this PC's name (against DNS rebinding). Add any other name you use for the PC
  here, comma separated. Other web pages open in the browser cannot change the
  tasks either: changes must come as JSON, which browsers only send cross-site
  after asking (and the server never allows it).

## Testing without the board

`python -m tasks_server --no-push` runs the page on its own. To see what the board
would receive, or to check the board's side, push by hand during its WiFi window:

```sh
printf 'v1\t%s\n1\t1\tCall Ana\n2\t0\tBuy milk\n' "$(date +%s)" |
  curl -s -H "X-Token: another-long-random-string" --data-binary @- http://rlcd-dashboard.local/tasks
```

The answer is `ok <n>`. `GET /tasks` (same header) lists what the board holds, and
`GET /status` what the PC programs check: the hashes of what it holds, its boot id,
uptime and WiFi window settings. The PC sends a `v2` header (`v2  <time>  <hash>
<pending>`, see `src/model/push_body.h`); a board with an older firmware refuses it
and gets the `v1` one instead.
