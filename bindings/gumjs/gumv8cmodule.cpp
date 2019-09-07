/*
 * Copyright (C) 2019 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumv8cmodule.h"

#include "gumcmodule.h"
#include "gumv8macros.h"

#define GUMJS_MODULE_NAME CModule

using namespace v8;

struct GumCModuleEntry
{
  GumPersistent<Object>::type * wrapper;
  GumPersistent<Object>::type * symbols;
  GumCModule * handle;
  GumV8CModule * module;
};

GUMJS_DECLARE_CONSTRUCTOR (gumjs_cmodule_construct)
GUMJS_DECLARE_FUNCTION (gumjs_cmodule_find_symbol_by_name)

static GumCModuleEntry * gum_cmodule_entry_new (Handle<Object> wrapper,
    Handle<Object> symbols, GumCModule * handle, GumV8CModule * module);
static void gum_cmodule_entry_free (GumCModuleEntry * self);
static void gum_cmodule_entry_on_weak_notify (
    const WeakCallbackInfo<GumCModuleEntry> & info);

static const GumV8Function gumjs_cmodule_functions[] =
{
  { "findSymbolByName", gumjs_cmodule_find_symbol_by_name },

  { NULL, NULL }
};

void
_gum_v8_cmodule_init (GumV8CModule * self,
                      GumV8Core * core,
                      Handle<ObjectTemplate> scope)
{
  auto isolate = core->isolate;

  self->core = core;

  auto module = External::New (isolate, self);

  auto cmodule = _gum_v8_create_class ("_CModule", gumjs_cmodule_construct,
      scope, module, isolate);
  _gum_v8_class_add (cmodule, gumjs_cmodule_functions, module, isolate);
}

void
_gum_v8_cmodule_realize (GumV8CModule * self)
{
  self->cmodules = g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) gum_cmodule_entry_free);
}

void
_gum_v8_cmodule_dispose (GumV8CModule * self)
{
  g_hash_table_unref (self->cmodules);
  self->cmodules = NULL;
}

void
_gum_v8_cmodule_finalize (GumV8CModule * self)
{
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_cmodule_construct)
{
  if (!info.IsConstructCall ())
  {
    _gum_v8_throw_ascii_literal (isolate,
        "use `new _CModule()` to create a new instance");
    return;
  }

  gchar * source;
  Local<Object> symbols;
  if (!_gum_v8_args_parse (args, "sO", &source, &symbols))
    return;

  GError * error = NULL;
  auto handle = gum_cmodule_new (source, &error);

  g_free (source);

  if (error == NULL)
  {
    auto context = isolate->GetCurrentContext ();

    gboolean valid = TRUE;

    Local<Array> names;
    if (symbols->GetOwnPropertyNames (context).ToLocal (&names))
    {
      guint count = names->Length ();
      for (guint i = 0; i != count; i++)
      {
        Local<Value> name_val;
        if (!names->Get (context, i).ToLocal (&name_val))
        {
          valid = FALSE;
          break;
        }

        Local<String> name_str;
        if (!name_val->ToString (context).ToLocal (&name_str))
        {
          valid = FALSE;
          break;
        }

        String::Utf8Value name_utf8 (isolate, name_str);

        Local<Value> value_val;
        if (!symbols->Get (context, name_val).ToLocal (&value_val))
        {
          valid = FALSE;
          break;
        }

        gpointer value;
        if (!_gum_v8_native_pointer_get (value_val, &value, core))
        {
          valid = FALSE;
          break;
        }

        gum_cmodule_add_symbol (handle, *name_utf8, value);
      }
    }
    else
    {
      valid = FALSE;
    }

    if (!valid)
    {
      gum_cmodule_free (handle);
      return;
    }

    gum_cmodule_link (handle, &error);
  }

  if (error != NULL)
  {
    _gum_v8_throw_literal (isolate, error->message);
    g_error_free (error);

    gum_cmodule_free (handle);

    return;
  }

  auto entry = gum_cmodule_entry_new (wrapper, symbols, handle, module);
  wrapper->SetAlignedPointerInInternalField (0, entry);
}

GUMJS_DEFINE_CLASS_METHOD (gumjs_cmodule_find_symbol_by_name, GumCModuleEntry)
{
  gchar * name;
  if (!_gum_v8_args_parse (args, "s", &name))
    return;

  auto address = gum_cmodule_find_symbol_by_name (self->handle, name);
  if (address != NULL)
    info.GetReturnValue ().Set (_gum_v8_native_pointer_new (address, core));
  else
    info.GetReturnValue ().SetNull ();

  g_free (name);
}

static GumCModuleEntry *
gum_cmodule_entry_new (Handle<Object> wrapper,
                       Handle<Object> symbols,
                       GumCModule * handle,
                       GumV8CModule * module)
{
  auto isolate = module->core->isolate;

  auto entry = g_slice_new (GumCModuleEntry);
  entry->wrapper = new GumPersistent<Object>::type (isolate, wrapper);
  entry->wrapper->SetWeak (entry, gum_cmodule_entry_on_weak_notify,
      WeakCallbackType::kParameter);
  entry->symbols = new GumPersistent<Object>::type (isolate, wrapper);
  entry->handle = handle;
  entry->module = module;

  g_hash_table_add (module->cmodules, entry);

  return entry;
}

static void
gum_cmodule_entry_free (GumCModuleEntry * self)
{
  gum_cmodule_free (self->handle);

  delete self->symbols;
  delete self->wrapper;

  g_slice_free (GumCModuleEntry, self);
}

static void
gum_cmodule_entry_on_weak_notify (
    const WeakCallbackInfo<GumCModuleEntry> & info)
{
  HandleScope handle_scope (info.GetIsolate ());
  auto self = info.GetParameter ();
  g_hash_table_remove (self->module->cmodules, self);
}
