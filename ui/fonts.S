/* OFL fonts compiled into the binary (see third_party/README.md); assembled with -Wa,-I<third_party/fonts> */
#ifdef _WIN32
	.section .rdata
#else
	.section .rodata
#endif
	.balign 16
	.global eiii_font_regular
#ifndef _WIN32
	.hidden eiii_font_regular
#endif
	.global eiii_font_regular_end
#ifndef _WIN32
	.hidden eiii_font_regular_end
#endif
eiii_font_regular:
	.incbin "NotoSans-CondensedMedium.ttf"
eiii_font_regular_end:
	.balign 16
	.global eiii_font_bold
#ifndef _WIN32
	.hidden eiii_font_bold
#endif
	.global eiii_font_bold_end
#ifndef _WIN32
	.hidden eiii_font_bold_end
#endif
eiii_font_bold:
	.incbin "NotoSans-CondensedSemiBold.ttf"
eiii_font_bold_end:
	.balign 16
	.global eiii_font_narrow
#ifndef _WIN32
	.hidden eiii_font_narrow
#endif
	.global eiii_font_narrow_end
#ifndef _WIN32
	.hidden eiii_font_narrow_end
#endif
eiii_font_narrow:
	.incbin "ArchivoNarrow-wght.ttf"
eiii_font_narrow_end:
#ifndef _WIN32
	.section .note.GNU-stack,"",%progbits
#endif
