// (Readline is GPLed, so that makes this file GPLed too, because this
// file will only run if it's linked against readline.)
//
// Copyright (c) 2002 Tony Garnock-Jones
// Copyright (c) 2006 Heath Johns (paren bouncing and auto-completion code)
// Copyright (c) 2014 Alexej Magura (added a lot of history functions and more)
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/poll.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <stdbool.h>
#include <errno.h>
#include <alloca.h>
#include <stdarg.h>
#include <limits.h>

/* **** BEGIN ****
 * Macros *
 */
#define RL_EGG_SYNC_SCM_HIST			\
     do {					\
	  previous_history();			\
     } while(0);

/* *** BEGIN ***
 * Conditional Macros *
 */
#ifndef INLINE
#if defined __GNUC__
#define INLINE __inline__
#else
#define INLINE inline
#endif
#endif

#ifndef __GNUC__
#ifndef __attribute__
#define __attribute__(x)
#endif
#endif

#ifndef DEBUG
#define DEBUG 0
#endif

#if DEBUG /* NOTE change this to 1 to enable debug messages */
#define RL_EGG_STACK(format, ...)		\
  do {						\
    RL_EGG_BEGIN_TRACE;				\
    RL_EGG_DEBUG((format), __VA_ARGS__);	\
    RL_EGG_END_TRACE;				\
  } while(0)

#define RL_EGG_BEGIN_TRACE fprintf(stderr, "++ %s{%d}:\n\n", __FILE__, __LINE__)
#define RL_EGG_END_TRACE fprintf(stderr, "\n-- %s %s `%s'\n\n", "In", "function", __FUNCTION__)
#define RL_EGG_DEBUG(format, ...)		\
  do {						\
    fprintf(stderr, "%s", "\e[36m");		\
    fprintf(stderr, (format), __VA_ARGS__);	\
    fprintf(stderr, "%s", "\e[0m");		\
  } while(0)
#else
#define RL_EGG_STACK(format, ...)
#define RL_EGG_BEGIN_TRACE
#define RL_EGG_END_TRACE
#define RL_EGG_DEBUG(format, ...)
#endif
/* *** END ***
   Macros
 */

static int gnu_readline_bounce_ms = 500;
static int gnu_history_newlines = 0;
static char *gnu_readline_buf = NULL;
static bool rl_egg_clear = false;

struct balance {
  int paren[3]; // 0 -> total, 1 -> open, 2 -> close
  int brace[3];
  int quote;
} balnc;

#if 0 // NOT YET IMPLEMENTED
static struct gnu_readline_current_paren_color_t {
  int enabled;
  char *with_match;
  char *without_match;
} paren_colors;
#endif

/* XXX readline already provides paren-bouncing:
 *  (gnu-readline-parse-and-bind "set blink-matching-paren on")
 *
 *  XXX it however does ____NOT____ work.  built-in: (line 'a) ')
 *                                                   ^ -------- ^ ; WRONG
 *  ~ Alexej
 */

char *memsafe_concat(const char *s1, ...) __attribute__ ((__sentinel__));

char *memsafe_concat(const char *s1, ...)
{
  va_list args;
  const char *s;
  char *p, *result;
  unsigned long l, m, n;

  m = n = strlen(s1);
  va_start(args, s1);
  while ((s = va_arg(args, char *))) {
    l = strlen(s);
    if ((m += l) < l) break;
  }
  va_end(args);
  if (s || m >= INT_MAX) return NULL;

  result = (char *)malloc(m + 1);
  if (!result) return NULL;

  memcpy(p = result, s1, n);
  p += n;
  va_start(args, s1);
  while ((s = va_arg(args, char *))) {
    l = strlen(s);
    if ((n += l) < l || n > m) break;
    memcpy(p, s, l);
    p += l;
  }
  va_end(args);
  if (s || m != n || p != result + n) {
    free(result);
    return NULL;
  }

  *p = 0;
  return result;
}

