/*
 * Copyright (C) 2009-2019 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 * Copyright (C) 2017 Antonio Ken Iannillo <ak.iannillo@gmail.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "stalker-arm64-fixture.c"

#include <lzma.h>
#ifdef HAVE_LINUX
# include <sys/prctl.h>
#endif

TESTLIST_BEGIN (stalker)

  /* EVENTS */
  TESTENTRY (no_events)
  TESTENTRY (call)
  TESTENTRY (ret)
  TESTENTRY (exec)
  TESTENTRY (call_depth)

  /* PROBES */
  TESTENTRY (call_probe)

  /* TRANSFORMERS */
  TESTENTRY (custom_transformer)
  TESTENTRY (unfollow_should_be_allowed_before_first_transform)
  TESTENTRY (unfollow_should_be_allowed_mid_first_transform)
  TESTENTRY (unfollow_should_be_allowed_after_first_transform)
  TESTENTRY (unfollow_should_be_allowed_before_second_transform)
  TESTENTRY (unfollow_should_be_allowed_mid_second_transform)
  TESTENTRY (unfollow_should_be_allowed_after_second_transform)

  /* EXCLUSION */
  TESTENTRY (exclude_bl)
  TESTENTRY (exclude_blr)
  TESTENTRY (exclude_bl_with_unfollow)
  TESTENTRY (exclude_blr_with_unfollow)

  /* BRANCH */
  TESTENTRY (unconditional_branch)
  TESTENTRY (unconditional_branch_reg)
  TESTENTRY (conditional_branch)
  TESTENTRY (compare_and_branch)
  TESTENTRY (test_bit_and_branch)

  /* FOLLOWS */
  TESTENTRY (follow_std_call)
  TESTENTRY (follow_return)
  TESTENTRY (follow_syscall)
  TESTENTRY (follow_thread)

  /* EXTRA */
  TESTENTRY (heap_api)
  TESTENTRY (no_register_clobber)
  TESTENTRY (performance)

TESTLIST_END ()

static void insert_extra_add_after_sub (GumStalkerIterator * iterator,
    GumStalkerWriter * output, gpointer user_data);
static void store_x0 (GumCpuContext * cpu_context, gpointer user_data);
static void unfollow_during_transform (GumStalkerIterator * iterator,
    GumStalkerWriter * output, gpointer user_data);
static gboolean store_range_of_test_runner (const GumModuleDetails * details,
    gpointer user_data);
static void pretend_workload (GumMemoryRange * runner_range);

gint gum_stalker_dummy_global_to_trick_optimizer = 0;

static const guint32 flat_code[] = {
  0xCB000000, /* SUB W0, W0, W0 */
  0x91000400, /* ADD W0, W0, #1 */
  0x91000400, /* ADD W0, W0, #1 */
  0xd65f03c0  /* RET            */
};

static StalkerTestFunc
invoke_flat_expecting_return_value (TestArm64StalkerFixture * fixture,
                                    GumEventType mask,
                                    guint expected_return_value)
{
  StalkerTestFunc func;
  gint ret;

  func = GUM_POINTER_TO_FUNCPTR (StalkerTestFunc,
      test_arm64_stalker_fixture_dup_code (fixture, flat_code,
      sizeof (flat_code)));

  fixture->sink->mask = mask;
  ret = test_arm64_stalker_fixture_follow_and_invoke (fixture, func, -1);
  g_assert_cmpint (ret, ==, expected_return_value);

  return func;
}

static StalkerTestFunc
invoke_flat (TestArm64StalkerFixture * fixture,
             GumEventType mask)
{
  return invoke_flat_expecting_return_value (fixture, mask, 2);
}

TESTCASE (no_events)
{
  invoke_flat (fixture, GUM_NOTHING);
  g_assert_cmpuint (fixture->sink->events->len, ==, 0);
}

TESTCASE (call)
{
  StalkerTestFunc func;
  GumCallEvent * ev;

  func = invoke_flat (fixture, GUM_CALL);

  g_assert_cmpuint (fixture->sink->events->len, ==, 2);
  g_assert_cmpint (g_array_index (fixture->sink->events, GumEvent,
      0).type, ==, GUM_CALL);
  ev = &g_array_index (fixture->sink->events, GumEvent, 0).call;
  GUM_ASSERT_CMPADDR (ev->location, ==, fixture->last_invoke_calladdr);
  GUM_ASSERT_CMPADDR (ev->target, ==, func);
}

TESTCASE (ret)
{
  StalkerTestFunc func;
  GumRetEvent * ev;

  func = invoke_flat (fixture, GUM_RET);

  g_assert_cmpuint (fixture->sink->events->len, ==, 1);
  g_assert_cmpint (g_array_index (fixture->sink->events, GumEvent,
      0).type, ==, GUM_RET);

  ev = &g_array_index (fixture->sink->events, GumEvent, 0).ret;

  GUM_ASSERT_CMPADDR (ev->location, ==, ((guint8 *) GSIZE_TO_POINTER (
      func)) + 3 * 4);
  GUM_ASSERT_CMPADDR (ev->target, ==, fixture->last_invoke_retaddr);
}

TESTCASE (exec)
{
  StalkerTestFunc func;
  GumRetEvent * ev;

  func = invoke_flat (fixture, GUM_EXEC);

  g_assert_cmpuint (fixture->sink->events->len, ==, INVOKER_INSN_COUNT + 4);
  g_assert_cmpint (g_array_index (fixture->sink->events, GumEvent,
      INVOKER_IMPL_OFFSET).type, ==, GUM_EXEC);
  ev =
      &g_array_index (fixture->sink->events, GumEvent, INVOKER_IMPL_OFFSET).ret;
  GUM_ASSERT_CMPADDR (ev->location, ==, func);
}

