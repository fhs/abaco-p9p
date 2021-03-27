#include <u.h>
#include <libc.h>
#include <draw.h>
#include <memdraw.h>
#include <fcall.h>
#include <9pclient.h>
#include <thread.h>
#include <cursor.h>
#include <mouse.h>
#include <keyboard.h>
#include <frame.h>
#include <plumb.h>
#include <html.h>
#include "dat.h"
#include "fns.h"

Url *
urlalloc(Runestr *src, Runestr *post, int m)
{
	Url *u;

	u = emalloc(sizeof(Url));
	copyrunestr(&u->src, src);
	if(m==HPost)
		copyrunestr(&u->post, post);
	u->method = m;
	incref(&u->ref);
	return u;
}

void
urlfree(Url *u)
{
	if(u && decref(&u->ref)==0){
		closerunestr(&u->src);
		closerunestr(&u->act);
		closerunestr(&u->post);
		closerunestr(&u->ctype);
		free(u);
	}
}

Url *
urldup(Url *a)
{
	Url *b;

	b = emalloc(sizeof(Url));
	b->method = a->method;
	copyrunestr(&b->src, &a->src);
	copyrunestr(&b->act, &a->act);
	copyrunestr(&b->post, &a->post);
	copyrunestr(&b->ctype, &a->ctype);
	return b;
}

static 
Runestr
getattr(int conn, char *s)
{
	char buf[BUFSIZE];
	CFid *fid;
	int n;

	snprint(buf, sizeof(buf), "%d/%s", conn, s);
	fid = fsopen(webfs, buf, OREAD);
	if(fid == nil)
		error("can't open attr file");

	n = fsread(fid, buf, sizeof(buf)-1);
	if(n < 0)
		error("can't read");

	fsclose(fid);
	buf[n] = '\0';
	return (Runestr){runesmprint("%s", buf), n};
}

int
urlopen(Url *u)
{
	char buf[BUFSIZE];
	CFid *cfid, *fid;
	int fd, conn, n;

	cfid = fsopen(webfs, "clone", ORDWR);
	if(cfid == nil)
		error("can't open clone file");

	n = fsread(cfid, buf, sizeof(buf)-1);
	if(n <= 0)
		error("reading clone");

	buf[n] = '\0';
	conn = atoi(buf);

	snprint(buf, sizeof(buf), "url %S", u->src.r);
	if(fswrite(cfid, buf, strlen(buf)) < 0){
//		fprint(2, "write: %s: %r\n", buf);
    Err:
		fsclose(cfid);
		return -1;
	}
	if(u->method==HPost && u->post.r != nil){
		snprint(buf, sizeof(buf), "%d/postbody", conn);
		fid = fsopen(webfs, buf, OWRITE);
		if(fid == nil){
//			fprint(2, "urlopen: bad query: %s: %r\n", buf);
			goto Err;
		}
		snprint(buf, sizeof(buf), "%S", u->post.r);
		if(fswrite(fid, buf, strlen(buf)) < 0)
//			fprint(2, "urlopen: bad query: %s: %r\n", buf);

		fsclose(fid);
	}
	snprint(buf, sizeof(buf), "%d/body", conn);
	fd = fsopenfd(webfs, buf, OREAD);
	if(fd < 0){
//		fprint(2, "open: %S: %r\n", u->src.r);
		goto Err;
	}
	u->ctype = getattr(conn, "contenttype");
	u->act = getattr(conn, "parsed/url");
	if(u->act.nr == 0)
		copyrunestr(&u->act, &u->src);

	fsclose(cfid);
	return fd;
}


void
urlcanon(Rune *name){
	Rune *s, *t;
	Rune **comp, **p, **q;
	Rune dot[] = {'.',0};
	Rune up[] = {'.','.',0};
	Rune e[] = {0};
	int rooted;
	
	name = runestrchr(name, L'/')+2;
	rooted=name[0]==L'/';
	/*
	 * Break the name into a list of components
	 */
	comp=emalloc(runestrlen(name)*sizeof(char *));
	p=comp;
	*p++=name;
	for(s=name;;s++){
		if(*s==L'/'){
			*p++=s+1;
			*s='\0';
			}
			else if(*s=='\0')
				break;
	}
	*p=0;
	/*
	 * go through the component list, deleting components that are empty (except
	 * the last component) or ., and any .. and its non-.. predecessor.
	 */
	p=q=comp;
	while(*p){
		if(runestrcmp(*p, e)==0 && p[1]!=0
		|| runestrcmp(*p, dot)==0)
			p++;
		else if(runestrcmp(*p, up)==0 && q!=comp && runestrcmp(q[-1], up)!=0){
			--q;
			p++;
		}
		else
			*q++=*p++;
	}
	*q=0;
	/*
	 * rebuild the path name
	 */
	s=name;
	if(rooted) *s++='/';
	for(p=comp;*p;p++){
		t=*p;
		while(*t) *s++=*t++;
		if(p[1]!=0) *s++='/';
	}
	*s='\0';
	free(comp);
}

/* this is a HACK */
Rune *
urlcombine(Rune *b, Rune *u)
{
	Rune *p, *q, *sep, *s;
	Rune endrune[] = { L'?', L'#' };
	int i, restore;
	if(u == nil)
		error("urlcombine: u == nil");

	if(validurl(u))
		return erunestrdup(u);

	if(b==nil || !validurl(b))
		error("urlcombine: b==nil || !validurl(b)");

	if(runestrncmp(u, (Rune[]){'/','/',0}, 2) == 0){
		q =  runestrchr(b, L':');
		return runesmprint("%.*S:%S", (int)(q-b), b, u);
	}
	p = runestrstr(b, (Rune[]){':','/','/',0})+3;
	sep = (Rune[]){0};
	q = nil;
	if(*u ==L'/')
		q = runestrchr(p, L'/');
	else if(*u==L'#' || *u==L'?'){
		for(i=0; i<nelem(endrune); i++)
			if(q = runestrchr(p, endrune[i]))
				break;
		q = runestrrchr(p+3, L'/');
	}else{
		sep = (Rune[]){'/',0} ;
		restore = 0;
		s = runestrchr(p, L'?');
		if(s != nil){
			*s = '\0';
			restore = 1;
		}
		q = runestrrchr(p, L'/');
		if(restore)
			*s = L'?';
	}
	if(q == nil)
		p =  runesmprint("%S%S%S", b, sep, u);
	else
		p= runesmprint("%.*S%S%S", (int)(q-b), b, sep, u);
	urlcanon(p);
	return p;
}
