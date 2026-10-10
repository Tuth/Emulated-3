/* OFL fonts compiled into the binary (see third_party/README.md); assembled with -Wa,-I<third_party/fonts> */
	.section .rodata
	.balign 16
	.global eiii_font_regular
	.hidden eiii_font_regular
	.global eiii_font_regular_end
	.hidden eiii_font_regular_end
eiii_font_regular:
	.incbin "NotoSans-CondensedMedium.ttf"
eiii_font_regular_end:
	.balign 16
	.global eiii_font_bold
	.hidden eiii_font_bold
	.global eiii_font_bold_end
	.hidden eiii_font_bold_end
eiii_font_bold:
	.incbin "NotoSans-CondensedSemiBold.ttf"
eiii_font_bold_end:
	.balign 16
	.global eiii_font_narrow
	.hidden eiii_font_narrow
	.global eiii_font_narrow_end
	.hidden eiii_font_narrow_end
eiii_font_narrow:
	.incbin "ArchivoNarrow-wght.ttf"
eiii_font_narrow_end:
	.section .note.GNU-stack,"",%progbits
