# vkNemu

fps limiter vulkan layer based on input idling and/or window focus. Similar idea to Radeon Chill.


> [!WARNING]
> Keyboard/Mouse idling only works if your compositor supports `ext_idle_notifier_v1` v2, otherwise you'll just get unfocus/gamepad support.
>
> Also both X11/Wayland games are supported. 

## Installation / Build

#### Arch (aur)

```bash
paru -S vknemu
```

#### Debian / Fedora

`.deb` and `.rpm` on the [releases page](https://github.com/reakjra/vkNemu/releases)

#### Flatpak

Grab one or both `.flatpak` files from the [releases page](https://github.com/reakjra/vkNemu/releases):

```bash
flatpak install vkNemu-24.08.flatpak
# or
flatpak install vkNemu-25.08.flatpak
```

#### Manual building

```bash
meson setup builddir --prefix="$HOME/.local" -Drelocatable_layer=true
ninja -C builddir install
```

for 32-bit:

```bash
meson setup builddir32 --prefix="$HOME/.local" --libdir=lib32 -Drelocatable_layer=true \
    --cross-file cross/i686-pc-linux-gnu.ini
ninja -C builddir32 install
```

## Use

use `VKNEMU=1` to use it. lol.

## Options

| Variable | Default | What? |
|:---|:---|:---|
| `VKNEMU_IDLE_FPS` | `30` | Frame rate cap while idle or unfocused |
| `VKNEMU_TIMEOUT` | `2` | Seconds without input before capping |
| `VKNEMU_DELAY` | `0` | Seconds after launch before being able to cap (e.g. avoid limiting during a game's intro screen) |
| `VKNEMU_INPUT` | `all` | Which input to read: `all`, `keyboard` (keyboard + mouse), `gamepad`  or `none` |
| `VKNEMU_DEADZONE` | `0.02` | Stick movement smaller than this is ignored |
| `VKNEMU_UNFOCUSED` | `1` | Cap right away when the game isn't the focused window |

## License

GPL-3.0-or-later
