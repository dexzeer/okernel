; The game's built-in levels: dmccl source files from games/, linked into the kernel image and compiled when a level starts.
; Add a level: put games/levelN.dmccl here and a row in the table in src/gamewin.c.

section .rodata
align 4
%macro LEVEL 2
global %1, %1_end
%1:
incbin %2
%1_end:
db 0
%endmacro

LEVEL lvl_1, "games/level1.dmccl"
LEVEL lvl_2, "games/level2.dmccl"
LEVEL lvl_3, "games/level3.dmccl"
LEVEL lvl_4, "games/level4.dmccl"

section .note.GNU-stack noalloc noexec nowrite progbits
