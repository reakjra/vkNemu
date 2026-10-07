if get_option('relocatable_layer')
    lib_install_dir = get_option('prefix') / '$LIB'
else
    lib_install_dir = get_option('prefix') / get_option('libdir')
endif

configure_file(
    input         : 'vknemu.json.in',
    output        : 'vknemu.json',
    configuration : { 'LIBRARY_PATH' : lib_install_dir / 'libVkLayer_vknemu.so' },
    install       : true,
    install_dir   : get_option('datadir') / 'vulkan' / 'implicit_layer.d')

subdir('dev')
