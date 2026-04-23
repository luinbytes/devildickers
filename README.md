# devildickers

Bhop and air control trainer for [Devil Daggers](https://devildaggers.com/) on Linux.

Hold space to auto bhop with perfect timing. Hold space + WASD to fly around the arena with full air control.

## Building

```bash
gcc -O2 -o dd-bhop dd-bhop.c -lX11 -lm
```

## Setup

Copy `example.conf` to `dd-bhop.conf` and fill in the offsets for your build. You need to find the pointer chain and hero struct layout yourself.

The tool looks for `dd-bhop.conf` in three places: next to the binary, current working directory, and `~/.config/dd-bhop.conf`.

## Usage

```
./dd-bhop              # auto bhop (hold space)
./dd-bhop --strafe     # bhop + air control (hold space + WASD)
./dd-bhop --diag       # live position/velocity/state readout
./dd-bhop --teleport X Z
```

### Modes

`dd-bhop` auto bhops with perfect timing every jump. Just hold space and move.

`dd-bhop --strafe` adds full air control on top. You can change direction instantly mid air, speed is preserved during fall, and gravity is reduced near the ground for longer air time.

`dd-bhop --diag` shows a live readout of position, velocity, yaw, alive state, and speed params.

`dd-bhop --teleport X Z` moves your hero to the given X,Z coordinates.

## Requirements

* Linux
* Devil Daggers running via Steam (Linux native build)
* X11 development headers (`libx11-dev` or equivalent)
* Your own `dd-bhop.conf` with the right offsets
