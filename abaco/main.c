#include <u.h>
#include <libc.h>
#include <draw.h>
#include <memdraw.h>
#include <fcall.h>
#include <9pclient.h>
#include <thread.h>
#include <fcall.h>
#include <9pclient.h>
#include <mouse.h>
#include <keyboard.h>
#include <cursor.h>
#include <frame.h>
#include <regexp.h>
#include <plumb.h>
#include <html.h>
#include "dat.h"
#include "fns.h"

void	mousethread(void *);
void	keyboardthread(void *);
void	iconinit(void);
void plumbproc(void*);


Channel	*cexit;
Channel	*cplumb;
Mousectl *mousectl;

char *fontnames[2] = {
#ifdef STANDALONE
	"/usr/share/abaco/freefont/sans/sans.13.font",
	"/usr/share/abaco/feefont/sans/sans.13.font"
#else
	"/usr/local/plan9/font/lucsans/unicode.8.font",
	"/usr/local/plan9/font/lucsans/passwd.6.font"
#endif
};

int	snarffd = -1;
int	mainpid;
CFid	*plumbwebfid;
CFid	*plumbsendfid;
char *charset = "iso-8859-1";
int	mainstacksize = STACK;

void	readpage(Column *, char *);
int	shutdown(void *, char *);

void
derror(Display *notused, char *s)
{
	error(s);
}

void
threadmain(int argc, char *argv[])
{
	Column *c;
	char *p;
	int i, ncol;

	rfork(RFENVG|RFNAMEG);
	notifyoff("sys: window size change");

	ncol = 1;
	ARGBEGIN{
	case 'c':
		p = ARGF();
		if(p == nil)
			goto Usage;
		ncol = atoi(p);
		if(ncol <= 0)
			goto Usage;
		break;
	case 'p':
		procstderr++;
		break;
	case 't':
		charset = ARGF();
		if(charset == nil)
			goto Usage;
		break;
	default:
    Usage:
		fprint(2, "usage: abaco [-c ncol] [-t charset] [url...]\n");
		threadexitsall("usage");
	}ARGEND

	/*
	int fds[3], fd[2];
	pipe(fd);
	fds[0] = fd[0];
	fds[1] = dup(2, 0);
	fds[2] = dup(2, 0);
	//threadspawnl(fds, "emu", "emu", "/dis/sh.dis", "-c", "styxmon {os dial unix!/tmp/ns.kris.:0/web >[1=0]}", nil);
	threadspawnl(fds, "sh", "sh", "-c", "dial unix!/tmp/ns.kris.:0/wmii >[1=0]", nil);
	*/
	webfs = nsmount("web", "");
	if(webfs == nil){
		fprint(2, "abaco: can't connect to webfs: %r\n");
		threadexitsall("webfs");
	}
	webctlfid = fsopen(webfs, "ctl", ORDWR);
	if(webctlfid == nil){
		fprint(2, "abaco: can't initialize webfs: %r\n");
		threadexitsall("webfs");
	}

	if(initdraw(derror, fontnames[0], "abaco") < 0){
		fprint(2, "abaco: can't open display: %r\n");
		threadexitsall("initdraw");
	}
	memimageinit();
	iconinit();
	timerinit();
	initfontpaths();
	cexit = chancreate(sizeof(int), 0);
	crefresh = chancreate(sizeof(Page *), 0);
	if(cexit==nil || crefresh==nil){
		fprint(2, "abaco: can't create initial channels: %r\n");
		threadexitsall("channels");
	}

	mousectl = initmouse(nil, screen);
	if(mousectl == nil){
		fprint(2, "abaco: can't initialize mouse: %r\n");
		threadexitsall("mouse");
	}
	mouse = &mousectl->m;
	keyboardctl = initkeyboard(nil);
	if(keyboardctl == nil){
		fprint(2, "abaco: can't initialize keyboard: %r\n");
		threadexitsall("keyboard");
	}
	mainpid = getpid();
	plumbwebfid = plumbopenfid("web", OREAD|OCEXEC);
	if(plumbwebfid){
		cplumb = chancreate(sizeof(Plumbmsg*), 0);
		proccreate(plumbproc, nil, STACK);
	}
	plumbsendfid = plumbopenfid("send", OWRITE|OCEXEC);

	rowinit(&row, screen->clipr);
	for(i=0; i<ncol; i++){
		c = rowadd(&row, nil, -1);
		if(c==nil && i==0)
			error("initializing columns");
	}
	enum{ WPERCOL=8 };
	c = row.col[row.ncol-1];
	for(i=0; i<argc; i++){
		if(i/WPERCOL >= row.ncol)
			readpage(c, argv[i]);
		else
			readpage(row.col[i/WPERCOL], argv[i]);
	}
	flushimage(display, 1);
	threadcreate(keyboardthread, nil, STACK);
	threadcreate(mousethread, nil, STACK);

	threadnotify(shutdown, 1);
	recvul(cexit);
	threadexitsall(nil);
}

