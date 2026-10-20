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
                                  ^                          |
                                  +---- "done 7", "deleted 3" (answer to the push)
```

```
tasks_server/
├── cli.py          # Arguments, starts the server and the push thread
├── web.py          # The page and its JSON API
├── store.py        # The tasks (data/tasks.json) and the body the board receives
├── delivery.py     # Pushes in every WiFi window, applies what the board did
└── static/index.html
```

## Using it

Open `http://localhost:8765` on the PC, or `http://<the PC's IP>:8765` from the
phone (same network). Each task has a text and a priority:

- High priority tasks go first, then the rest oldest first, the same order the
  board shows. The board shows up to 20 tasks (`TASKS_MAX_ITEMS`); the page marks
  the ones that don't fit yet.
- Changes reach the board in its next WiFi window, within 15 minutes. The page
  shows when the last delivery was and when the next window is.

On the board's tasks screen (KEY switches screens), **BOOT** works on the selected
(highlighted) task:

| Gesture | Action |
| ------- | ------ |
| Click | Select the next task |
| Double click | Mark it as done |
| Hold 1 s | Delete it |

The board tells the PC at the next push, and the page then shows the task as done
(under "Done", where it can be brought back) or removes it. A task brought back
gets a new id, so the board doesn't keep it hidden.

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

## Testing without the board

`python -m tasks_server --no-push` runs the page on its own. To see what the board
would receive, or to check the board's side, push by hand during its WiFi window:

```sh
printf 'v1\t%s\n1\t1\tCall Ana\n2\t0\tBuy milk\n' "$(date +%s)" |
  curl -s -H "X-Token: another-long-random-string" --data-binary @- http://rlcd-dashboard.local/tasks
```

The answer is `ok <n>` followed by the tasks finished or deleted on the board
(`done <id>`, `deleted <id>`). `GET /tasks` (same header) lists what the board holds.
