; Desktop wallpaper: a 1920x1080 JPEG rendered by tools/wallpaper/render.sh
; (raymarched 3D KAnarchy emblem), decoded at boot by the web engine's JPEG
; decoder (desktop.c -> graphics_set_wallpaper).

section .rodata
align 4
global wp_jpg, wp_jpg_end
wp_jpg:
incbin "tools/wallpaper/wallpaper.jpg"
wp_jpg_end:

section .note.GNU-stack noalloc noexec nowrite progbits
