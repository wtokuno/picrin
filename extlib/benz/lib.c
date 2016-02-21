/**
 * See Copyright Notice in picrin.h
 */

#include "picrin.h"
#include "picrin/extra.h"
#include "picrin/private/object.h"
#include "picrin/private/state.h"

KHASH_DEFINE(ltable, const char *, struct lib, kh_str_hash_func, kh_str_cmp_func)

static struct lib *
get_library_opt(pic_state *pic, const char *lib)
{
  khash_t(ltable) *h = &pic->ltable;
  int it;

  it = kh_get(ltable, h, lib);
  if (it == kh_end(h)) {
    return NULL;
  }
  return &kh_val(h, it);
}

static struct lib *
get_library(pic_state *pic, const char *lib)
{
  struct lib *libp;

  if ((libp = get_library_opt(pic, lib)) == NULL) {
    pic_errorf(pic, "library not found: %s", lib);
  }
  return libp;
}

static pic_value
make_library_env(pic_state *pic, pic_value name)
{
  struct env *env;
  pic_value e;

  env = (struct env *)pic_obj_alloc(pic, sizeof(struct env), PIC_TYPE_ENV);
  env->up = NULL;
  env->lib = pic_str_ptr(pic, name);
  kh_init(env, &env->map);

  e = pic_obj_value(env);

#define REGISTER(name) pic_put_identifier(pic, pic_intern_lit(pic, name), pic_intern_lit(pic, name), e)

  /* set up default environment */
  REGISTER("define-library");
  REGISTER("import");
  REGISTER("export");
  REGISTER("cond-expand");

  return e;
}

void
pic_make_library(pic_state *pic, const char *lib)
{
  khash_t(ltable) *h = &pic->ltable;
  const char *old_lib = NULL;
  pic_value name, env, exports;
  int it;
  int ret;

  if (pic->lib) {
    old_lib = pic_current_library(pic);
  }

  name = pic_cstr_value(pic, lib);
  env = make_library_env(pic, name);
  exports = pic_make_dict(pic);

  it = kh_put(ltable, h, pic_str(pic, name), &ret);
  if (ret == 0) {               /* if exists */
    pic_errorf(pic, "library name already in use: %s", lib);
  }

  kh_val(h, it).name = pic_str_ptr(pic, name);
  kh_val(h, it).env = pic_env_ptr(pic, env);
  kh_val(h, it).exports = pic_dict_ptr(pic, exports);

  if (pic->lib) {
    pic->lib = get_library(pic, old_lib); /* ltable might be rehashed */
  }
}

void
pic_in_library(pic_state *pic, const char *lib)
{
  pic->lib = get_library(pic, lib);
}

bool
pic_find_library(pic_state *pic, const char *lib)
{
  return get_library_opt(pic, lib) != NULL;
}

const char *
pic_current_library(pic_state *pic)
{
  return pic_str(pic, pic_obj_value(pic->lib->name));
}

pic_value
pic_library_environment(pic_state *pic, const char *lib)
{
  return pic_obj_value(get_library(pic, lib)->env);
}

void
pic_import(pic_state *pic, const char *lib)
{
  pic_value name, realname, uid;
  int it = 0;
  struct lib *libp;

  libp = get_library(pic, lib);

  while (pic_dict_next(pic, pic_obj_value(libp->exports), &it, &name, &realname)) {
    uid = pic_find_identifier(pic, realname, pic_obj_value(libp->env));
    if (! pic_weak_has(pic, pic->globals, uid) && ! pic_weak_has(pic, pic->macros, uid)) {
      pic_errorf(pic, "attempted to export undefined variable '~s'", realname);
    }
    pic_put_identifier(pic, name, uid, pic_obj_value(pic->lib->env));
  }
}

void
pic_export(pic_state *pic, pic_value name)
{
  pic_dict_set(pic, pic_obj_value(pic->lib->exports), name, name);
}

static pic_value
pic_lib_make_library(pic_state *pic)
{
  const char *lib;

  pic_get_args(pic, "z", &lib);

  pic_make_library(pic, lib);

  return pic_undef_value(pic);
}

static pic_value
pic_lib_find_library(pic_state *pic)
{
  const char *lib;

  pic_get_args(pic, "z", &lib);

  return pic_bool_value(pic, pic_find_library(pic, lib));
}

static pic_value
pic_lib_current_library(pic_state *pic)
{
  const char *lib;
  int n;

  n = pic_get_args(pic, "|z", &lib);

  if (n == 0) {
    return pic_obj_value(pic->lib->name);
  }
  else {
    pic_in_library(pic, lib);

    return pic_undef_value(pic);
  }
}

static pic_value
pic_lib_library_import(pic_state *pic)
{
  const char *lib;
  pic_value name, alias, realname, uid;
  struct lib *libp;
  int n;

  n = pic_get_args(pic, "zm|m", &lib, &name, &alias);

  if (n == 2) {
    alias = name;
  }

  libp = get_library(pic, lib);

  if (! pic_dict_has(pic, pic_obj_value(libp->exports), name)) {
    pic_errorf(pic, "library-import: variable is not exported '~s'", name);
  } else {
    realname = pic_dict_ref(pic, pic_obj_value(libp->exports), name);
  }

  uid = pic_find_identifier(pic, realname, pic_obj_value(libp->env));
  if (! pic_weak_has(pic, pic->globals, uid) && ! pic_weak_has(pic, pic->macros, uid)) {
    pic_errorf(pic, "attempted to export undefined variable '~s'", realname);
  }

  pic_put_identifier(pic, alias, uid, pic_obj_value(pic->lib->env));

  return pic_undef_value(pic);
}

static pic_value
pic_lib_library_export(pic_state *pic)
{
  pic_value name, alias = pic_false_value(pic);
  int n;

  n = pic_get_args(pic, "m|m", &name, &alias);

  if (n == 1) {
    alias = name;
  }

  pic_dict_set(pic, pic_obj_value(pic->lib->exports), alias, name);

  return pic_undef_value(pic);
}

static pic_value
pic_lib_library_exports(pic_state *pic)
{
  const char *lib;
  pic_value sym, exports = pic_nil_value(pic);
  int it = 0;
  struct lib *libp;

  pic_get_args(pic, "z", &lib);

  libp = get_library(pic, lib);

  while (pic_dict_next(pic, pic_obj_value(libp->exports), &it, &sym, NULL)) {
    pic_push(pic, sym, exports);
  }

  return exports;
}

static pic_value
pic_lib_library_environment(pic_state *pic)
{
  const char *lib;

  pic_get_args(pic, "z", &lib);

  return pic_obj_value(get_library(pic, lib)->env);
}

void
pic_init_lib(pic_state *pic)
{
  pic_defun(pic, "make-library", pic_lib_make_library);
  pic_defun(pic, "find-library", pic_lib_find_library);
  pic_defun(pic, "library-exports", pic_lib_library_exports);
  pic_defun(pic, "library-environment", pic_lib_library_environment);

  pic_defun(pic, "current-library", pic_lib_current_library);
  pic_defun(pic, "library-import", pic_lib_library_import);
  pic_defun(pic, "library-export", pic_lib_library_export);
}