char *concat (const char *s1, ...)
{
     va_list ap;
     size_t allocated = 8;
     char *result = (char *) malloc (allocated);

     if (result != NULL) {
	  char *newp;
	  char *tmp;
	  const char *s;

	  va_start (ap, s1);

	  tmp = result;
	  for (s = s1; s != NULL; s = va_arg (ap, const char *)) {
	       size_t len = strlen (s);

	       /* `s' is too big to fit into `result', resize `result' */
	       if (tmp + len + 1 > result + allocated) {
		    allocated = (allocated + len) * 2;
		    newp = (char *) realloc (result, allocated);
		    if (newp == NULL) {
			 free (result);
			 return NULL;
		    }
		    tmp = newp + (tmp - result);
		    result = tmp;
	       }
	       tmp = mempcpy (tmp, s, len);
	  }

	  *tmp++ = '\0';

	  newp = realloc (result, tmp - result);
	  if (newp != NULL)
	       result = newp;

	  va_end (ap);
     }
     return result;
}

INLINE char peek_chr(char *cp, char *stringp, bool direct)
{
    if (direct) {
    char next = '\0';
    if (cp != &stringp[strlen(stringp) - 1]) {
      next = *(++cp);
      --cp;
    } else {
      next = *cp;
    }
    return next;
  } else {
    char prev = '\0';
    if (cp != stringp) {
      prev = *(--cp);
      ++cp;
    } else {
      prev = *cp;
    }
    return prev;
  }
}

INLINE int strnof_delim(char *str, char odelim, char cdelim, int count[2])
{
  memset(count, 0, sizeof(*count)*2);
  RL_EGG_BEGIN_TRACE;
  RL_EGG_DEBUG("str: %s\n", str);
  char *cp = strchr(str, '\0');
  RL_EGG_END_TRACE;

  if (cp == str)
    return 1;

  do {
    if (cp != str && peek_chr(cp, str, false) == '\\')
      continue;

    if (*cp == cdelim) {
      ++count[1]; // increment close
    } else if (*cp == odelim) {
      ++count[0]; // increment open
    }
  } while(cp-- != str);

  if (odelim == cdelim && count[1] > 0) {
    if (count[1] % 2 == 1)
      while(count[0]++ < --count[1]);
    else {
      count[0] = count[1] * 0.5;
      count[1] *= 0.5;
    }
  }

  RL_EGG_BEGIN_TRACE;
  RL_EGG_DEBUG("open: %d\nclose: %d\n", count[0], count[1]);
  RL_EGG_END_TRACE;
  return 0;
}

INLINE char *str_unquotd(char *str)
{
  //RL_EGG_BEGIN_TRACE;
  //RL_EGG_DEBUG("str: `%s'\n", str);
  //RL_EGG_END_TRACE;

  char *buf = "";
  char *nbuf = "";
  int count[2];
  strnof_delim(str, '"', '"', count);
  //RL_EGG_DEBUG("idx[0]: %d\n", count[0]);
  //RL_EGG_DEBUG("idx[1]: %d\n", count[1]);

  if (count[0] == 0)
    return str;
  int even = count[0] - abs(count[0] - count[1]);
  char *token, *tmp, *rest;
  bool need_free = true;
  token = NULL;
  rest = NULL;
  tmp = strdupa(str);

  if (tmp == NULL) {
#if DEBUG
    perror(__FUNCTION__);
#endif
    return NULL;
  }

  token = strtok_r(tmp, "\"", &rest);
  if (token == NULL)
    return str;
  //RL_EGG_DEBUG("token: %s\n", token);
  buf = memsafe_concat(buf, token, NULL);

  while ((token = strtok_r(NULL, "\"", &rest)) != NULL) {
    //RL_EGG_DEBUG("token (while): %s\n", token);
    //RL_EGG_DEBUG("even (while): %d\n", even);
    if (even % 2 == 1) {
      if (need_free) {
	nbuf = memsafe_concat(buf, token, NULL);
	free(buf);
	need_free = false;
      } else {
	buf = memsafe_concat(nbuf, token, NULL);
	free(nbuf);
	need_free = true;
      }
      --even;
    } else {
      ++even;
    }
  }
  return buf;
}

INLINE void clear_parbar(char token)
{
  int idx = 0;
  for (; idx < 3; ++idx) {
    if (token == '(' || token == ')')
      balnc.paren[idx] = 0;
    if (token == '[' || token == ']')
      balnc.brace[idx] = 0;
  }
}

