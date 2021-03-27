#!/bin/sh
if [ ! -f $HOME/.webcookies ]; then
	touch $HOME/.webcookies
fi
if [ ! -e `namespace`/web ]; then
	webfs -s web
fi

# export PLAN9=/usr/share/abaco 
abaco.bin $*
