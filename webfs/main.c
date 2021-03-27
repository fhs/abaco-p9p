#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ip.h>
#include <plumb.h>
#include <thread.h>
#include <fcall.h>
#include <9p.h>
#include "dat.h"
#include "fns.h"

char *cookiefile;
char *service = "web";

Ctl globalctl = 
{
	1,	/* accept cookies */
	1,	/* send cookies */
	10,	/* redirect limit */
	"webfs/2.0 (plan 9)"	/* user agent */
};

void
usage(void)
{
	fprint(2, "usage: webfs [-c cookies] [-s service]\n");
	threadexitsall("usage");
}

int
threadmaybackground(void)
{
	return 1;
}

//#include <pool.h>
void
threadmain(int argc, char **argv)
{
	rfork(RFNOTEG);
	ARGBEGIN{
	case 'd':
		//mainmem->flags |= POOL_PARANOIA|POOL_ANTAGONISM;
		break;
	case 'D':
		chatty9p++;
		break;
	case 'c':
		cookiefile = EARGF(usage());
		break;
	case 's':
		service = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND

	quotefmtinstall();
	if(argc != 0)
		usage();

	plumbinit();
	globalctl.useragent = estrdup(globalctl.useragent);
	initcookies(cookiefile);
	initurl();
	initfs();
	threadpostmountsrv(&fs, service, nil, MREPL);
	threadexits(nil);
}
