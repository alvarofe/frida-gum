/*
 * Copyright (C) 2019 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumcmodule.h"

#include <gio/gio.h>
#include <gum/gum.h>
#include <libtcc.h>

struct _GumCModule
{
  TCCState * state;
  GumMemoryRange range;
};

static void gum_propagate_tcc_error (void * opaque, const char * msg);

static const gchar * gum_cmodule_builtins[] =
{
  "int strcmp (const char * s1, const char * s2);",
  "char * strstr (const char * haystack, const char * needle);",
  "char * strchr (const char * s, int c);",
  "char * strrchr (const char * s, int c);",

  "typedef void * gpointer;",
  "typedef int gint;",
  "typedef unsigned int guint;",

  "typedef struct _GumInvocationContext GumInvocationContext;",
  "gpointer gum_invocation_context_get_nth_argument ("
      "GumInvocationContext * ctx, guint n);",
  "void gum_invocation_context_replace_nth_argument ("
      "GumInvocationContext * context, guint n, gpointer value);",
  "gpointer gum_invocation_context_get_return_value ("
      "GumInvocationContext * context);",
  "void gum_invocation_context_replace_return_value ("
      "GumInvocationContext * context, gpointer value);",
};

GumCModule *
gum_cmodule_new (const gchar * source,
                 GError ** error)
{
  GumCModule * cmodule;
  TCCState * state;
  GString * combined_source;
  guint i;
  gint res;

  cmodule = g_slice_new0 (GumCModule);

  state = tcc_new ();
  cmodule->state = state;

  tcc_set_error_func (state, error, gum_propagate_tcc_error);

  tcc_set_options (state, "-nostdlib");
  tcc_set_output_type (state, TCC_OUTPUT_MEMORY);

  combined_source = g_string_sized_new (256);

  g_string_append (combined_source, "#line 1 \"gum-cmodule-builtins.h\"\n");
  for (i = 0; i != G_N_ELEMENTS (gum_cmodule_builtins); i++)
  {
    g_string_append (combined_source, gum_cmodule_builtins[i]);
    g_string_append_c (combined_source, '\n');
  }

  g_string_append (combined_source, "#line 1 \"module.c\"\n");
  g_string_append (combined_source, source);

  res = tcc_compile_string (state, combined_source->str);

  g_string_free (combined_source, TRUE);

  tcc_set_error_func (state, NULL, NULL);

  if (res == -1)
    goto failure;

#define GUM_ADD_SYMBOL(name) \
  tcc_add_symbol (state, G_STRINGIFY (name), name)

  GUM_ADD_SYMBOL (strcmp);
  GUM_ADD_SYMBOL (strstr);
  GUM_ADD_SYMBOL (strchr);
  GUM_ADD_SYMBOL (strrchr);

  GUM_ADD_SYMBOL (gum_invocation_context_get_nth_argument);
  GUM_ADD_SYMBOL (gum_invocation_context_replace_nth_argument);
  GUM_ADD_SYMBOL (gum_invocation_context_get_return_value);
  GUM_ADD_SYMBOL (gum_invocation_context_replace_return_value);

#undef GUM_ADD_SYMBOL

  return cmodule;

failure:
  {
    gum_cmodule_free (cmodule);
    return NULL;
  }
}

void
gum_cmodule_free (GumCModule * cmodule)
{
  GumMemoryRange * r;

  if (cmodule == NULL)
    return;

  r = &cmodule->range;
  if (r->base_address != 0)
  {
    gum_cloak_remove_range (r);

    gum_memory_free (GSIZE_TO_POINTER (r->base_address), r->size);
  }

  tcc_delete (cmodule->state);

  g_slice_free (GumCModule, cmodule);
}

void
gum_cmodule_add_symbol (GumCModule * self,
                        const gchar * name,
                        gconstpointer value)
{
  tcc_add_symbol (self->state, name, value);
}

gboolean
gum_cmodule_link (GumCModule * self,
                  GError ** error)
{
  TCCState * state = self->state;
  gint res;
  guint size;
  gpointer base;

  g_assert (self->range.base_address == 0);

  tcc_set_error_func (state, error, gum_propagate_tcc_error);

  res = tcc_relocate (state, NULL);
  if (res == -1)
    goto beach;
  size = res;

  base = gum_memory_allocate (NULL, size, gum_query_page_size (), GUM_PAGE_RW);

  res = tcc_relocate (state, base);
  if (res == 0)
  {
    GumMemoryRange * r = &self->range;

    r->base_address = GUM_ADDRESS (base);
    r->size = size;

    gum_memory_mark_code (base, size);

    gum_cloak_add_range (r);
  }
  else
  {
    gum_memory_free (base, size);
  }

beach:
  tcc_set_error_func (state, NULL, NULL);

  return res == 0;
}

gpointer
gum_cmodule_find_symbol_by_name (GumCModule * self,
                                 const gchar * name)
{
  g_assert (self->range.base_address != 0);

  return tcc_get_symbol (self->state, name);
}

static void
gum_propagate_tcc_error (void * opaque,
                         const char * msg)
{
  GError ** error = opaque;

  if (error != NULL && *error == NULL)
  {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "Compilation failed: %s", msg);
  }
}
