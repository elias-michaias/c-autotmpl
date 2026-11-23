/*
 * c-autotmpl standalone CLI -- one self-contained ISO C99 source file.
 *
 * This tool discovers c-autotmpl decorator comments, scans source text for
 * template-style usages, and writes the same generated re-include headers as
 * the clangd feature module. It does not use LLVM, libclang, clang headers, a
 * compilation database, or a C++ runtime.
 *
 * Build with any C99 compiler:
 *
 *   cc -std=c99 -O2 autotmpl.c -o autotmpl
 *
 * ISO C99 has no directory API, so the small POSIX/Win32 filesystem layer
 * below is conditionally compiled. Everything above it is plain C99.
 */

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif


/* ==================== portable C99 utilities ==================== */

/* Small C99 utilities. No LLVM, clang, libclang or C++ runtime is used. */

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} Str;

typedef struct {
  void **items;
  size_t len;
  size_t cap;
} Vec;

typedef struct {
  char **items;
  size_t len;
  size_t cap;
} Set;

typedef struct {
  char *key;
  char *val;
} MapPair;

typedef struct {
  MapPair *items;
  size_t len;
  size_t cap;
} Map;

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup_n(const char *s, size_t n);
char *xstrdup(const char *s);

void str_init(Str *s);
void str_free(Str *s);
void str_clear(Str *s);
void str_reserve(Str *s, size_t extra);
void str_append_n(Str *s, const char *p, size_t n);
void str_append(Str *s, const char *p);
void str_append_char(Str *s, char c);
char *str_take(Str *s);
char *str_copy(const Str *s);

void vec_push(Vec *v, void *p);

int set_has(const Set *s, const char *key);
int set_add(Set *s, const char *key);

const char *map_get(const Map *m, const char *key);
void map_set(Map *m, const char *key, const char *val);

int is_space(char c);
int is_ident_start(char c);
int is_ident_char(char c);
char *trim_copy(const char *s);
char *trim_copy_n(const char *s, size_t n);

char *path_join(const char *a, const char *b);
char *path_dirname(const char *p);
char *path_basename(const char *p);
char *path_stem(const char *p);
char *path_ext(const char *p);
char *path_absolute(const char *p);
char *path_relative(const char *from_dir, const char *to_file);
int path_is_absolute(const char *p);

int path_exists(const char *p);
int path_is_dir(const char *p);
int path_is_file(const char *p);
int make_dir(const char *p);

char *read_file(const char *p, size_t *len_out);
int write_file(const char *p, const char *data, size_t len);

typedef int (*VisitFileFn)(const char *path, void *ctx);
/* Returns nonzero when a visitor asked to stop the walk. */
int walk_dir(const char *dir, VisitFileFn visit, void *ctx);
int skip_dir_name(const char *name);

void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) {
    fputs("autotmpl: out of memory\n", stderr);
    exit(1);
  }
  return p;
}

void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n ? n : 1);
  if (!q) {
    fputs("autotmpl: out of memory\n", stderr);
    exit(1);
  }
  return q;
}

