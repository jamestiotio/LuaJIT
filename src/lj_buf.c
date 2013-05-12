/*
** Buffer handling.
** Copyright (C) 2005-2013 Mike Pall. See Copyright Notice in luajit.h
*/

#include <stdio.h>

#define lj_buf_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"

LJ_NOINLINE void LJ_FASTCALL lj_buf_grow(SBuf *sb, char *en)
{
  lua_State *L = sbufL(sb);
  char *b = sbufB(sb);
  MSize sz = (MSize)(en - b);
  MSize osz = (MSize)(sbufE(sb) - b), nsz = osz;
  MSize n = (MSize)(sbufP(sb) - b);
  if (LJ_UNLIKELY(sz > LJ_MAX_MEM))
    lj_err_mem(L);
  if (nsz < LJ_MIN_SBUF) nsz = LJ_MIN_SBUF;
  while (nsz < sz) nsz += nsz;
  b = (char *)lj_mem_realloc(L, b, osz, nsz);
  setmref(sb->b, b);
  setmref(sb->p, b + n);
  setmref(sb->e, b + nsz);
}

char * LJ_FASTCALL lj_buf_tmp(lua_State *L, MSize sz)
{
  SBuf *sb = &G(L)->tmpbuf;
  setsbufL(sb, L);
  return lj_buf_need(sb, sz);
}

void LJ_FASTCALL lj_buf_shrink(lua_State *L, SBuf *sb)
{
  char *b = sbufB(sb);
  MSize osz = (MSize)(sbufE(sb) - b);
  if (osz > 2*LJ_MIN_SBUF) {
    MSize n = (MSize)(sbufP(sb) - b);
    b = lj_mem_realloc(L, b, osz, (osz >> 1));
    setmref(sb->b, b);
    setmref(sb->p, b + n);
    setmref(sb->e, b + (osz >> 1));
  }
}

char *lj_buf_wmem(char *p, const void *q, MSize len)
{
  const char *s = (const char *)q, *e = s + len;
  while (s < e) *p++ = *s++;
  return p;
}

SBuf * lj_buf_putmem(SBuf *sb, const void *q, MSize len)
{
  char *p = lj_buf_more(sb, len);
  p = lj_buf_wmem(p, q, len);
  setsbufP(sb, p);
  return sb;
}

#if LJ_HASJIT
SBuf * LJ_FASTCALL lj_buf_putstr(SBuf *sb, GCstr *s)
{
  MSize len = s->len;
  char *p = lj_buf_more(sb, len);
  p = lj_buf_wmem(p, strdata(s), len);
  setsbufP(sb, p);
  return sb;
}

SBuf * LJ_FASTCALL lj_buf_putchar(SBuf *sb, int c)
{
  char *p = lj_buf_more(sb, 1);
  *p++ = (char)c;
  setsbufP(sb, p);
  return sb;
}

SBuf * LJ_FASTCALL lj_buf_putint(SBuf *sb, int32_t k)
{
  setsbufP(sb, lj_str_bufint(lj_buf_more(sb, LJ_STR_INTBUF), k));
  return sb;
}

SBuf * LJ_FASTCALL lj_buf_putnum(SBuf *sb, cTValue *o)
{
  setsbufP(sb, lj_str_bufnum(lj_buf_more(sb, LJ_STR_NUMBUF), o));
  return sb;
}
#endif

SBuf * LJ_FASTCALL lj_buf_putstr_reverse(SBuf *sb, GCstr *s)
{
  MSize len = s->len;
  char *p = lj_buf_more(sb, len), *e = p+len;
  const char *q = strdata(s)+len-1;
  while (p < e)
    *p++ = *q--;
  setsbufP(sb, p);
  return sb;
}

SBuf * LJ_FASTCALL lj_buf_putstr_lower(SBuf *sb, GCstr *s)
{
  MSize len = s->len;
  char *p = lj_buf_more(sb, len), *e = p+len;
  const char *q = strdata(s);
  for (; p < e; p++, q++) {
    uint32_t c = *(unsigned char *)q;
#if LJ_TARGET_PPC
    *p = c + ((c >= 'A' && c <= 'Z') << 5);
#else
    if (c >= 'A' && c <= 'Z') c += 0x20;
    *p = c;
#endif
  }
  setsbufP(sb, p);
  return sb;
}

