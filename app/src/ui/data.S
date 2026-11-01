// The fonts (font.bin, from TokenStation5's make_font.py) and the logo at two
// sizes (make_logo.py, made by build.sh), linked into the app as they are.
    .section .rodata
    .balign 16
    .global ws_font_data
    .global ws_font_data_end
ws_font_data:
    .incbin "font.bin"
ws_font_data_end:
    .balign 16
    .global ws_logo_small
    .global ws_logo_small_end
ws_logo_small:
    .incbin "logo_small.bin"
ws_logo_small_end:
    .balign 16
    .global ws_logo_big
    .global ws_logo_big_end
ws_logo_big:
    .incbin "logo_big.bin"
ws_logo_big_end:
    .section .note.GNU-stack, "", %progbits
