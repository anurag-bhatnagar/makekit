/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1997-2005
 *	Herbert Xu <herbert@gondor.apana.org.au>.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Kenneth Almquist.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Shell output routines.  We use our own output routines because:
 *	When a builtin command is interrupted we have to discard
 *		any pending output.
 *	When a builtin command appears in back quotes, we want to
 *		save the output of the command in a region obtained
 *		via malloc, rather than doing a fork and reading the
 *		output of the command via a pipe.
 *	Our output routines may be smaller than the stdio routines.
 */

#include <sys/types.h>		/* quad_t */
#include <sys/param.h>		/* BSD4_4 */
#include <sys/ioctl.h>

#include <stdio.h>	/* defines BUFSIZ */
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#ifdef USE_GLIBC_STDIO
#include <fcntl.h>
#endif
#include <limits.h>

#include "shell.h"
#include "syntax.h"
#include "output.h"
#include "memalloc.h"
#include "error.h"
#include "main.h"
#include "system.h"


#define OUTBUFSIZ BUFSIZ
#define MEM_OUT -3		/* output to dynamically allocated memory */


#ifdef USE_GLIBC_STDIO
struct output output = {
	stream: 0, nextc: 0, end: 0, buf: 0, bufsize: 0, fd: 1, flags: 0
};
struct output errout = {
	stream: 0, nextc: 0, end: 0, buf: 0, bufsize: 0, fd: 2, flags: 0
}
#ifdef notyet
struct output memout = {
	stream: 0, nextc: 0, end: 0, buf: 0, bufsize: 0, fd: MEM_OUT, flags: 0
};
#endif
#else
struct output output = {
	nextc: 0, end: 0, buf: 0, bufsize: OUTBUFSIZ, fd: 1, flags: 0
};
struct output errout = {
	nextc: 0, end: 0, buf: 0, bufsize: 0, fd: 2, flags: 0
};
struct output preverrout;
#ifdef notyet
struct output memout = {
	nextc: 0, end: 0, buf: 0, bufsize: 0, fd: MEM_OUT, flags: 0
};
#endif
#endif
struct output *out1 = &output;
struct output *out2 = &errout;


#ifndef USE_GLIBC_STDIO
static void __outstr(const char *, size_t, struct output *);
#endif
static int xvsnprintf(char *, size_t, const char *, va_list);


#ifdef mkinit

INCLUDE "output.h"
INCLUDE "memalloc.h"

INIT {
#ifdef USE_GLIBC_STDIO
	initstreams();
#endif
}

RESET {
#ifdef notyet
	out1 = &output;
	out2 = &errout;
#ifdef USE_GLIBC_STDIO
	if (memout.stream != NULL)
		__closememout();
#endif
	if (memout.buf != NULL) {
		ckfree(memout.buf);
		memout.buf = NULL;
	}
#endif
}

#endif


#ifndef USE_GLIBC_STDIO
static void
__outstr(const char *p, size_t len, struct output *dest)
{
	size_t bufsize;
	size_t offset;
	size_t nleft;

	nleft = dest->end - dest->nextc;
	if (nleft >= len) {
buffered:
		dest->nextc = mempcpy(dest->nextc, p, len);
		return;
	}

	bufsize = dest->bufsize;
	if (!bufsize) {
		;
	} else if (dest->buf == NULL) {
		if (dest->fd == MEM_OUT && len > bufsize) {
			bufsize = len;
		}
		offset = 0;
		goto alloc;
	} else if (dest->fd == MEM_OUT) {
		offset = bufsize;
		if (bufsize >= len) {
			bufsize <<= 1;
		} else {
			bufsize += len;
		}
		if (bufsize < offset)
			goto err;
alloc:
		INTOFF;
		dest->buf = ckrealloc(dest->buf, bufsize);
		dest->bufsize = bufsize;
		dest->end = dest->buf + bufsize;
		dest->nextc = dest->buf + offset;
		INTON;
	} else {
		flushout(dest);
	}

	nleft = dest->end - dest->nextc;
	if (nleft > len)
		goto buffered;

	if ((xwrite(dest->fd, p, len))) {
err:
		dest->flags |= OUTPUT_ERR;
	}
}
#endif


void
outstr(const char *p, struct output *file)
{
#ifdef USE_GLIBC_STDIO
	INTOFF;
	fputs(p, file->stream);
	INTON;
#else
	size_t len;

	len = strlen(p);
	__outstr(p, len, file);
#endif
}


#ifndef USE_GLIBC_STDIO


void
outcslow(int c, struct output *dest)
{
	char buf = c;
	__outstr(&buf, 1, dest);
}
#endif