INLINE int parbar_in_string(char *str, char add_token)
{
  int *idxp = NULL;
  char sub_token = '\0';
  if (add_token == '(') {
    idxp = balnc.paren;
    sub_token = ')';
  } else if (add_token == '[') {
    idxp = balnc.brace;
    sub_token = ']';
  }

  char *cp = strchr(str, '\0');

  if (cp == str)
    return 0;

  do {
    if (cp != str && peek_chr(cp, str, false) == '\\')
      continue;

    RL_EGG_DEBUG("balnc.quote: %d\n", balnc.quote);
    if (*cp == add_token && balnc.quote < 1) {
      ++(*idxp); // increment total
      ++(*(++idxp)); // move to open, and increment
      --idxp; // move back to total
    } else if (*cp == sub_token && balnc.quote < 1) {
      --(*idxp); // deincrement total
      ++idxp; // move to open
      ++(*(++idxp)); // move to close and increment
      --idxp; // move back to open
      --idxp; // move back to total
    }
  } while (cp-- != str);
  return *idxp;
}

INLINE int quote_in_string(char *str)
{
  if (str == NULL)
    return 0;

  char *cp = strchr(str, '\0');

  if (cp == str)
    return 0;

  do {
    if (cp != str && peek_chr(cp, str, false) == '\\')
      continue;

    if (*cp == '"') {
      ++balnc.quote;
    }
  } while (cp-- != str);

  if (balnc.quote == 0)
    return -1;
  RL_EGG_DEBUG("return balance.quote: %d\n", balnc.quote);
  return balnc.quote % 2;
}

#if 0 // NOT YET IMPLEMENTED
int
highlight_paren()
{
  char *rl_ptr = rl_line_buffer;
  while (rl_ptr != &rl_line_buffer[rl_point]) { ++rl_ptr; }
  RL_EGG_DEBUG("%s\n", rl_ptr);
  return 0;
}
#endif

////\\\\//// Paren Bouncing ////\\\\////

/* Returns: (if positive) the position of the matching paren
            (if negative) the number of unmatched closing parens */
int gnu_readline_skip(int pos, char open_key, char close_key)
{
  while (--pos > -1) {
    if (pos > 0 && rl_line_buffer[pos - 1] == '\\')
      continue;
    else if (rl_line_buffer[pos] == open_key)
      return pos;
    else if (rl_line_buffer[pos] == close_key)
      pos = gnu_readline_skip(pos, open_key, close_key);
    else if (rl_line_buffer[pos] == '"')
      pos = gnu_readline_skip(pos, '"', '"');
  }
  return pos;
}

// Finds the matching paren (starting from just left of the cursor)
int gnu_readline_find_match(char key)
{
  if (key == ')')
    return gnu_readline_skip(rl_point - 1, '(', ')');
  else if (key == ']')
    return gnu_readline_skip(rl_point - 1, '[', ']');
  return 0;
}

// Delays, but returns early if key press occurs
void gnu_readline_timid_delay(int ms)
{
  struct pollfd pfd;

  pfd.fd = fileno(rl_instream);
  pfd.events = POLLIN | /* | */ POLLPRI;
  pfd.revents = 0;
  poll(&pfd, 1, ms);
}
/* *** END ***
 * Inline Functions
 */

// Bounces the cursor to the matching paren for a while
int gnu_readline_paren_bounce(int count, int key)
{
  int insert_success;
  int old_point;
  int matching;

  if (gnu_readline_bounce_ms == 0)
    return 0;

  // Write the just entered paren out first
  insert_success = rl_insert(count, key);
  if (insert_success != 0)
    return insert_success;
  rl_redisplay();

  // Need at least two chars to bounce...
  if (rl_point < 2) // rl_point set to next char (implicit +1)
    return 0;

  // If it's an escaped paren, don't bounce...
  if (rl_line_buffer[rl_point - 2] == '\\')
    return 0;

  // Bounce
  old_point = rl_point;
  matching = gnu_readline_find_match(key);
  if (matching < 0)
    return 0;
  else
    rl_point = matching;
  rl_redisplay();
  gnu_readline_timid_delay(gnu_readline_bounce_ms);
  rl_point = old_point;
  return 0;
}

