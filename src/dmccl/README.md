# dmccl - the dex mega cool coding language(tm), C edition

dmccl is a tiny scripting language with a sense of humour. This is the C rewrite of the original Rust interpreter
(`D:\projects\dmccl`): same words (`when run do`, `endifchain`, `waitfor:5s`, `suicide`, `above(1)`), new powers
(things, members, events, loops), and it runs inside the kernel as the scripting language of the KAnarchy Wheels game.

```
from dexlib import world
from dexlib import fx

thing mama is "ball" do
    when run do
        world:ball(me.x, me.y - 1, 0.25)        # a baby
        waitfor:4s
        fx:text(me.x, me.y - 1.5, "BYE KID")
        child.abort then suicide                # abort the child, then die. yes.
    endifchain
endifchain
```

## The language in one page

| | |
|---|---|
| comments | `# to the end of the line` |
| values | numbers (`3`, `0.5`, `2s`, `250ms` - units are just numbers of seconds), `"text"`, `true`, `false`, `nil`, and things (entities of the game) |
| variables | `x = 3`, `x += 1`, `x -= 1` - everything outside a `thing` is global; a thing keeps its own state in `me.field = value` |
| operators | `+ - * / %`, `== != < > <= >=`, `&`/`and`, `|`/`or`, `not`; `+` joins text; `&` and `|` stop at the first answer |
| calls | `module:function(args)` or `function(args)` (imports below); `math("10 + 5 * 2")` evaluates text |
| members | `guy.x`, `guy.x = 5`, `guy.hp -= 10`, `guy.hurt(10)`, `child.abort` - a verb needs no parens |
| `then` | `a then b` runs two statements on one line (`also` means the same) |
| `if` | `if cond do ... elseif cond do ... else do ... endifchain` |
| `repeat` | `repeat 5 as i do ... endifchain` (`as i` is optional; `i` counts from 0) |
| `forever` | `forever do ... endifchain` - one pass per game step unless the body waits |
| `waitfor` | `waitfor:2s` - sleeps THIS handler only; the game keeps running |
| `checkfor` | `checkfor ready 5s do ... endifchain` - wait up to 5 s for `ready`; if it never happens, run the body. A text condition is the NAME of a variable (original dmccl) |
| `above` | `above(1)` runs the previous statement again; `above(3)` the third one back; `above(loop)` starts the block over, forever |
| `stop` / `suicide` | `stop` ends this handler. `suicide` kills the thing the script belongs to (`me`); at top level it ends the level |
| imports | `from dexlib import world` before using `world:...` (`log:` and `basics` are always there) |

### Handlers: `when`

```
when run do ... endifchain                     # once, when the level (or the thing) starts
when tick do ...                               # every game step (60 per second)
when every 2s do ...                           # every 2 seconds
when key "space" do ...                        # a key press (R is built in: restart)
when guy touches "spikes" do ...               # `guy` = a variable holding a thing, "spikes" = a tag; either order
when guy dies do ...
when run do child.abort then suicide           # a handler whose first statement is on the `do` line ends at the end of the line
```

Inside a handler: `me` (the thing the script belongs to; nil at level scope), `who`/`hit` (the two sides of a touch), `child`
(the last thing `me` made), `parent`, `time` (seconds since the level began).

### Things

```
thing barrel is "barrel" do                    # `is` picks the body: ball barrel crate box block plank
    when me touches "guy" do
        fx:boom(me, 45)
        suicide
    endifchain
endifchain

when run do  world:make("barrel", 40, 5)  endifchain
```

Every `world:make("barrel", x, y)` is a new barrel whose handlers run with `me` = that barrel.

### Safety

Nothing a script does can hang the machine: each handler runs at most 6000 instructions per game step and 120 000 all together,
then it is put to sleep until the next step (a runaway loop just runs slowly). A runtime error stops that one handler and
prints `[dmccl] error at line N: ...`; the rest of the level keeps going. Verbs on dead things are ignored, members of dead
things read as `nil`. Strings are garbage collected.

## The game modules (`src/game/game.c`)

`world:` block(x,y,w,h) slope(x1,y1,x2,y2) ramp(x,y,w,h,deg) bouncy(x,y,w) crate(x,y,[size]) ball(x,y,[r]) plank(x,y,w,h)
spikes(x,y,[n]) goal(x,y) bridge(x1,y1,x2,y2,[planks]) wrecker(x,y,[links,radius]) seesaw(x,y,[len]) make("thing",x,y)
spawn("wheelchair",x,y)

`fx:` blood(thing|x,y,[n]) boom(thing|x,y,[power]) shake(n) text(x,y,"words")

`game:` win() lose("why") message("text",[secs]) gore(0|1) zoom(f) gravity(g) held("up")

Members of a thing: `x y vx vy speed angle mass hp w h alive kind tag` (readable), `x y vx vy angle hp tag` (settable);
verbs `kill abort hurt(n) push(vx,vy) spin(w) freeze thaw boom(power) eject`; any other name you assign is stored on the thing.
y grows DOWNWARD (the road in the shipped levels is at y = 5.5).

## Where things live

- `dm_lex.c` tokens, `dm_compile.c` one-pass compiler to bytecode, `dm_vm.c` cooperative VM (threads per handler, strings, GC),
  `dm_lib.c` standard functions, `dm.h` the embedding API, `dm_sys.h` the kernel/host shim.
- Tests: `sh tests/dmccl/run.sh` (ASan + UBSan, a fake world, error messages, limits, mutation fuzzer); command line runner
  `build-host/dmccl file.dmccl` (`tools/dmccl/cli.c`). The Rust version stays the reference: its examples print the same.
