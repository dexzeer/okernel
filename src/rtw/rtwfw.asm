; The RTL8821C firmware (Realtek's binary; it runs on the chip's own processor, see src/rtw/rtw.h).
section .rodata
align 4
global rtw_fw_start, rtw_fw_end
rtw_fw_start:
incbin "firmware/rtw8821c_fw.bin"
rtw_fw_end:
align 4

; must stay the LAST section
section .note.GNU-stack noalloc noexec nowrite progbits