TESTCASE (call_depth)
{
  guint8 * code;
  GumArm64Writer cw;
  gpointer func_a, func_b;
  const gchar * start_lbl = "start";
  StalkerTestFunc func;
  gint r;

  code = gum_alloc_n_pages (1, GUM_PAGE_RW);
  gum_arm64_writer_init (&cw, code);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_follow_me), 3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->transformer),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->sink));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_b_label (&cw, start_lbl);

  func_b = gum_arm64_writer_cur (&cw);
  gum_arm64_writer_put_add_reg_reg_imm (&cw, ARM64_REG_X0, ARM64_REG_X0, 7);
  gum_arm64_writer_put_ret (&cw);

  func_a = gum_arm64_writer_cur (&cw);
  gum_arm64_writer_put_push_reg_reg (&cw, ARM64_REG_X19, ARM64_REG_LR);
  gum_arm64_writer_put_add_reg_reg_imm (&cw, ARM64_REG_X0, ARM64_REG_X0, 3);
  gum_arm64_writer_put_bl_imm (&cw, GUM_ADDRESS (func_b));
  gum_arm64_writer_put_pop_reg_reg (&cw, ARM64_REG_X19, ARM64_REG_LR);
  gum_arm64_writer_put_ret (&cw);

  gum_arm64_writer_put_label (&cw, start_lbl);
  gum_arm64_writer_put_push_reg_reg (&cw, ARM64_REG_X19, ARM64_REG_LR);
  gum_arm64_writer_put_bl_imm (&cw, GUM_ADDRESS (func_a));
  gum_arm64_writer_put_pop_reg_reg (&cw, ARM64_REG_X19, ARM64_REG_LR);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_unfollow_me), 1,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_ret (&cw);

  gum_arm64_writer_flush (&cw);
  gum_memory_mark_code (code, gum_arm64_writer_offset (&cw));
  gum_arm64_writer_clear (&cw);

  fixture->sink->mask = GUM_CALL | GUM_RET;
  func = GUM_POINTER_TO_FUNCPTR (StalkerTestFunc, code);
  r = func (2);

  g_assert_cmpint (r, ==, 12);
  g_assert_cmpuint (fixture->sink->events->len, ==, 5);
  g_assert_cmpint (NTH_EVENT_AS_CALL (0)->depth, ==, 0);
  g_assert_cmpint (NTH_EVENT_AS_CALL (1)->depth, ==, 1);
  g_assert_cmpint (NTH_EVENT_AS_RET (2)->depth, ==, 2);
  g_assert_cmpint (NTH_EVENT_AS_RET (3)->depth, ==, 1);

  gum_free_pages (code);
}

typedef struct _CallProbeContext CallProbeContext;

struct _CallProbeContext
{
  guint callback_count;
  guint8 * block_start;
  gpointer call_address;
  gpointer return_address;
};

static void probe_func_a_invocation (GumCallSite * site, gpointer user_data);

TESTCASE (call_probe)
{
  const guint32 code_template[] =
  {
    0xa9bf7bf3, /* push {x19, lr} */
    0xd2801553, /* mov x19, #0xaa */
    0xd2800883, /* mov x3, #0x44  */
    0xd2800662, /* mov x2, #0x33  */
    0xd2800441, /* mov x1, #0x22  */
    0xd2800220, /* mov x0, #0x11  */
    0xa9bf07e0, /* push {x0, x1}  */
    0x94000009, /* bl func_a      */
    0xa8c107e0, /* pop {x0, x1}   */
    0xd2801103, /* mov x3, #0x88  */
    0xd2800ee2, /* mov x2, #0x77  */
    0xd2800cc1, /* mov x1, #0x66  */
    0xd2800aa0, /* mov x0, #0x55  */
    0x94000005, /* bl func_b      */
    0xa8c17bf3, /* pop {x19, lr}  */
    0xd65f03c0, /* ret            */

    /* func_a: */
    0xd2801100, /* mov x0, #0x88  */
    0xd65f03c0, /* ret            */

    /* func_b: */
    0xd2801320, /* mov x0, #0x99  */
    0xd65f03c0, /* ret            */
  };
  StalkerTestFunc func;
  guint8 * func_a_address;
  CallProbeContext probe_ctx, secondary_probe_ctx;
  GumProbeId probe_id;

  func = GUM_POINTER_TO_FUNCPTR (StalkerTestFunc,
      test_arm64_stalker_fixture_dup_code (fixture, code_template,
          sizeof (code_template)));

  func_a_address = fixture->code + (16 * 4);

  probe_ctx.callback_count = 0;
  probe_ctx.block_start = fixture->code;
  probe_ctx.call_address = fixture->code + (7 * 4);
  probe_ctx.return_address = fixture->code + (8 * 4);
  probe_id = gum_stalker_add_call_probe (fixture->stalker,
      func_a_address, probe_func_a_invocation, &probe_ctx, NULL);
  test_arm64_stalker_fixture_follow_and_invoke (fixture, func, 0);
  g_assert_cmpuint (probe_ctx.callback_count, ==, 1);

  secondary_probe_ctx.callback_count = 0;
  secondary_probe_ctx.block_start = fixture->code;
  secondary_probe_ctx.call_address = probe_ctx.call_address;
  secondary_probe_ctx.return_address = probe_ctx.return_address;
  gum_stalker_add_call_probe (fixture->stalker,
      func_a_address, probe_func_a_invocation, &secondary_probe_ctx, NULL);
  test_arm64_stalker_fixture_follow_and_invoke (fixture, func, 0);
  g_assert_cmpuint (probe_ctx.callback_count, ==, 2);
  g_assert_cmpuint (secondary_probe_ctx.callback_count, ==, 1);

  gum_stalker_remove_call_probe (fixture->stalker, probe_id);
  test_arm64_stalker_fixture_follow_and_invoke (fixture, func, 0);
  g_assert_cmpuint (probe_ctx.callback_count, ==, 2);
  g_assert_cmpuint (secondary_probe_ctx.callback_count, ==, 2);
}

static void
probe_func_a_invocation (GumCallSite * site,
                         gpointer user_data)
{
  CallProbeContext * ctx = (CallProbeContext *) user_data;

  ctx->callback_count++;

  GUM_ASSERT_CMPADDR (site->block_address, ==, ctx->block_start);
  g_assert_cmphex (site->cpu_context->x[0], ==, 0x11);
  g_assert_cmphex (site->cpu_context->x[1], ==, 0x22);
  g_assert_cmphex (site->cpu_context->x[2], ==, 0x33);
  g_assert_cmphex (site->cpu_context->x[3], ==, 0x44);
  g_assert_cmphex (site->cpu_context->x[19], ==, 0xaa);
  g_assert_cmphex (site->cpu_context->pc,
      ==, GPOINTER_TO_SIZE (ctx->call_address));
  g_assert_cmphex (site->cpu_context->lr,
      ==, GPOINTER_TO_SIZE (ctx->return_address));
  g_assert_cmphex (((gsize *) site->stack_data)[0], ==, 0x11);
  g_assert_cmphex (((gsize *) site->stack_data)[1], ==, 0x22);
}