#if 0
void paren_match_highlights(char *match_color, char *no_match_color)
{
  paren_colors.with_match = (match_color[0] == '\0'
      ? "\x1b[36m"
      : match_color);
  paren_colors.without_match = (no_match_color[0] == '\0'
      ? "\x1b[35m"
      : no_match_color);
}
#endif

////\\\\//// Tab Completion ////\\\\////

// Prototype for callback into scm
#ifndef RL_EGG_TESTING
C_word gnu_readline_scm_complete(char *, int, int);

// Gets called (repeatedly) when readline tries to do a completion
// FIXME, I use _strncpy_, but I should probably use _strlcpy_ or _strncat_
char *gnu_readline_tab_complete(const char *text, int status) {
  C_word result;
  char *str;
  int len;
  char *copied_str;

  /* All of this is for older versions of chicken (< 2.3), which don't
     reliably null-terminate strings */

  // Get scheme string for possible completion via callback
  result = gnu_readline_scm_complete((char *)text, strlen(text), status);

  if (result == C_SCHEME_FALSE)
    return NULL;

  // Convert into C types
  str = C_c_string(result);
  len = C_num_to_int(C_i_string_length(result));

  if (len == 0)
    return NULL;

  // Copy (note: the readline lib frees this copy)
  copied_str = (char *)malloc(len + 1);
  strncpy(copied_str, str, len); /* XXX this probably isn't the best, although it is safe
                                    thanks to the next line, way to copy these strings. */
  copied_str[len] = '\0';
  return copied_str;
}
#endif

// grants RO access to the gnu_history_newlines variable.
int gnu_history_new_lines()
{
  return gnu_history_newlines;
}

int gnu_readline_append_history(char *filename)
{
  return append_history(gnu_history_newlines, filename);
}

#if 0
void gnu_readline_highlight_matches()
{
  RL_EGG_DEBUG("match-position: %d\n", find_match(')'));
}
#endif
#if 0
char *gnu_make_arrow_code()
{};
#endif

// Set everything up
#ifndef RL_EGG_TESTING
void gnu_readline_init()
{
  using_history();
  rl_bind_key(')', gnu_readline_paren_bounce);
  rl_bind_key(']', gnu_readline_paren_bounce);
#if 0
  rl_bind_keyseq("[D", highlight_paren);
#endif
  rl_completion_entry_function = &gnu_readline_tab_complete;
  rl_variable_bind("rl_catch_signals", 0);
  rl_clear_signals();
  rl_set_signals();
  rl_completer_quote_characters = "\"";
}

inline void noop()
{}

#define RL_EGG_BUILDST(FAKE, RL_NAME, RL_VAR, RL_DO_1)			\
  FAKE ## RL_NAME (int set)						\
  {									\
    if (set == -1)							\
      goto check;							\
    else if (set == false)						\
      RL_VAR && (RL_VAR = false);					\
    else								\
      RL_VAR || (RL_VAR = true);					\
  check:								\
    if (RL_VAR)								\
      RL_DO_1();							\
    return RL_VAR;							\
  }

bool RL_EGG_BUILDST(, clear_hist, rl_egg_clear, clear_history);

// Called from scheme to get user input
char *gnu_readline_readline(char *prompt, char *prompt2, bool norec)
{
     HIST_ENTRY *entry;

     if (gnu_readline_buf != NULL) {
	  free(gnu_readline_buf);
	  gnu_readline_buf = NULL;
     }

     if (!(balnc.quote || balnc.paren[0] || balnc.brace[0]))
	  gnu_readline_buf = readline(prompt);
     else
	  gnu_readline_buf = readline(prompt2);

     RL_EGG_DEBUG("norec: `%d'\n", norec);

     if (norec)
	  goto skipped;

     if (gnu_readline_buf != NULL && *gnu_readline_buf != '\0') {
	  entry = history_get(history_base + history_length - 1);
	  if (entry == NULL || strcmp(entry->line, gnu_readline_buf) != 0) {
	       add_history(gnu_readline_buf);
	       ++gnu_history_newlines;
	  }
     }

skipped:
     if (rl_end > 0) {
	  int adx = quote_in_string(rl_line_buffer);
	  RL_EGG_DEBUG("quote_in_string: %d\n", adx);
	  int bdx = parbar_in_string(rl_line_buffer, '(');
	  int cdx = parbar_in_string(rl_line_buffer, '[');
	  balnc.quote = (adx == -1 ? 0 : adx);
	  if (bdx == -1)
	       clear_parbar('(');
	  if (cdx == -1)
	       clear_parbar('[');
     }
     clear_hist(-1);
     return (gnu_readline_buf);
}
#endif

