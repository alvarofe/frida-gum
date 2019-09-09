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

static void gum_cmodule_call_init (GumCModule * self);
static void gum_cmodule_call_finalize (GumCModule * self);

static const gchar * gum_cmodule_builtins[] =
{
  "typedef signed char int8_t;",
  "typedef unsigned char uint8_t;",

  "typedef signed short int int16_t;",
  "typedef unsigned short int uint16_t;",

  "typedef signed int int32_t;",
  "typedef unsigned int uint32_t;",

#ifdef __LP64__
  "typedef signed long int int64_t;",
  "typedef unsigned long int uint64_t;",
#else
  "typedef signed long long int int64_t;",
  "typedef unsigned long long int uint64_t;",
#endif

#if GLIB_SIZEOF_VOID_P == 8
  "typedef int64_t ssize_t;",
  "typedef uint64_t size_t;",
#else
  "typedef int32_t ssize_t;",
  "typedef uint32_t size_t;",
#endif

  "size_t strlen (const char * s);",
  "int strcmp (const char * s1, const char * s2);",
  "char * strstr (const char * haystack, const char * needle);",
  "char * strchr (const char * s, int c);",
  "char * strrchr (const char * s, int c);",
  "void * memcpy (void * restrict dst, const void * restrict src, size_t n);",
  "void * memmove (void * dst, const void * src, size_t len);",

  "typedef struct _FILE FILE;",
  "int puts (const char * s);"
  "int fputs (const char * restrict s, FILE * restrict stream);",
  "int fflush (FILE * stream);",
  "int printf (const char * restrict format, ...);",
  "int fprintf (FILE * restrict stream, const char * restrict format, ...);",
  "extern FILE * stdout;",
  "extern FILE * stderr;",

  "typedef void * gpointer;",
  "typedef const void * gconstpointer;",

  "typedef ssize_t gssize;",
  "typedef size_t gsize;",

  "typedef int gint;",
  "typedef unsigned int guint;",

  "typedef int8_t gint8;",
  "typedef uint8_t guint8;",

  "typedef int16_t gint16;",
  "typedef uint16_t guint16;",

  "typedef int32_t gint32;",
  "typedef uint32_t guint32;",

  "typedef int64_t gint64;",
  "typedef uint64_t guint64;",

  "typedef char gchar;",
  "typedef unsigned char guchar;",

  "typedef gint gboolean;",

  "typedef void (* GCallback) (void);",

  "gchar * g_strdup_printf (const gchar * format, ...);",
  "gpointer g_malloc (gsize n_bytes);",
  "gpointer g_malloc0 (gsize n_bytes);",
  "gpointer g_realloc (gpointer mem, gsize n_bytes);",
  "gpointer g_memdup (gconstpointer mem, guint byte_size);",
  "void g_free (gpointer mem);",

  "typedef struct _GThread GThread;",
  "typedef gpointer (* GThreadFunc) (gpointer data);",
  "GThread * g_thread_new (const gchar * name, GThreadFunc func, "
      "gpointer data);",
  "gpointer g_thread_join (GThread * thread);",
  "GThread * g_thread_ref (GThread * thread);",
  "void g_thread_unref (GThread * thread);",
  "void g_thread_yield (void);",

  "typedef union _GMutex GMutex;",
  "typedef struct _GCond GCond;",
  "union _GMutex",
  "{",
  "  gpointer p;",
  "  guint i[2];",
  "};",
  "struct _GCond",
  "{",
  "  gpointer p;",
  "  guint i[2];",
  "};",
  "void g_mutex_init (GMutex * mutex);",
  "void g_mutex_clear (GMutex * mutex);",
  "void g_mutex_lock (GMutex * mutex);",
  "void g_mutex_unlock (GMutex * mutex);",
  "gboolean g_mutex_trylock (GMutex * mutex);",
  "void g_cond_init (GCond * cond);",
  "void g_cond_clear (GCond * cond);",
  "void g_cond_wait (GCond * cond, GMutex * mutex);",
  "void g_cond_signal (GCond * cond);",
  "void g_cond_broadcast (GCond * cond);",

  "gint g_atomic_int_add (volatile gint * atomic, gint val);",
  "gssize g_atomic_pointer_add (volatile void * atomic, gssize val);",

  "typedef struct _GumCpuContext GumCpuContext;",

  "struct _GumCpuContext",
  "{",
#if defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 4
  "  guint32 eip;",

  "  guint32 edi;",
  "  guint32 esi;",
  "  guint32 ebp;",
  "  guint32 esp;",
  "  guint32 ebx;",
  "  guint32 edx;",
  "  guint32 ecx;",
  "  guint32 eax;",
#elif defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 8
  "  guint64 rip;",

  "  guint64 r15;",
  "  guint64 r14;",
  "  guint64 r13;",
  "  guint64 r12;",
  "  guint64 r11;",
  "  guint64 r10;",
  "  guint64 r9;",
  "  guint64 r8;",

  "  guint64 rdi;",
  "  guint64 rsi;",
  "  guint64 rbp;",
  "  guint64 rsp;",
  "  guint64 rbx;",
  "  guint64 rdx;",
  "  guint64 rcx;",
  "  guint64 rax;",
#elif defined (HAVE_ARM)
  "  guint32 cpsr;",
  "  guint32 pc;",
  "  guint32 sp;",

  "  guint32 r8;",
  "  guint32 r9;",
  "  guint32 r10;",
  "  guint32 r11;",
  "  guint32 r12;",

  "  guint32 r[8];",
  "  guint32 lr;",
#elif defined (HAVE_ARM64)
  "  guint64 pc;",
  "  guint64 sp;",

  "  guint64 x[29];",
  "  guint64 fp;",
  "  guint64 lr;",
  "  guint8 q[128];",
#elif defined (HAVE_MIPS)
  "  gsize pc;",

  "  gsize gp;",
  "  gsize sp;",
  "  gsize fp;",
  "  gsize ra;",

  "  gsize hi;",
  "  gsize lo;",

  "  gsize at;",

  "  gsize v0;",
  "  gsize v1;",

  "  gsize a0;",
  "  gsize a1;",
  "  gsize a2;",
  "  gsize a3;",

  "  gsize t0;",
  "  gsize t1;",
  "  gsize t2;",
  "  gsize t3;",
  "  gsize t4;",
  "  gsize t5;",
  "  gsize t6;",
  "  gsize t7;",
  "  gsize t8;",
  "  gsize t9;",

  "  gsize s0;",
  "  gsize s1;",
  "  gsize s2;",
  "  gsize s3;",
  "  gsize s4;",
  "  gsize s5;",
  "  gsize s6;",
  "  gsize s7;",

  "  gsize k0;",
  "  gsize k1;",
#endif
  "};",

  "#define GUM_IC_GET_THREAD_DATA(context, data_type) ((data_type *) "
      "gum_invocation_context_get_listener_thread_data (context, "
      "sizeof (data_type)))",
  "#define GUM_IC_GET_FUNC_DATA(context, data_type) ((data_type) "
      "gum_invocation_context_get_listener_function_data (context))",
  "#define GUM_IC_GET_INVOCATION_DATA(context, data_type) ((data_type *) "
      "gum_invocation_context_get_listener_invocation_data (context, "
      "sizeof (data_type)))",

  "#define GUM_IC_GET_REPLACEMENT_DATA(ctx, data_type) "
      "((data_type) gum_invocation_context_get_replacement_data (ctx))",

  "typedef struct _GumInvocationContext GumInvocationContext;",
  "typedef struct _GumInvocationBackend GumInvocationBackend;",

  "struct _GumInvocationContext",
  "{",
  "  GCallback function;",
  "  GumCpuContext * cpu_context;",
  "  gint system_error;",

  "  GumInvocationBackend * backend;",
  "};",

  "GumInvocationContext * gum_interceptor_get_current_invocation (void);",

  "gpointer gum_invocation_context_get_nth_argument ("
      "GumInvocationContext * ctx, guint n);",
  "void gum_invocation_context_replace_nth_argument ("
      "GumInvocationContext * context, guint n, gpointer value);",
  "gpointer gum_invocation_context_get_return_value ("
      "GumInvocationContext * context);",
  "void gum_invocation_context_replace_return_value ("
      "GumInvocationContext * context, gpointer value);",

  "gpointer gum_invocation_context_get_return_address ("
      "GumInvocationContext * context);",

  "guint gum_invocation_context_get_thread_id ("
      "GumInvocationContext * context);",
  "guint gum_invocation_context_get_depth (GumInvocationContext * context);",

  "gpointer gum_invocation_context_get_listener_thread_data ("
      "GumInvocationContext * context, gsize required_size);",
  "gpointer gum_invocation_context_get_listener_function_data ("
      "GumInvocationContext * context);",
  "gpointer gum_invocation_context_get_listener_invocation_data ("
      "GumInvocationContext * context, gsize required_size);",

  "gpointer gum_invocation_context_get_replacement_data ("
      "GumInvocationContext * context);",
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

  combined_source = g_string_sized_new (2048);

  g_string_append (combined_source, "#line 1 \"module-builtins.h\"\n");
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

  GUM_ADD_SYMBOL (strlen);
  GUM_ADD_SYMBOL (strcmp);
  GUM_ADD_SYMBOL (strstr);
  GUM_ADD_SYMBOL (strchr);
  GUM_ADD_SYMBOL (strrchr);

  GUM_ADD_SYMBOL (puts);
  GUM_ADD_SYMBOL (fputs);
  GUM_ADD_SYMBOL (fflush);
  GUM_ADD_SYMBOL (printf);
  GUM_ADD_SYMBOL (fprintf);
  GUM_ADD_SYMBOL (stdout);
  GUM_ADD_SYMBOL (stderr);

  GUM_ADD_SYMBOL (g_strdup_printf);
  GUM_ADD_SYMBOL (g_malloc);
  GUM_ADD_SYMBOL (g_malloc0);
  GUM_ADD_SYMBOL (g_realloc);
  GUM_ADD_SYMBOL (g_memdup);
  GUM_ADD_SYMBOL (g_free);

  GUM_ADD_SYMBOL (g_thread_new);
  GUM_ADD_SYMBOL (g_thread_join);
  GUM_ADD_SYMBOL (g_thread_ref);
  GUM_ADD_SYMBOL (g_thread_unref);
  GUM_ADD_SYMBOL (g_thread_yield);

  GUM_ADD_SYMBOL (g_mutex_init);
  GUM_ADD_SYMBOL (g_mutex_clear);
  GUM_ADD_SYMBOL (g_mutex_lock);
  GUM_ADD_SYMBOL (g_mutex_unlock);
  GUM_ADD_SYMBOL (g_mutex_trylock);
  GUM_ADD_SYMBOL (g_cond_init);
  GUM_ADD_SYMBOL (g_cond_clear);
  GUM_ADD_SYMBOL (g_cond_wait);
  GUM_ADD_SYMBOL (g_cond_signal);
  GUM_ADD_SYMBOL (g_cond_broadcast);

  GUM_ADD_SYMBOL (g_atomic_int_add);
  GUM_ADD_SYMBOL (g_atomic_pointer_add);

  GUM_ADD_SYMBOL (gum_interceptor_get_current_invocation);

  GUM_ADD_SYMBOL (gum_invocation_context_get_nth_argument);
  GUM_ADD_SYMBOL (gum_invocation_context_replace_nth_argument);
  GUM_ADD_SYMBOL (gum_invocation_context_get_return_value);
  GUM_ADD_SYMBOL (gum_invocation_context_replace_return_value);

  GUM_ADD_SYMBOL (gum_invocation_context_get_return_address);

  GUM_ADD_SYMBOL (gum_invocation_context_get_thread_id);
  GUM_ADD_SYMBOL (gum_invocation_context_get_depth);

  GUM_ADD_SYMBOL (gum_invocation_context_get_listener_thread_data);
  GUM_ADD_SYMBOL (gum_invocation_context_get_listener_function_data);
  GUM_ADD_SYMBOL (gum_invocation_context_get_listener_invocation_data);

  GUM_ADD_SYMBOL (gum_invocation_context_get_replacement_data);

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
    gum_cmodule_call_finalize (cmodule);

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

    gum_cmodule_call_init (self);
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

static void
gum_cmodule_call_init (GumCModule * self)
{
  void (* init) (void);

  init = tcc_get_symbol (self->state, "init");
  if (init != NULL)
    init ();
}

static void
gum_cmodule_call_finalize (GumCModule * self)
{
  void (* finalize) (void);

  finalize = tcc_get_symbol (self->state, "finalize");
  if (finalize != NULL)
    finalize ();
}
