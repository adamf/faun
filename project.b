options [
    flac:   'libflac    "FLAC loader implementation ('libflac 'foxen none)"
    static: false       "Build static library"
    ftest:  false       "Build faun_test program (modifies library)"
    wtest:  false       "Build write_test program (modifies library)"
    load-mem: true      "Include functions to load buffers from memory"
    examples: false     "Build example programs"
]

if int? flac [
    flac: pick [libflac foxen] flac
]

if wtest [
    static: true
    ftest: false
]

libfaun: [
    cflags "-DUSE_SFX_GEN"
    switch flac [
        libflac [cflags "-DUSE_FLAC=1"]
        foxen   [cflags "-DUSE_FLAC=2"]
    ]
    if ftest [cflags "-DCAPTURE"]
    if wtest [cflags "-DREPORT"]
    if load-mem [cflags "-DUSE_LOAD_MEM"]
    include_from %support
    if msvc [include_from %../usr/include]
    sources [
        %support/tmsg.c
        %faun.c
    ]
]

faun-dep: [
    if eq? flac 'libflac [libs %FLAC]
    linux [libs [%pulse %vorbisfile %pthread %m]]
    win32 [
        either msvc
            [libs_from %../usr/lib [%vorbisfile]]
            [libs [%vorbisfile]]
       ;libs [%dsound %dxguid %uuid %user32]
        libs [%ole32 %user32]
    ]
]

flink: [
    include_from %.
    libs_from %. %faun
]

either static [
    lib %faun libfaun
    faun-link: does append flink faun-dep
][
    shlib [%faun 0,2,3] append append libfaun faun-dep [
        win32 [lflags either msvc "/def:faun.def" "faun.def"]
    ]
    faun-link: does flink
]

if ftest [
    exe %faun_test [console faun-link sources [%faun_test.c]]
]
if wtest [
    exe %write_test [console faun-link sources [%write_test.c]]
]
if examples [
    exe %basic     [console faun-link sources [%example/basic.c]]
    exe %crossfade [console faun-link sources [%example/crossfade.c]]
]

dist [
    %faun.def
    %support/cpuCounter.h
    %sys_pulseaudio.c
    %sys_wasapi.c
    %write_test.c
]
