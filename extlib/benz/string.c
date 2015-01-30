/**
 * See Copyright Notice in picrin.h
 */

#include "picrin.h"
#include "picrin/string.h"
#include "picrin/pair.h"
#include "picrin/port.h"
#include "picrin/error.h"

struct pic_chunk {
  char *str;
  int refcnt;
  size_t len;
  char autofree, zeroterm;
};

struct pic_rope {
  int refcnt;
  size_t weight;
  struct pic_chunk *chunk;
  size_t offset;
  struct pic_rope *left, *right;
};

#define XR_CHUNK_INCREF(c) do {                 \
    (c)->refcnt++;                              \
  } while (0)

#define XR_CHUNK_DECREF(c) do {                 \
    struct pic_chunk *c__ = (c);                \
    if (! --c__->refcnt) {                      \
      if (c__->autofree)                        \
        free(c__->str);                         \
      free(c__);                                \
    }                                           \
  } while (0)

void
XROPE_INCREF(struct pic_rope *x) {
  x->refcnt++;
}

void
XROPE_DECREF(struct pic_rope *x) {
  if (! --x->refcnt) {
    if (x->chunk) {
      XR_CHUNK_DECREF(x->chunk);
      free(x);
      return;
    }
    XROPE_DECREF(x->left);
    XROPE_DECREF(x->right);
    free(x);
  }
}

static struct pic_rope *
xr_new_copy(const char *str, size_t len)
{
  char *buf;
  struct pic_chunk *c;
  struct pic_rope *x;

  buf = (char *)malloc(len + 1);
  buf[len] = '\0';
  memcpy(buf, str, len);

  c = (struct pic_chunk *)malloc(sizeof(struct pic_chunk));
  c->refcnt = 1;
  c->str = buf;
  c->len = len;
  c->autofree = 1;
  c->zeroterm = 1;

  x = (struct pic_rope *)malloc(sizeof(struct pic_rope));
  x->refcnt = 1;
  x->left = NULL;
  x->right = NULL;
  x->weight = c->len;
  x->offset = 0;
  x->chunk = c;

  return x;
}

static size_t
xr_len(struct pic_rope *x)
{
  return x->weight;
}

static char
xr_at(struct pic_rope *x, size_t i)
{
  if (x->weight <= i) {
    return -1;
  }
  if (x->chunk) {
    return x->chunk->str[x->offset + i];
  }
  return (i < x->left->weight)
    ? xr_at(x->left, i)
    : xr_at(x->right, i - x->left->weight);
}

static struct pic_rope *
xr_cat(struct pic_rope *x, struct pic_rope *y)
{
  struct pic_rope *z;

  z = (struct pic_rope *)malloc(sizeof(struct pic_rope));
  z->refcnt = 1;
  z->left = x;
  z->right = y;
  z->weight = x->weight + y->weight;
  z->offset = 0;
  z->chunk = NULL;

  XROPE_INCREF(x);
  XROPE_INCREF(y);

  return z;
}

static struct pic_rope *
xr_sub(struct pic_rope *x, size_t i, size_t j)
{
  assert(i <= j);
  assert(j <= x->weight);

  if (i == 0 && x->weight == j) {
    XROPE_INCREF(x);
    return x;
  }

  if (x->chunk) {
    struct pic_rope *y;

    y = (struct pic_rope *)malloc(sizeof(struct pic_rope));
    y->refcnt = 1;
    y->left = NULL;
    y->right = NULL;
    y->weight = j - i;
    y->offset = x->offset + i;
    y->chunk = x->chunk;

    XR_CHUNK_INCREF(x->chunk);

    return y;
  }

  if (j <= x->left->weight) {
    return xr_sub(x->left, i, j);
  }
  else if (x->left->weight <= i) {
    return xr_sub(x->right, i - x->left->weight, j - x->left->weight);
  }
  else {
    struct pic_rope *r, *l;

    l = xr_sub(x->left, i, x->left->weight);
    r = xr_sub(x->right, 0, j - x->left->weight);
    x = xr_cat(l, r);

    XROPE_DECREF(l);
    XROPE_DECREF(r);

    return x;
  }
}

static void
xr_fold(struct pic_rope *x, struct pic_chunk *c, size_t offset)
{
  if (x->chunk) {
    memcpy(c->str + offset, x->chunk->str + x->offset, x->weight);
    XR_CHUNK_DECREF(x->chunk);

    x->chunk = c;
    x->offset = offset;
    XR_CHUNK_INCREF(c);
    return;
  }
  xr_fold(x->left, c, offset);
  xr_fold(x->right, c, offset + x->left->weight);

  XROPE_DECREF(x->left);
  XROPE_DECREF(x->right);
  x->left = x->right = NULL;
  x->chunk = c;
  x->offset = offset;
  XR_CHUNK_INCREF(c);
}