bool int_to_bool(int boo)
{
     return (bool)boo;
}

void gnu_readline_signal_cleanup()
{
  clear_parbar('(');
  clear_parbar('[');
  free(gnu_readline_buf);
  gnu_readline_buf = NULL;
  rl_free_line_state();
  rl_cleanup_after_signal();
}

char gnu_unclosed_exp()
{
  if (balnc.quote)
    return 'q';
  else if (balnc.brace[0])
    return 'b';
  else if (balnc.paren[0])
    return 'p';
  return '\0';
}

static HIST_ENTRY *last_history_entry(const bool del_current, const bool script)
{
     HIST_ENTRY *he;

     using_history();
#if DEBUG
     if ((he = current_history()) != 0)
	  RL_EGG_DEBUG("current history: `%s'\n", he->line);
#endif
     if (del_current && !(script)) {
	  if ((he = current_history()) != 0) {
	       RL_EGG_DEBUG("deleting: `%s'\n", he->line);
	       free_history_entry(remove_history(where_history()));
	  } else if ((he = previous_history()) != 0) {
	       RL_EGG_DEBUG("deleting: `%s'\n", he->line);
	       free_history_entry(remove_history(where_history()));
	  }
     } else if (!script) {
	  previous_history();
     }
     he = previous_history();
     using_history();
     return he;
}

char *last_history_line(const bool del_current, const bool script)
{
     RL_EGG_DEBUG("del_current: `%d'\n", del_current);
     RL_EGG_DEBUG("script: `%d'\n", script);
     HIST_ENTRY *he;
     if ((he = last_history_entry(del_current, script)) == 0)
	  return ((char *)NULL);
     return he->line;
}

void insert_last_history_line(const bool del_current, const bool script, const bool add_eol)
{
     char *text = (add_eol
		   ? memsafe_concat(last_history_line(del_current, script), "\n", NULL)
		   : last_history_line(del_current, script));
     if (text != NULL) {
	  char *endp = strchr(text, '\0');
	  for (char *s = text; s != endp; ++s) {
	       rl_stuff_char(*s);
	  }
     }
     if (add_eol)
	  free(text);
}

void run_last_history_line(const bool del_current, const bool script)
{
     insert_last_history_line(del_current, script, true);
}

void safely_remove_history(int pos)
{
     using_history();
     free_history_entry(remove_history(pos));
}

/* safely concatenates the history_list's entry strings and returns them via a pointer */
char *gnu_history_list() /* may look a bit messy, but it seems to work great ;D */
{
  /* should be free'd by Chicken Scheme */
  char *list_0 = "";
  char *list_1 = "";
  bool need_free = true;


  HIST_ENTRY **hist_list = history_list();
  int idx;

  if (hist_list == NULL)
    return NULL;
  list_0 = memsafe_concat("", hist_list[0]->line, "\n", NULL);
  RL_EGG_DEBUG("buf@%d: %s\n", __LINE__, list_0);

  for (idx = 1; idx < history_length; ++idx) {
    if (need_free) {
      list_1 = memsafe_concat(list_0, hist_list[idx]->line, "\n", NULL);
      free(list_0);
      need_free = false;
    } else {
      list_0 = memsafe_concat(list_1, hist_list[idx]->line, "\n", NULL);
      free(list_1);
      need_free = true;
    }
  }
  return need_free ? list_0 : list_1;
}

int gnu_history_list_length()
{
  return history_length;
}