TESTCASE (custom_transformer)
{
  guint64 last_x0 = 0;

  fixture->transformer = gum_stalker_transformer_make_from_callback (
      insert_extra_add_after_sub, &last_x0, NULL);

  g_assert_cmpuint (last_x0, ==, 0);

  invoke_flat_expecting_return_value (fixture, GUM_NOTHING, 3);

  g_assert_cmpuint (last_x0, ==, 3);
}

static void
insert_extra_add_after_sub (GumStalkerIterator * iterator,
                            GumStalkerWriter * output,
                            gpointer user_data)
{
  guint64 * last_x0 = user_data;
  const cs_insn * insn;
  gboolean in_leaf_func;

  in_leaf_func = FALSE;

  while (gum_stalker_iterator_next (iterator, &insn))
  {
    if (in_leaf_func && insn->id == ARM64_INS_RET)
    {
      gum_stalker_iterator_put_callout (iterator, store_x0, last_x0, NULL);
    }

    gum_stalker_iterator_keep (iterator);

    if (insn->id == ARM64_INS_SUB)
    {
      in_leaf_func = TRUE;

      gum_arm64_writer_put_add_reg_reg_imm (&output->arm64, ARM64_REG_W0,
          ARM64_REG_W0, 1);
    }
  }
}

static void
store_x0 (GumCpuContext * cpu_context,
          gpointer user_data)
{
  guint64 * last_x0 = user_data;

  *last_x0 = cpu_context->x[0];
}

TESTCASE (unfollow_should_be_allowed_before_first_transform)
{
  UnfollowTransformContext ctx;

  ctx.stalker = fixture->stalker;
  ctx.num_blocks_transformed = 0;
  ctx.target_block = 0;
  ctx.max_instructions = 0;

  fixture->transformer = gum_stalker_transformer_make_from_callback (
      unfollow_during_transform, &ctx, NULL);

  invoke_flat_expecting_return_value (fixture, GUM_NOTHING, 2);
}

TESTCASE (unfollow_should_be_allowed_mid_first_transform)
{
  UnfollowTransformContext ctx;

  ctx.stalker = fixture->stalker;
  ctx.num_blocks_transformed = 0;
  ctx.target_block = 0;
  ctx.max_instructions = 1;

  fixture->transformer = gum_stalker_transformer_make_from_callback (
      unfollow_during_transform, &ctx, NULL);

  invoke_flat_expecting_return_value (fixture, GUM_NOTHING, 2);
}

TESTCASE (unfollow_should_be_allowed_after_first_transform)
{
  UnfollowTransformContext ctx;

  ctx.stalker = fixture->stalker;
  ctx.num_blocks_transformed = 0;
  ctx.target_block = 0;
  ctx.max_instructions = -1;

  fixture->transformer = gum_stalker_transformer_make_from_callback (
      unfollow_during_transform, &ctx, NULL);

  invoke_flat_expecting_return_value (fixture, GUM_NOTHING, 2);
}

TESTCASE (unfollow_should_be_allowed_before_second_transform)
{
  UnfollowTransformContext ctx;

  ctx.stalker = fixture->stalker;
  ctx.num_blocks_transformed = 0;
  ctx.target_block = 1;
  ctx.max_instructions = 0;

  fixture->transformer = gum_stalker_transformer_make_from_callback (
      unfollow_during_transform, &ctx, NULL);

  invoke_flat_expecting_return_value (fixture, GUM_NOTHING, 2);
}

TESTCASE (unfollow_should_be_allowed_mid_second_transform)
{
  UnfollowTransformContext ctx;

  ctx.stalker = fixture->stalker;
  ctx.num_blocks_transformed = 0;
  ctx.target_block = 1;
  ctx.max_instructions = 1;

  fixture->transformer = gum_stalker_transformer_make_from_callback (
      unfollow_during_transform, &ctx, NULL);

  invoke_flat_expecting_return_value (fixture, GUM_NOTHING, 2);
}

TESTCASE (unfollow_should_be_allowed_after_second_transform)
{
  UnfollowTransformContext ctx;

  ctx.stalker = fixture->stalker;
  ctx.num_blocks_transformed = 0;
  ctx.target_block = 1;
  ctx.max_instructions = -1;

  fixture->transformer = gum_stalker_transformer_make_from_callback (
      unfollow_during_transform, &ctx, NULL);

  invoke_flat_expecting_return_value (fixture, GUM_NOTHING, 2);
}

static void
unfollow_during_transform (GumStalkerIterator * iterator,
                           GumStalkerWriter * output,
                           gpointer user_data)
{
  UnfollowTransformContext * ctx = user_data;
  const cs_insn * insn;

  if (ctx->num_blocks_transformed == ctx->target_block)
  {
    gint n;

    for (n = 0; n != ctx->max_instructions &&
        gum_stalker_iterator_next (iterator, &insn); n++)
    {
      gum_stalker_iterator_keep (iterator);
    }

    gum_stalker_unfollow_me (ctx->stalker);
  }
  else
  {
    while (gum_stalker_iterator_next (iterator, &insn))
      gum_stalker_iterator_keep (iterator);
  }

  ctx->num_blocks_transformed++;
}

TESTCASE (exclude_bl)
{
  const guint32 code_template[] =
  {
    0xa9bf7bf3, /* push {x19, lr} */
    0xd2801553, /* mov x19, #0xaa */
    0xd2800883, /* mov x3, #0x44  */
    0xd2800662, /* mov x2, #0x33  */
    0xd2800441, /* mov x1, #0x22  */
    0xd2800220, /* mov x0, #0x11  */
    0xa9bf07e0, /* push {x0, x1}  */
    0x94000009, /* bl func_a      */
    0xa8c107e0, /* pop {x0, x1}   */
    0xd2801103, /* mov x3, #0x88  */
    0xd2800ee2, /* mov x2, #0x77  */
    0xd2800cc1, /* mov x1, #0x66  */
    0xd2800aa0, /* mov x0, #0x55  */
    0x94000005, /* bl func_b      */
    0xa8c17bf3, /* pop {x19, lr}  */
    0xd65f03c0, /* ret            */

    /* func_a: */
    0xd2801100, /* mov x0, #0x88  */
    0xd65f03c0, /* ret            */

    /* func_b: */
    0xd2801320, /* mov x0, #0x99  */
    0xd65f03c0, /* ret            */
  };
  StalkerTestFunc func;
  guint8 * func_a_address;
  GumMemoryRange memory_range;

  fixture->sink->mask = GUM_EXEC;

  func = GUM_POINTER_TO_FUNCPTR (StalkerTestFunc,
      test_arm64_stalker_fixture_dup_code (fixture, code_template,
          sizeof (code_template)));

  func_a_address = fixture->code + (16 * 4);
  memory_range.base_address = (GumAddress) func_a_address;
  memory_range.size = 4 * 2;
  gum_stalker_exclude (fixture->stalker, &memory_range);

  g_assert_cmpuint (fixture->sink->events->len, ==, 0);

  test_arm64_stalker_fixture_follow_and_invoke (fixture, func, 0);

  g_assert_cmpuint (fixture->sink->events->len, ==, 24);
}

