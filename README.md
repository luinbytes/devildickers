# DaggerFall

Bhop and air control trainer for [Devil Daggers](https://devildaggers.com/) on Linux.

Hold space to auto bhop with perfect timing. Hold space + WASD to fly around the arena with full air control.

## Building

```bash
gcc -O2 -o daggerfall daggerfall.c -lX11 -lm $(pkg-config --cflags --libs gtk+-3.0 gtk-layer-shell-0)
```

## Setup

Copy `example.conf` to `daggerfall.conf` and fill in the offsets for your build. You need to find the pointer chain and hero struct layout yourself. Do not ask me for offsets, I am not giving them out. If you brick your game or get banned that's on you — this is a research project not a product. Use at your own risk.

The tool looks for `daggerfall.conf` in three places: next to the binary, current working directory, and `~/.config/daggerfall.conf`.

## Usage

```
./daggerfall              # auto bhop (hold space)
./daggerfall --strafe     # bhop + air control (hold space + WASD)
./daggerfall --diag       # live position/velocity/state readout
./daggerfall --aim        # dagger landing prediction overlay (read-only)
./daggerfall --teleport X Z
```

### Modes

`daggerfall` auto bhops with perfect timing every jump. Just hold space and move.

`daggerfall --strafe` adds full air control on top. You can change direction instantly mid air, speed is preserved during fall, and gravity is reduced near the ground for longer air time.

`daggerfall --diag` shows a live readout of position, velocity, yaw, alive state, and speed params.

`daggerfall --aim` renders a click-through overlay showing where your daggers will land on the floor. Read-only — no memory writes. Uses GTK3 layer-shell so it works on Wayland compositors. Requires `h_pitch` and projectile physics params in your config.

`daggerfall --teleport X Z` moves your hero to the given X,Z coordinates.

## Requirements

* Linux
* Devil Daggers running via Steam (Linux native build)
* X11 development headers (`libx11-dev` or equivalent)
* GTK3 development headers + `gtk-layer-shell` (for `--aim` overlay)
* Your own `daggerfall.conf` with the right offsets
