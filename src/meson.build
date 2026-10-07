wayland_scanner = find_program('wayland-scanner')
idle_notify_xml = dependency('wayland-protocols').get_variable('pkgdatadir') / 'staging' / 'ext-idle-notify' / 'ext-idle-notify-v1.xml'

idle_notify_protocol = [
    custom_target('idle-notify-client-header',
        input   : idle_notify_xml,
        output  : '@BASENAME@-client-protocol.h',
        command : [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
    custom_target('idle-notify-private-code',
        input   : idle_notify_xml,
        output  : '@BASENAME@-protocol.c',
        command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]

layer_lib = shared_library('VkLayer_vknemu',
    'dispatch_map.c',
    'gamepad_input.c',
    'idle_notify.c',
    'layer.c',
    'pacer.c',
    'wayland_focus.c',
    'x11_focus.c',
    idle_notify_protocol,
    c_args                : '-D_GNU_SOURCE',
    gnu_symbol_visibility : 'hidden',
    dependencies          : [
        dependency('vulkan').partial_dependency(compile_args : true),
        dependency('x11').partial_dependency(compile_args : true),
        dependency('xcb'),
        dependency('wayland-client'),
        dependency('threads'),
    ],
    install               : true)