static const char *
xr_cstr(struct pic_rope *x)
{
  struct pic_chunk *c;

  if (x->chunk && x->offset == 0 && x->weight == x->chunk->len && x->chunk->zeroterm) {
    return x->chunk->str;       /* reuse cached chunk */
  }

  c = (struct pic_chunk *)malloc(sizeof(struct pic_chunk));
  c->refcnt = 1;
  c->len = x->weight;
  c->autofree = 1;
  c->zeroterm = 1;
  c->str = (char *)malloc(c->len + 1);
  c->str[c->len] = '\0';

  xr_fold(x, c, 0);

  XR_CHUNK_DECREF(c);
  return c->str;
}

static pic_str *
make_str_rope(pic_state *pic, struct pic_rope *rope)
{
  pic_str *str;

  str = (pic_str *)pic_obj_alloc(pic, sizeof(pic_str), PIC_TT_STRING);
  str->rope = rope;             /* delegate ownership */
  return str;
}

pic_str *
pic_make_str(pic_state *pic, const char *imbed, size_t len)
{
  if (imbed == NULL && len > 0) {
    pic_errorf(pic, "zero length specified against NULL ptr");
  }
  return make_str_rope(pic, xr_new_copy(imbed, len));
}

pic_str *
pic_make_str_cstr(pic_state *pic, const char *cstr)
{
  return pic_make_str(pic, cstr, strlen(cstr));
}

pic_str *
pic_make_str_fill(pic_state *pic, size_t len, char fill)
{
  size_t i;
  char *buf = pic_malloc(pic, len);
  pic_str *str;

  for (i = 0; i < len; ++i) {
    buf[i] = fill;
  }
  buf[i] = '\0';

  str = pic_make_str(pic, buf, len);

  pic_free(pic, buf);

  return str;
}

size_t
pic_strlen(pic_str *str)
{
  return xr_len(str->rope);
}

char
pic_str_ref(pic_state *pic, pic_str *str, size_t i)
{
  int c;

  c = xr_at(str->rope, i);
  if (c == -1) {
    pic_errorf(pic, "index out of range %d", i);
  }
  return (char)c;
}

pic_str *
pic_strcat(pic_state *pic, pic_str *a, pic_str *b)
{
  return make_str_rope(pic, xr_cat(a->rope, b->rope));
}

pic_str *
pic_substr(pic_state *pic, pic_str *str, size_t s, size_t e)
{
  return make_str_rope(pic, xr_sub(str->rope, s, e));
}

int
pic_strcmp(pic_str *str1, pic_str *str2)
{
  return strcmp(xr_cstr(str1->rope), xr_cstr(str2->rope));
}

const char *
pic_str_cstr(pic_str *str)
{
  return xr_cstr(str->rope);
}

pic_value
pic_xvfformat(pic_state *pic, xFILE *file, const char *fmt, va_list ap)
{
  char c;
  pic_value irrs = pic_nil_value();

  while ((c = *fmt++)) {
    switch (c) {
    default:
      xfputc(c, file);
      break;
    case '%':
      c = *fmt++;
      if (! c)
        goto exit;
      switch (c) {
      default:
        xfputc(c, file);
        break;
      case '%':
        xfputc('%', file);
        break;
      case 'c':
        xfprintf(file, "%c", va_arg(ap, int));
        break;
      case 's':
        xfprintf(file, "%s", va_arg(ap, const char *));
        break;
      case 'd':
        xfprintf(file, "%d", va_arg(ap, int));
        break;
      case 'p':
        xfprintf(file, "%p", va_arg(ap, void *));
        break;
#if PIC_ENABLE_FLOAT
      case 'f':
        xfprintf(file, "%f", va_arg(ap, double));
        break;
#endif
      }
      break;
    case '~':
      c = *fmt++;
      if (! c)
        goto exit;
      switch (c) {
      default:
        xfputc(c, file);
        break;
      case '~':
        xfputc('~', file);
        break;
      case '%':
        xfputc('\n', file);
        break;
      case 'a':
        irrs = pic_cons(pic, pic_fdisplay(pic, va_arg(ap, pic_value), file), irrs);
        break;
      case 's':
        irrs = pic_cons(pic, pic_fwrite(pic, va_arg(ap, pic_value), file), irrs);
        break;
      }
      break;
    }
  }
 exit:

  return pic_reverse(pic, irrs);
}

pic_value
pic_xvformat(pic_state *pic, const char *fmt, va_list ap)
{
  struct pic_port *port;
  pic_value irrs;

  port = pic_open_output_string(pic);

  irrs = pic_xvfformat(pic, port->file, fmt, ap);
  irrs = pic_cons(pic, pic_obj_value(pic_get_output_string(pic, port)), irrs);

  pic_close_port(pic, port);
  return irrs;
}