TESTCASE (exclude_blr)
{
  StalkerTestFunc func;
  guint8 * code;
  GumArm64Writer cw;
  gpointer func_a;
  GumMemoryRange memory_range;
  const gchar * start_lbl = "start";

  fixture->sink->mask = GUM_EXEC;

  code = gum_alloc_n_pages (1, GUM_PAGE_RW);
  gum_arm64_writer_init (&cw, code);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_follow_me), 3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->transformer),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->sink));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_b_label (&cw, start_lbl);

  func_a = gum_arm64_writer_cur (&cw);
  gum_arm64_writer_put_add_reg_reg_imm (&cw, ARM64_REG_X0, ARM64_REG_X0, 10);
  gum_arm64_writer_put_ret (&cw);

  gum_arm64_writer_put_label (&cw, start_lbl);
  gum_arm64_writer_put_push_reg_reg (&cw, ARM64_REG_X19, ARM64_REG_LR);
  gum_arm64_writer_put_ldr_reg_address (&cw, ARM64_REG_X1,
      GUM_ADDRESS (func_a));
  gum_arm64_writer_put_blr_reg (&cw, ARM64_REG_X1);
  gum_arm64_writer_put_pop_reg_reg (&cw, ARM64_REG_X19, ARM64_REG_LR);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_unfollow_me), 1,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_ret (&cw);

  gum_arm64_writer_flush (&cw);
  gum_memory_mark_code (code, gum_arm64_writer_offset (&cw));
  gum_arm64_writer_clear (&cw);

  memory_range.base_address = GUM_ADDRESS (func_a);
  memory_range.size = 4 * 2;
  gum_stalker_exclude (fixture->stalker, &memory_range);

  func = GUM_POINTER_TO_FUNCPTR (StalkerTestFunc, code);

  g_assert_cmpuint (fixture->sink->events->len, ==, 0);

  g_assert_cmpint (func (2), ==, 12);

#ifdef HAVE_DARWIN
  g_assert_cmpuint (fixture->sink->events->len, ==, 41);
#else
  g_assert_cmpuint (fixture->sink->events->len, ==, 42);
#endif

  gum_free_pages (code);
}

TESTCASE (exclude_bl_with_unfollow)
{
  StalkerTestFunc func;
  guint8 * code;
  GumArm64Writer cw;
  gpointer func_a;
  GumMemoryRange memory_range;
  const gchar * start_lbl = "start";

  fixture->sink->mask = GUM_EXEC;

  code = gum_alloc_n_pages (1, GUM_PAGE_RW);
  gum_arm64_writer_init (&cw, code);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_follow_me), 3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->transformer),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->sink));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_b_label (&cw, start_lbl);

  func_a = gum_arm64_writer_cur (&cw);
  gum_arm64_writer_put_push_reg_reg (&cw, ARM64_REG_X19, ARM64_REG_LR);
  gum_arm64_writer_put_add_reg_reg_imm (&cw, ARM64_REG_X0, ARM64_REG_X0, 10);
  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_unfollow_me), 1,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker));
  gum_arm64_writer_put_pop_all_x_registers (&cw);
  gum_arm64_writer_put_pop_reg_reg (&cw, ARM64_REG_X19, ARM64_REG_LR);
  gum_arm64_writer_put_ret (&cw);

  gum_arm64_writer_put_label (&cw, start_lbl);

  gum_arm64_writer_put_push_reg_reg (&cw, ARM64_REG_X19, ARM64_REG_LR);
  gum_arm64_writer_put_bl_imm (&cw, GUM_ADDRESS (func_a));
  gum_arm64_writer_put_pop_reg_reg (&cw, ARM64_REG_X19, ARM64_REG_LR);

  gum_arm64_writer_put_ret (&cw);

  gum_arm64_writer_flush (&cw);
  gum_memory_mark_code (code, gum_arm64_writer_offset (&cw));
  gum_arm64_writer_clear (&cw);

  memory_range.base_address = GUM_ADDRESS (func_a);
  memory_range.size = 4 * 20;
  gum_stalker_exclude (fixture->stalker, &memory_range);

  func = GUM_POINTER_TO_FUNCPTR (StalkerTestFunc, code);

  g_assert_cmpuint (fixture->sink->events->len, ==, 0);

  g_assert_cmpint (func (2), ==, 12);

  g_assert_cmpuint (fixture->sink->events->len, ==, 21);

  gum_free_pages (code);
}

TESTCASE (exclude_blr_with_unfollow)
{
  StalkerTestFunc func;
  guint8 * code;
  GumArm64Writer cw;
  gpointer func_a;
  GumMemoryRange memory_range;
  const gchar * start_lbl = "start";

  fixture->sink->mask = GUM_EXEC;

  code = gum_alloc_n_pages (1, GUM_PAGE_RW);
  gum_arm64_writer_init (&cw, code);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_follow_me), 3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->transformer),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->sink));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_b_label (&cw, start_lbl);

  func_a = gum_arm64_writer_cur (&cw);
  gum_arm64_writer_put_push_reg_reg (&cw, ARM64_REG_X19, ARM64_REG_LR);
  gum_arm64_writer_put_add_reg_reg_imm (&cw, ARM64_REG_X0, ARM64_REG_X0, 10);
  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_unfollow_me), 1,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker));
  gum_arm64_writer_put_pop_all_x_registers (&cw);
  gum_arm64_writer_put_pop_reg_reg (&cw, ARM64_REG_X19, ARM64_REG_LR);
  gum_arm64_writer_put_ret (&cw);

  gum_arm64_writer_put_label (&cw, start_lbl);

  gum_arm64_writer_put_push_reg_reg (&cw, ARM64_REG_X19, ARM64_REG_LR);
  gum_arm64_writer_put_ldr_reg_address (&cw, ARM64_REG_X1,
      GUM_ADDRESS (func_a));
  gum_arm64_writer_put_blr_reg (&cw, ARM64_REG_X1);
  gum_arm64_writer_put_pop_reg_reg (&cw, ARM64_REG_X19, ARM64_REG_LR);

  gum_arm64_writer_put_ret (&cw);

  gum_arm64_writer_flush (&cw);
  gum_memory_mark_code (code, gum_arm64_writer_offset (&cw));
  gum_arm64_writer_clear (&cw);

  memory_range.base_address = GUM_ADDRESS (func_a);
  memory_range.size = 4 * 20;
  gum_stalker_exclude (fixture->stalker, &memory_range);

  func = GUM_POINTER_TO_FUNCPTR (StalkerTestFunc, code);

  g_assert_cmpuint (fixture->sink->events->len, ==, 0);

  g_assert_cmpint (func (2), ==, 12);

  g_assert_cmpuint (fixture->sink->events->len, ==, 21);

  gum_free_pages (code);
}

