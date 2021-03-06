/* MIT License
 *
 * Copyright (c) 2018 Sebastian Katzer, appPlant GmbH
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "mruby.h"
#include "mruby/ext/fnmatch.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Copy string src to buffer dst of size dsize.  At most dsize-1
 * chars will be copied.  Always NUL terminates (unless dsize == 0).
 * Returns strlen(src); if retval >= dsize, truncation occurred.
 */
static size_t
_strlcpy_(char *dst, const char *src, size_t dsize)
{
    const char *osrc = src;
    size_t nleft = dsize;

    /* Copy as many bytes as will fit. */
    if (nleft != 0) {
        while (--nleft != 0) {
            if ((*dst++ = *src++) == '\0')
                break;
        }
    }

    /* Not enough room in dst, add NUL and traverse rest of src. */
    if (nleft == 0) {
        if (dsize != 0)
            *dst = '\0';        /* NUL-terminate dst */
        while (*src++)
            ;
    }

    return(src - osrc - 1); /* count does not include NUL */
}

#define downcase(c) (nocase && isupper(c) ? tolower(c) : (c))
#define compare(c1, c2) (((unsigned char)(c1)) - ((unsigned char)(c2)))
#define Next(p) ((p) + 1)
#define Inc(p) (++(p))
#define Compare(p1, p2) (compare(downcase(*(p1)), downcase(*(p2))))

static char *
bracket(const char* p, const char* s, int flags)
{
    const int nocase =   flags & MRB_FNM_CASEFOLD;
    const int escape = !(flags & MRB_FNM_NOESCAPE);

    int ok = 0, nope = 0;

    if (*p == '!' || *p == '^') {
        nope = 1;
        p++;
    }

    while (*p != ']') {
        const char *t1 = p;
        if (escape && *t1 == '\\') t1++;
        if (!*t1) return NULL;

        p = Next(t1);
        if (p[0] == '-' && p[1] != ']') {
            const char *t2 = p + 1;
            if (escape && *t2 == '\\') t2++;
            if (!*t2) return NULL;

            p = Next(t2);
            if (!ok && Compare(t1, s) <= 0 && Compare(s, t2) <= 0) ok = 1;
        } else {
            if (!ok && Compare(t1, s) == 0) ok = 1;
        }
    }

    return ok == nope ? NULL : (char *)p + 1;
}

int
mrb_file_fnmatch_ex(const char* p, const char* path, int flags)
{
    const int escape   = !(flags & MRB_FNM_NOESCAPE);
    const char *s      = p;
    const char *lbrace = 0, *rbrace = 0;
    int nest           = 0, status = 0;

    while (*p) {
        if (*p == '{' && nest++ == 0) {
            lbrace = p;
        }
        if (*p == '}' && lbrace && --nest == 0) {
            rbrace = p;
            break;
        }
        if (*p == '\\' && escape) {
            if (!*++p) break;
        }
        Inc(p);
    }

    if (lbrace && rbrace) {
        size_t len = strlen(s) + 1;
        char *buf = malloc(sizeof(char) * len);
        long shift;

        if (!buf) return -1;
        memcpy(buf, s, lbrace-s);
        shift = (lbrace-s);
        p = lbrace;
        while (p < rbrace) {
            const char *t = ++p;
            nest = 0;
            while (p < rbrace && !(*p == ',' && nest == 0)) {
                if (*p == '{') nest++;
                if (*p == '}') nest--;
                if (*p == '\\' && escape) {
                    if (++p == rbrace) break;
                }
                Inc(p);
            }
            memcpy(buf+shift, t, p-t);
            _strlcpy_(buf+shift+(p-t), rbrace+1, len-(shift+(p-t)));
            status = mrb_file_fnmatch(buf, path, flags) == 0;
            if (status) break;
        }

        free(buf);
    }
    else if (!lbrace && !rbrace) {
        status = mrb_file_fnmatch(s, path, flags) == 0;
    }

    return status;
}

#define UNESCAPE(p) (escape && *(p) == '\\' ? (p) + 1 : (p))
#define ISEND(p) (!*(p) || (pathname && *(p) == '/'))
#define RETURN(val) return *pcur = p, *scur = s, (val);

