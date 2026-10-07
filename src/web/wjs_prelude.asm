; The okai Web API prelude (src/web/wjs_prelude.js), embedded NUL-terminated
; for wjs.c. Path is relative to the repository root (make runs there).

section .rodata
align 4

global wjs_prelude_src, wjs_prelude_len
wjs_prelude_src:
incbin "src/web/wjs_prelude.js"
wjs_prelude_end:
db 0
align 4
wjs_prelude_len:
dd wjs_prelude_end - wjs_prelude_src

section .note.GNU-stack noalloc noexec nowrite progbits