TESTCASE (unconditional_branch)
{
  guint8 * code;
  GumArm64Writer cw;
  GumAddress address;
  const gchar * my_ken_lbl = "my_ken";
  StalkerTestFunc func;

  code = gum_alloc_n_pages (1, GUM_PAGE_RW);
  gum_arm64_writer_init (&cw, code);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_follow_me), 3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->transformer),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->sink));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_nop (&cw);
  gum_arm64_writer_put_nop (&cw);
  gum_arm64_writer_put_b_label (&cw, my_ken_lbl);

  address = GUM_ADDRESS (gum_arm64_writer_cur (&cw));
  gum_arm64_writer_put_add_reg_reg_imm (&cw, ARM64_REG_X0, ARM64_REG_X0, 10);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_unfollow_me), 1,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_ret (&cw);

  gum_arm64_writer_put_label (&cw, my_ken_lbl);
  gum_arm64_writer_put_add_reg_reg_imm (&cw, ARM64_REG_X0, ARM64_REG_X0, 1);
  gum_arm64_writer_put_b_imm (&cw, address);

  gum_arm64_writer_flush (&cw);
  gum_memory_mark_code (code, gum_arm64_writer_offset (&cw));
  gum_arm64_writer_clear (&cw);

  fixture->sink->mask = GUM_CALL | GUM_RET | GUM_EXEC;
  func = GUM_POINTER_TO_FUNCPTR (StalkerTestFunc, code);

  g_assert_cmpint (func (2), ==, 13);

  gum_free_pages (code);
}

TESTCASE (unconditional_branch_reg)
{
  guint8 * code;
  GumArm64Writer cw;
  GumAddress address;
  const gchar * my_ken_lbl = "my_ken";
  StalkerTestFunc func;
  arm64_reg reg = ARM64_REG_X13;

  code = gum_alloc_n_pages (1, GUM_PAGE_RW);
  gum_arm64_writer_init (&cw, code);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_follow_me), 3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->transformer),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->sink));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_nop (&cw);
  gum_arm64_writer_put_nop (&cw);
  gum_arm64_writer_put_b_label (&cw, my_ken_lbl);

  address = GUM_ADDRESS (gum_arm64_writer_cur (&cw));
  gum_arm64_writer_put_add_reg_reg_imm (&cw, ARM64_REG_X0, ARM64_REG_X0, 10);
  if (reg == ARM64_REG_X29 || reg == ARM64_REG_X30)
    gum_arm64_writer_put_pop_reg_reg (&cw, reg, ARM64_REG_XZR);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_unfollow_me), 1,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_ret (&cw);

  gum_arm64_writer_put_label (&cw, my_ken_lbl);
  gum_arm64_writer_put_add_reg_reg_imm (&cw, ARM64_REG_X0, ARM64_REG_X0, 1);
  if (reg == ARM64_REG_X29 || reg == ARM64_REG_X30)
    gum_arm64_writer_put_push_reg_reg (&cw, reg, reg);
  gum_arm64_writer_put_ldr_reg_address (&cw, reg, address);
  gum_arm64_writer_put_br_reg (&cw, reg);

  gum_arm64_writer_flush (&cw);
  gum_memory_mark_code (code, gum_arm64_writer_offset (&cw));
  gum_arm64_writer_clear (&cw);

  fixture->sink->mask = GUM_CALL | GUM_RET | GUM_EXEC;
  func = GUM_POINTER_TO_FUNCPTR (StalkerTestFunc, code);

  g_assert_cmpint (func (2), ==, 13);

  gum_free_pages (code);
}

TESTCASE (conditional_branch)
{
  guint8 * code;
  GumArm64Writer cw;
  GumAddress address;
  arm64_cc cc = ARM64_CC_EQ;
  const gchar * my_ken_lbl = "my_ken";
  StalkerTestFunc func;
  gint r;

  code = gum_alloc_n_pages (1, GUM_PAGE_RW);
  gum_arm64_writer_init (&cw, code);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_follow_me), 3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->transformer),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->sink));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_nop (&cw);
  gum_arm64_writer_put_nop (&cw);
  gum_arm64_writer_put_instruction (&cw, 0xF1000800);  /* SUBS X0, X0, #2 */
  gum_arm64_writer_put_b_cond_label (&cw, cc, my_ken_lbl);

  address = GUM_ADDRESS (gum_arm64_writer_cur (&cw));
  gum_arm64_writer_put_nop (&cw);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_unfollow_me), 1,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_ret (&cw);

  gum_arm64_writer_put_label (&cw, my_ken_lbl);
  gum_arm64_writer_put_add_reg_reg_imm (&cw, ARM64_REG_X0, ARM64_REG_X0, 1);
  gum_arm64_writer_put_b_imm (&cw, address);

  gum_arm64_writer_flush (&cw);
  gum_memory_mark_code (code, gum_arm64_writer_offset (&cw));
  gum_arm64_writer_clear (&cw);

  fixture->sink->mask = GUM_CALL | GUM_RET | GUM_EXEC;
  func = GUM_POINTER_TO_FUNCPTR (StalkerTestFunc, code);
  r = func (2);

  g_assert_cmpint (r, ==, 1);

  gum_free_pages (code);
}