static int
fnmatch_helper(const char** pcur, const char** scur, int flags)
{
    const int period   = !(flags & MRB_FNM_DOTMATCH);
    const int pathname =   flags & MRB_FNM_PATHNAME;
    const int escape   = !(flags & MRB_FNM_NOESCAPE);
    const int nocase   =   flags & MRB_FNM_CASEFOLD;

    const char *ptmp = 0;
    const char *stmp = 0;

    const char *p = *pcur;
    const char *s = *scur;

    if (period && *s == '.' && *UNESCAPE(p) != '.') /* leading period */
        RETURN(MRB_FNM_NOMATCH);

    while (1) {
        switch (*p) {
            case '*':
                do { p++; } while (*p == '*');
                if (ISEND(UNESCAPE(p))) {
                    p = UNESCAPE(p);
                    RETURN(0);
                }
                if (ISEND(s)) RETURN(MRB_FNM_NOMATCH);

                ptmp = p;
                stmp = s;
                continue;

            case '?':
                if (ISEND(s)) RETURN(MRB_FNM_NOMATCH);
                p++;
                Inc(s);
                continue;

            case '[': {
                const char *t;
                if (ISEND(s)) RETURN(MRB_FNM_NOMATCH);
                if ((t = bracket(p + 1, s, flags)) != 0) {
                    p = t;
                    Inc(s);
                    continue;
                }
                goto failed;
            }
        }

        /* ordinary */
        p = UNESCAPE(p);
        if (ISEND(s)) RETURN(ISEND(p) ? 0 : MRB_FNM_NOMATCH);
        if (ISEND(p)) goto failed;
        if (Compare(p, s) != 0) goto failed;

        Inc(p);
        Inc(s);
        continue;

    failed: /* try next '*' position */
        if(ptmp && stmp) {
            p = ptmp;
            Inc(stmp); /* !ISEND(*stmp) */
            s = stmp;
            continue;
        }
        RETURN(MRB_FNM_NOMATCH);
    }
}

int
mrb_file_fnmatch(const char* p, const char* s, int flags)
{
    const int period   = !(flags & MRB_FNM_DOTMATCH);
    const int pathname =   flags & MRB_FNM_PATHNAME;

    const char *ptmp = 0;
    const char *stmp = 0;

    if (pathname) {
        while (1) {
            if (p[0] == '*' && p[1] == '*' && p[2] == '/') {
                do { p += 3; } while (p[0] == '*' && p[1] == '*' && p[2] == '/');
                ptmp = p;
                stmp = s;
            }
            if (fnmatch_helper(&p, &s, flags) == 0) {
                while (*s && *s != '/') Inc(s);
                if (*p && *s) {
                    p++;
                    s++;
                    continue;
                }
                if (!*p && !*s)
                    return 0;
            }
            /* failed : try next recursion */
            if (ptmp && stmp && !(period && *stmp == '.')) {
                while (*stmp && *stmp != '/') Inc(stmp);
                if (*stmp) {
                    p = ptmp;
                    stmp++;
                    s = stmp;
                    continue;
                }
            }
            return MRB_FNM_NOMATCH;
        }
    } else {
        return fnmatch_helper(&p, &s, flags);
    }
}

static mrb_value
mrb_f_fnmatch(mrb_state *mrb, mrb_value klass)
{
    const char *pattern, *path;
    mrb_int flags = 0;
    mrb_bool res  = FALSE;

    mrb_get_args(mrb, "zz|i", &pattern, &path, &flags);

    if (flags & MRB_FNM_EXTGLOB) {
        res = mrb_file_fnmatch_ex(pattern, path, flags) > 0;
    } else {
        res = mrb_file_fnmatch(pattern, path, flags) == 0;
    }

    return mrb_bool_value(res);
}

void
mrb_mruby_file_fnmatch_gem_init (mrb_state *mrb)
{
    struct RClass *cls;

    if (mrb_class_defined(mrb, "File")) {
        cls = mrb_class_get(mrb, "File");
    } else {
        cls = mrb_define_class(mrb, "File", mrb->object_class);
    }

    mrb_define_class_method(mrb, cls, "fnmatch",  mrb_f_fnmatch, MRB_ARGS_ARG(2,1));
    mrb_define_class_method(mrb, cls, "fnmatch?", mrb_f_fnmatch, MRB_ARGS_ARG(2,1));

    mrb_define_const(mrb, cls, "FNM_NOESCAPE", mrb_fixnum_value(MRB_FNM_NOESCAPE));
    mrb_define_const(mrb, cls, "FNM_PATHNAME", mrb_fixnum_value(MRB_FNM_PATHNAME));
    mrb_define_const(mrb, cls, "FNM_DOTMATCH", mrb_fixnum_value(MRB_FNM_DOTMATCH));
    mrb_define_const(mrb, cls, "FNM_CASEFOLD", mrb_fixnum_value(MRB_FNM_CASEFOLD));
    mrb_define_const(mrb, cls, "FNM_EXTGLOB",  mrb_fixnum_value(MRB_FNM_EXTGLOB));
}

void
mrb_mruby_file_fnmatch_gem_final (mrb_state *mrb)
{

}
