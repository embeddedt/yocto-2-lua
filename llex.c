/*
** $Id: llex.c,v 1.125 2003/09/04 20:00:28 roberto Exp roberto $
** Lexical Analyzer
** See Copyright Notice in lua.h
*/


#include <ctype.h>
#include <string.h>

#define llex_c

#include "lua.h"

#include "ldo.h"
#include "llex.h"
#include "lobject.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "lzio.h"



#define next(ls) (ls->current = zgetc(ls->z))


#define MINLEXBUF	32

#define save(ls,c)  { \
  Mbuffer *b = ls->buff; \
  if (b->n + 1 > b->buffsize) \
    luaZ_resizebuffer(ls->L, b, ((b->buffsize*2) + MINLEXBUF)); \
  b->buffer[b->n++] = cast(char, c); }



#define currIsNewline(ls)	(ls->current == '\n' || ls->current == '\r')


/* ORDER RESERVED */
static const char *const token2string [] = {
    "and", "break", "do", "else", "elseif",
    "end", "false", "for", "function", "if",
    "in", "local", "nil", "not", "or", "repeat",
    "return", "then", "true", "until", "while", "*name",
    "..", "...", "==", ">=", "<=", "~=",
    "*number", "*string", "<eof>"
};


void luaX_init (lua_State *L) {
  int i;
  for (i=0; i<NUM_RESERVED; i++) {
    TString *ts = luaS_new(L, token2string[i]);
    luaS_fix(ts);  /* reserved words are never collected */
    lua_assert(strlen(token2string[i])+1 <= TOKEN_LEN);
    ts->tsv.reserved = cast(lu_byte, i+1);  /* reserved word */
  }
}


#define MAXSRC          80


void luaX_checklimit (LexState *ls, int val, int limit, const char *msg) {
  if (val > limit) {
    msg = luaO_pushfstring(ls->L, "too many %s (limit=%d)", msg, limit);
    luaX_syntaxerror(ls, msg);
  }
}


void luaX_errorline (LexState *ls, const char *s, const char *token, int line) {
  lua_State *L = ls->L;
  char buff[MAXSRC];
  luaO_chunkid(buff, getstr(ls->source), MAXSRC);
  luaO_pushfstring(L, "%s:%d: %s near `%s'", buff, line, s, token); 
  luaD_throw(L, LUA_ERRSYNTAX);
}


static void luaX_error (LexState *ls, const char *s, const char *token) {
  luaX_errorline(ls, s, token, ls->linenumber);
}


const char *luaX_token2str (LexState *ls, int token) {
  if (token < FIRST_RESERVED) {
    lua_assert(token == (unsigned char)token);
    return (iscntrl(token)) ? luaO_pushfstring(ls->L, "char(%d)", token) :
                              luaO_pushfstring(ls->L, "%c", token);
  }
  else
    return token2string[token-FIRST_RESERVED];
}


static const char *txtToken (LexState *ls, int token) {
  switch (token) {
    case TK_NAME:
    case TK_STRING:
    case TK_NUMBER:
      save(ls, '\0');
      return luaZ_buffer(ls->buff);
    default:
      return luaX_token2str(ls, token);
  }
}


static void luaX_lexerror (LexState *ls, const char *msg, int token) {
    luaX_error(ls, msg, txtToken(ls, token));
}


void luaX_syntaxerror (LexState *ls, const char *msg) {
  luaX_lexerror(ls, msg, ls->t.token);
}


TString *luaX_newstring (LexState *ls, const char *str, size_t l) {
  lua_State *L = ls->L;
  TString *ts = luaS_newlstr(L, str, l);
  TObject *o = luaH_setstr(L, ls->fs->h, ts);  /* entry for `str' */
  if (ttisnil(o))
    setbvalue(o, 1);  /* make sure `str' will not be collected */
  return ts;
}


static void inclinenumber (LexState *ls) {
  int old = ls->current;
  lua_assert(currIsNewline(ls));
  next(ls);  /* skip `\n' or `\r' */
  if (currIsNewline(ls) && ls->current != old)
    next(ls);  /* skip `\n\r' or `\r\n' */
  ++ls->linenumber;
  luaX_checklimit(ls, ls->linenumber, MAX_INT, "lines in a chunk");
}