TESTCASE (compare_and_branch)
{
  guint8 * code;
  GumArm64Writer cw;
  const gchar * my_ken_lbl = "my_ken";
  const gchar * my_nken_lbl = "my_nken";
  StalkerTestFunc func;
  gint r;

  code = gum_alloc_n_pages (1, GUM_PAGE_RW);
  gum_arm64_writer_init (&cw, code);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_follow_me), 3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->transformer),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->sink));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_nop (&cw);
  gum_arm64_writer_put_nop (&cw);
  gum_arm64_writer_put_sub_reg_reg_imm (&cw, ARM64_REG_X0, ARM64_REG_X0, 2);
  gum_arm64_writer_put_cbz_reg_label (&cw, ARM64_REG_X0, my_ken_lbl);

  gum_arm64_writer_put_label (&cw, my_nken_lbl);
  gum_arm64_writer_put_nop (&cw);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_unfollow_me), 1,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_ret (&cw);

  gum_arm64_writer_put_label (&cw, my_ken_lbl);
  gum_arm64_writer_put_add_reg_reg_imm (&cw, ARM64_REG_X0, ARM64_REG_X0, 1);
  gum_arm64_writer_put_cbnz_reg_label (&cw, ARM64_REG_X0, my_nken_lbl);

  gum_arm64_writer_flush (&cw);
  gum_memory_mark_code (code, gum_arm64_writer_offset (&cw));
  gum_arm64_writer_clear (&cw);

  fixture->sink->mask = GUM_CALL | GUM_RET | GUM_EXEC;
  func = GUM_POINTER_TO_FUNCPTR (StalkerTestFunc, code);
  r = func (2);

  g_assert_cmpint (r, ==, 1);

  gum_free_pages (code);
}

TESTCASE (test_bit_and_branch)
{
  guint8 * code;
  GumArm64Writer cw;
  const gchar * my_ken_lbl = "my_ken";
  const gchar * my_nken_lbl = "my_nken";
  StalkerTestFunc func;
  gint r;

  code = gum_alloc_n_pages (1, GUM_PAGE_RW);
  gum_arm64_writer_init (&cw, code);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_follow_me), 3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->transformer),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->sink));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_nop (&cw);
  gum_arm64_writer_put_nop (&cw);
  gum_arm64_writer_put_sub_reg_reg_imm (&cw, ARM64_REG_X0, ARM64_REG_X0, 2);
  gum_arm64_writer_put_tbz_reg_imm_label (&cw, ARM64_REG_W0, 0, my_ken_lbl);

  gum_arm64_writer_put_label (&cw, my_nken_lbl);
  gum_arm64_writer_put_nop (&cw);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_unfollow_me), 1,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_ret (&cw);

  gum_arm64_writer_put_label (&cw, my_ken_lbl);
  gum_arm64_writer_put_add_reg_reg_imm (&cw, ARM64_REG_X0, ARM64_REG_X0, 1);
  gum_arm64_writer_put_tbnz_reg_imm_label (&cw, ARM64_REG_W0, 0, my_nken_lbl);

  gum_arm64_writer_flush (&cw);
  gum_memory_mark_code (code, gum_arm64_writer_offset (&cw));
  gum_arm64_writer_clear (&cw);

  fixture->sink->mask = GUM_CALL | GUM_RET | GUM_EXEC;
  func = GUM_POINTER_TO_FUNCPTR (StalkerTestFunc, code);
  r = func (2);

  g_assert_cmpint (r, ==, 1);

  gum_free_pages (code);
}

TESTCASE (follow_std_call)
{
  guint8 * code;
  GumArm64Writer cw;
  GumAddress address;
  const gchar * my_ken_lbl = "my_ken";
  StalkerTestFunc func;
  gint r;

  code = gum_alloc_n_pages (1, GUM_PAGE_RW);
  gum_arm64_writer_init (&cw, code);

  gum_arm64_writer_put_push_reg_reg (&cw, ARM64_REG_X30, ARM64_REG_X29);
  gum_arm64_writer_put_mov_reg_reg (&cw, ARM64_REG_X29, ARM64_REG_SP);

  gum_arm64_writer_put_b_label (&cw, my_ken_lbl);

  address = GUM_ADDRESS (gum_arm64_writer_cur (&cw));
  gum_arm64_writer_put_add_reg_reg_imm (&cw, ARM64_REG_X0, ARM64_REG_X0, 1);
  gum_arm64_writer_put_ret (&cw);

  gum_arm64_writer_put_label (&cw, my_ken_lbl);
  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_follow_me), 3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->transformer),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->sink));
  gum_arm64_writer_put_pop_all_x_registers (&cw);
  gum_arm64_writer_put_add_reg_reg_imm (&cw, ARM64_REG_X0, ARM64_REG_X0, 1);
  gum_arm64_writer_put_bl_imm (&cw, address);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_unfollow_me), 1,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_pop_reg_reg (&cw, ARM64_REG_X30, ARM64_REG_X29);
  gum_arm64_writer_put_ret (&cw);

  gum_arm64_writer_flush (&cw);
  gum_memory_mark_code (code, gum_arm64_writer_offset (&cw));
  gum_arm64_writer_clear (&cw);

  fixture->sink->mask = GUM_CALL | GUM_RET | GUM_EXEC;
  func = GUM_POINTER_TO_FUNCPTR (StalkerTestFunc, code);
  r = func (2);

  g_assert_cmpint (r, ==, 4);

  gum_free_pages (code);
}

TESTCASE (follow_return)
{
  guint8 * code;
  GumArm64Writer cw;
  GumAddress address;
  const gchar * my_ken_lbl = "my_ken";
  StalkerTestFunc func;
  gint r;

  code = gum_alloc_n_pages (1, GUM_PAGE_RW);
  gum_arm64_writer_init (&cw, code);

  gum_arm64_writer_put_push_reg_reg (&cw, ARM64_REG_X30, ARM64_REG_X29);
  gum_arm64_writer_put_mov_reg_reg (&cw, ARM64_REG_X29, ARM64_REG_SP);

  gum_arm64_writer_put_b_label (&cw, my_ken_lbl);

  address = GUM_ADDRESS (gum_arm64_writer_cur (&cw));
  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_follow_me), 3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->transformer),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->sink));
  gum_arm64_writer_put_pop_all_x_registers (&cw);
  /*
   * alternative for instruction RET X15
   * gum_arm64_writer_put_mov_reg_reg (&cw, ARM64_REG_X15, ARM64_REG_X30);
   * gum_arm64_writer_put_instruction (&cw, 0xD65F01E0);
   */
  gum_arm64_writer_put_ret (&cw);

  gum_arm64_writer_put_label (&cw, my_ken_lbl);
  gum_arm64_writer_put_nop (&cw);
  gum_arm64_writer_put_add_reg_reg_imm (&cw, ARM64_REG_X0, ARM64_REG_X0, 1);
  gum_arm64_writer_put_bl_imm (&cw, address);
  gum_arm64_writer_put_add_reg_reg_imm (&cw, ARM64_REG_X0, ARM64_REG_X0, 1);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_unfollow_me), 1,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_pop_reg_reg (&cw, ARM64_REG_X30, ARM64_REG_X29);
  gum_arm64_writer_put_ret (&cw);

  gum_arm64_writer_flush (&cw);
  gum_memory_mark_code (code, gum_arm64_writer_offset (&cw));
  gum_arm64_writer_clear (&cw);

  fixture->sink->mask = GUM_CALL | GUM_RET | GUM_EXEC;
  func = GUM_POINTER_TO_FUNCPTR (StalkerTestFunc, code);
  r = func (2);

  g_assert_cmpint (r, ==, 4);

  gum_free_pages (code);
}