pic_value
pic_xformat(pic_state *pic, const char *fmt, ...)
{
  va_list ap;
  pic_value objs;

  va_start(ap, fmt);
  objs = pic_xvformat(pic, fmt, ap);
  va_end(ap);

  return objs;
}

void
pic_vfformat(pic_state *pic, xFILE *file, const char *fmt, va_list ap)
{
  pic_xvfformat(pic, file, fmt, ap);
}

pic_str *
pic_vformat(pic_state *pic, const char *fmt, va_list ap)
{
  struct pic_port *port;
  pic_str *str;

  port = pic_open_output_string(pic);

  pic_vfformat(pic, port->file, fmt, ap);
  str = pic_get_output_string(pic, port);

  pic_close_port(pic, port);
  return str;
}

pic_str *
pic_format(pic_state *pic, const char *fmt, ...)
{
  va_list ap;
  pic_str *str;

  va_start(ap, fmt);
  str = pic_vformat(pic, fmt, ap);
  va_end(ap);

  return str;
}

static pic_value
pic_str_string_p(pic_state *pic)
{
  pic_value v;

  pic_get_args(pic, "o", &v);

  return pic_bool_value(pic_str_p(v));
}

static pic_value
pic_str_string(pic_state *pic)
{
  size_t argc, i;
  pic_value *argv;
  pic_str *str;
  char *buf;

  pic_get_args(pic, "*", &argc, &argv);

  buf = pic_alloc(pic, (size_t)argc);

  for (i = 0; i < argc; ++i) {
    pic_assert_type(pic, argv[i], char);
    buf[i] = pic_char(argv[i]);
  }

  str = pic_make_str(pic, buf, (size_t)argc);
  pic_free(pic, buf);

  return pic_obj_value(str);
}

static pic_value
pic_str_make_string(pic_state *pic)
{
  size_t len;
  char c = ' ';

  pic_get_args(pic, "k|c", &len, &c);

  return pic_obj_value(pic_make_str_fill(pic, len, c));
}

static pic_value
pic_str_string_length(pic_state *pic)
{
  pic_str *str;

  pic_get_args(pic, "s", &str);

  return pic_size_value(pic_strlen(str));
}

static pic_value
pic_str_string_ref(pic_state *pic)
{
  pic_str *str;
  size_t k;

  pic_get_args(pic, "sk", &str, &k);

  return pic_char_value(pic_str_ref(pic, str, k));
}

#define DEFINE_STRING_CMP(name, op)                                     \
  static pic_value                                                      \
  pic_str_string_##name(pic_state *pic)                                 \
  {                                                                     \
    size_t argc, i;                                                     \
    pic_value *argv;                                                    \
                                                                        \
    pic_get_args(pic, "*", &argc, &argv);                               \
                                                                        \
    if (argc < 1 || ! pic_str_p(argv[0])) {                             \
      return pic_false_value();                                         \
    }                                                                   \
                                                                        \
    for (i = 1; i < argc; ++i) {                                        \
      if (! pic_str_p(argv[i])) {                                       \
	return pic_false_value();                                       \
      }                                                                 \
      if (! (pic_strcmp(pic_str_ptr(argv[i-1]), pic_str_ptr(argv[i])) op 0)) { \
	return pic_false_value();                                       \
      }                                                                 \
    }                                                                   \
    return pic_true_value();                                            \
  }

DEFINE_STRING_CMP(eq, ==)
DEFINE_STRING_CMP(lt, <)
DEFINE_STRING_CMP(gt, >)
DEFINE_STRING_CMP(le, <=)
DEFINE_STRING_CMP(ge, >=)

static pic_value
pic_str_string_copy(pic_state *pic)
{
  pic_str *str;
  int n;
  size_t start, end;

  n = pic_get_args(pic, "s|kk", &str, &start, &end);

  switch (n) {
  case 1:
    start = 0;
  case 2:
    end = pic_strlen(str);
  }

  return pic_obj_value(pic_substr(pic, str, start, end));
}

static pic_value
pic_str_string_append(pic_state *pic)
{
  size_t argc, i;
  pic_value *argv;
  pic_str *str;

  pic_get_args(pic, "*", &argc, &argv);

  str = pic_make_str(pic, NULL, 0);
  for (i = 0; i < argc; ++i) {
    if (! pic_str_p(argv[i])) {
      pic_errorf(pic, "type error");
    }
    str = pic_strcat(pic, str, pic_str_ptr(argv[i]));
  }
  return pic_obj_value(str);
}

