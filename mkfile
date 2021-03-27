<$PLAN9/src/mkhdr

MKSHELL=rc

DIRS=abaco webfs

none:V: all

all clean nuke install installall:V:
	for (i in $DIRS) @{
		cd $i
		mk $target
	}
