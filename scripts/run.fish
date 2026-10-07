#!/usr/bin/env fish

set -l root (realpath (dirname (status filename))/..)

if test (count $argv) -eq 0
    echo "usage: scripts/run.fish <vulkan-app> [args...]" >&2
    exit 2
end

set -x VK_ADD_LAYER_PATH $root/builddir/layer/dev
set -x VK_LOADER_LAYERS_ENABLE VK_LAYER_vknemu_idle_limiter

exec $argv

# and bash users can cry. duh
