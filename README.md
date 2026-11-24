# CE Command Prompt

A Windows **CMD**-style command shell for the **TI-84 Plus CE**, written in C with the
[CE C toolchain](https://ce-programming.github.io/toolchain/).

It has a black console window with a title bar and 200 lines of scrollback. Files live on a
virtual `C:\` drive with real folders, and that drive is saved to the calculator's archive
memory. You also get a full batch-file interpreter, pipes and redirection, and a text editor.

```
C:\Users\User>dir /b | sort /r > list.txt & type list.txt
readme.txt
Documents
demo.bat

C:\Users\User>set /a x=6*7, x<<=1
84

C:\Users\User>for /l %i in (1,1,3) do @echo Line %i
Line 1
Line 2
Line 3
```

## Install

1. Get the C libraries ([`clibs.8xg`](https://github.com/CE-Programming/libraries/releases/latest)) onto the calculator. Most C programs need them, and you may already have them.
2. Send **`bin/CMD.8xp`** to the calculator with TI Connect CE (or TILP).
3. Run it:
   - **TI-OS 5.4 and older:** `Asm(prgmCMD)` (or just `prgmCMD` on 5.3+).
   - **TI-OS 5.5 and newer:** TI blocked native programs on these versions. You need
     [arTIfiCE](https://yvantt.github.io/arTIfiCE/) to run them, optionally with a shell
     like [Cesium](https://github.com/mateoconlechuga/cesium).
     This applies to every C or assembly program, not just this one.

> **Memory:** the program is compressed to about 59 KB, but it unpacks to about **105 KB of
> RAM** when it starts. If you get a memory error, archive your other programs and variables
> first (`[2nd][+]` → Mem Mgmt).

Exit with `exit` or **`[2nd][mode]`** (QUIT). Drive `C:` is saved to archive memory when you exit.

## Typing on a calculator keypad

The status bar at the bottom shows the input mode: **`abc`**, **`ABC`** or **`123`**.
Press **`[alpha]`** to switch modes. Type `keys` for the full map on the calculator.

| Key | Does |
|---|---|
| `[alpha]` | Switch abc → ABC → 123 |
| `[2nd]` | The next key types a symbol (see below) |
| `[enter]` | Run the command |
| `[del]` / `[2nd][del]` | Backspace / delete |
| `[clear]` | Clear the line |
| `◄ ►`, `[2nd]◄ ►` | Move the cursor, Home/End |
| `▲ ▼` | Command history |
| `[2nd]▲ ▼` | Scroll back through earlier output |
| `[y=]` | **Tab**: completes command, file and folder names (press again for the next match) |
| `[2nd][enter]` | Recall the last command |
| `[on]` | **Ctrl+C**: cancels the line or stops a running command or batch file |
| `[2nd][mode]` | Exit CMD |

**These keys type the same thing in every mode:**
`[X,T,θ,n]` space · `[window]` `\` · `[zoom]` `.` · `[trace]` `/` · `[graph]` `"` · `[vars]` `%` · `[stat]` `*` · `[mode]` `=`

**abc mode:** letters are where they're printed on the keys. Also `0`=space, `.`=`:`, `(-)`=`?`, `+`=`"`, `3`=`_`

**123 mode:** digits and `+ - * / ^ ( ) , .`, plus `(-)`=`-` and `[sto→]`=`>`.
`[math]`…`[cos]` type `a`–`f`, so you can type hex like `color 0a`.

**`[2nd]` + key:**

| | | | | | |
|---|---|---|---|---|---|
| `0` → `=` | `1` → `!` | `2` → `@` | `3` → `#` | `4` → `$` | `5` → `%` |
| `6` → `&` | `7` → `<` | `8` → `>` | `9` → `\|` | `.` → `:` | `(-)` → `~` |
| `÷` → `\` | `×` → `*` | `-` → `_` | `+` → `+` | `(` → `[` | `)` → `]` |
| `,` → `;` | `sto→` → `'` | `^` → `{` | `x²` → `}` | `x⁻¹` → `` ` `` | `math` → `?` |

## Commands

Every command accepts `/?`, and `help <command>` shows its usage.

| Category | Commands |
|---|---|
| Files & folders | `dir` (`/s /b /w /a /p`), `cd`/`chdir`, `md`/`mkdir`, `rd`/`rmdir` (`/s /q`), `copy` (wildcards, `a+b`, `copy con`), `xcopy` (`/s /e`), `move`, `ren` (incl. `*.txt *.bak`), `del`/`erase` (`/s /q /f`), `attrib` (`+r -h …`), `tree` (`/f`), `pushd`/`popd`, `label`, `vol`, `format` |
| Text | `type`, `more`, `find` (`/i /v /c /n`), `findstr` (regex: `. * + ? ^ $ [a-z] [^x]`, `/i /v /n /b /e /m /c:`), `sort` (`/r`), `fc`, `edit`/`notepad` (full-screen editor) |
| Shell | `echo`, `set` (incl. `/a` math, `/p` input), `setlocal`/`endlocal` (`EnableDelayedExpansion`), `if`, `for` (`/l /d /f`), `goto`, `call`, `shift`, `exit` (`/b n`), `pause`, `choice`, `timeout`, `rem`, `cls`, `color`, `title`, `prompt`, `path`, `doskey` (macros), `history`, `cmd /c`, `where`, `help`, `keys` |
| System | `ver`, `systeminfo`, `mem`, `date`, `time`, `whoami`, `hostname`, `shutdown` |
| Calculator | `tasklist` lists programs and AppVars. `start PRGM` (or just type a program's name) runs a TI program and returns to CMD when it ends. `taskkill /im NAME` deletes a program or AppVar. `export file VAR` / `import VAR file [/p]` copy data between C: and real AppVars or programs. |

### Shell features
- **Chaining:** `a & b`, `a && b`, `a || b`, `( a & b )` blocks.
- **Pipes:** `dir /b | find "txt" | sort`.
- **Redirection:** `>`, `>>`, `<`, `>nul`, `2>nul`, `2>&1`.
- **Variables:** `%VAR%`, `%VAR:~2,3%`, `%VAR:old=new%`, `!VAR!` (delayed expansion).
  Dynamic variables: `%CD% %DATE% %TIME% %RANDOM% %ERRORLEVEL%`.
- **Batch files** (`.bat`/`.cmd`, found in the current folder or on `%PATH%`):
  - Arguments: `%0`–`%9`, `%*`, `%~1`, `%~dp0` / `%~nx1` modifiers.
  - Control: `:labels`, `goto :eof`, `call :sub args`, `exit /b n`, multi-line `( … ) else ( … )` blocks.
  - `@echo off` and `setlocal` work as in CMD.
- **Startup:** `C:\autoexec.bat` runs when CMD starts, if it exists.
- **Opening other files:** typing the name of a `.txt` file opens it in the editor.
- **Try these:** `demo` shows off the batch features, and `guess` is a number-guessing game.

### Editor (`edit file.txt`)
- Arrows move the cursor. `[2nd]◄ ►` jump to the start or end of a line, and `[2nd]▲ ▼` page up and down.
- `[del]` is backspace and `[2nd][del]` deletes forward. `[clear]` deletes to the end of the line.
- `[y=]` saves. `[graph]` quits and asks whether to save unsaved changes.

## How it works

| File | What it does |
|---|---|
| `src/ui.c` | graphx console (40×27 characters, 8×8 font, 16-color CMD palette); keypad scanning with auto-repeat; keypad → character maps; line editor with history and Tab completion; output routing for pipes |
| `src/fs.c` | The virtual drive. The folder tree (up to 160 entries) is stored in the AppVar `CMDFSYS`. Each file's contents are stored in their own AppVar `CMDFxx`, up to about 12 KB per file for editing. Everything is archived on exit. |
| `src/shell.c` | Parser: `%`/`!` expansion, `& && \|\| \|`, redirection, `if`, `for`, the batch interpreter, `setlocal`, doskey macros |
| `src/cmds.c` | The built-in commands |
| `src/expr.c` | `set /a` expression evaluator and the regex engine for `findstr` |
| `src/edit.c` | The full-screen editor |

The eZ80's C stack is only 4 KB. Big buffers are therefore allocated on the heap. The shell also
checks the real stack pointer before nesting deeper, so deeply nested batch scripts stop with an
error instead of crashing the calculator.

## Building

1. Install the CE C toolchain (v12+).
2. Run `make`. The output is `bin/CMD.8xp`.

### Tests on a PC

`tests/run_host.sh` builds the same shell code for Linux with gcc, ASan and UBSan. It swaps in
stub TI headers (`tests/host/`) and feeds keystrokes as **keypad scan codes**, so the key maps,
line editor and editor are exercised along with the commands:

```sh
tests/run_host.sh tests/scripts/batch.txt
```

The scripts in `tests/scripts/` cover file operations, batch files, `for`/`if`, pipes, the editor,
Tab completion, history and the pager.

These tests cover the shell logic. Graphics and real keypad timing can only be checked on a
calculator or in CEmu.
