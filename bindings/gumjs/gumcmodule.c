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

static const gchar * gum_cmodule_builtins =
    "typedef struct _InvocationContext InvocationContext;\n"
    "void* getArg(InvocationContext* ctx, unsigned int n);\n"
    ;

GumCModule *
gum_cmodule_new (const gchar * source,
                 GError ** error)
{
  GumCModule * cmodule;
  TCCState * state;
  gchar * combined_source;
  gint res;

  cmodule = g_slice_new0 (GumCModule);

  state = tcc_new ();
  cmodule->state = state;

  tcc_set_error_func (state, error, gum_propagate_tcc_error);

  tcc_set_options (state, "-nostdlib");
  tcc_set_output_type (state, TCC_OUTPUT_MEMORY);

  combined_source = g_strconcat (gum_cmodule_builtins, source, NULL);

  res = tcc_compile_string (state, combined_source);

  g_free (combined_source);

  tcc_set_error_func (state, NULL, NULL);

  if (res == -1)
    goto failure;

  tcc_add_symbol (state, "getArg", gum_invocation_context_get_nth_argument);

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
