/*
 * Copyright (C) 2019 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumdukcmodule.h"

#include "gumcmodule.h"
#include "gumdukmacros.h"

GUMJS_DECLARE_CONSTRUCTOR (gumjs_cmodule_construct)
GUMJS_DECLARE_FINALIZER (gumjs_cmodule_finalize)
GUMJS_DECLARE_FUNCTION (gumjs_cmodule_find_symbol_by_name)

static const duk_function_list_entry gumjs_cmodule_functions[] =
{
  { "findSymbolByName", gumjs_cmodule_find_symbol_by_name, 1 },

  { NULL, NULL, 0 }
};

void
_gum_duk_cmodule_init (GumDukCModule * self,
                       GumDukCore * core)
{
  GumDukScope scope = GUM_DUK_SCOPE_INIT (core);
  duk_context * ctx = scope.ctx;

  self->core = core;

  duk_push_c_function (ctx, gumjs_cmodule_construct, 2);
  duk_push_object (ctx);
  duk_put_function_list (ctx, -1, gumjs_cmodule_functions);
  duk_push_c_function (ctx, gumjs_cmodule_finalize, 1);
  duk_set_finalizer (ctx, -2);
  duk_put_prop_string (ctx, -2, "prototype");
  duk_put_global_string (ctx, "_CModule");
}

void
_gum_duk_cmodule_dispose (GumDukCModule * self)
{
}

void
_gum_duk_cmodule_finalize (GumDukCModule * self)
{
}

static GumCModule *
gumjs_cmodule_from_args (const GumDukArgs * args)
{
  duk_context * ctx = args->ctx;
  GumCModule * self;

  duk_push_this (ctx);
  self = _gum_duk_require_data (ctx, -1);
  duk_pop (ctx);

  return self;
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_cmodule_construct)
{
  const gchar * source;
  GumDukHeapPtr symbols;
  GumCModule * cmodule;
  GError * error;

  if (!duk_is_constructor_call (ctx))
    _gum_duk_throw (ctx, "use `new _CModule()` to create a new instance");

  _gum_duk_args_parse (args, "sO", &source, &symbols);

  error = NULL;
  cmodule = gum_cmodule_new (source, &error);
  if (error != NULL)
    goto failure;

  duk_push_this (ctx);

  duk_push_heapptr (ctx, symbols);

  duk_enum (ctx, -1, DUK_ENUM_OWN_PROPERTIES_ONLY);
  while (duk_next (ctx, -1, TRUE))
  {
    const gchar * name;
    gconstpointer value;

    name = duk_to_string (ctx, -2);
    value = _gum_duk_require_pointer (ctx, -1, args->core);

    gum_cmodule_add_symbol (cmodule, name, value);

    duk_pop_2 (ctx);
  }
  duk_pop (ctx);

  /* Anchor lifetime to CModule instance. */
  duk_put_prop_string (ctx, -2, DUK_HIDDEN_SYMBOL ("symbols"));

  if (!gum_cmodule_link (cmodule, &error))
    goto failure;

  _gum_duk_put_data (ctx, -1, cmodule);

  return 0;

failure:
  {
    gum_cmodule_free (cmodule);

    duk_push_error_object (ctx, DUK_ERR_ERROR, "%s", error->message);
    g_error_free (error);

    (void) duk_throw (ctx);

    return 0;
  }
}

GUMJS_DEFINE_FINALIZER (gumjs_cmodule_finalize)
{
  GumCModule * self;

  self = _gum_duk_steal_data (ctx, 0);
  if (self == NULL)
    return 0;

  gum_cmodule_free (self);

  return 0;
}

GUMJS_DEFINE_FUNCTION (gumjs_cmodule_find_symbol_by_name)
{
  GumCModule * self;
  const gchar * name;
  gpointer address;

  self = gumjs_cmodule_from_args (args);

  _gum_duk_args_parse (args, "s", &name);

  address = gum_cmodule_find_symbol_by_name (self, name);

  if (address != NULL)
    _gum_duk_push_native_pointer (ctx, address, args->core);
  else
    duk_push_null (ctx);
  return 1;
}