static pic_value
pic_str_string_map(pic_state *pic)
{
  struct pic_proc *proc;
  pic_value *argv, vals, val;
  size_t argc, i, len, j;
  pic_str *str;
  char *buf;

  pic_get_args(pic, "l*", &proc, &argc, &argv);

  if (argc == 0) {
    pic_errorf(pic, "string-map: one or more strings expected, but got zero");
  } else {
    pic_assert_type(pic, argv[0], str);
    len = pic_strlen(pic_str_ptr(argv[0]));
  }
  for (i = 1; i < argc; ++i) {
    pic_assert_type(pic, argv[i], str);

    len = len < pic_strlen(pic_str_ptr(argv[i]))
      ? len
      : pic_strlen(pic_str_ptr(argv[i]));
  }
  buf = pic_malloc(pic, len);

  pic_try {
    for (i = 0; i < len; ++i) {
      vals = pic_nil_value();
      for (j = 0; j < argc; ++j) {
        pic_push(pic, pic_char_value(pic_str_ref(pic, pic_str_ptr(argv[j]), i)), vals);
      }
      val = pic_apply(pic, proc, vals);

      pic_assert_type(pic, val, char);
      buf[i] = pic_char(val);
    }
    str = pic_make_str(pic, buf, len);
  }
  pic_catch {
    pic_free(pic, buf);
    pic_raise(pic, pic->err);
  }

  pic_free(pic, buf);

  return pic_obj_value(str);
}

static pic_value
pic_str_string_for_each(pic_state *pic)
{
  struct pic_proc *proc;
  size_t argc, len, i, j;
  pic_value *argv, vals;

  pic_get_args(pic, "l*", &proc, &argc, &argv);

  if (argc == 0) {
    pic_errorf(pic, "string-map: one or more strings expected, but got zero");
  } else {
    pic_assert_type(pic, argv[0], str);
    len = pic_strlen(pic_str_ptr(argv[0]));
  }
  for (i = 1; i < argc; ++i) {
    pic_assert_type(pic, argv[i], str);

    len = len < pic_strlen(pic_str_ptr(argv[i]))
      ? len
      : pic_strlen(pic_str_ptr(argv[i]));
  }

  for (i = 0; i < len; ++i) {
    vals = pic_nil_value();
    for (j = 0; j < argc; ++j) {
      pic_push(pic, pic_char_value(pic_str_ref(pic, pic_str_ptr(argv[j]), i)), vals);
    }
    pic_apply(pic, proc, vals);
  }

  return pic_none_value();
}

static pic_value
pic_str_list_to_string(pic_state *pic)
{
  pic_str *str;
  pic_value list, e, it;
  size_t i = 0;
  char *buf;

  pic_get_args(pic, "o", &list);

  if (pic_length(pic, list) == 0) {
    return pic_obj_value(pic_make_str(pic, NULL, 0));
  }

  buf = pic_malloc(pic, pic_length(pic, list));

  pic_try {
    pic_for_each (e, list, it) {
      pic_assert_type(pic, e, char);

      buf[i++] = pic_char(e);
    }

    str = pic_make_str(pic, buf, i);
  }
  pic_catch {
    pic_free(pic, buf);
    pic_raise(pic, pic->err);
  }
  pic_free(pic, buf);

  return pic_obj_value(str);
}

static pic_value
pic_str_string_to_list(pic_state *pic)
{
  pic_str *str;
  pic_value list;
  int n;
  size_t start, end, i;

  n = pic_get_args(pic, "s|kk", &str, &start, &end);

  switch (n) {
  case 1:
    start = 0;
  case 2:
    end = pic_strlen(str);
  }

  list = pic_nil_value();

  for (i = start; i < end; ++i) {
    pic_push(pic, pic_char_value(pic_str_ref(pic, str, i)), list);
  }
  return pic_reverse(pic, list);
}

void
pic_init_str(pic_state *pic)
{
  pic_defun(pic, "string?", pic_str_string_p);
  pic_defun(pic, "string", pic_str_string);
  pic_defun(pic, "make-string", pic_str_make_string);
  pic_defun(pic, "string-length", pic_str_string_length);
  pic_defun(pic, "string-ref", pic_str_string_ref);
  pic_defun(pic, "string-copy", pic_str_string_copy);
  pic_defun(pic, "string-append", pic_str_string_append);
  pic_defun(pic, "string-map", pic_str_string_map);
  pic_defun(pic, "string-for-each", pic_str_string_for_each);
  pic_defun(pic, "list->string", pic_str_list_to_string);
  pic_defun(pic, "string->list", pic_str_string_to_list);

  pic_defun(pic, "string=?", pic_str_string_eq);
  pic_defun(pic, "string<?", pic_str_string_lt);
  pic_defun(pic, "string>?", pic_str_string_gt);
  pic_defun(pic, "string<=?", pic_str_string_le);
  pic_defun(pic, "string>=?", pic_str_string_ge);
}