TESTCASE (follow_syscall)
{
  fixture->sink->mask = (GumEventType) (GUM_EXEC | GUM_CALL | GUM_RET);

  gum_stalker_follow_me (fixture->stalker, fixture->transformer,
      GUM_EVENT_SINK (fixture->sink));
  g_usleep (1);
  gum_stalker_unfollow_me (fixture->stalker);

  g_assert_cmpuint (fixture->sink->events->len, >, 0);
}

static gpointer
stalker_victim (gpointer data)
{
  StalkerVictimContext * ctx = (StalkerVictimContext *) data;

  g_mutex_lock (&ctx->mutex);

  /* 2: Signal readyness, giving our thread id */
  ctx->state = STALKER_VICTIM_READY_FOR_FOLLOW;
  ctx->thread_id = gum_process_get_current_thread_id ();
  g_cond_signal (&ctx->cond);

  /* 3: Wait for master to tell us we're being followed */
  while (ctx->state != STALKER_VICTIM_IS_FOLLOWED)
    g_cond_wait (&ctx->cond, &ctx->mutex);

  /* 6: Signal that we're ready to be unfollowed */
  ctx->state = STALKER_VICTIM_READY_FOR_UNFOLLOW;
  g_cond_signal (&ctx->cond);

  /* 7: Wait for master to tell us we're no longer followed */
  while (ctx->state != STALKER_VICTIM_IS_UNFOLLOWED)
    g_cond_wait (&ctx->cond, &ctx->mutex);

  /* 10: Signal that we're ready for a reset */
  ctx->state = STALKER_VICTIM_READY_FOR_SHUTDOWN;
  g_cond_signal (&ctx->cond);

  /* 11: Wait for master to tell us we can call it a day */
  while (ctx->state != STALKER_VICTIM_IS_SHUTDOWN)
    g_cond_wait (&ctx->cond, &ctx->mutex);

  g_mutex_unlock (&ctx->mutex);

  return NULL;
}

TESTCASE (follow_thread)
{
  StalkerVictimContext ctx;
  GumThreadId thread_id;
  GThread * thread;
#ifdef HAVE_LINUX
  int prev_dumpable;

  /* Android spawns non-debuggable applications as not dumpable by default. */
  prev_dumpable = prctl (PR_GET_DUMPABLE);
  prctl (PR_SET_DUMPABLE, 0);
#endif

  ctx.state = STALKER_VICTIM_CREATED;
  g_mutex_init (&ctx.mutex);
  g_cond_init (&ctx.cond);

  thread = g_thread_new ("stalker-test-victim", stalker_victim, &ctx);

  /* 1: Wait for victim to tell us it's ready, giving its thread id */
  g_mutex_lock (&ctx.mutex);
  while (ctx.state != STALKER_VICTIM_READY_FOR_FOLLOW)
    g_cond_wait (&ctx.cond, &ctx.mutex);
  thread_id = ctx.thread_id;
  g_mutex_unlock (&ctx.mutex);

  /* 4: Follow and notify victim about it */
  fixture->sink->mask = (GumEventType) (GUM_EXEC | GUM_CALL | GUM_RET);
  gum_stalker_follow (fixture->stalker, thread_id, NULL,
      GUM_EVENT_SINK (fixture->sink));
  g_mutex_lock (&ctx.mutex);
  ctx.state = STALKER_VICTIM_IS_FOLLOWED;
  g_cond_signal (&ctx.cond);
  g_mutex_unlock (&ctx.mutex);

  /* 5: Wait for victim to tell us to unfollow */
  g_mutex_lock (&ctx.mutex);
  while (ctx.state != STALKER_VICTIM_READY_FOR_UNFOLLOW)
    g_cond_wait (&ctx.cond, &ctx.mutex);
  g_mutex_unlock (&ctx.mutex);

  g_assert_cmpuint (fixture->sink->events->len, >, 0);

  /* 8: Unfollow and notify victim about it */
  gum_stalker_unfollow (fixture->stalker, thread_id);
  g_mutex_lock (&ctx.mutex);
  ctx.state = STALKER_VICTIM_IS_UNFOLLOWED;
  g_cond_signal (&ctx.cond);
  g_mutex_unlock (&ctx.mutex);

  /* 9: Wait for victim to tell us it's ready for us to reset the sink */
  g_mutex_lock (&ctx.mutex);
  while (ctx.state != STALKER_VICTIM_READY_FOR_SHUTDOWN)
    g_cond_wait (&ctx.cond, &ctx.mutex);
  g_mutex_unlock (&ctx.mutex);

  gum_fake_event_sink_reset (fixture->sink);

  /* 12: Tell victim to it' */
  g_mutex_lock (&ctx.mutex);
  ctx.state = STALKER_VICTIM_IS_SHUTDOWN;
  g_cond_signal (&ctx.cond);
  g_mutex_unlock (&ctx.mutex);

  g_thread_join (thread);

  g_assert_cmpuint (fixture->sink->events->len, ==, 0);

  g_mutex_clear (&ctx.mutex);
  g_cond_clear (&ctx.cond);

#ifdef HAVE_LINUX
  prctl (PR_SET_DUMPABLE, prev_dumpable);
#endif
}

TESTCASE (heap_api)
{
  gpointer p;

  fixture->sink->mask = (GumEventType) (GUM_EXEC | GUM_CALL | GUM_RET);

  gum_stalker_follow_me (fixture->stalker, fixture->transformer,
      GUM_EVENT_SINK (fixture->sink));
  p = malloc (1);
  free (p);
  gum_stalker_unfollow_me (fixture->stalker);

  g_assert_cmpuint (fixture->sink->events->len, >, 0);
}

typedef void (* ClobberFunc) (GumCpuContext * ctx);