void
readpage(Column *c, char *s)
{
	Window *w;
	Runestr rs;

	w = coladd(c, nil, nil, -1);
	bytetorunestr(s, &rs);
	pageget(&w->page, &rs, nil, HGet, TRUE);
	closerunestr(&rs);
}

char *oknotes[] = {
	"delete",
	"hangup",
	"kill",
	"exit",
	nil
};

int
shutdown(void *notused, char *msg)
{
	int i;

	for(i=0; oknotes[i]; i++)
		if(strncmp(oknotes[i], msg, strlen(oknotes[i])) == 0)
			threadexitsall(msg);
	print("abaco: %s\n", msg);
	abort();
	return 0;
}

void
plumbproc(void *notused)
{
	Plumbmsg *m;

	threadsetname("plumbproc");
	for(;;){
		m = plumbrecvfid(plumbwebfid);
		if(m == nil)
			threadexits(nil);
		sendp(cplumb, m);
	}
}

void
keyboardthread(void *notused)
{
	Timer *timer;
	Text *t;
	Rune r;

	enum { KTimer, KKey, NKALT };
	static Alt alts[NKALT+1];

	alts[KTimer].c = nil;
	alts[KTimer].v = nil;
	alts[KTimer].op = CHANNOP;
	alts[KKey].c = keyboardctl->c;
	alts[KKey].v = &r;
	alts[KKey].op = CHANRCV;
	alts[NKALT].op = CHANEND;

	timer = nil;
	threadsetname("keyboardthread");
	for(;;){
		switch(alt(alts)){
		case KTimer:
			timerstop(timer);
			alts[KTimer].c = nil;
			alts[KTimer].op = CHANNOP;
			break;
		case KKey:
		casekeyboard:
			typetext = rowwhich(&row, mouse->xy, r, TRUE);
			t = typetext;
			if(t!=nil && t->col!=nil && !(r==Kdown || r==Kleft || r==Kright))	/* scrolling doesn't change activecol */
				activecol = t->col;
			if(timer != nil)
				timercancel(timer);
			if(t!=nil){
				texttype(t, r);
				timer = timerstart(500);
				alts[KTimer].c = timer->c;
				alts[KTimer].op = CHANRCV;
			}else{
				timer = nil;
				alts[KTimer].c = nil;
				alts[KTimer].op = CHANNOP;
			}
			if(nbrecv(keyboardctl->c, &r) > 0)
				goto casekeyboard;
			flushimage(display, 1);
			break;
		}
	}
}

void
mousethread(void *notused)
{
	Plumbmsg *pm;
	Mouse m;
	Text *t;
	int but;
	enum { MResize, MMouse, MPlumb, MRefresh, NMALT };
	static Alt alts[NMALT+1];

	threadsetname("mousethread");
	alts[MResize].c = mousectl->resizec;
	alts[MResize].v = nil;
	alts[MResize].op = CHANRCV;
	alts[MMouse].c = mousectl->c;
	alts[MMouse].v = &mousectl->m;
	alts[MMouse].op = CHANRCV;
	alts[MPlumb].c = cplumb;
	alts[MPlumb].v = &pm;
	alts[MPlumb].op = CHANRCV;
	alts[MRefresh].c = crefresh;
	alts[MRefresh].v = nil;
	alts[MRefresh].op = CHANRCV;
	if(cplumb == nil)
		alts[MPlumb].op = CHANNOP;
	alts[NMALT].op = CHANEND;

	for(;;){
		qlock(&row.lock);
		flushrefresh();
		qunlock(&row.lock);
		flushimage(display, 1);
		switch(alt(alts)){
		case MResize:
			if(getwindow(display, Refnone) < 0)
				error("resized");
			scrlresize();
			tmpresize();
			rowresize(&row, screen->clipr);
			break;
		case MPlumb:
			plumblook(pm);
			plumbfree(pm);
			break;
		case MRefresh:
			break;
		case MMouse:
			m = mousectl->m;
			if(m.buttons == 0)
				continue;

			qlock(&row.lock);
			but = 0;
			if(m.buttons == 1)
				but = 1;
			else if(m.buttons == 2)
				but = 2;
			else if(m.buttons == 4)
				but = 3;
			
			if(m.buttons & (8|16)){
				if(m.buttons & 8)
					but = Kscrolloneup;
				else
					but = Kscrollonedown;
				rowwhich(&row, m.xy, but, TRUE);
			}else	if(but){
				t = rowwhich(&row, m.xy, but, FALSE);
				if(t)
					textmouse(t, m.xy, but);
			}
			qunlock(&row.lock);
			break;
		}
	}
}