SBuf * LJ_FASTCALL lj_buf_putstr_upper(SBuf *sb, GCstr *s)
{
  MSize len = s->len;
  char *p = lj_buf_more(sb, len), *e = p+len;
  const char *q = strdata(s);
  for (; p < e; p++, q++) {
    uint32_t c = *(unsigned char *)q;
#if LJ_TARGET_PPC
    *p = c - ((c >= 'a' && c <= 'z') << 5);
#else
    if (c >= 'a' && c <= 'z') c -= 0x20;
    *p = c;
#endif
  }
  setsbufP(sb, p);
  return sb;
}

SBuf *lj_buf_putstr_rep(SBuf *sb, GCstr *s, int32_t rep)
{
  MSize len = s->len;
  if (rep > 0 && len) {
    uint64_t tlen = (uint64_t)rep * len;
    char *p;
    if (LJ_UNLIKELY(tlen > LJ_MAX_STR))
      lj_err_mem(sbufL(sb));
    p = lj_buf_more(sb, (MSize)tlen);
    if (len == 1) {  /* Optimize a common case. */
      uint32_t c = strdata(s)[0];
      do { *p++ = c; } while (--rep > 0);
    } else {
      const char *e = strdata(s) + len;
      do {
	const char *q = strdata(s);
	do { *p++ = *q++; } while (q < e);
      } while (--rep > 0);
    }
    setsbufP(sb, p);
  }
  return sb;
}

SBuf *lj_buf_puttab(SBuf *sb, GCtab *t, GCstr *sep, int32_t i, int32_t e)
{
  MSize seplen = sep ? sep->len : 0;
  if (i <= e) {
    for (;;) {
      cTValue *o = lj_tab_getint(t, i);
      char *p;
      if (!o) {
      badtype:  /* Error: bad element type. */
	setsbufP(sb, (intptr_t)i);  /* Store failing index. */
	return NULL;
      } else if (tvisstr(o)) {
	MSize len = strV(o)->len;
	p = lj_buf_wmem(lj_buf_more(sb, len + seplen), strVdata(o), len);
      } else if (tvisint(o)) {
	p = lj_str_bufint(lj_buf_more(sb, LJ_STR_INTBUF + seplen), intV(o));
      } else if (tvisnum(o)) {
	p = lj_str_bufnum(lj_buf_more(sb, LJ_STR_NUMBUF + seplen), o);
      } else {
	goto badtype;
      }
      if (i++ == e) {
	setsbufP(sb, p);
	break;
      }
      if (seplen) p = lj_buf_wmem(p, strdata(sep), seplen);
      setsbufP(sb, p);
    }
  }
  return sb;
}

GCstr * LJ_FASTCALL lj_buf_tostr(SBuf *sb)
{
  return lj_str_new(sbufL(sb), sbufB(sb), sbuflen(sb));
}

GCstr *lj_buf_cat2str(lua_State *L, GCstr *s1, GCstr *s2)
{
  MSize len1 = s1->len, len2 = s2->len;
  char *buf = lj_buf_tmp(L, len1 + len2);
  memcpy(buf, strdata(s1), len1);
  memcpy(buf+len1, strdata(s2), len2);
  return lj_str_new(L, buf, len1 + len2);
}

uint32_t LJ_FASTCALL lj_buf_ruleb128(const char **pp)
{
  const uint8_t *p = (const uint8_t *)*pp;
  uint32_t v = *p++;
  if (LJ_UNLIKELY(v >= 0x80)) {
    int sh = 0;
    v &= 0x7f;
    do { v |= ((*p & 0x7f) << (sh += 7)); } while (*p++ >= 0x80);
  }
  *pp = (const char *)p;
  return v;
}

char * LJ_FASTCALL lj_buf_wuleb128(char *p, uint32_t v)
{
  for (; v >= 0x80; v >>= 7)
    *p++ = (char)((v & 0x7f) | 0x80);
  *p++ = (char)v;
  return p;
}