TESTCASE (no_register_clobber)
{
#ifndef HAVE_DARWIN
  guint8 * code;
  GumArm64Writer cw;
  gint i;
  ClobberFunc func;
  GumCpuContext ctx;

  code = gum_alloc_n_pages (1, GUM_PAGE_RW);
  gum_arm64_writer_init (&cw, code);

  gum_arm64_writer_put_push_all_x_registers (&cw);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_follow_me), 3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->transformer),
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->sink));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  for (i = ARM64_REG_X0; i <= ARM64_REG_X28; i++)
  {
    gum_arm64_writer_put_ldr_reg_u64 (&cw, i, i);
  }
  gum_arm64_writer_put_ldr_reg_u64 (&cw, ARM64_REG_FP, ARM64_REG_FP);
  gum_arm64_writer_put_ldr_reg_u64 (&cw, ARM64_REG_LR, ARM64_REG_LR);

  gum_arm64_writer_put_push_all_x_registers (&cw);
  gum_arm64_writer_put_call_address_with_arguments (&cw,
      GUM_ADDRESS (gum_stalker_unfollow_me), 1,
      GUM_ARG_ADDRESS, GUM_ADDRESS (fixture->stalker));
  gum_arm64_writer_put_pop_all_x_registers (&cw);

  gum_arm64_writer_put_push_reg_reg (&cw, ARM64_REG_FP, ARM64_REG_LR);
  gum_arm64_writer_put_ldr_reg_reg_offset (&cw, ARM64_REG_FP, ARM64_REG_SP,
      (2 + 30) * sizeof (gpointer));
  for (i = ARM64_REG_X0; i <= ARM64_REG_X28; i++)
  {
    gum_arm64_writer_put_str_reg_reg_offset (&cw, i, ARM64_REG_FP,
        G_STRUCT_OFFSET (GumCpuContext, x[i - ARM64_REG_X0]));
  }
  gum_arm64_writer_put_pop_reg_reg (&cw, ARM64_REG_FP, ARM64_REG_LR);

  gum_arm64_writer_put_ldr_reg_reg_offset (&cw, ARM64_REG_X0, ARM64_REG_SP,
      30 * sizeof (gpointer));
  gum_arm64_writer_put_str_reg_reg_offset (&cw, ARM64_REG_FP, ARM64_REG_X0,
      G_STRUCT_OFFSET (GumCpuContext, fp));
  gum_arm64_writer_put_str_reg_reg_offset (&cw, ARM64_REG_LR, ARM64_REG_X0,
      G_STRUCT_OFFSET (GumCpuContext, lr));

  gum_arm64_writer_put_pop_all_x_registers (&cw);
  gum_arm64_writer_put_ret (&cw);

  gum_arm64_writer_flush (&cw);
  gum_memory_mark_code (code, gum_arm64_writer_offset (&cw));
  gum_arm64_writer_clear (&cw);

  fixture->sink->mask = GUM_CALL | GUM_RET | GUM_EXEC;
  func = GUM_POINTER_TO_FUNCPTR (ClobberFunc, code);
  func (&ctx);

  for (i = ARM64_REG_X0; i <= ARM64_REG_X28; i++)
  {
    g_assert_cmphex (ctx.x[i - ARM64_REG_X0], ==, i);
  }
  g_assert_cmphex (ctx.fp, ==, ARM64_REG_FP);
  g_assert_cmphex (ctx.lr, ==, ARM64_REG_LR);

  gum_free_pages (code);
#endif
}

TESTCASE (performance)
{
  GumMemoryRange runner_range;
  GTimer * timer;
  gdouble duration_direct, duration_stalked;

#ifdef HAVE_IOS
  if (!gum_process_is_debugger_attached ())
  {
    g_print ("<skipping, no debugger attached> ");
    return;
  }
#endif

  runner_range.base_address = 0;
  runner_range.size = 0;
  gum_process_enumerate_modules (store_range_of_test_runner, &runner_range);
  g_assert_true (runner_range.base_address != 0 && runner_range.size != 0);

  timer = g_timer_new ();
  pretend_workload (&runner_range);

  g_timer_reset (timer);
  pretend_workload (&runner_range);
  duration_direct = g_timer_elapsed (timer, NULL);

  fixture->sink->mask = GUM_NOTHING;

  gum_stalker_set_trust_threshold (fixture->stalker, 0);
  gum_stalker_follow_me (fixture->stalker, fixture->transformer,
      GUM_EVENT_SINK (fixture->sink));

  /* warm-up */
  g_timer_reset (timer);
  pretend_workload (&runner_range);
  g_timer_elapsed (timer, NULL);

  /* the real deal */
  gum_stalker_set_counters_enabled (TRUE);
  g_timer_reset (timer);
  pretend_workload (&runner_range);
  duration_stalked = g_timer_elapsed (timer, NULL);

  gum_stalker_unfollow_me (fixture->stalker);

  g_timer_destroy (timer);

  g_print ("<duration_direct=%f duration_stalked=%f ratio=%f> ",
      duration_direct, duration_stalked, duration_stalked / duration_direct);

  gum_stalker_dump_counters ();
}

static gboolean
store_range_of_test_runner (const GumModuleDetails * details,
                            gpointer user_data)
{
  GumMemoryRange * runner_range = user_data;

  if (strstr (details->name, "gum-tests") != NULL)
  {
    *runner_range = *details->range;
    return FALSE;
  }

  return TRUE;
}

GUM_NOINLINE static void
pretend_workload (GumMemoryRange * runner_range)
{
  lzma_stream stream = LZMA_STREAM_INIT;
  const uint32_t preset = 9 | LZMA_PRESET_EXTREME;
  lzma_ret ret;
  guint8 * outbuf;
  gsize outbuf_size;
  const gsize outbuf_size_increment = 1024 * 1024;

  ret = lzma_easy_encoder (&stream, preset, LZMA_CHECK_CRC64);
  g_assert_cmpint (ret, ==, LZMA_OK);

  outbuf_size = outbuf_size_increment;
  outbuf = malloc (outbuf_size);

  stream.next_in = GSIZE_TO_POINTER (runner_range->base_address);
  stream.avail_in = MIN (runner_range->size, 65536);
  stream.next_out = outbuf;
  stream.avail_out = outbuf_size;

  while (TRUE)
  {
    ret = lzma_code (&stream, LZMA_FINISH);

    if (stream.avail_out == 0)
    {
      gsize compressed_size;

      compressed_size = outbuf_size;

      outbuf_size += outbuf_size_increment;
      outbuf = realloc (outbuf, outbuf_size);

      stream.next_out = outbuf + compressed_size;
      stream.avail_out = outbuf_size - compressed_size;
    }

    if (ret != LZMA_OK)
    {
      g_assert_cmpint (ret, ==, LZMA_STREAM_END);
      break;
    }
  }

  lzma_end (&stream);

  free (outbuf);
}