void
flushall(void)
{
	flushout(&output);
#ifdef FLUSHERR
	flushout(&errout);
#endif
}


void
flushout(struct output *dest)
{
#ifdef USE_GLIBC_STDIO
	INTOFF;
	fflush(dest->stream);
	INTON;
#else
	size_t len;

	len = dest->nextc - dest->buf;
	if (!len || dest->fd < 0)
		return;
	dest->nextc = dest->buf;
	if ((xwrite(dest->fd, dest->buf, len)))
		dest->flags |= OUTPUT_ERR;
#endif
}


void
outfmt(struct output *file, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	doformat(file, fmt, ap);
	va_end(ap);
}


void
out1fmt(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	doformat(out1, fmt, ap);
	va_end(ap);
}


int
fmtstr(char *outbuf, size_t length, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = xvsnprintf(outbuf, length, fmt, ap);
	va_end(ap);
	return ret;
}


#ifndef USE_GLIBC_STDIO
void
doformat(struct output *dest, const char *f, va_list ap)
{
	struct stackmark smark;
	char *s;
	int len, ret;
	size_t size;
	va_list ap2;

	va_copy(ap2, ap);
	size = dest->end - dest->nextc;
	len = xvsnprintf(dest->nextc, size, f, ap2);
	va_end(ap2);
	if (len < 0) {
		dest->flags |= OUTPUT_ERR;
		return;
	}
	if (len < size) {
		dest->nextc += len;
		return;
	}
	setstackmark(&smark);
	s = stalloc((len >= stackblocksize() ? len : stackblocksize()) + 1);
	ret = xvsnprintf(s, len + 1, f, ap);
	if (ret == len)
		__outstr(s, len, dest);
	else
		dest->flags |= OUTPUT_ERR;
	popstackmark(&smark);
}
#endif



/*
 * Version of write which resumes after a signal is caught.
 */

int
xwrite(int fd, const void *p, size_t n)
{
	const char *buf = p;

	while (n) {
		ssize_t i;
		size_t m;

		m = n;
		if (m > SSIZE_MAX)
			m = SSIZE_MAX;
		do {
			i = write(fd, buf, m);
		} while (i < 0 && errno == EINTR);
		if (i < 0)
			return -1;
		buf += i;
		n -= i;
	}
	return 0;
}


#ifdef notyet
#ifdef USE_GLIBC_STDIO
void initstreams() {
	output.stream = stdout;
	errout.stream = stderr;
}


void
openmemout(void) {
	INTOFF;
	memout.stream = open_memstream(&memout.buf, &memout.bufsize);
	INTON;
}


int
__closememout(void) {
	int error;
	error = fclose(memout.stream);
	memout.stream = NULL;
	return error;
}
#endif
#endif

#ifdef __hpux
static int
xvsnprintf(char *outbuf, size_t length, const char *fmt, va_list ap)
{
	int ret;
	char* dummy = NULL;
	char* dummy_new = NULL;
	size_t dummy_len = 8;
	va_list ap_mine;

	if (length > 0) {
		INTOFF;
		va_copy(ap_mine, ap);
                errno = 0;
		ret = vsnprintf(outbuf, length, fmt, ap_mine);
		va_end(ap_mine);
		INTON;
	} else {
		ret = -1;
		errno = 0;
	}

	if (ret < 0 && errno == 0) {
		do {
			dummy_len *= 2;
			dummy_new = realloc(dummy, dummy_len);
			if (!dummy_new) {
				ret = -1;
				errno = ENOMEM;
				break;
			}
			dummy = dummy_new;
			INTOFF;
			va_copy(ap_mine, ap);
                        errno = 0;
			ret = vsnprintf(dummy, dummy_len, fmt, ap_mine);
			va_end(ap_mine);
			INTON;
		} while (ret < 0 && errno == 0);

		if (ret >= 0 && length) {
			memcpy(outbuf, dummy, length);
		}
		if (dummy) free(dummy);
	}

	return ret;
}

#else

static int
xvsnprintf(char *outbuf, size_t length, const char *fmt, va_list ap)
{
	int ret;

#ifdef __sun
	/*
	 * vsnprintf() on older versions of Solaris returns -1 when
	 * passed a length of 0.  To avoid this, use a dummy
	 * 1-character buffer instead.
	 */
	char dummy[1];

	if (length == 0) {
		outbuf = dummy;
		length = sizeof(dummy);
	}
#endif

	INTOFF;
	ret = vsnprintf(outbuf, length, fmt, ap);
	INTON;
	return ret;
}

#endif
