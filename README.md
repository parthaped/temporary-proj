# ECE 434 Sp26 — Project 2 (Group 12)

This folder contains everything needed to build, run, and re-create the
screenshots used in `Report.tex`.

## Contents

| File | Purpose |
|------|---------|
| `datagen.c` | Generates `input.txt` with `L` integers and `H` hidden negative keys |
| `project1.c` | Project 1 Part 1 rebuilt with the four grader fixes |
| `project2_p1.c` | Problem 1 (signals + Rule 1/2/3 child fate decisions) |
| `project2_p2.c` | Problem 2 Part 1 (4 children, 8 signals, masks, sum 0..10·pid) |
| `project2_p2_q3.c` | Problem 2 Q3 (split blocking masks, pending queues) |
| `Makefile` | Builds all binaries above |
| `Report.tex` | Overleaf-ready report (compiles with pdfLaTeX) |
| `input.txt` | Sample test input (re-generate any time with `./datagen`) |
| `output.txt` | Captured run of `./project1` |
| `figures/` | Screenshots referenced by `Report.tex` |

## Build

```bash
make clean && make
```

This produces `datagen`, `project1`, `project2_p1`, `project2_p2`,
`project2_p2_q3`. For the Rule 3 alternate experiment (SIGINT ignored
instead of handled) also run:

```bash
make project2_p1_exp2
```

## Generate the test input

```bash
./datagen 12000 50
```

`12000` is the array size `L`, `50` is the number of hidden negative keys `H`.
Adjust as needed; `L` must be at least 12000 per the assignment.

## Run order

```bash
./project1 12000 50 4 > output.txt 2>&1     # smoke test, sanity check Avg ~ 499
./project2_p1 12000 50 6                    # Problem 1 (PN >= 6 exercises all 3 rules)
./project2_p2                               # Problem 2 Part 1 + Q2
./project2_p2_q3                            # Problem 2 Q3
```

## Screenshots to capture

Save PNGs into `figures/` with **exactly** the file names listed below.
The LaTeX file auto-detects them with `\IfFileExists`, so once a PNG is
present, the next compile uses it; if it is missing, the report shows a
placeholder box.

### 1. `figures/p1_pstree_start.png`

Run `./project2_p1 12000 50 6`. The program prints:

```
[ROOT] pstree right after spawning the tree:
```

followed by a `pstree -p` block. **Capture this first `pstree` block** plus
the worker prints just above it.

### 2. `figures/p1_pstree_mid.png`

Same run as above. A few seconds later the program prints:

```
[ROOT] pstree while RULE 1 branches are sleeping:
```

**Capture this second `pstree` block.** The Rule 1 worker(s) should still
be visible while Rule 2 and Rule 3 children are already gone.

### 3. `figures/p2_part1_console.png`

Open **two PuTTY windows**.

In window 1:
```bash
./project2_p2
```

The parent prints something like:
```
[parent PID 12345] open another PuTTY and try: kill -SIGHUP 12345
```

In window 2 (replace `<pid>` with the actual PID printed above):
```bash
kill -SIGHUP <pid>
kill -SIGINT <pid>
kill -SIGTSTP <pid>
```

**Capture window 1**, showing handler lines like:

```
[handler] PID 12345 caught SIGHUP (#1) from sender PID 67890 (uid 1001, code 0)
```

### 4. `figures/p2_q2_peer.png`

Still inside the same `./project2_p2` run, the program prints `[Q2]` and
`[Q2 driver]` lines automatically:

```
[Q2] child 0 sends SIGINT once to child 1 (PID ...)
[Q2] child 0 sends SIGINT once to parent (PID ...)
[Q2 driver] parent sends SIGINT once to child 1 ...
```

**Capture those `[Q2]` lines and the matching `[handler]` prints that
follow them.**

### 5. `figures/p2_q3_pending.png`

```bash
./project2_p2_q3
```

The output includes `[pending]` lines:

```
[pending] parent before fork PID 12345: SIGINT SIGQUIT SIGTSTP
[pending] after-first-burst PID 12346: SIGINT SIGQUIT SIGTSTP
[pending] after-first-burst PID 12348: SIGABRT SIGILL SIGCHLD SIGSEGV SIGFPE SIGHUP
[child 0 sigtimedwait] pulled SIGINT from PID 12345
[child 2 sigwait] received SIGABRT
```

**Capture enough lines to show:**
- one `[pending] parent ...` line,
- one child-half-1 line (3 signals: SIGINT/SIGQUIT/SIGTSTP),
- one child-half-2 line (6 signals: SIGABRT/SIGILL/SIGCHLD/SIGSEGV/SIGFPE/SIGHUP),
- a couple of `sigtimedwait` / `sigwait` lines.

## Filename quick map

| Figure in `Report.tex` | Save PNG as |
|---|---|
| Process tree right after forking | `figures/p1_pstree_start.png` |
| Tree mid-run while Rule 1 sleeps | `figures/p1_pstree_mid.png` |
| `project2_p2` running, second PuTTY sending kills | `figures/p2_part1_console.png` |
| Q2: child 0 signaling sibling and parent | `figures/p2_q2_peer.png` |
| Q3: `[pending]` output from `sigpending` | `figures/p2_q3_pending.png` |

## Compile the report

Upload the whole folder (including `figures/`) to Overleaf. Engine: **pdfLaTeX**.
Main file: `Report.tex`. No special flags needed.

## Push screenshots to GitHub

After all PNGs are in `figures/`:

```bash
git add figures/*.png
git commit -m "Add PuTTY screenshots for Project 2 figures"
git push
```

## Clean up

```bash
make clean
```

Removes built binaries and the sample input/output/trace files.