Cursor boxcursor = {
	{-7, -7},
	{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	 0xFF, 0xFF, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8, 0x1F,
	 0xF8, 0x1F, 0xF8, 0x1F, 0xF8, 0x1F, 0xFF, 0xFF,
	 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
	{0x00, 0x00, 0x7F, 0xFE, 0x7F, 0xFE, 0x7F, 0xFE,
	 0x70, 0x0E, 0x70, 0x0E, 0x70, 0x0E, 0x70, 0x0E,
	 0x70, 0x0E, 0x70, 0x0E, 0x70, 0x0E, 0x70, 0x0E,
	 0x7F, 0xFE, 0x7F, 0xFE, 0x7F, 0xFE, 0x00, 0x00}
};

void
iconinit(void)
{
	Rectangle r;

	/* Green */
	tagcols[BACK] = allocimagemix(display, DPalegreen, DWhite);
	if(tagcols[BACK] == nil)
		error("allocimagemix");
	tagcols[HIGH] = eallocimage(display, Rect(0,0,1,1), screen->chan, 1, DDarkgreen);
	tagcols[BORD] = eallocimage(display, Rect(0,0,1,1), screen->chan, 1, DMedgreen);
	tagcols[TEXT] = display->black;
	tagcols[HTEXT] = display->black;

	/* Grey */
	textcols[BACK] = display->white;
	textcols[HIGH] = eallocimage(display, Rect(0,0,1,1), CMAP8,1, 0xCCCCCCFF);
	textcols[BORD] = display->black;
	textcols[TEXT] = display->black;
	textcols[HTEXT] = display->black;

	r = Rect(0, 0, Scrollsize+2, font->height+1);
	button = eallocimage(display, r, screen->chan, 0, DNofill);
	draw(button, r, tagcols[BACK], nil, r.min);
	r.max.x -= 2;
	border(button, r, 2, tagcols[BORD], ZP);

	r = button->r;
	colbutton = eallocimage(display, r, screen->chan, 0, 0x00994CFF);

	but2col = eallocimage(display, Rect(0,0,1,2), screen->chan, 1, 0xAA0000FF);
	but3col = eallocimage(display, Rect(0,0,1,2), screen->chan, 1, 0x444488FF);

	passfont = openfont(display, fontnames[1]);
	if(passfont == nil)
		error("openfont");
}

/*
 * /dev/snarf updates when the file is closed, so we must open our own
 * fd here rather than use snarffd
 */

/* rio truncates larges snarf buffers, so this avoids using the
 * service if the string is huge */

enum
{
	NSnarf = 1000,
	MAXSNARF = 100*1024,
};

/*
void
putsnarf(Runestr *rs)
{
	int fd, i, n;

	if(snarffd<0 || rs->nr==0)
		return;
	if(rs->nr > MAXSNARF)
		return;
	fd = open("/dev/snarf", OWRITE);
	if(fd < 0)
		return;
	for(i=0; i<rs->nr; i+=n){
		n = rs->nr-i;
		if(n > NSnarf)
			n =NSnarf;
		if(fprint(fd, "%.*S", n, rs->r) < 0)
			break;
	}
	close(fd);
}

void
getsnarf(Runestr *rs)
{
	int i, n, nb, nulls;
	char *sn, buf[BUFSIZE];

	if(snarffd < 0)
		return;
	sn = nil;
	i = 0;
	seek(snarffd, 0, 0);
	while((n=read(snarffd, buf, sizeof(buf))) > 0){
		sn = erealloc(sn, i+n+1);
		memmove(sn+i, buf, n);
		i += n;
		sn[i] = 0;
	}
	if(i > 0){
		rs->r = runemalloc(i+1);
		cvttorunes(sn, i, rs->r, &nb, &rs->nr, &nulls);
		free(sn);
	}
}
*/