char *xstrdup_n(const char *s, size_t n) {
  char *p = (char *)xmalloc(n + 1);
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

char *xstrdup(const char *s) { return xstrdup_n(s, strlen(s)); }

void str_init(Str *s) {
  s->data = NULL;
  s->len = 0;
  s->cap = 0;
}

void str_free(Str *s) {
  free(s->data);
  str_init(s);
}

void str_clear(Str *s) {
  s->len = 0;
  if (s->data)
    s->data[0] = '\0';
}

void str_reserve(Str *s, size_t extra) {
  size_t need = s->len + extra + 1;
  if (need <= s->cap)
    return;
  size_t cap = s->cap ? s->cap : 64;
  while (cap < need)
    cap *= 2;
  s->data = (char *)xrealloc(s->data, cap);
  s->cap = cap;
}

void str_append_n(Str *s, const char *p, size_t n) {
  if (!n)
    return;
  str_reserve(s, n);
  memcpy(s->data + s->len, p, n);
  s->len += n;
  s->data[s->len] = '\0';
}

void str_append(Str *s, const char *p) { str_append_n(s, p, strlen(p)); }

void str_append_char(Str *s, char c) { str_append_n(s, &c, 1); }

char *str_take(Str *s) {
  if (!s->data)
    return xstrdup("");
  char *p = s->data;
  str_init(s);
  return p;
}

char *str_copy(const Str *s) {
  return s->data ? xstrdup(s->data) : xstrdup("");
}

void vec_push(Vec *v, void *p) {
  if (v->len == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 16;
    v->items = (void **)xrealloc(v->items, v->cap * sizeof(void *));
  }
  v->items[v->len++] = p;
}

static size_t lower_bound_str(char **items, size_t n, const char *key) {
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (strcmp(items[mid], key) < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

int set_has(const Set *s, const char *key) {
  size_t i = lower_bound_str(s->items, s->len, key);
  return i < s->len && strcmp(s->items[i], key) == 0;
}

int set_add(Set *s, const char *key) {
  size_t i = lower_bound_str(s->items, s->len, key);
  if (i < s->len && strcmp(s->items[i], key) == 0)
    return 0;
  if (s->len == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 16;
    s->items = (char **)xrealloc(s->items, s->cap * sizeof(char *));
  }
  memmove(s->items + i + 1, s->items + i, (s->len - i) * sizeof(char *));
  s->items[i] = xstrdup(key);
  ++s->len;
  return 1;
}

static size_t map_lower_bound(const Map *m, const char *key) {
  size_t lo = 0, hi = m->len;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (strcmp(m->items[mid].key, key) < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

const char *map_get(const Map *m, const char *key) {
  size_t i = map_lower_bound(m, key);
  if (i < m->len && strcmp(m->items[i].key, key) == 0)
    return m->items[i].val;
  return NULL;
}

void map_set(Map *m, const char *key, const char *val) {
  size_t i = map_lower_bound(m, key);
  if (i < m->len && strcmp(m->items[i].key, key) == 0) {
    free(m->items[i].val);
    m->items[i].val = xstrdup(val);
    return;
  }
  if (m->len == m->cap) {
    m->cap = m->cap ? m->cap * 2 : 16;
    m->items = (MapPair *)xrealloc(m->items, m->cap * sizeof(MapPair));
  }
  memmove(m->items + i + 1, m->items + i, (m->len - i) * sizeof(MapPair));
  m->items[i].key = xstrdup(key);
  m->items[i].val = xstrdup(val);
  ++m->len;
}

int is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
         c == '\v';
}

int is_ident_start(char c) {
  return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int is_ident_char(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }

char *trim_copy(const char *s) { return trim_copy_n(s, strlen(s)); }

char *trim_copy_n(const char *s, size_t n) {
  size_t begin = 0;
  while (begin < n && is_space(s[begin]))
    ++begin;
  size_t end = n;
  while (end > begin && is_space(s[end - 1]))
    --end;
  return xstrdup_n(s + begin, end - begin);
}

static char *normalize_slashes(const char *p) {
  size_t n = strlen(p);
  char *out = (char *)xmalloc(n + 1);
  for (size_t i = 0; i < n; ++i)
    out[i] = p[i] == '\\' ? '/' : p[i];
  out[n] = '\0';
  return out;
}

int path_is_absolute(const char *p) {
  int drive = p[0] && p[1] == ':' &&
              ((p[0] >= 'A' && p[0] <= 'Z') ||
               (p[0] >= 'a' && p[0] <= 'z'));
  return p[0] == '/' || drive;
}

char *path_join(const char *a, const char *b) {
  if (path_is_absolute(b))
    return normalize_slashes(b);
  size_t na = strlen(a), nb = strlen(b);
  Str out;
  str_init(&out);
  str_append_n(&out, a, na);
  if (na && a[na - 1] != '/' && a[na - 1] != '\\')
    str_append_char(&out, '/');
  str_append_n(&out, b, nb);
  char *tmp = str_take(&out);
  char *norm = normalize_slashes(tmp);
  free(tmp);
  return norm;
}

char *path_dirname(const char *p) {
  size_t n = strlen(p);
  size_t slash = n;
  while (slash > 0 && p[slash - 1] != '/' && p[slash - 1] != '\\')
    --slash;
  if (slash == 0)
    return xstrdup(".");
  if (slash == 1)
    return xstrdup_n(p, 1);
  /* Drop trailing separators, but keep the root. */
  size_t end = slash;
  while (end > 1 && (p[end - 1] == '/' || p[end - 1] == '\\'))
    --end;
  return xstrdup_n(p, end);
}

char *path_basename(const char *p) {
  size_t n = strlen(p);
  size_t start = n;
  while (start > 0 && p[start - 1] != '/' && p[start - 1] != '\\')
    --start;
  return xstrdup_n(p + start, n - start);
}

char *path_stem(const char *p) {
  char *base = path_basename(p);
  char *dot = strrchr(base, '.');
  if (dot && dot != base) {
    char *stem = xstrdup_n(base, (size_t)(dot - base));
    free(base);
    return stem;
  }
  return base;
}

char *path_ext(const char *p) {
  char *base = path_basename(p);
  char *dot = strrchr(base, '.');
  if (!dot || dot == base) {
    free(base);
    return xstrdup("");
  }
  char *ext = xstrdup(dot);
  free(base);
  return ext;
}

static char *current_dir(void) {
  char buf[4096];
#if defined(_WIN32)
  if (!_getcwd(buf, (int)sizeof(buf)))
#else
  if (!getcwd(buf, sizeof(buf)))
#endif
    return xstrdup(".");
  return normalize_slashes(buf);
}

static void push_components(Vec *out, const char *p, size_t start) {
  size_t n = strlen(p);
  while (start < n) {
    while (start < n && p[start] == '/')
      ++start;
    size_t end = start;
    while (end < n && p[end] != '/')
      ++end;
    if (end > start) {
      char *part = xstrdup_n(p + start, end - start);
      if (strcmp(part, ".") == 0) {
        free(part);
      } else if (strcmp(part, "..") == 0 && out->len > 0) {
        free(part);
        free(out->items[--out->len]);
      } else {
        vec_push(out, part);
      }
    }
    start = end + 1;
  }
}

char *path_absolute(const char *p) {
  char *norm = normalize_slashes(p);
  char *abs;
  if (path_is_absolute(norm)) {
    abs = norm;
  } else {
    char *cwd = current_dir();
    abs = path_join(cwd, norm);
    free(cwd);
    free(norm);
  }

  Str out;
  str_init(&out);
  size_t start = 0;
  if (abs[0] == '/') {
    str_append_char(&out, '/');
    start = 1;
  } else if (abs[0] && abs[1] == ':') {
    str_append_char(&out, abs[0]);
    str_append_char(&out, ':');
    start = 2;
    if (abs[start] == '/') {
      str_append_char(&out, '/');
      ++start;
    }
  }

  Vec parts;
  parts.items = NULL;
  parts.len = parts.cap = 0;
  push_components(&parts, abs, start);
  for (size_t i = 0; i < parts.len; ++i) {
    if (out.len && out.data[out.len - 1] != '/')
      str_append_char(&out, '/');
    str_append(&out, (char *)parts.items[i]);
    free(parts.items[i]);
  }
  free(parts.items);
  free(abs);
  return str_take(&out);
}

static void split_components(Vec *out, const char *p) {
  size_t n = strlen(p), i = 0;
  while (i < n) {
    while (i < n && p[i] == '/')
      ++i;
    size_t start = i;
    while (i < n && p[i] != '/')
      ++i;
    if (i > start)
      vec_push(out, xstrdup_n(p + start, i - start));
  }
}

char *path_relative(const char *from_dir, const char *to_file) {
  Vec from, to;
  from.items = NULL;
  from.len = from.cap = 0;
  to.items = NULL;
  to.len = to.cap = 0;
  split_components(&from, from_dir);
  split_components(&to, to_file);

  if (from.len && to.len && strcmp((char *)from.items[0], (char *)to.items[0]) != 0) {
    char *result = xstrdup(to_file);
    for (size_t i = 0; i < from.len; ++i)
      free(from.items[i]);
    for (size_t i = 0; i < to.len; ++i)
      free(to.items[i]);
    free(from.items);
    free(to.items);
    return result;
  }

  size_t common = 0;
  while (common < from.len && common < to.len &&
         strcmp((char *)from.items[common], (char *)to.items[common]) == 0)
    ++common;

  Str out;
  str_init(&out);
  for (size_t i = common; i < from.len; ++i) {
    if (out.len)
      str_append_char(&out, '/');
    str_append(&out, "..");
  }
  for (size_t i = common; i < to.len; ++i) {
    if (out.len)
      str_append_char(&out, '/');
    str_append(&out, (char *)to.items[i]);
  }
  for (size_t i = 0; i < from.len; ++i)
    free(from.items[i]);
  for (size_t i = 0; i < to.len; ++i)
    free(to.items[i]);
  free(from.items);
  free(to.items);
  return out.len ? str_take(&out) : xstrdup(".");
}

int path_exists(const char *p) {
#if defined(_WIN32)
  return _access(p, 0) == 0;
#else
  return access(p, F_OK) == 0;
#endif
}

int path_is_dir(const char *p) {
#if defined(_WIN32)
  struct _stat st;
  return _stat(p, &st) == 0 && (st.st_mode & _S_IFDIR);
#else
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

int path_is_file(const char *p) {
#if defined(_WIN32)
  struct _stat st;
  return _stat(p, &st) == 0 && (st.st_mode & _S_IFREG);
#else
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

int make_dir(const char *p) {
  if (!p || !*p || path_is_dir(p))
    return 0;

  char *parent = path_dirname(p);
  if (strcmp(parent, p) != 0 && strcmp(parent, ".") != 0 &&
      !path_is_dir(parent)) {
    if (make_dir(parent) != 0) {
      free(parent);
      return -1;
    }
  }
  free(parent);

#if defined(_WIN32)
  if (_mkdir(p) == 0)
    return 0;
#else
  if (mkdir(p, 0755) == 0)
    return 0;
#endif
  return errno == EEXIST && path_is_dir(p) ? 0 : -1;
}

char *read_file(const char *p, size_t *len_out) {
  FILE *f = fopen(p, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long end = ftell(f);
  if (end < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  size_t n = (size_t)end;
  char *buf = (char *)xmalloc(n + 1);
  size_t got = fread(buf, 1, n, f);
  fclose(f);
  buf[got] = '\0';
  if (len_out)
    *len_out = got;
  return buf;
}

int write_file(const char *p, const char *data, size_t len) {
  FILE *f = fopen(p, "wb");
  if (!f)
    return -1;
  size_t wrote = fwrite(data, 1, len, f);
  int bad = fflush(f) != 0 || ferror(f) != 0;
  if (fclose(f) != 0)
    bad = 1;
  return bad || wrote != len ? -1 : 0;
}

int skip_dir_name(const char *name) {
  if (!name || !*name)
    return 0;
  if (name[0] == '.')
    return 1;
  return strcmp(name, "build") == 0 || strcmp(name, "node_modules") == 0 ||
         strcmp(name, "gen") == 0;
}

typedef struct {
  char *name;
  int dir;
} DirEntry;

static int dir_entry_cmp(const void *a, const void *b) {
  DirEntry *ea = *(DirEntry * const *)a;
  DirEntry *eb = *(DirEntry * const *)b;
  return strcmp(ea->name, eb->name);
}

int walk_dir(const char *dir, VisitFileFn visit, void *ctx) {
  Vec entries;
  entries.items = NULL;
  entries.len = entries.cap = 0;

#if defined(_WIN32)
  char *pattern = path_join(dir, "*");
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  free(pattern);
  if (h == INVALID_HANDLE_VALUE)
    return 0;
  do {
    if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
      continue;
    if (skip_dir_name(fd.cFileName))
      continue;
    DirEntry *entry = (DirEntry *)xmalloc(sizeof(DirEntry));
    entry->name = xstrdup(fd.cFileName);
    entry->dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    vec_push(&entries, entry);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR *d = opendir(dir);
  if (!d)
    return 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    if (skip_dir_name(ent->d_name))
      continue;
    char *child = path_join(dir, ent->d_name);
    struct stat st;
    if (lstat(child, &st) == 0) {
      DirEntry *entry = NULL;
      if (S_ISDIR(st.st_mode))
        entry = (DirEntry *)xmalloc(sizeof(DirEntry));
      else if (S_ISREG(st.st_mode))
        entry = (DirEntry *)xmalloc(sizeof(DirEntry));
      if (entry) {
        entry->name = xstrdup(ent->d_name);
        entry->dir = S_ISDIR(st.st_mode);
        vec_push(&entries, entry);
      }
    }
    free(child);
  }
  closedir(d);
#endif

  qsort(entries.items, entries.len, sizeof(void *), dir_entry_cmp);

  int stop = 0;
  for (int pass = 0; pass < 2 && !stop; ++pass) {
    for (size_t i = 0; i < entries.len && !stop; ++i) {
      DirEntry *entry = (DirEntry *)entries.items[i];
      if ((pass == 1) != entry->dir)
        continue;
      char *child = path_join(dir, entry->name);
      stop = entry->dir ? walk_dir(child, visit, ctx) : visit(child, ctx);
      free(child);
    }
  }

  for (size_t i = 0; i < entries.len; ++i) {
    DirEntry *entry = (DirEntry *)entries.items[i];
    free(entry->name);
    free(entry);
  }
  free(entries.items);
  return stop;
}

/* ==================== scanner and generator ===================== */

/* ------------------------------- tokens -------------------------------- */


typedef struct {
  const char *text;
  size_t len;
  size_t offset;
} Token;

static char *strip_comments_literals(const char *src) {
  size_t n = strlen(src);
  char *out = xstrdup(src);
  for (size_t i = 0; i < n;) {
    if (out[i] == '/' && i + 1 < n) {
      if (out[i + 1] == '/') {
        out[i++] = ' ';
        out[i++] = ' ';
        while (i < n && out[i] != '\n')
          out[i++] = ' ';
        continue;
      }
      if (out[i + 1] == '*') {
        out[i++] = ' ';
        out[i++] = ' ';
        while (i < n) {
          if (out[i] == '*' && i + 1 < n && out[i + 1] == '/') {
            out[i++] = ' ';
            out[i++] = ' ';
            break;
          }
          if (out[i] != '\n')
            out[i] = ' ';
          ++i;
        }
        continue;
      }
    }
    if (out[i] == '"' || out[i] == '\'') {
      char quote = out[i];
      out[i++] = ' ';
      while (i < n) {
        if (out[i] == '\\') {
          out[i++] = ' ';
          if (i < n)
            out[i++] = ' ';
          continue;
        }
        if (out[i] == quote) {
          out[i++] = ' ';
          break;
        }
        if (out[i] != '\n')
          out[i] = ' ';
        ++i;
      }
      continue;
    }
    ++i;
  }
  return out;
}

static void tokenize(const char *clean, Vec *out) {
  size_t n = strlen(clean);
  for (size_t i = 0; i < n;) {
    if (is_space(clean[i])) {
      ++i;
      continue;
    }
    Token *t = (Token *)xmalloc(sizeof(Token));
    t->offset = i;
    if (is_ident_start(clean[i])) {
      size_t start = i++;
      while (i < n && is_ident_char(clean[i]))
        ++i;
      t->text = clean + start;
      t->len = i - start;
    } else {
      t->text = clean + i;
      t->len = 1;
      ++i;
    }
    vec_push(out, t);
  }
}

static int token_is(const Token *t, const char *word) {
  size_t n = strlen(word);
  return t->len == n && memcmp(t->text, word, n) == 0;
}

static char *token_copy(const Token *t) { return xstrdup_n(t->text, t->len); }

/* ------------------------------ names ---------------------------------- */

static char *last_identifier(const char *s) {
  size_t end = strlen(s);
  while (end > 0 && !is_ident_char(s[end - 1]))
    --end;
  size_t begin = end;
  while (begin > 0 && is_ident_char(s[begin - 1]))
    --begin;
  return xstrdup_n(s + begin, end - begin);
}

static int builtin_name(const char *s) {
  static const char *names[] = {
      "void",     "bool",     "char",     "short",    "int",
      "long",     "float",    "double",   "signed",   "unsigned",
      "_Bool",    "size_t",   "ssize_t",  "int8_t",   "uint8_t",
      "int16_t",  "uint16_t", "int32_t",  "uint32_t", "int64_t",
      "uint64_t", "intptr_t", "uintptr_t"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    if (strcmp(s, names[i]) == 0)
      return 1;
  return 0;
}

static int keyword_name(const char *s) {
  static const char *names[] = {
      "struct",   "union",    "enum",     "class",    "const",
      "volatile", "restrict", "static",   "inline",   "extern",
      "register", "auto",     "signed",   "unsigned", "long",
      "short",    "int",      "char",     "float",    "double",
      "void",     "_Bool",    "typedef"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    if (strcmp(s, names[i]) == 0)
      return 1;
  return 0;
}

static int needs_guard(const char *spelling) {
  char *name = last_identifier(spelling);
  int result = name[0] && !builtin_name(name);
  free(name);
  return result;
}

static char *strip_qualifiers(const char *spelling) {
  char *work = trim_copy(spelling);
  int changed = 1;
  while (changed) {
    changed = 0;
    static const char *quals[] = {"const", "volatile", "restrict", "struct",
                                  "union", "enum",     "class"};
    for (size_t i = 0; i < sizeof(quals) / sizeof(quals[0]); ++i) {
      size_t n = strlen(quals[i]);
      if (strncmp(work, quals[i], n) != 0)
        continue;
      if (work[n] && !is_space(work[n]))
        continue;
      char *next = trim_copy(work + n);
      free(work);
      work = next;
      changed = 1;
    }
  }
  return work;
}

static char *normalize_ws(const char *s) {
  Str out;
  str_init(&out);
  int in_space = 0, started = 0;
  for (; *s; ++s) {
    if (is_space(*s)) {
      if (started)
        in_space = 1;
      continue;
    }
    if (in_space) {
      str_append_char(&out, ' ');
      in_space = 0;
    }
    str_append_char(&out, *s);
    started = 1;
  }
  return str_take(&out);
}

/* --------------------------- include guards ---------------------------- */

static char *read_identifier(const char *s) {
  size_t i = 0;
  while (is_space(s[i]))
    ++i;
  size_t start = i;
  while (is_ident_char(s[i]))
    ++i;
  return xstrdup_n(s + start, i - start);
}

static char *guard_macro_from_line(const char *line) {
  const char *s = line;
  while (is_space(*s))
    ++s;
  if (strncmp(s, "#ifndef", 7) == 0 && (s[7] == '\0' || is_space(s[7])))
    return read_identifier(s + 7);
  if (strncmp(s, "#if", 3) != 0 || (s[3] && !is_space(s[3])))
    return xstrdup("");
  s += 3;
  while (is_space(*s))
    ++s;
  if (*s != '!')
    return xstrdup("");
  ++s;
  while (is_space(*s))
    ++s;
  if (strncmp(s, "defined", 7) != 0)
    return xstrdup("");
  s += 7;
  while (is_space(*s))
    ++s;
  if (*s == '(')
    ++s;
  return read_identifier(s);
}

static int defines_macro_soon(const char *rest, const char *macro) {
  int lines = 0;
  while (*rest && lines < 8) {
    const char *end = strchr(rest, '\n');
    size_t n = end ? (size_t)(end - rest) : strlen(rest);
    char *line = xstrdup_n(rest, n);
    char *s = line;
    while (is_space(*s))
      ++s;
    if (*s && strncmp(s, "//", 2) != 0 && strncmp(s, "/*", 2) != 0 &&
        strncmp(s, "#define", 7) == 0) {
      char *name = read_identifier(s + 7);
      int match = strcmp(name, macro) == 0;
      free(name);
      free(line);
      return match;
    }
    free(line);
    if (!end)
      break;
    rest = end + 1;
    ++lines;
  }
  return 0;
}

static char *find_guard_in_buffer(const char *buffer) {
  const char *rest = buffer;
  for (int line_no = 0; line_no < 80 && *rest; ++line_no) {
    const char *end = strchr(rest, '\n');
    size_t n = end ? (size_t)(end - rest) : strlen(rest);
    char *line = xstrdup_n(rest, n);
    char *macro = guard_macro_from_line(line);
    if (macro[0] && defines_macro_soon(end ? end + 1 : "", macro)) {
      free(line);
      return macro;
    }
    free(macro);
    free(line);
    if (!end)
      break;
    rest = end + 1;
  }
  return xstrdup("");
}

static int has_tmpl_header_guard(const char *buffer) {
  char *clean = strip_comments_literals(buffer);
  const char *rest = clean;
  int found = 0;
  while (*rest) {
    const char *end = strchr(rest, '\n');
    size_t n = end ? (size_t)(end - rest) : strlen(rest);
    char *line = xstrdup_n(rest, n);
    char *macro = guard_macro_from_line(line);
    if (strcmp(macro, "TMPL_HEADER") == 0)
      found = 1;
    free(macro);
    free(line);
    if (found || !end)
      break;
    rest = end + 1;
  }
  free(clean);
  return found;
}

static char *upper_macro(const char *a, const char *b) {
  Str out;
  str_init(&out);
  const char *parts[2] = {a, b};
  for (int p = 0; p < 2; ++p) {
    if (p)
      str_append_char(&out, '_');
    for (const char *s = parts[p]; *s; ++s) {
      char c = *s;
      if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
      else if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
        c = '_';
      str_append_char(&out, c);
    }
  }
  str_append(&out, "_H");
  return str_take(&out);
}

static char *generated_header_guard(const char *path) {
  char *content = read_file(path, NULL);
  if (content) {
    char *guard = find_guard_in_buffer(content);
    free(content);
    if (guard[0])
      return guard;
    free(guard);
  }
  char *dir = path_dirname(path);
  char *parent = path_basename(dir);
  char *stem = path_stem(path);
  char *guard = upper_macro(parent, stem);
  free(dir);
  free(parent);
  free(stem);
  return guard;
}

/* ------------------------------ registry ------------------------------- */

typedef enum { TEMPLATE_TYPE, TEMPLATE_FUNCTION } TemplateKind;

typedef struct {
  TemplateKind kind;
  char *name;
  char **params;
  size_t nparams;
  char *header;
  char *generated;
  char *include;
  char *impl;             /* grouped implementation TU, when split */
  char *impl_include;     /* template header relative to impl TU */
  int has_tmpl_header;
} TemplateDef;

typedef struct {
  Vec items; /* TemplateDef* */
} Registry;

static TemplateDef *registry_find(const Registry *r, const char *name) {
  for (size_t i = 0; i < r->items.len; ++i) {
    TemplateDef *d = (TemplateDef *)r->items.items[i];
    if (strcmp(d->name, name) == 0)
      return d;
  }
  return NULL;
}

/* A bare registered type-template name is a lexical constructor argument, not
 * a concrete type. It is passed through to the generated block as a macro name
 * and must not trigger a user-type include guard. */
static int is_constructor_arg(const char *spelling, const Registry *reg) {
  char *trimmed = trim_copy(spelling);
  if (!trimmed[0]) {
    free(trimmed);
    return 0;
  }
  for (size_t i = 0; trimmed[i]; ++i)
    if (!is_ident_char(trimmed[i])) {
      free(trimmed);
      return 0;
    }
  TemplateDef *def = registry_find(reg, trimmed);
  free(trimmed);
  return def && def->kind == TEMPLATE_TYPE;
}

static TemplateDef *registry_match_mangled(const Registry *r, const char *text,
                                           const char **arg_out) {
  TemplateDef *best = NULL;
  size_t best_len = 0;
  const char *best_arg = NULL;
  for (size_t i = 0; i < r->items.len; ++i) {
    TemplateDef *d = (TemplateDef *)r->items.items[i];
    size_t n = strlen(d->name);
    /* Mangled-name scanning is for type arguments only. Function templates use
     * the explicit `name(Type)` call spelling so helper names such as
     * `name_impl` are not mistaken for instantiations. */
    if (d->kind == TEMPLATE_FUNCTION)
      continue;
    if (strlen(text) <= n + 1 || text[n] != '_' || strncmp(text, d->name, n))
      continue;
    if (best && n <= best_len)
      continue;
    best = d;
    best_len = n;
    best_arg = text + n + 1;
  }
  if (arg_out)
    *arg_out = best_arg;
  return best;
}

static int registry_may_contain(const Registry *r, const char *text) {
  for (size_t i = 0; i < r->items.len; ++i)
    if (strstr(text, ((TemplateDef *)r->items.items[i])->name))
      return 1;
  return 0;
}

static char *lower_copy(const char *s) {
  char *out = xstrdup(s);
  for (char *p = out; *p; ++p)
    if (*p >= 'A' && *p <= 'Z')
      *p = (char)(*p - 'A' + 'a');
  return out;
}

static char *generated_header_path(const char *header, const char *name) {
  char *dir = path_dirname(header);
  char *dir_name = path_basename(dir);
  char *base;
  if (strcmp(dir_name, "tmpl") == 0 || strcmp(dir_name, "templates") == 0) {
    char *parent = path_dirname(dir);
    base = parent;
  } else {
    base = xstrdup(dir);
  }
  char *gen_dir = path_join(base, "gen");
  char *lower = lower_copy(name);
  char *file = NULL;
  size_t need = strlen(lower) + 3;
  file = (char *)xmalloc(need);
  snprintf(file, need, "%s.h", lower);
  char *path = path_join(gen_dir, file);
  char *abs = path_absolute(path);
  free(dir);
  free(dir_name);
  free(base);
  free(gen_dir);
  free(lower);
  free(file);
  free(path);
  return abs;
}

static char *generated_impl_path(const char *header, const char *name) {
  char *generated = generated_header_path(header, name);
  char *gen_dir = path_dirname(generated);
  char *impl_dir = path_join(gen_dir, "impl");
  char *lower = lower_copy(name);
  char *file = (char *)xmalloc(strlen(lower) + 3);
  sprintf(file, "%s.c", lower);
  char *path = path_join(impl_dir, file);
  char *abs = path_absolute(path);
  free(generated);
  free(gen_dir);
  free(impl_dir);
  free(lower);
  free(file);
  free(path);
  return abs;
}

static int parse_decorator(const char *line, TemplateDef *out) {
  const char *s = line;
  while (is_space(*s))
    ++s;
  if (strncmp(s, "//", 2))
    return 0;
  s += 2;
  while (is_space(*s))
    ++s;
  if (strncmp(s, "--", 2))
    return 0;
  s += 2;
  while (is_space(*s))
    ++s;
  if (strncmp(s, "template", 8) || (s[8] && !is_space(s[8])))
    return 0;
  s += 8;
  while (is_space(*s))
    ++s;

  TemplateKind kind = TEMPLATE_TYPE;
  size_t word = 0;
  while (is_ident_char(s[word]))
    ++word;
  if (word == 4 && strncmp(s, "type", 4) == 0 &&
      (!s[4] || is_space(s[4]))) {
    s += 4;
    while (is_space(*s))
      ++s;
  } else if (word == 8 && strncmp(s, "function", 8) == 0 &&
             (!s[8] || is_space(s[8]))) {
    kind = TEMPLATE_FUNCTION;
    s += 8;
    while (is_space(*s))
      ++s;
  }

  size_t begin = 0;
  while (is_ident_char(s[begin]))
    ++begin;
  if (!begin)
    return 0;
  free(out->name);
  out->name = xstrdup_n(s, begin);
  out->kind = kind;
  s += begin;
  while (is_space(*s))
    ++s;
  if (*s++ != '(')
    return 0;

  for (size_t i = 0; i < out->nparams; ++i)
    free(out->params[i]);
  free(out->params);
  out->params = NULL;
  out->nparams = 0;
  while (*s && *s != ')') {
    while (is_space(*s) || *s == ',')
      ++s;
    size_t p = 0;
    while (is_ident_char(s[p]))
      ++p;
    if (p) {
      out->params = (char **)xrealloc(out->params,
                                      (out->nparams + 1) * sizeof(char *));
      out->params[out->nparams++] = xstrdup_n(s, p);
      s += p;
    } else if (*s) {
      ++s;
    }
  }
  return *s == ')';
}

static int is_header_ext(const char *ext) {
  return strcmp(ext, ".h") == 0 || strcmp(ext, ".hh") == 0 ||
         strcmp(ext, ".hpp") == 0 || strcmp(ext, ".hxx") == 0;
}

typedef struct {
  Registry *reg;
  int found;
} TemplateScan;

static int visit_template_file(const char *path, void *ctx) {
  TemplateScan *scan = (TemplateScan *)ctx;
  char *ext = path_ext(path);
  int header = is_header_ext(ext);
  free(ext);
  if (!header)
    return 0;

  char *content = read_file(path, NULL);
  if (!content)
    return 0;
  TemplateDef def;
  memset(&def, 0, sizeof(def));
  const char *rest = content;
  int ok = 0;
  while (*rest) {
    const char *end = strchr(rest, '\n');
    size_t n = end ? (size_t)(end - rest) : strlen(rest);
    char *line = xstrdup_n(rest, n);
    if (parse_decorator(line, &def)) {
      ok = 1;
      free(line);
      break;
    }
    free(line);
    if (!end)
      break;
    rest = end + 1;
  }
  def.has_tmpl_header = has_tmpl_header_guard(content);
  free(content);
  if (!ok || !def.name[0]) {
    free(def.name);
    free(def.params);
    return 0;
  }

  def.header = path_absolute(path);
  def.generated = generated_header_path(def.header, def.name);
  def.impl = def.has_tmpl_header
                 ? generated_impl_path(def.header, def.name)
                 : NULL;
  char *gen_dir = path_dirname(def.generated);
  def.include = path_relative(gen_dir, def.header);
  free(gen_dir);
  if (def.impl) {
    char *impl_dir = path_dirname(def.impl);
    def.impl_include = path_relative(impl_dir, def.header);
    free(impl_dir);
  }

  TemplateDef *owned = (TemplateDef *)xmalloc(sizeof(TemplateDef));
  *owned = def;
  vec_push(&scan->reg->items, owned);
  scan->found = 1;
  return 0;
}

static void registry_scan(Registry *reg, const char *root) {
  if (!path_is_dir(root))
    return;
  const char *dirs[] = {"tmpl", "templates"};
  for (size_t i = 0; i < 2; ++i) {
    char *dir = path_join(root, dirs[i]);
    if (path_is_dir(dir)) {
      TemplateScan scan = {reg, 0};
      walk_dir(dir, visit_template_file, &scan);
      free(dir);
      if (reg->items.len)
        return;
      continue;
    }
    free(dir);
  }
  TemplateScan scan = {reg, 0};
  walk_dir(root, visit_template_file, &scan);
}

/* ----------------------------- type index ------------------------------ */

typedef struct {
  Map guards;
  Map guard_headers; /* guard macro -> declaring header path */
} TypeIndex;

static int is_keyword_token(const Token *t) {
  char *word = token_copy(t);
  int result = keyword_name(word);
  free(word);
  return result;
}

static void add_declared_name(TypeIndex *idx, const Token *name,
                              const char *guard) {
  if (!name)
    return;
  char *word = token_copy(name);
  if (!word[0] || !is_ident_start(word[0]) || keyword_name(word) ||
      builtin_name(word)) {
    free(word);
    return;
  }
  if (!map_get(&idx->guards, word))
    map_set(&idx->guards, word, guard);
  free(word);
}

static void index_header(TypeIndex *idx, const char *buffer, const char *guard) {
  char *clean = strip_comments_literals(buffer);
  Vec tokens;
  tokens.items = NULL;
  tokens.len = tokens.cap = 0;
  tokenize(clean, &tokens);

  for (size_t i = 0; i < tokens.len; ++i) {
    Token *t = (Token *)tokens.items[i];
    if (token_is(t, "struct") || token_is(t, "union") ||
        token_is(t, "enum") || token_is(t, "class")) {
      if (i + 1 < tokens.len)
        add_declared_name(idx, (Token *)tokens.items[i + 1], guard);
      continue;
    }
    if (!token_is(t, "typedef"))
      continue;
    int depth = 0;
    Token *last = NULL;
    for (size_t j = i + 1; j < tokens.len; ++j) {
      Token *u = (Token *)tokens.items[j];
      if (token_is(u, "{")) {
        ++depth;
        continue;
      }
      if (token_is(u, "}")) {
        if (depth > 0)
          --depth;
        continue;
      }
      if (token_is(u, ";") && depth == 0) {
        add_declared_name(idx, last, guard);
        last = NULL;
        break;
      }
      if (token_is(u, ",") && depth == 0) {
        add_declared_name(idx, last, guard);
        last = NULL;
        continue;
      }
      if (u->len && is_ident_start(u->text[0]) && !is_keyword_token(u))
        last = u;
    }
  }
}

typedef struct {
  TypeIndex *idx;
  Set *wanted;
} TypeScan;

static int all_wanted_found(const TypeIndex *idx, const Set *wanted) {
  for (size_t i = 0; i < wanted->len; ++i)
    if (!map_get(&idx->guards, wanted->items[i]))
      return 0;
  return 1;
}

static int visit_type_header(const char *path, void *ctx) {
  TypeScan *scan = (TypeScan *)ctx;
  char *ext = path_ext(path);
  int header = is_header_ext(ext);
  free(ext);
  if (!header)
    return 0;
  char *buffer = read_file(path, NULL);
  if (!buffer)
    return 0;
  int relevant = scan->wanted->len == 0;
  for (size_t i = 0; !relevant && i < scan->wanted->len; ++i)
    if (strstr(buffer, scan->wanted->items[i]))
      relevant = 1;
  if (!relevant) {
    free(buffer);
    return 0;
  }

  char *guard = find_guard_in_buffer(buffer);
  if (!guard[0]) {
    free(guard);
    free(buffer);
    return 0;
  }
  index_header(scan->idx, buffer, guard);
  if (!map_get(&scan->idx->guard_headers, guard)) {
    char *abs = path_absolute(path);
    map_set(&scan->idx->guard_headers, guard, abs);
    free(abs);
  }
  free(guard);
  free(buffer);
  return scan->wanted->len > 0 && all_wanted_found(scan->idx, scan->wanted);
}

static void type_index_scan(TypeIndex *idx, const char *root, Set *wanted) {
  if (!path_is_dir(root))
    return;
  TypeScan scan = {idx, wanted};
  walk_dir(root, visit_type_header, &scan);
}

/* ------------------------------- usages -------------------------------- */

typedef struct {
  const TemplateDef *tmpl;
  char *type;
} Dep;

typedef struct {
  const TemplateDef *tmpl;
  char *type;      /* canonical "A, B" spelling used by markers */
  Vec args;       /* canonical individual arguments */
  Vec deps;       /* Dep* */
  int nested;
} Usage;

static Dep *dep_new(const TemplateDef *tmpl, const char *type) {
  Dep *d = (Dep *)xmalloc(sizeof(Dep));
  d->tmpl = tmpl;
  d->type = xstrdup(type);
  return d;
}

static void split_type_arg(const char *s, size_t n, size_t *arg_len,
                           size_t *next) {
  int depth = 0;
  for (size_t i = 0; i < n; ++i) {
    char c = s[i];
    if (c == '(' || c == '<' || c == '[')
      ++depth;
    else if (c == ')' || c == '>' || c == ']') {
      if (depth > 0)
        --depth;
    } else if (c == ',' && depth == 0) {
      *arg_len = i;
      *next = i + 1;
      return;
    }
  }
  *arg_len = n;
  *next = n;
}

static void split_type_args(const char *s, Vec *out) {
  while (*s) {
    size_t n = strlen(s);
    size_t len, next;
    split_type_arg(s, n, &len, &next);
    char *arg = trim_copy_n(s, len);
    if (arg[0])
      vec_push(out, arg);
    else
      free(arg);
    if (next >= n)
      break;
    s += next;
  }
}

static char *join_strings(Vec *items, const char *sep) {
  Str out;
  str_init(&out);
  for (size_t i = 0; i < items->len; ++i) {
    if (i)
      str_append(&out, sep);
    str_append(&out, (char *)items->items[i]);
  }
  return str_take(&out);
}

static Usage *usage_new_with_args(const TemplateDef *tmpl, Vec *args) {
  Usage *u = (Usage *)xmalloc(sizeof(Usage));
  u->tmpl = tmpl;
  u->type = join_strings(args, ", ");
  u->args.items = NULL;
  u->args.len = u->args.cap = 0;
  for (size_t i = 0; i < args->len; ++i)
    vec_push(&u->args, xstrdup((char *)args->items[i]));
  u->deps.items = NULL;
  u->deps.len = u->deps.cap = 0;
  u->nested = 0;
  return u;
}

static Usage *usage_new(const TemplateDef *tmpl, const char *type) {
  Vec args;
  args.items = NULL;
  args.len = args.cap = 0;
  if (tmpl->kind == TEMPLATE_FUNCTION ||
      (tmpl->kind == TEMPLATE_TYPE && tmpl->nparams > 1))
    split_type_args(type, &args);
  else
    vec_push(&args, xstrdup(type));
  Usage *u = usage_new_with_args(tmpl, &args);
  for (size_t i = 0; i < args.len; ++i)
    free(args.items[i]);
  free(args.items);
  return u;
}

static void usage_free(Usage *u) {
  for (size_t i = 0; i < u->args.len; ++i)
    free(u->args.items[i]);
  free(u->args.items);
  for (size_t i = 0; i < u->deps.len; ++i) {
    Dep *d = (Dep *)u->deps.items[i];
    free(d->type);
    free(d);
  }
  free(u->deps.items);
  free(u->type);
  free(u);
}

static void copy_deps(Vec *out, Vec *expanded, size_t begin) {
  for (size_t i = begin; i < expanded->len; ++i) {
    Usage *u = (Usage *)expanded->items[i];
    vec_push(out, dep_new(u->tmpl, u->type));
  }
}

static char *canonicalize_type(const char *type, Registry *reg, Vec *expanded) {
  char *clean = strip_comments_literals(type);
  size_t n = strlen(clean);
  size_t i = 0;
  while (i < n && is_space(clean[i]))
    ++i;

  while (i < n) {
    while (i < n && is_space(clean[i]))
      ++i;
    size_t start = i;
    if (i >= n || !is_ident_start(clean[i]))
      break;
    ++i;
    while (i < n && is_ident_char(clean[i]))
      ++i;
    char *word = xstrdup_n(clean + start, i - start);
    if (strcmp(word, "struct") == 0 || strcmp(word, "union") == 0 ||
        strcmp(word, "enum") == 0 || strcmp(word, "class") == 0 ||
        strcmp(word, "const") == 0 || strcmp(word, "volatile") == 0 ||
        strcmp(word, "restrict") == 0) {
      free(word);
      continue;
    }

    size_t k = i;
    while (k < n && is_space(clean[k]))
      ++k;
    if (k >= n || clean[k] != '(') {
      free(word);
      break;
    }
    size_t arg_start = k + 1;
    int depth = 1;
    size_t close = arg_start;
    for (; close < n && depth > 0; ++close) {
      if (clean[close] == '(')
        ++depth;
      else if (clean[close] == ')')
        --depth;
    }
    size_t rest = close;
    while (rest < n && is_space(clean[rest]))
      ++rest;
    TemplateDef *def = registry_find(reg, word);
    if (!def || def->kind == TEMPLATE_FUNCTION || depth != 0 || rest != n) {
      free(word);
      break;
    }

    size_t dep_start = expanded->len;
    Vec canon_args;
    canon_args.items = NULL;
    canon_args.len = canon_args.cap = 0;
    size_t pos = arg_start;
    size_t args_end = close - 1;
    while (pos < args_end) {
      size_t arg_len, next;
      split_type_arg(clean + pos, args_end - pos, &arg_len, &next);
      char *raw = xstrdup_n(clean + pos, arg_len);
      char *trimmed = trim_copy(raw);
      if (!trimmed[0]) {
        free(raw);
        free(trimmed);
        free(word);
        free(clean);
        return xstrdup(type);
      }
      char *canon = canonicalize_type(trimmed, reg, expanded);
      vec_push(&canon_args, canon);
      free(raw);
      free(trimmed);
      pos += next;
      while (pos < args_end && is_space(clean[pos]))
        ++pos;
    }
    if (!canon_args.len) {
      free(word);
      free(clean);
      return xstrdup(type);
    }

    char *canon_all = join_strings(&canon_args, ", ");
    Usage *app = usage_new(def, canon_all);
    copy_deps(&app->deps, expanded, dep_start);
    app->nested = 1;
    vec_push(expanded, app);

    Str mangled;
    str_init(&mangled);
    str_append(&mangled, def->name);
    str_append_char(&mangled, '_');
    for (size_t j = 0; j < canon_args.len; ++j) {
      if (j)
        str_append_char(&mangled, '_');
      str_append(&mangled, (char *)canon_args.items[j]);
    }

    char *prefix = trim_copy_n(clean, start);
    char *result;
    if (!prefix[0]) {
      result = str_take(&mangled);
    } else {
      Str combined;
      str_init(&combined);
      str_append(&combined, prefix);
      str_append_char(&combined, ' ');
      str_append(&combined, mangled.data ? mangled.data : "");
      result = str_take(&combined);
      str_free(&mangled);
    }
    free(prefix);
    free(canon_all);
    for (size_t j = 0; j < canon_args.len; ++j)
      free(canon_args.items[j]);
    free(canon_args.items);
    free(word);
    free(clean);
    return result;
  }

  char *out = normalize_ws(clean);
  free(clean);
  const char *arg;
  TemplateDef *def = registry_match_mangled(reg, out, &arg);
  if (def) {
    size_t dep_start = expanded->len;
    char *canon_arg = canonicalize_type(arg, reg, expanded);
    Usage *app = usage_new(def, canon_arg);
    copy_deps(&app->deps, expanded, dep_start);
    app->nested = 1;
    vec_push(expanded, app);
    free(canon_arg);
  }
  return out;
}

static void scan_usages_impl(const char *code, Registry *reg, Vec *out,
                             int scan_headers);

static void scan_usages(const char *code, Registry *reg, Vec *out) {
  scan_usages_impl(code, reg, out, 1);
}

static void scan_header_deps(const TemplateDef *tmpl, Vec *args, Registry *reg,
                             Vec *out);

static void push_usage_chain(const TemplateDef *tmpl, Vec *canon_args,
                             Vec *expanded, Vec *out) {
  for (size_t i = 0; i < expanded->len; ++i)
    ((Usage *)expanded->items[i])->nested = 1;
  for (size_t i = 0; i < expanded->len; ++i)
    vec_push(out, expanded->items[i]);
  Usage *outer = usage_new_with_args(tmpl, canon_args);
  for (size_t i = 0; i < expanded->len; ++i) {
    Usage *dep = (Usage *)expanded->items[i];
    vec_push(&outer->deps, dep_new(dep->tmpl, dep->type));
  }
  vec_push(out, outer);
}

static int is_declared_param(const TemplateDef *tmpl, const char *arg);

static void scan_template_usage(const TemplateDef *tmpl, const char *raw,
                                Registry *reg, Vec *out, int scan_headers) {
  Vec expanded;
  expanded.items = NULL;
  expanded.len = expanded.cap = 0;
  Vec canon_args;
  canon_args.items = NULL;
  canon_args.len = canon_args.cap = 0;
  Vec raw_args;
  raw_args.items = NULL;
  raw_args.len = raw_args.cap = 0;
  char *single_canon = NULL;
  int single_pushed = 0;

  if (tmpl->nparams > 1) {
    split_type_args(raw, &raw_args);
    if (raw_args.len != tmpl->nparams)
      goto cleanup;
    /* The template header spells its own parameters, e.g. User(F, Payload). */
    int placeholders = 1;
    for (size_t i = 0; i < raw_args.len; ++i)
      if (!is_declared_param(tmpl, (char *)raw_args.items[i]))
        placeholders = 0;
    if (placeholders)
      goto cleanup;
    for (size_t i = 0; i < raw_args.len; ++i) {
      char *canon = canonicalize_type((char *)raw_args.items[i], reg, &expanded);
      if (!canon[0]) {
        free(canon);
        goto cleanup;
      }
      vec_push(&canon_args, canon);
    }
  } else {
    char *trimmed = trim_copy(raw);
    if (is_declared_param(tmpl, trimmed)) {
      free(trimmed);
      goto cleanup;
    }
    free(trimmed);
    single_canon = canonicalize_type(raw, reg, &expanded);
    single_pushed = single_canon[0] != 0;
    if (single_pushed)
      vec_push(&canon_args, single_canon);
  }
  for (size_t i = 0; i < raw_args.len; ++i)
    free(raw_args.items[i]);
  free(raw_args.items);

  if (scan_headers)
    scan_header_deps(tmpl, &canon_args, reg, &expanded);
  push_usage_chain(tmpl, &canon_args, &expanded, out);
  for (size_t i = 0; i < canon_args.len; ++i)
    free(canon_args.items[i]);
  free(canon_args.items);
  if (single_canon && !single_pushed)
    free(single_canon);
  free(expanded.items);
  return;

cleanup:
  for (size_t i = 0; i < canon_args.len; ++i)
    free(canon_args.items[i]);
  free(canon_args.items);
  if (single_canon && !single_pushed)
    free(single_canon);
  for (size_t i = 0; i < raw_args.len; ++i)
    free(raw_args.items[i]);
  free(raw_args.items);
  for (size_t i = 0; i < expanded.len; ++i)
    usage_free((Usage *)expanded.items[i]);
  free(expanded.items);
}

static int is_declared_param(const TemplateDef *tmpl, const char *arg) {
  for (size_t i = 0; i < tmpl->nparams; ++i)
    if (strcmp(tmpl->params[i], arg) == 0)
      return 1;
  return 0;
}

static void scan_function_usage(const TemplateDef *tmpl, const char *raw,
                                Registry *reg, Vec *out, int scan_headers) {
  Vec raw_args;
  raw_args.items = NULL;
  raw_args.len = raw_args.cap = 0;
  split_type_args(raw, &raw_args);
  size_t expected = tmpl->nparams ? tmpl->nparams : 1;
  if (raw_args.len != expected)
    goto done;

  /* The function definition inside a template header spells its own macro
   * parameters, e.g. `my_func(T)(T value)`. Those are not instantiations. */
  int placeholders = 1;
  for (size_t i = 0; i < raw_args.len; ++i)
    if (!is_declared_param(tmpl, (char *)raw_args.items[i]))
      placeholders = 0;
  if (placeholders)
    goto done;

  Vec expanded;
  expanded.items = NULL;
  expanded.len = expanded.cap = 0;
  Vec canon_args;
  canon_args.items = NULL;
  canon_args.len = canon_args.cap = 0;
  for (size_t i = 0; i < raw_args.len; ++i) {
    char *canon = canonicalize_type((char *)raw_args.items[i], reg, &expanded);
    if (!canon[0]) {
      free(canon);
      goto cleanup;
    }
    vec_push(&canon_args, canon);
  }
  if (scan_headers)
    scan_header_deps(tmpl, &canon_args, reg, &expanded);
  push_usage_chain(tmpl, &canon_args, &expanded, out);
  free(canon_args.items);
  free(expanded.items);
  goto done;

cleanup:
  for (size_t i = 0; i < canon_args.len; ++i)
    free(canon_args.items[i]);
  free(canon_args.items);
  for (size_t i = 0; i < expanded.len; ++i)
    usage_free((Usage *)expanded.items[i]);
  free(expanded.items);
done:
  for (size_t i = 0; i < raw_args.len; ++i)
    free(raw_args.items[i]);
  free(raw_args.items);
}

static char *substitute_params(const char *code, const TemplateDef *tmpl,
                               Vec *args) {
  Str out;
  str_init(&out);
  for (size_t i = 0; code[i];) {
    if (is_ident_start(code[i])) {
      size_t start = i++;
      while (code[i] && is_ident_char(code[i]))
        ++i;
      size_t len = i - start;
      const char *replacement = NULL;
      for (size_t p = 0; p < tmpl->nparams && p < args->len; ++p) {
        size_t plen = strlen(tmpl->params[p]);
        if (plen == len && memcmp(code + start, tmpl->params[p], plen) == 0) {
          replacement = (char *)args->items[p];
          break;
        }
      }
      if (replacement)
        str_append(&out, replacement);
      else
        str_append_n(&out, code + start, len);
      continue;
    }
    str_append_char(&out, code[i++]);
  }
  return str_take(&out);
}

static void scan_header_deps(const TemplateDef *tmpl, Vec *args, Registry *reg,
                             Vec *out) {
  if (!tmpl->header || !args->len)
    return;
  char *code = read_file(tmpl->header, NULL);
  if (!code)
    return;
  char *substituted = substitute_params(code, tmpl, args);
  Vec found;
  found.items = NULL;
  found.len = found.cap = 0;
  scan_usages_impl(substituted, reg, &found, 0);

  char *self = join_strings(args, ", ");
  for (size_t i = 0; i < found.len;) {
    Usage *u = (Usage *)found.items[i];
    if (u->tmpl == tmpl && strcmp(u->type, self) == 0) {
      usage_free(u);
      for (size_t j = i + 1; j < found.len; ++j)
        found.items[j - 1] = found.items[j];
      --found.len;
    } else {
      ++i;
    }
  }
  for (size_t i = 0; i < found.len; ++i)
    vec_push(out, found.items[i]);
  free(found.items);
  free(self);
  free(substituted);
  free(code);
}

static void scan_usages_impl(const char *code, Registry *reg, Vec *out,
                             int scan_headers) {
  if (!registry_may_contain(reg, code))
    return;
  char *clean = strip_comments_literals(code);
  size_t n = strlen(clean);
  for (size_t i = 0; i < n;) {
    if (i == 0 || clean[i - 1] == '\n') {
      size_t k = i;
      while (k < n && (clean[k] == ' ' || clean[k] == '\t'))
        ++k;
      if (k < n && clean[k] == '#') {
        while (i < n && clean[i] != '\n')
          ++i;
        continue;
      }
    }
    if (!is_ident_start(clean[i])) {
      ++i;
      continue;
    }
    size_t end = i + 1;
    while (end < n && is_ident_char(clean[end]))
      ++end;
    char *ident = xstrdup_n(clean + i, end - i);
    TemplateDef *def = registry_find(reg, ident);
    if (def) {
      size_t k = end;
      while (k < n && is_space(clean[k]))
        ++k;
      if (k < n && clean[k] == '(') {
        size_t begin = k + 1;
        int depth = 1;
        size_t close = begin;
        for (; close < n && depth > 0; ++close) {
          if (clean[close] == '(')
            ++depth;
          else if (clean[close] == ')')
            --depth;
        }
        if (depth == 0 && close > begin) {
          char *raw = xstrdup_n(clean + begin, close - begin - 1);
          char *type = trim_copy(raw);
          if (type[0]) {
            if (def->kind == TEMPLATE_FUNCTION)
              scan_function_usage(def, type, reg, out, scan_headers);
            else
              scan_template_usage(def, type, reg, out, scan_headers);
          }
          free(type);
          free(raw);
          i = close;
          free(ident);
          continue;
        }
      }
    } else {
      const char *arg;
      TemplateDef *mangled = registry_match_mangled(reg, ident, &arg);
      if (mangled)
        scan_template_usage(mangled, arg, reg, out, scan_headers);
    }
    free(ident);
    i = end;
  }
  free(clean);
}

/* ------------------------------ generator ------------------------------ */

typedef struct {
  Registry *reg;
  TypeIndex *idx;
  int dry_run;
  int verbose;
  unsigned added;
  unsigned skipped;
  Set warned;
  Map files;
} EmitCtx;

typedef struct {
  const TemplateDef *tmpl;
  Vec usages;
  Set includes;
} ImplGroup;

/* Generated headers are queried once per distinct usage type, not once per
 * call site. This is the hot path for dry-run/no-op build-script invocations. */
static const char *cached_file(EmitCtx *ctx, const char *path) {
  const char *cached = map_get(&ctx->files, path);
  if (cached)
    return cached;
  char *content = read_file(path, NULL);
  map_set(&ctx->files, path, content ? content : "");
  free(content);
  return map_get(&ctx->files, path);
}

static char *instantiation_marker(const char *name, const char *type) {
  Str out;
  str_init(&out);
  str_append(&out, "/* c-autotmpl: ");
  str_append(&out, name);
  str_append_char(&out, '(');
  str_append(&out, type);
  str_append(&out, ") */");
  return str_take(&out);
}

static char *presence_macro_prefixed(const char *prefix, const char *name,
                                     const char *type) {
  Str key;
  str_init(&key);
  str_append(&key, name);
  str_append_char(&key, '_');
  str_append(&key, type);
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < key.len; ++i) {
    hash ^= (unsigned char)key.data[i];
    hash *= 16777619u;
  }
  Str out;
  str_init(&out);
  str_append(&out, prefix);
  for (size_t i = 0; i < key.len; ++i) {
    unsigned char c = (unsigned char)key.data[i];
    char upper;
    if (c >= 'a' && c <= 'z')
      upper = (char)(c - 'a' + 'A');
    else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
      upper = (char)c;
    else
      upper = '_';
    str_append_char(&out, upper);
  }
  char hex[9];
  snprintf(hex, sizeof(hex), "%08X", hash);
  str_append_char(&out, '_');
  str_append(&out, hex);
  str_free(&key);
  return str_take(&out);
}

static char *presence_macro(const char *name, const char *type) {
  return presence_macro_prefixed("C_AUTOTMPL_", name, type);
}

static char *impl_presence_macro(const char *name, const char *type) {
  return presence_macro_prefixed("C_AUTOTMPL_IMPL_", name, type);
}

static int starts_with_word(const char *line, const char *word) {
  size_t n = strlen(word);
  return strncmp(line, word, n) == 0 &&
         (line[n] == '\0' || is_space(line[n]));
}

static char *define_type_from_line(const char *line, const char *param) {
  char *trimmed = trim_copy(line);
  if (!starts_with_word(trimmed, "#define")) {
    free(trimmed);
    return NULL;
  }
  char *rest = trimmed + strlen("#define");
  while (is_space(*rest))
    ++rest;
  size_t n = strlen(param);
  if (strncmp(rest, param, n) || (rest[n] && !is_space(rest[n]))) {
    free(trimmed);
    return NULL;
  }
  char *type = trim_copy(rest + n);
  free(trimmed);
  return type;
}

static int include_matches(const char *line, const char *include) {
  char *trimmed = trim_copy(line);
  int result = 0;
  if (strncmp(trimmed, "#include", 8) == 0) {
    char *p = trimmed + 8;
    while (is_space(*p))
      ++p;
    if (*p == '"') {
      char *end = strchr(p + 1, '"');
      if (end) {
        char *path = xstrdup_n(p + 1, (size_t)(end - p - 1));
        result = strcmp(path, include) == 0;
        free(path);
      }
    }
  }
  free(trimmed);
  return result;
}

static int undef_matches(const char *line, const char *param) {
  char *trimmed = trim_copy(line);
  int result = starts_with_word(trimmed, "#undef") &&
               strcmp(trim_copy(trimmed + strlen("#undef")), param) == 0;
  free(trimmed);
  return result;
}

static const char *next_line(const char *p) {
  const char *nl = strchr(p, '\n');
  return nl ? nl + 1 : p + strlen(p);
}

static size_t param_count(const TemplateDef *tmpl) {
  if (tmpl->nparams)
    return tmpl->nparams;
  return 1;
}

static const char *param_name(const TemplateDef *tmpl, size_t i) {
  if (i < tmpl->nparams)
    return tmpl->params[i];
  return "T";
}

static int all_seen(const char *seen, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (!seen[i])
      return 0;
  return 1;
}

static int has_manual_block(const char *content, const TemplateDef *tmpl,
                            Vec *args) {
  size_t count = param_count(tmpl);
  if (args->len < count)
    return 0;
  const char *param = param_name(tmpl, 0);
  const char *type = (char *)args->items[0];
  const char *p = content;
  while (*p) {
    char *line = xstrdup_n(p, (size_t)(next_line(p) - p));
    char *trimmed = trim_copy(line);
    if (trimmed[0] == '/' && trimmed[1] == '*' &&
        strstr(trimmed, "c-autotmpl:") != NULL) {
      /* Skip a generated block so its own `#define ...` lines are not
         mistaken for a hand-written instantiation. */
      char *seen = (char *)xmalloc(count);
      memset(seen, 0, count);
      const char *q = next_line(p);
      while (*q) {
        char *body = xstrdup_n(q, (size_t)(next_line(q) - q));
        for (size_t i = 0; i < count; ++i)
          if (undef_matches(body, param_name(tmpl, i)))
            seen[i] = 1;
        free(body);
        q = next_line(q);
        if (all_seen(seen, count))
          break;
      }
      free(seen);
      while (*q) {
        char *body = xstrdup_n(q, (size_t)(next_line(q) - q));
        char *body_trim = trim_copy(body);
        int stop = body_trim[0] && strncmp(body_trim, "#endif", 6) != 0;
        free(body_trim);
        free(body);
        if (stop)
          break;
        q = next_line(q);
      }
      p = q;
      free(trimmed);
      free(line);
      continue;
    }

    char *defined = define_type_from_line(trimmed, param);
    if (defined && strcmp(defined, type) == 0) {
      const char *q = next_line(p);
      char *seen_def = (char *)xmalloc(count);
      char *seen_undef = (char *)xmalloc(count);
      memset(seen_def, 0, count);
      memset(seen_undef, 0, count);
      int saw_include = 0;
      for (int i = 0; i < 14 + 5 * (int)count && *q; ++i) {
        char *body = xstrdup_n(q, (size_t)(next_line(q) - q));
        if (include_matches(body, tmpl->include))
          saw_include = 1;
        for (size_t j = 0; j < count; ++j) {
          const char *name = param_name(tmpl, j);
          char *line_define = define_type_from_line(body, name);
          if (line_define && strcmp(line_define, (char *)args->items[j]) == 0)
            seen_def[j] = 1;
          free(line_define);
          if (undef_matches(body, name))
            seen_undef[j] = 1;
        }
        free(body);
        if (saw_include && all_seen(seen_def, count) &&
            all_seen(seen_undef, count))
          break;
        q = next_line(q);
      }
      int manual = saw_include && all_seen(seen_def, count) &&
                   all_seen(seen_undef, count);
      free(seen_def);
      free(seen_undef);
      free(defined);
      free(trimmed);
      free(line);
      if (manual)
        return 1;
      p = next_line(p);
      continue;
    }
    free(defined);
    free(trimmed);
    free(line);
    p = next_line(p);
  }
  return 0;
}

static int has_instantiation(const char *path, const TemplateDef *tmpl,
                             Usage *usage, EmitCtx *ctx) {
  const char *content = cached_file(ctx, path);
  char *marker = instantiation_marker(tmpl->name, usage->type);
  int found = strstr(content, marker) != NULL;
  if (!found)
    found = has_manual_block(content, tmpl, &usage->args);
  free(marker);
  return found;
}

static char *guard_for_arg(const char *spelling, EmitCtx *ctx) {
  if (!needs_guard(spelling) || is_constructor_arg(spelling, ctx->reg))
    return xstrdup("");
  char *type = strip_qualifiers(spelling);
  const char *arg;
  TemplateDef *def = registry_match_mangled(ctx->reg, type, &arg);
  if (def) {
    char *guard = guard_for_arg(arg, ctx);
    free(type);
    return guard;
  }
  char *name = last_identifier(type);
  const char *guard = map_get(&ctx->idx->guards, name);
  char *result = xstrdup(guard ? guard : "");
  free(name);
  free(type);
  return result;
}

static int is_generated_type(const char *spelling, EmitCtx *ctx) {
  char *type = strip_qualifiers(spelling);
  const char *arg;
  int result = registry_match_mangled(ctx->reg, type, &arg) != NULL;
  free(type);
  return result;
}

static int collect_guards(Usage *usage, EmitCtx *ctx, Set *guards) {
  int missing = 0;
  for (size_t i = 0; i < usage->args.len; ++i) {
    const char *arg = (char *)usage->args.items[i];
    if (!needs_guard(arg) || is_constructor_arg(arg, ctx->reg))
      continue;
    char *guard = guard_for_arg(arg, ctx);
    if (guard[0])
      set_add(guards, guard);
    else if (!is_generated_type(arg, ctx))
      missing = 1;
    free(guard);
  }
  return missing;
}

static char *make_block(Usage *usage, Set *guards) {
  const TemplateDef *tmpl = usage->tmpl;
  Str out;
  str_init(&out);
  char *marker = instantiation_marker(tmpl->name, usage->type);
  char *presence = presence_macro(tmpl->name, usage->type);
  str_append(&out, marker);
  str_append_char(&out, '\n');
  str_append(&out, "#ifndef ");
  str_append(&out, presence);
  str_append_char(&out, '\n');

  for (size_t i = 0; i < guards->len; ++i) {
    str_append(&out, "#ifdef ");
    str_append(&out, (char *)guards->items[i]);
    str_append_char(&out, '\n');
  }
  str_append(&out, "#define ");
  str_append(&out, presence);
  str_append_char(&out, '\n');

  size_t count = param_count(tmpl);
  for (size_t i = 0; i < count; ++i) {
    str_append(&out, "#define ");
    str_append(&out, param_name(tmpl, i));
    str_append_char(&out, ' ');
    str_append(&out, (char *)usage->args.items[i]);
    str_append_char(&out, '\n');
  }
  if (tmpl->has_tmpl_header)
    str_append(&out, "#define TMPL_HEADER 1\n");
  str_append(&out, "#include \"");
  str_append(&out, tmpl->include);
  str_append_char(&out, '"');
  str_append_char(&out, '\n');
  if (tmpl->has_tmpl_header)
    str_append(&out, "#undef TMPL_HEADER\n");
  for (size_t i = count; i > 0; --i) {
    str_append(&out, "#undef ");
    str_append(&out, param_name(tmpl, i - 1));
    str_append_char(&out, '\n');
  }
  for (size_t i = guards->len; i > 0; --i)
    str_append(&out, "#endif\n");
  str_append(&out, "#endif\n\n");
  free(marker);
  free(presence);
  return str_take(&out);
}

static int append_block(const char *path, const char *block, EmitCtx *ctx) {
  char *dir = path_dirname(path);
  make_dir(dir);
  free(dir);

  if (ctx->dry_run)
    return 0;

  const char *content = cached_file(ctx, path);
  size_t len = strlen(content);
  Str out;
  str_init(&out);
  if (!content || len == 0) {
    char *guard = generated_header_guard(path);
    str_append(&out, "#ifndef ");
    str_append(&out, guard);
    str_append(&out, "\n#define ");
    str_append(&out, guard);
    str_append(&out, "\n\n");
    str_append(&out, block);
    str_append(&out, "#endif\n");
    free(guard);
  } else {
    const char *p = content;
    size_t insert = len;
    while (*p) {
      char *line = xstrdup_n(p, (size_t)(next_line(p) - p));
      char *trimmed = trim_copy(line);
      if (strncmp(trimmed, "#endif", 6) == 0)
        insert = (size_t)(p - content);
      free(trimmed);
      free(line);
      const char *next = next_line(p);
      if (next == p)
        break;
      p = next;
    }
    if (insert > len)
      insert = len;
    str_append_n(&out, content, insert);
    str_append(&out, block);
    str_append_n(&out, content + insert, len - insert);
  }
  int rc = write_file(path, out.data ? out.data : "", out.len);
  if (rc == 0)
    map_set(&ctx->files, path, out.data ? out.data : "");
  str_free(&out);
  return rc;
}

/* ------------------------- implementation TUs --------------------------- */

static char *make_impl_block(Usage *usage, Set *guards) {
  const TemplateDef *tmpl = usage->tmpl;
  Str out;
  str_init(&out);
  char *marker = instantiation_marker(tmpl->name, usage->type);
  char *presence = impl_presence_macro(tmpl->name, usage->type);
  str_append(&out, marker);
  str_append_char(&out, '\n');
  str_append(&out, "#ifndef ");
  str_append(&out, presence);
  str_append_char(&out, '\n');
  for (size_t i = 0; i < guards->len; ++i) {
    str_append(&out, "#ifdef ");
    str_append(&out, (char *)guards->items[i]);
    str_append_char(&out, '\n');
  }
  str_append(&out, "#define ");
  str_append(&out, presence);
  str_append_char(&out, '\n');
  size_t count = param_count(tmpl);
  for (size_t i = 0; i < count; ++i) {
    str_append(&out, "#define ");
    str_append(&out, param_name(tmpl, i));
    str_append_char(&out, ' ');
    str_append(&out, (char *)usage->args.items[i]);
    str_append_char(&out, '\n');
  }
  str_append(&out, "#include \"");
  str_append(&out, tmpl->impl_include);
  str_append_char(&out, '"');
  str_append_char(&out, '\n');
  for (size_t i = count; i > 0; --i) {
    str_append(&out, "#undef ");
    str_append(&out, param_name(tmpl, i - 1));
    str_append_char(&out, '\n');
  }
  for (size_t i = guards->len; i > 0; --i)
    str_append(&out, "#endif\n");
  str_append(&out, "#endif\n\n");
  free(marker);
  free(presence);
  return str_take(&out);
}

static void collect_impl_include_arg(const char *spelling, EmitCtx *ctx,
                                     Set *includes) {
  if (!needs_guard(spelling) || is_constructor_arg(spelling, ctx->reg))
    return;
  char *type = strip_qualifiers(spelling);
  const char *arg;
  if (registry_match_mangled(ctx->reg, type, &arg)) {
    collect_impl_include_arg(arg, ctx, includes);
    free(type);
    return;
  }
  char *name = last_identifier(type);
  const char *guard = map_get(&ctx->idx->guards, name);
  if (guard) {
    const char *header = map_get(&ctx->idx->guard_headers, guard);
    if (header)
      set_add(includes, header);
  }
  free(name);
  free(type);
}

static void collect_impl_includes(Usage *usage, EmitCtx *ctx, Set *includes) {
  for (size_t i = 0; i < usage->args.len; ++i)
    collect_impl_include_arg((char *)usage->args.items[i], ctx, includes);
}

static char *build_impl_prologue(const TemplateDef *tmpl, Set *includes) {
  char *impl_dir = path_dirname(tmpl->impl);
  Str out;
  str_init(&out);
  str_append(&out, "/* Generated by c-autotmpl. Do not edit. */\n");
  str_append(&out, "#undef TMPL_HEADER\n");
  for (size_t i = 0; i < includes->len; ++i) {
    char *rel = path_relative(impl_dir, (char *)includes->items[i]);
    str_append(&out, "#include \"");
    str_append(&out, rel);
    str_append_char(&out, '"');
    str_append_char(&out, '\n');
    free(rel);
  }
  str_append_char(&out, '\n');
  free(impl_dir);
  return str_take(&out);
}

static int ensure_impl_file(const TemplateDef *tmpl, Set *includes,
                            EmitCtx *ctx) {
  char *dir = path_dirname(tmpl->impl);
  make_dir(dir);
  free(dir);

  char *prologue = build_impl_prologue(tmpl, includes);
  const char *content = cached_file(ctx, tmpl->impl);
  Str out;
  str_init(&out);
  int changed = 0;

  if (!content[0]) {
    str_append(&out, prologue);
    changed = 1;
  } else if (!strstr(content, "#undef TMPL_HEADER")) {
    str_append(&out, prologue);
    str_append(&out, content);
    changed = 1;
  } else {
    Str missing;
    str_init(&missing);
    const char *p = prologue;
    while (*p) {
      char *line = xstrdup_n(p, (size_t)(next_line(p) - p));
      char *trimmed = trim_copy(line);
      if (strncmp(trimmed, "#include", 8) == 0 &&
          !strstr(content, line))
        str_append(&missing, line);
      free(trimmed);
      free(line);
      p = next_line(p);
    }
    int inserted = 0;
    p = content;
    while (*p) {
      char *line = xstrdup_n(p, (size_t)(next_line(p) - p));
      char *trimmed = trim_copy(line);
      str_append(&out, line);
      if (!inserted && strcmp(trimmed, "#undef TMPL_HEADER") == 0) {
        if (missing.len) {
          str_append(&out, missing.data);
          changed = 1;
        }
        inserted = 1;
      }
      free(trimmed);
      free(line);
      p = next_line(p);
    }
    if (!inserted) {
      str_clear(&out);
      str_append(&out, prologue);
      str_append(&out, content);
      changed = 1;
    }
    str_free(&missing);
  }

  if (ctx->dry_run || !changed || !out.data) {
    free(prologue);
    str_free(&out);
    return 0;
  }
  int rc = write_file(tmpl->impl, out.data, out.len);
  if (rc == 0)
    map_set(&ctx->files, tmpl->impl, out.data);
  free(prologue);
  str_free(&out);
  return rc;
}

static int append_impl_block(const char *path, const char *block, EmitCtx *ctx) {
  char *dir = path_dirname(path);
  make_dir(dir);
  free(dir);
  if (ctx->dry_run)
    return 0;
  const char *content = cached_file(ctx, path);
  Str out;
  str_init(&out);
  str_append(&out, content);
  if (content[0] && out.data && out.data[out.len - 1] != '\n')
    str_append_char(&out, '\n');
  str_append(&out, block);
  int rc = write_file(path, out.data ? out.data : "", out.len);
  if (rc == 0)
    map_set(&ctx->files, path, out.data ? out.data : "");
  str_free(&out);
  return rc;
}

static int emit_impl_usage(Usage *usage, EmitCtx *ctx) {
  const TemplateDef *tmpl = usage->tmpl;
  if (has_instantiation(tmpl->impl, tmpl, usage, ctx))
    return 0;

  Set guards;
  guards.items = NULL;
  guards.len = guards.cap = 0;
  if (collect_guards(usage, ctx, &guards)) {
    for (size_t i = 0; i < guards.len; ++i)
      free(guards.items[i]);
    free(guards.items);
    return 0;
  }

  char *block = make_impl_block(usage, &guards);
  if (append_impl_block(tmpl->impl, block, ctx) != 0) {
    fprintf(stderr, "error: failed writing %s\n", tmpl->impl);
    free(block);
    for (size_t i = 0; i < guards.len; ++i)
      free(guards.items[i]);
    free(guards.items);
    return -1;
  }
  ++ctx->added;
  if (ctx->verbose)
    printf("%s impl %s(%s) in %s\n", ctx->dry_run ? "would generate" : "generated",
           tmpl->name, usage->type, tmpl->impl);
  free(block);
  for (size_t i = 0; i < guards.len; ++i)
    free(guards.items[i]);
  free(guards.items);
  return 0;
}

static char *impl_makefile_path(const char *root) {
  char *gen = path_join(root, "gen");
  char *impl = path_join(gen, "impl");
  char *path = path_join(impl, "c-autotmpl.mk");
  free(gen);
  free(impl);
  return path_absolute(path);
}

static int write_impl_makefile(const char *root, Vec *groups, EmitCtx *ctx) {
  if (!groups->len)
    return 0;
  char *path = impl_makefile_path(root);
  char *dir = path_dirname(path);
  make_dir(dir);
  free(dir);

  Str out;
  str_init(&out);
  str_append(&out, "# Generated by c-autotmpl. Do not edit.\n");
  for (size_t i = 0; i < groups->len; ++i) {
    ImplGroup *group = (ImplGroup *)groups->items[i];
    if (!group->usages.len)
      continue;
    char *rel = path_relative(root, group->tmpl->impl);
    str_append(&out, "C_AUTOTMPL_IMPL_SRCS += ");
    str_append(&out, rel);
    str_append_char(&out, '\n');
    free(rel);
  }

  char *old = read_file(path, NULL);
  int changed = !old || strcmp(old, out.data ? out.data : "") != 0;
  free(old);
  if (ctx->verbose && changed)
    printf("%s makefile fragment %s\n",
           ctx->dry_run ? "would generate" : "generated", path);
  int rc = 0;
  if (changed && !ctx->dry_run)
    rc = write_file(path, out.data ? out.data : "", out.len);
  free(path);
  str_free(&out);
  if (rc != 0)
    fprintf(stderr, "error: failed writing makefile fragment\n");
  return rc;
}

static int chain_has_usage(Vec *usages, size_t begin, size_t end,
                           const Dep *dep) {
  for (size_t i = begin; i <= end; ++i) {
    Usage *u = (Usage *)usages->items[i];
    if (u->tmpl == dep->tmpl && strcmp(u->type, dep->type) == 0)
      return 1;
  }
  return 0;
}

static int emit_usage(Usage *usage, const char *output_header, EmitCtx *ctx,
                      Vec *chain, size_t begin, size_t end) {
  if (has_instantiation(output_header, usage->tmpl, usage, ctx))
    return 0;

  Set guards;
  guards.items = NULL;
  guards.len = guards.cap = 0;
  for (size_t i = 0; i < usage->deps.len; ++i) {
    Dep *dep = (Dep *)usage->deps.items[i];
    if (strcmp(dep->tmpl->generated, output_header) == 0)
      continue;
    /* The whole chain is written into this header before the outer block, so
     * a dependency already in the chain needs no cross-header guard. */
    if (chain && chain_has_usage(chain, begin, end, dep))
      continue;
    char *guard = generated_header_guard(dep->tmpl->generated);
    set_add(&guards, guard);
    free(guard);
  }

  if (collect_guards(usage, ctx, &guards)) {
    Str key;
    str_init(&key);
    str_append(&key, usage->tmpl->name);
    str_append(&key, "(");
    str_append(&key, usage->type);
    str_append(&key, ")");
    char *k = str_take(&key);
    if (set_add(&ctx->warned, k)) {
      fprintf(stderr, "warning: skipping %s: no traditional include guard found for type\n", k);
      ++ctx->skipped;
    }
    free(k);
    for (size_t i = 0; i < guards.len; ++i)
      free(guards.items[i]);
    free(guards.items);
    return 0;
  }

  char *block = make_block(usage, &guards);
  if (append_block(output_header, block, ctx) != 0) {
    fprintf(stderr, "error: failed writing %s\n", output_header);
    free(block);
    for (size_t i = 0; i < guards.len; ++i)
      free(guards.items[i]);
    free(guards.items);
    return -1;
  }
  ++ctx->added;
  if (ctx->verbose)
    printf("%s %s(%s) in %s\n", ctx->dry_run ? "would generate" : "generated",
           usage->tmpl->name, usage->type, output_header);
  free(block);
  for (size_t i = 0; i < guards.len; ++i)
    free(guards.items[i]);
  free(guards.items);
  return 0;
}

/* Only names that can resolve to a guarded header need indexing. Mangled
 * applications recurse to their innermost argument, so Box(List(int)) asks
 * only for int and does not force a scan of every unrelated header. */
static void add_wanted_type(const char *spelling, Registry *reg, Set *wanted) {
  if (!needs_guard(spelling) || is_constructor_arg(spelling, reg))
    return;
  char *type = strip_qualifiers(spelling);
  const char *arg;
  if (registry_match_mangled(reg, type, &arg)) {
    add_wanted_type(arg, reg, wanted);
    free(type);
    return;
  }
  char *name = last_identifier(type);
  if (name[0] && !builtin_name(name))
    set_add(wanted, name);
  free(name);
  free(type);
}

static void add_wanted_usage(Usage *usage, Registry *reg, Set *wanted) {
  for (size_t i = 0; i < usage->args.len; ++i)
    add_wanted_type((char *)usage->args.items[i], reg, wanted);
}

/* -------------------------------- CLI ---------------------------------- */

typedef struct {
  Vec inputs;
  char *root;
  int dry_run;
  int verbose;
} Options;

static void usage(const char *prog) {
  fprintf(stderr,
          "usage: %s [--root DIR] [--dry-run] [--verbose] <file-or-dir>...\n",
          prog);
}

static int parse_args(int argc, char **argv, Options *opt) {
  opt->inputs.items = NULL;
  opt->inputs.len = opt->inputs.cap = 0;
  opt->root = NULL;
  opt->dry_run = 0;
  opt->verbose = 0;
  for (int i = 1; i < argc; ++i) {
    const char *a = argv[i];
    if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
      usage(argv[0]);
      return -1;
    }
    if (strcmp(a, "--root") == 0 || strcmp(a, "-r") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: %s needs a directory\n", a);
        return 0;
      }
      free(opt->root);
      opt->root = xstrdup(argv[++i]);
      continue;
    }
    if (strcmp(a, "--dry-run") == 0 || strcmp(a, "-n") == 0) {
      opt->dry_run = 1;
      continue;
    }
    if (strcmp(a, "--verbose") == 0 || strcmp(a, "-v") == 0) {
      opt->verbose = 1;
      continue;
    }
    if (strcmp(a, "--") == 0) {
      for (++i; i < argc; ++i)
        vec_push(&opt->inputs, xstrdup(argv[i]));
      break;
    }
    if (a[0] == '-') {
      fprintf(stderr, "error: unknown option %s\n", a);
      usage(argv[0]);
      return 0;
    }
    vec_push(&opt->inputs, xstrdup(a));
  }
  return 1;
}

typedef struct {
  Vec *files;
  Set *seen;
} FileCollect;

static int is_source_ext(const char *ext) {
  return strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0 ||
         strcmp(ext, ".cc") == 0 || strcmp(ext, ".cpp") == 0 ||
         strcmp(ext, ".cxx") == 0 || strcmp(ext, ".hh") == 0 ||
         strcmp(ext, ".hpp") == 0 || strcmp(ext, ".hxx") == 0;
}

static int visit_source(const char *path, void *ctx) {
  FileCollect *collect = (FileCollect *)ctx;
  char *ext = path_ext(path);
  int source = is_source_ext(ext);
  free(ext);
  if (!source)
    return 0;
  char *abs = path_absolute(path);
  if (set_add(collect->seen, abs))
    vec_push(collect->files, abs);
  else
    free(abs);
  return 0;
}

static int path_ancestor(const char *root, const char *path) {
  if (strcmp(root, path) == 0 || strcmp(root, "/") == 0)
    return 1;
  size_t n = strlen(root);
  while (n > 1 && root[n - 1] == '/')
    --n;
  return strlen(path) > n && strncmp(path, root, n) == 0 && path[n] == '/';
}

static char *determine_root(Options *opt, Vec *files) {
  if (opt->root)
    return path_absolute(opt->root);
  if (!files->len)
    return path_absolute(".");
  char *root = path_dirname((char *)files->items[0]);
  for (size_t i = 1; i < files->len; ++i) {
    while (!path_ancestor(root, (char *)files->items[i])) {
      char *parent = path_dirname(root);
      int at_top = strcmp(parent, root) == 0;
      free(root);
      root = parent;
      if (at_top)
        break;
    }
  }
  return root;
}

typedef struct {
  char *path;
  Vec usages;
} FileUsages;

static ImplGroup *find_impl_group(Vec *groups, const TemplateDef *tmpl) {
  for (size_t i = 0; i < groups->len; ++i) {
    ImplGroup *group = (ImplGroup *)groups->items[i];
    if (group->tmpl == tmpl)
      return group;
  }
  return NULL;
}

int main(int argc, char **argv) {
  Options opt;
  int parsed = parse_args(argc, argv, &opt);
  if (parsed < 0)
    return argc > 1 &&
                   (strcmp(argv[1], "--help") == 0 ||
                    strcmp(argv[1], "-h") == 0)
               ? 0
               : 1;
  if (!parsed)
    return 1;
  if (!opt.inputs.len) {
    usage(argv[0]);
    return 1;
  }

  Vec files;
  files.items = NULL;
  files.len = files.cap = 0;
  Set seen;
  seen.items = NULL;
  seen.len = seen.cap = 0;
  for (size_t i = 0; i < opt.inputs.len; ++i) {
    const char *input = (char *)opt.inputs.items[i];
    if (path_is_dir(input)) {
      visit_source(input, &(FileCollect){&files, &seen});
      walk_dir(input, visit_source, &(FileCollect){&files, &seen});
    } else if (path_is_file(input)) {
      char *abs = path_absolute(input);
      if (set_add(&seen, abs))
        vec_push(&files, abs);
      else
        free(abs);
    } else {
      fprintf(stderr, "error: no such file or directory: %s\n", input);
    }
  }
  if (!files.len) {
    fputs("error: no source files found\n", stderr);
    return 1;
  }

  char *root = determine_root(&opt, &files);
  Registry reg;
  reg.items.items = NULL;
  reg.items.len = reg.items.cap = 0;
  registry_scan(&reg, root);
  if (!reg.items.len) {
    if (opt.verbose)
      printf("no templates found under %s\n", root);
    return 0;
  }

  Set generated_outputs;
  generated_outputs.items = NULL;
  generated_outputs.len = generated_outputs.cap = 0;
  for (size_t i = 0; i < reg.items.len; ++i) {
    TemplateDef *def = (TemplateDef *)reg.items.items[i];
    set_add(&generated_outputs, def->generated);
    if (def->impl)
      set_add(&generated_outputs, def->impl);
  }

  Vec file_usages;
  file_usages.items = NULL;
  file_usages.len = file_usages.cap = 0;
  Set wanted_names;
  wanted_names.items = NULL;
  wanted_names.len = wanted_names.cap = 0;

  for (size_t i = 0; i < files.len; ++i) {
    char *path = (char *)files.items[i];
    if (set_has(&generated_outputs, path))
      continue;
    char *content = read_file(path, NULL);
    if (!content)
      continue;
    FileUsages *fu = (FileUsages *)xmalloc(sizeof(FileUsages));
    fu->path = path;
    fu->usages.items = NULL;
    fu->usages.len = fu->usages.cap = 0;
    scan_usages(content, &reg, &fu->usages);
    free(content);
    for (size_t j = 0; j < fu->usages.len; ++j)
      add_wanted_usage((Usage *)fu->usages.items[j], &reg, &wanted_names);
    vec_push(&file_usages, fu);
  }

  TypeIndex idx;
  idx.guards.items = NULL;
  idx.guards.len = idx.guards.cap = 0;
  idx.guard_headers.items = NULL;
  idx.guard_headers.len = idx.guard_headers.cap = 0;
  if (wanted_names.len)
    type_index_scan(&idx, root, &wanted_names);

  EmitCtx emit;
  emit.reg = &reg;
  emit.idx = &idx;
  emit.dry_run = opt.dry_run;
  emit.verbose = opt.verbose;
  emit.added = 0;
  emit.skipped = 0;
  emit.warned.items = NULL;
  emit.warned.len = emit.warned.cap = 0;
  emit.files.items = NULL;
  emit.files.len = emit.files.cap = 0;

  for (size_t f = 0; f < file_usages.len; ++f) {
    FileUsages *fu = (FileUsages *)file_usages.items[f];
    for (size_t i = 0; i < fu->usages.len; ++i) {
      if (((Usage *)fu->usages.items[i])->nested)
        continue;
      size_t begin = i;
      while (begin > 0 && ((Usage *)fu->usages.items[begin - 1])->nested)
        --begin;
      const char *output = ((Usage *)fu->usages.items[i])->tmpl->generated;
      for (size_t j = begin; j <= i; ++j) {
        Usage *u = (Usage *)fu->usages.items[j];
        if (emit_usage(u, output, &emit, &fu->usages, begin, i) != 0)
          return 1;
        if (j != i && strcmp(u->tmpl->generated, output) != 0 &&
            emit_usage(u, u->tmpl->generated, &emit, NULL, 0, 0) != 0)
          return 1;
      }
    }
  }

  Vec impl_groups;
  impl_groups.items = NULL;
  impl_groups.len = impl_groups.cap = 0;
  for (size_t i = 0; i < reg.items.len; ++i) {
    TemplateDef *def = (TemplateDef *)reg.items.items[i];
    if (!def->impl)
      continue;
    ImplGroup *group = (ImplGroup *)xmalloc(sizeof(ImplGroup));
    group->tmpl = def;
    group->usages.items = NULL;
    group->usages.len = group->usages.cap = 0;
    group->includes.items = NULL;
    group->includes.len = group->includes.cap = 0;
    vec_push(&impl_groups, group);
  }
  for (size_t f = 0; f < file_usages.len; ++f) {
    FileUsages *fu = (FileUsages *)file_usages.items[f];
    for (size_t i = 0; i < fu->usages.len; ++i) {
      Usage *usage = (Usage *)fu->usages.items[i];
      ImplGroup *group = find_impl_group(&impl_groups, usage->tmpl);
      if (group)
        vec_push(&group->usages, usage);
    }
  }
  for (size_t i = 0; i < impl_groups.len; ++i) {
    ImplGroup *group = (ImplGroup *)impl_groups.items[i];
    if (!group->usages.len)
      continue;
    for (size_t j = 0; j < group->usages.len; ++j)
      collect_impl_includes((Usage *)group->usages.items[j], &emit,
                            &group->includes);
    if (ensure_impl_file(group->tmpl, &group->includes, &emit) != 0)
      return 1;
    for (size_t j = 0; j < group->usages.len; ++j)
      if (emit_impl_usage((Usage *)group->usages.items[j], &emit) != 0)
        return 1;
  }
  if (write_impl_makefile(root, &impl_groups, &emit) != 0)
    return 1;

  if (opt.verbose)
    printf("templates=%zu added=%u skipped=%u\n", reg.items.len, emit.added,
           emit.skipped);
  return 0;
}
