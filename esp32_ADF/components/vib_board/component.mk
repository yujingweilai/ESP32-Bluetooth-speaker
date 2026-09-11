#
# "main" pseudo-component makefile.
#
# (Uses default behaviour of compiling all source files in directory, adding 'include' to include path.)

ifdef CONFIG_AUDIO_BOARD_CUSTOM
	ifdef CONFIG_VIB_BOARD_V1_0
	COMPONENT_ADD_INCLUDEDIRS += ./board_v1_0
	COMPONENT_SRCDIRS += ./board_v1_0
	endif

	ifdef CONFIG_VIB_BOARD_V3_0
	COMPONENT_ADD_INCLUDEDIRS += ./board_v3_0
	COMPONENT_SRCDIRS += ./board_v3_0
	endif

endif