void luaX_setinput (lua_State *L, LexState *ls, ZIO *z, TString *source) {
  ls->L = L;
  ls->lookahead.token = TK_EOS;  /* no look-ahead token */
  ls->z = z;
  ls->fs = NULL;
  ls->linenumber = 1;
  ls->lastline = 1;
  ls->source = source;
  next(ls);  /* read first char */
}



/*
** =======================================================
** LEXICAL ANALYZER
** =======================================================
*/



static void save_and_next (LexState *ls) {
  save(ls, ls->current);
  next(ls);
}



/* LUA_NUMBER */
static void read_numeral (LexState *ls, SemInfo *seminfo) {
  while (isdigit(ls->current)) {
    save_and_next(ls);
  }
  if (ls->current == '.') {
    save_and_next(ls);
    if (ls->current == '.') {
      save_and_next(ls);
      luaX_lexerror(ls,
                 "ambiguous syntax (decimal point x string concatenation)",
                 TK_NUMBER);
    }
  }
  while (isdigit(ls->current)) {
    save_and_next(ls);
  }
  if (ls->current == 'e' || ls->current == 'E') {
    save_and_next(ls);  /* read `E' */
    if (ls->current == '+' || ls->current == '-')
      save_and_next(ls);  /* optional exponent sign */
    while (isdigit(ls->current)) {
      save_and_next(ls);
    }
  }
  save(ls, '\0');
  if (!luaO_str2d(luaZ_buffer(ls->buff), &seminfo->r))
    luaX_lexerror(ls, "malformed number", TK_NUMBER);
}


static int skip_ast (LexState *ls) {
  int count = 0;
  int s = ls->current;
  lua_assert(s == '[' || s == ']');
  save_and_next(ls);
  while (ls->current == '*') {
    save_and_next(ls);
    count++;
  }
  return (ls->current == s) ? count : (-count) - 1;
}


static void read_long_string (LexState *ls, SemInfo *seminfo, int ast) {
  int cont = 0;
  save_and_next(ls);  /* skip 2nd `[' */
  if (currIsNewline(ls))  /* string starts with a newline? */
    inclinenumber(ls);  /* skip it */
  for (;;) {
    switch (ls->current) {
      case EOZ:
        luaX_lexerror(ls, (seminfo) ? "unfinished long string" :
                                   "unfinished long comment", TK_EOS);
        break;  /* to avoid warnings */
      case '[':
        if (skip_ast(ls) == ast) {
          save_and_next(ls);  /* skip 2nd `[' */
          cont++;
        }
        continue;
      case ']':
        if (skip_ast(ls) == ast) {
          save_and_next(ls);  /* skip 2nd `]' */
          if (cont-- == 0) goto endloop;
        }
        continue;
      case '\n':
      case '\r':
        save(ls, '\n');
        inclinenumber(ls);
        if (!seminfo) luaZ_resetbuffer(ls->buff);  /* avoid wasting space */
        continue;
      default:
        if (seminfo) save_and_next(ls);
        else next(ls);
    }
  } endloop:
  if (seminfo)
    seminfo->ts = luaX_newstring(ls, luaZ_buffer(ls->buff) + (2 + ast),
                                     luaZ_bufflen(ls->buff) - 2*(2 + ast));
}


static void read_string (LexState *ls, int del, SemInfo *seminfo) {
  save_and_next(ls);
  while (ls->current != del) {
    switch (ls->current) {
      case EOZ:
        luaX_lexerror(ls, "unfinished string", TK_EOS);
        continue;  /* to avoid warnings */
      case '\n':
      case '\r':
        luaX_lexerror(ls, "unfinished string", TK_STRING);
        continue;  /* to avoid warnings */
      case '\\': {
        int c;
        next(ls);  /* do not save the `\' */
        switch (ls->current) {
          case 'a': c = '\a'; break;
          case 'b': c = '\b'; break;
          case 'f': c = '\f'; break;
          case 'n': c = '\n'; break;
          case 'r': c = '\r'; break;
          case 't': c = '\t'; break;
          case 'v': c = '\v'; break;
          case '\n':  /* go through */
          case '\r': save(ls, '\n'); inclinenumber(ls); continue;
          case EOZ: continue;  /* will raise an error next loop */
          default: {
            if (!isdigit(ls->current))
              save_and_next(ls);  /* handles \\, \", \', and \? */
            else {  /* \xxx */
              int i = 0;
              c = 0;
              do {
                c = 10*c + (ls->current-'0');
                next(ls);
              } while (++i<3 && isdigit(ls->current));
              if (c > UCHAR_MAX)
                luaX_lexerror(ls, "escape sequence too large", TK_STRING);
              save(ls, c);
            }
            continue;
          }
        }
        save(ls, c);
        next(ls);
        continue;
      }
      default:
        save_and_next(ls);
    }
  }
  save_and_next(ls);  /* skip delimiter */
  seminfo->ts = luaX_newstring(ls, luaZ_buffer(ls->buff) + 1,
                                   luaZ_bufflen(ls->buff) - 2);
}


int luaX_lex (LexState *ls, SemInfo *seminfo) {
  luaZ_resetbuffer(ls->buff);
  for (;;) {
    switch (ls->current) {
      case '\n':
      case '\r': {
        inclinenumber(ls);
        continue;
      }
      case '-': {
        next(ls);
        if (ls->current != '-') return '-';
        /* else is a comment */
        next(ls);
        if (ls->current == '[') {
          int ast = skip_ast(ls);
          luaZ_resetbuffer(ls->buff);  /* `skip_ast' may dirty the buffer */
          if (ast >= 0) {
            read_long_string(ls, NULL, ast);  /* long comment */
            luaZ_resetbuffer(ls->buff);
            continue;
          }
        }
        /* else short comment */
        while (!currIsNewline(ls) && ls->current != EOZ)
          next(ls);
        continue;
      }
      case '[': {
        int ast = skip_ast(ls);
        if (ast >= 0) {
          read_long_string(ls, seminfo, ast);
          return TK_STRING;
        }
        else if (ast == -1) return '[';
        else luaX_lexerror(ls, "invalid long string delimiter", TK_STRING);
      }
      case '=': {
        next(ls);
        if (ls->current != '=') return '=';
        else { next(ls); return TK_EQ; }
      }
      case '<': {
        next(ls);
        if (ls->current != '=') return '<';
        else { next(ls); return TK_LE; }
      }
      case '>': {
        next(ls);
        if (ls->current != '=') return '>';
        else { next(ls); return TK_GE; }
      }
      case '~': {
        next(ls);
        if (ls->current != '=') return '~';
        else { next(ls); return TK_NE; }
      }
      case '"':
      case '\'': {
        read_string(ls, ls->current, seminfo);
        return TK_STRING;
      }
      case '.': {
        save_and_next(ls);
        if (ls->current == '.') {
          next(ls);
          if (ls->current == '.') {
            next(ls);
            return TK_DOTS;   /* ... */
          }
          else return TK_CONCAT;   /* .. */
        }
        else if (!isdigit(ls->current)) return '.';
        else {
          read_numeral(ls, seminfo);
          return TK_NUMBER;
        }
      }
      case EOZ: {
        return TK_EOS;
      }
      default: {
        if (isspace(ls->current)) {
          lua_assert(!currIsNewline(ls));
          next(ls);
          continue;
        }
        else if (isdigit(ls->current)) {
          read_numeral(ls, seminfo);
          return TK_NUMBER;
        }
        else if (isalpha(ls->current) || ls->current == '_') {
          /* identifier or reserved word */
          TString *ts;
          do {
            save_and_next(ls);
          } while (isalnum(ls->current) || ls->current == '_');
          ts = luaX_newstring(ls, luaZ_buffer(ls->buff),
                                  luaZ_bufflen(ls->buff));
          if (ts->tsv.reserved > 0)  /* reserved word? */
            return ts->tsv.reserved - 1 + FIRST_RESERVED;
          else {
            seminfo->ts = ts;
            return TK_NAME;
          }
        }
        else {
          int c = ls->current;
          next(ls);
          return c;  /* single-char tokens (+ - / ...) */
        }
      }
    }
  }
}

#undef next
