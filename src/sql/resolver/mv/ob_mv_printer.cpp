/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX SQL_RESV
#include "common/ob_smart_call.h"
#include "sql/rewrite/ob_expand_aggregate_utils.h"
#include "sql/resolver/mv/ob_mv_printer.h"
#include "sql/optimizer/ob_optimizer_util.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

// view name
const ObString ObMVPrinter::DELTA_TABLE_VIEW_NAME  = "DLT_T$$";
const ObString ObMVPrinter::DELTA_BASIC_MAV_VIEW_NAME = "DLT_BASIC_MV$$";
const ObString ObMVPrinter::DELTA_MAV_VIEW_NAME = "DLT_MV$$";
// column name
const ObString ObMVPrinter::HEAP_TABLE_ROWKEY_COL_NAME  = "M_ROW$$";
const ObString ObMVPrinter::OLD_NEW_COL_NAME  = "OLD_NEW$$";
const ObString ObMVPrinter::SEQUENCE_COL_NAME  = "SEQUENCE$$";
const ObString ObMVPrinter::DML_FACTOR_COL_NAME  = "DMLFACTOR$$";
const ObString ObMVPrinter::WIN_MAX_SEQ_COL_NAME  = "MAXSEQ$$";
const ObString ObMVPrinter::WIN_MIN_SEQ_COL_NAME  = "MINSEQ$$";

int ObMVPrinter::print_mv_operators(const share::schema::ObTableSchema &mv_schema,
                                    const ObSelectStmt &view_stmt,
                                    const bool for_rt_expand,
                                    const share::SCN &last_refresh_scn,
                                    const share::SCN &refresh_scn,
                                    ObIAllocator &alloc,
                                    ObIAllocator &str_alloc,
                                    ObSchemaGetterGuard *schema_guard,
                                    ObStmtFactory &stmt_factory,
                                    ObRawExprFactory &expr_factory,
                                    ObSQLSessionInfo *session_info,
                                    ObIArray<ObString> &operators,
                                    ObMVRefreshableType &refreshable_type)
{
  int ret = OB_SUCCESS;
  operators.reuse();
  refreshable_type = OB_MV_REFRESH_INVALID;
  ObMVChecker checker(view_stmt, expr_factory, session_info);
  ObMVPrinter printer(alloc, mv_schema, checker, for_rt_expand, stmt_factory, expr_factory);
  ObSEArray<ObDMLStmt*, 4> dml_stmts;
  if (OB_ISNULL(view_stmt.get_query_ctx())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret), K(view_stmt.get_query_ctx()));
  } else if (OB_FAIL(checker.check_mv_refresh_type())) {
    LOG_WARN("failed to check mv refresh type", K(ret));
  } else if (OB_MV_COMPLETE_REFRESH >= (refreshable_type = checker.get_refersh_type())) {
    LOG_TRACE("mv not support fast refresh", K(refreshable_type), K(mv_schema.get_table_name()));
  } else if (OB_FAIL(printer.init(last_refresh_scn, refresh_scn))) {
    LOG_WARN("failed to init mv printer", K(ret));
  } else if (OB_FAIL(printer.gen_mv_operator_stmts(dml_stmts))) {
    LOG_WARN("failed to print mv operators", K(ret));
  } else if (OB_UNLIKELY(dml_stmts.empty())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected empty array", K(ret), K(dml_stmts.empty()));
  } else if (OB_FAIL(operators.prepare_allocate(dml_stmts.count()))) {
    LOG_WARN("failed to prepare allocate ObSqlString arrays", K(ret), K(dml_stmts.count()));
  } else {
    ObObjPrintParams obj_print_params(view_stmt.get_query_ctx()->get_timezone_info());
    obj_print_params.print_origin_stmt_ = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < dml_stmts.count(); ++i) {
      if (OB_FAIL(ObSQLUtils::reconstruct_sql(str_alloc, dml_stmts.at(i), operators.at(i), schema_guard))) {
        LOG_WARN("fail to reconstruct sql", K(ret));
      } else {
        LOG_TRACE("generate one mv operator", K(i), K(operators.at(i)));
      }
    }
  }
  return ret;
}

int ObMVPrinter::gen_mv_operator_stmts(ObIArray<ObDMLStmt*> &dml_stmts)
{
  int ret = OB_SUCCESS;
  dml_stmts.reuse();
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("printer is not inited", K(ret));
  } else {
    switch (mv_checker_.get_refersh_type()) {
      case OB_MV_FAST_REFRESH_SIMPLE_MAV: {
        if (OB_FAIL(gen_refresh_dmls_for_simple_mav(dml_stmts))) {
          LOG_WARN("fail to gen refresh dmls for simple mav", K(ret));
        }
        break;
      }
      default:  {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected refresh type", K(ret), K(mv_checker_.get_refersh_type()));
        break;
      }
    }
  }
  return ret;
}

int ObMVPrinter::gen_refresh_dmls_for_simple_mav(ObIArray<ObDMLStmt*> &dml_stmts)
{
  int ret = OB_SUCCESS;
  dml_stmts.reuse();
  ObMergeStmt *merge_stmt = NULL;
  if (for_rt_expand_) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("not supported now", K(ret));
  } else if (lib::is_mysql_mode()) {
    if (OB_FAIL(gen_update_insert_delete_for_simple_mav(dml_stmts))) {
      LOG_WARN("failed to gen update insert delete for simple mav", K(ret));
    }
  } else if (OB_FAIL(gen_merge_for_simple_mav(merge_stmt))) {
    LOG_WARN("failed to gen merge into for simple mav", K(ret));
  } else if (OB_FAIL(dml_stmts.push_back(merge_stmt))) {
    LOG_WARN("failed to push back", K(ret));
  }
  return ret;
}

int ObMVPrinter::gen_update_insert_delete_for_simple_mav(ObIArray<ObDMLStmt*> &dml_stmts)
{
  int ret = OB_SUCCESS;
  dml_stmts.reuse();
  ObUpdateStmt *update_stmt = NULL;
  ObInsertStmt *insert_stmt = NULL;
  ObDeleteStmt *delete_stmt = NULL;
  ObSelectStmt *delta_mv_stmt = NULL;
  ObSEArray<ObRawExpr*, 16> values;
  if (OB_FAIL(gen_simple_mav_delta_mv_view(delta_mv_stmt))) {
    LOG_WARN("failed gen simple source stmt", K(ret));
  } else if (OB_FAIL(gen_insert_for_mav(delta_mv_stmt, values, insert_stmt))) {
    LOG_WARN("failed to gen insert for mav", K(ret));
  } else if (OB_FAIL(gen_update_for_mav(delta_mv_stmt, insert_stmt->get_values_desc(),
                                        values, update_stmt))) {
    LOG_WARN("failed to gen update for mav", K(ret));
  } else if (OB_FAIL(dml_stmts.push_back(update_stmt))  // pushback and execute in this ordering
             || OB_FAIL(dml_stmts.push_back(insert_stmt))) {
    LOG_WARN("failed to push back", K(ret));
  } else if (mv_checker_.get_stmt().is_scala_group_by()) {
    /* no need delete for scalar group by */
  } else if (OB_FAIL(gen_delete_for_mav(insert_stmt->get_values_desc(), delete_stmt))) {
    LOG_WARN("failed gen delete for mav", K(ret));
  } else if (OB_FAIL(dml_stmts.push_back(delete_stmt))) { // pushback and execute in this ordering
    LOG_WARN("failed to push back", K(ret));
  }
  return ret;
}

/*
  insert into mv
  select ... from delta_mv
  where not exists (select 1 from mv where delta_mv.c1 <=> mv.c1);
*/
int ObMVPrinter::gen_insert_for_mav(ObSelectStmt *delta_mv_stmt,
                                    ObIArray<ObRawExpr*> &values,
                                    ObInsertStmt *&insert_stmt)
{
  int ret = OB_SUCCESS;
  TableItem *target_table = NULL;
  TableItem *source_table = NULL;
  ObSelectStmt *sel_stmt = NULL;
  ObRawExpr *filter_expr = NULL;
  if (OB_FAIL(create_simple_stmt(sel_stmt))
      || OB_FAIL(create_simple_stmt(insert_stmt))) {
    LOG_WARN("failed to create simple stmt", K(ret));
  } else if (OB_FAIL(create_simple_table_item(sel_stmt, DELTA_BASIC_MAV_VIEW_NAME, source_table, delta_mv_stmt))) {
    LOG_WARN("failed to create simple table item", K(ret));
  } else if (OB_FAIL(create_simple_table_item(insert_stmt, mv_schema_.get_table_name(), target_table, NULL, false))) {
    LOG_WARN("failed to create simple table item", K(ret));
  } else if (OB_FAIL(gen_insert_values_and_desc(target_table,
                                                source_table,
                                                insert_stmt->get_values_desc(),
                                                values))) {
    LOG_WARN("failed to gen insert values and desc", K(ret), K(*target_table), K(*source_table));
  } else if (OB_FAIL(create_simple_table_item(insert_stmt, DELTA_MAV_VIEW_NAME, source_table, sel_stmt))) {
    LOG_WARN("failed to create simple table item", K(ret));
  } else if (OB_FAIL(gen_select_for_insert_subquery(values, sel_stmt->get_select_items()))) {
    LOG_WARN("failed to gen select for insert subquery ", K(ret));
  } else if (OB_FAIL(gen_exists_cond_for_insert(values, sel_stmt->get_condition_exprs()))) {
    LOG_WARN("failed to gen conds for insert subquery", K(ret));
  } else {
    target_table->database_name_ = mv_db_name_;
  }
  return ret;
}

int ObMVPrinter::gen_select_for_insert_subquery(const ObIArray<ObRawExpr*> &values,
                                                ObIArray<SelectItem> &select_items)
{
  int ret = OB_SUCCESS;
  select_items.reuse();
  SelectItem sel_item;
  const ObIArray<SelectItem> &orig_select_items = mv_checker_.get_stmt().get_select_items();
  for (int64_t i = 0; OB_SUCC(ret) && i < orig_select_items.count(); ++i) {
    sel_item.expr_ = values.at(i);
    sel_item.is_real_alias_ = true;
    sel_item.alias_name_ = orig_select_items.at(i).alias_name_;
    if (OB_FAIL(select_items.push_back(sel_item))) {
      LOG_WARN("failed to pushback", K(ret));
    }
  }
  return ret;
}

//  generate: not exists (select 1 from mv where delta_mv.c1 <=> mv.c1)
int ObMVPrinter::gen_exists_cond_for_insert(const ObIArray<ObRawExpr*> &values,
                                            ObIArray<ObRawExpr*> &conds)
{
  int ret = OB_SUCCESS;
  conds.reuse();
  ObOpRawExpr *not_exists_expr = NULL;
  ObQueryRefRawExpr *query_ref_expr = NULL;
  ObSelectStmt *subquery = NULL;
  TableItem *mv_table = NULL;
  SelectItem sel_item;
  sel_item.expr_ = exprs_.int_one_;
  const ObIArray<SelectItem> &select_items = mv_checker_.get_stmt().get_select_items();
  const ObIArray<ObRawExpr*> &group_by_exprs = mv_checker_.get_stmt().get_group_exprs();
  if (OB_UNLIKELY(values.count() != select_items.count())) {
    LOG_WARN("unexpected params", K(ret), K(values.count()), K(select_items.count()));
  } else if (OB_FAIL(create_simple_stmt(subquery))) {
    LOG_WARN("failed to create simple stmt", K(ret));
  } else if (OB_FAIL(expr_factory_.create_raw_expr(T_REF_QUERY, query_ref_expr))
             || OB_FAIL(expr_factory_.create_raw_expr(T_OP_NOT_EXISTS, not_exists_expr))) {
    LOG_WARN("failed to create raw expr", K(ret));
  } else if (OB_ISNULL(query_ref_expr) || OB_ISNULL(not_exists_expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected NULL", K(ret), K(query_ref_expr), K(not_exists_expr));
  } else if (OB_FAIL(not_exists_expr->add_param_expr(query_ref_expr))) {
    LOG_WARN("failed to add param expr", K(ret));
  } else if (OB_FAIL(conds.push_back(not_exists_expr))) {
    LOG_WARN("failed to push back not exists expr", K(ret));
  } else if (OB_FAIL(create_simple_table_item(subquery, mv_schema_.get_table_name(), mv_table))) {
    LOG_WARN("failed to create simple table item", K(ret));
  } else if (OB_FAIL(subquery->get_select_items().push_back(sel_item))) {
    LOG_WARN("failed to push back not exists expr", K(ret));
  } else {
    mv_table->database_name_ = mv_db_name_;
    query_ref_expr->set_ref_stmt(subquery);
    ObRawExpr *expr = NULL;
    ObRawExpr *match_cond = NULL;
    for (int64_t i = 0; OB_SUCC(ret) && i < select_items.count(); ++i) {
      if (!ObOptimizerUtil::find_item(group_by_exprs, select_items.at(i).expr_)) {
        /* not group by exprs, do nothing */
      } else if (OB_FAIL(create_simple_column_expr(mv_table->get_table_name(), select_items.at(i).alias_name_, mv_table->table_id_, expr))) {
        LOG_WARN("failed to create simple column exprs", K(ret));
      } else if (OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_NSEQ, values.at(i), expr, match_cond))) {
        LOG_WARN("failed to build null safe equal expr", K(ret));
      } else if (OB_FAIL(subquery->get_condition_exprs().push_back(match_cond))) {
        LOG_WARN("failed to push back null safe equal expr", K(ret));
      }
    }
  }
  return ret;
}

/*
  update mv, delta_mv
  set mv.cnt = mv.cnt + delta_mv.d_cnt,
      ...
  where delta_mv.c1 <=> mv.c1);
*/
int ObMVPrinter::gen_update_for_mav(ObSelectStmt *delta_mv_stmt,
                                    const ObIArray<ObColumnRefRawExpr*> &mv_columns,
                                    const ObIArray<ObRawExpr*> &values,
                                    ObUpdateStmt *&update_stmt)
{
  int ret = OB_SUCCESS;
  update_stmt = NULL;
  TableItem *target_table = NULL;
  TableItem *source_table = NULL;
  void *ptr = NULL;
  ObUpdateTableInfo *table_info = NULL;
  if (OB_FAIL(create_simple_stmt(update_stmt))) {
    LOG_WARN("failed to create simple stmt", K(ret));
  } else if (OB_ISNULL((ptr = alloc_.alloc(sizeof(ObUpdateTableInfo))))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocate table info", K(ret));
  } else if (OB_FALSE_IT(table_info = new(ptr)ObUpdateTableInfo())) {
  } else if (OB_FAIL(update_stmt->get_update_table_info().push_back(table_info))) {
    LOG_WARN("failed to push back", K(ret));
  } else if (OB_FAIL(create_simple_table_item(update_stmt, mv_schema_.get_table_name(), target_table))) {
    LOG_WARN("failed to create simple table item", K(ret));
  } else if (OB_FAIL(create_simple_table_item(update_stmt, DELTA_BASIC_MAV_VIEW_NAME, source_table, delta_mv_stmt))) {
    LOG_WARN("failed to create simple table item", K(ret));
  } else if (OB_FAIL(gen_update_assignments(mv_columns, values, source_table, table_info->assignments_))) {
    LOG_WARN("failed gen update assignments", K(ret));
  } else if (OB_FAIL(gen_update_conds(mv_columns, values, update_stmt->get_condition_exprs()))) {
    LOG_WARN("failed gen update conds", K(ret));
  } else {
    target_table->database_name_ = mv_db_name_;
  }
  return ret;
}

int ObMVPrinter::gen_update_conds(const ObIArray<ObColumnRefRawExpr*> &mv_columns,
                                  const ObIArray<ObRawExpr*> &values,
                                  ObIArray<ObRawExpr*> &conds)
{
  int ret = OB_SUCCESS;
  conds.reuse();
  const ObIArray<ObRawExpr*> &group_by_exprs = mv_checker_.get_stmt().get_group_exprs();
  const ObIArray<SelectItem> &select_items = mv_checker_.get_stmt().get_select_items();
  ObRawExpr *cond = NULL;
  if (OB_UNLIKELY(mv_columns.count() != select_items.count() || mv_columns.count() != values.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected params", K(ret), K(mv_columns.count()), K(values.count()), K(select_items.count()));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < select_items.count(); ++i) {
    if (!ObOptimizerUtil::find_item(group_by_exprs, select_items.at(i).expr_)) {
      /* do nothing */
    } else if (OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_NSEQ, mv_columns.at(i), values.at(i), cond))) {
      LOG_WARN("failed to build null safe equal expr", K(ret));
    } else if (OB_FAIL(conds.push_back(cond))) {
      LOG_WARN("failed to push back null safe equal expr", K(ret));
    }
  }
  return ret;
}

int ObMVPrinter::gen_delete_for_mav(const ObIArray<ObColumnRefRawExpr*> &mv_columns,
                                    ObDeleteStmt *&delete_stmt)
{
  int ret = OB_SUCCESS;
  delete_stmt = NULL;
  TableItem *target_table = NULL;
  if (OB_FAIL(create_simple_stmt(delete_stmt))) {
    LOG_WARN("failed to create simple stmt", K(ret));
  } else if (OB_FAIL(create_simple_table_item(delete_stmt, mv_schema_.get_table_name(), target_table))) {
    LOG_WARN("failed to create simple table item", K(ret));
  } else if (OB_FAIL(gen_delete_conds(mv_columns, delete_stmt->get_condition_exprs()))) {
    LOG_WARN("failed gen update conds", K(ret));
  } else {
    target_table->database_name_ = mv_db_name_;
  }
  return ret;
}

int ObMVPrinter::gen_delete_conds(const ObIArray<ObColumnRefRawExpr*> &mv_columns,
                                  ObIArray<ObRawExpr*> &conds)
{
  int ret = OB_SUCCESS;
  conds.reuse();
  const ObIArray<SelectItem> &select_items = mv_checker_.get_stmt().get_select_items();
  ObRawExpr *expr = NULL;
  ObRawExpr *cond = NULL;
  if (OB_UNLIKELY(mv_columns.count() != select_items.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected params", K(ret), K(mv_columns.count()), K(select_items.count()));
  }
  for (int64_t i = 0; NULL == cond && OB_SUCC(ret) && i < select_items.count(); ++i) {
    if (OB_ISNULL(expr = select_items.at(i).expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected select item", K(ret), K(i), K(select_items));
    } else if (T_FUN_COUNT != expr->get_expr_type() ||
               0 != static_cast<ObAggFunRawExpr*>(expr)->get_real_param_count()) {
      /* do nothing */
    } else if (OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_EQ, mv_columns.at(i), exprs_.int_zero_, cond))) {
      LOG_WARN("failed to build equal expr", K(ret));
    } else if (OB_FAIL(conds.push_back(cond))) {
      LOG_WARN("failed to push back expr", K(ret));
    }
  }
  return ret;
}

/*
  merge into mv
  using delta_mv d_mv
  on (mv.c1 <=> d_mv.c1 and mv.c2 <=> d_mv.c2)
  when matched then update set
      cnt = cnt + d_cnt,
      cnt_c3 = cnt_c3 + d_cnt_c3,
      sum_c3 = case when cnt_c3 + d_cnt_c3 = 0 then null
                    when cnt_c3 = 0 then d_sum_c3
                    when d_cnt_c3 = 0 then sum_c3
                    else d_sum_c3 + sum_c3 end,
      avg_c3 = case when ... sum_c3 new value ... end
                        / (cnt_c3 + d_cnt_c3),
      calc_1 = ...
    delete where cnt = 0
  when not matched then insert
      (c1, c2, cnt, sum_c3, avg_c3, cnt_c3, calc_1, calc_2)
      values (d_mv.c1,
              d_mv.c2,
              d_mv.d_cnt,    -- count(*)
              case when d_mv.d_cnt_c3 = 0 then null else d_mv.d_sum_c3 end,    -- sum(c3)
              (case when d_mv.d_cnt_c3 = 0 then null else d_mv.d_sum_c3 end)
              / d_mv.d_cnt_c3,    -- avg(c3)
              d_mv.d_cnt_c3,    -- count(c3)
              ...
    where d_mv.d_cnt <> 0;
 */
int ObMVPrinter::gen_merge_for_simple_mav(ObMergeStmt *&merge_stmt)
{
  int ret = OB_SUCCESS;
  merge_stmt = NULL;
  TableItem *target_table = NULL;
  TableItem *source_table = NULL;
  if (OB_FAIL(create_simple_stmt(merge_stmt))) {
    LOG_WARN("failed to create simple stmt", K(ret));
  } else if (OB_FAIL(gen_merge_tables(*merge_stmt, target_table, source_table))) {
    LOG_WARN("failed to gen merge tables", K(ret));
  } else if (OB_FAIL(gen_insert_values_and_desc(target_table,
                                                source_table,
                                                merge_stmt->get_values_desc(),
                                                merge_stmt->get_values_vector()))) {
    LOG_WARN("failed to gen insert values and desc", K(ret), K(*target_table), K(*source_table));
  } else if (OB_FAIL(gen_update_assignments(merge_stmt->get_values_desc(),
                                            merge_stmt->get_values_vector(),
                                            source_table,
                                            merge_stmt->get_table_assignments()))) {
    LOG_WARN("failed to gen update assignments", K(ret));
  } else if (OB_FAIL(gen_merge_conds(*merge_stmt))) {
    LOG_WARN("failed to gen merge conds", K(ret));
  }
  return ret;
}

int ObMVPrinter::gen_merge_tables(ObMergeStmt &merge_stmt,
                                  TableItem *&target_table,
                                  TableItem *&source_table)
{
  int ret = OB_SUCCESS;
  target_table = NULL;
  source_table = NULL;
  ObSelectStmt *source_stmt = NULL;
  if (OB_FAIL(create_simple_table_item(&merge_stmt, mv_schema_.get_table_name(), target_table, NULL, false))) {
    LOG_WARN("failed to create simple table item", K(ret));
  } else if (OB_FAIL(gen_simple_mav_delta_mv_view(source_stmt))) {
    LOG_WARN("failed gen simple source stmt", K(ret));
  } else if (OB_FAIL(create_simple_table_item(&merge_stmt, DELTA_BASIC_MAV_VIEW_NAME, source_table, source_stmt, false))) {
    LOG_WARN("failed to create simple table item", K(ret));
  } else if (OB_ISNULL(target_table) || OB_ISNULL(source_table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret), K(target_table), K(source_table));
  } else {
    target_table->database_name_ = mv_db_name_;
    merge_stmt.set_target_table_id(target_table->table_id_);
    merge_stmt.set_source_table_id(source_table->table_id_);
  }
  return ret;
}

// generate insert values in 3 steps:
// 1. group by exprs and count aggr
// 2. sum aggr
// 3. other select exprs
int ObMVPrinter::gen_insert_values_and_desc(const TableItem *target_table,
                                            const TableItem *source_table,
                                            ObIArray<ObColumnRefRawExpr*> &target_columns,
                                            ObIArray<ObRawExpr*> &values_exprs)
{
  int ret = OB_SUCCESS;
  target_columns.reuse();
  values_exprs.reuse();
  const ObIArray<SelectItem> &select_items = mv_checker_.get_stmt().get_select_items();
  const ObIArray<ObRawExpr*> &orig_group_by_exprs = mv_checker_.get_stmt().get_group_exprs();
  ObRawExpr *expr = NULL;
  ObRawExpr *target_expr = NULL;
  ObRawExpr *source_expr = NULL;
  ObRawExprCopier copier(expr_factory_);
  if (OB_ISNULL(target_table) || OB_ISNULL(source_table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("target table or source table is null", K(ret), K(target_table), K(source_table));
  } else if (OB_FAIL(target_columns.prepare_allocate(select_items.count()))
             || OB_FAIL(values_exprs.prepare_allocate(select_items.count()))) {
    LOG_WARN("failed to prepare allocate arrays", K(ret));
  }

  // 1. add group by exprs and count aggr
  for (int64_t i = 0; OB_SUCC(ret) && i < select_items.count(); ++i) {
    const SelectItem &select_item = select_items.at(i);
    if (OB_ISNULL(expr = select_item.expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected select item", K(ret), K(i), K(select_items));
    } else if (OB_FAIL(create_simple_column_expr(target_table->get_table_name(), select_item.alias_name_, target_table->table_id_, target_expr))) {
      LOG_WARN("failed to create simple column exprs", K(ret));
    } else if (!ObOptimizerUtil::find_item(orig_group_by_exprs, expr)
                && T_FUN_COUNT != expr->get_expr_type()) {
      target_columns.at(i) = static_cast<ObColumnRefRawExpr*>(target_expr);
      values_exprs.at(i) = NULL;
    } else if (OB_FAIL(create_simple_column_expr(source_table->get_table_name(), select_item.alias_name_, source_table->table_id_, source_expr))) {
      LOG_WARN("failed to create simple column exprs", K(ret));
    } else if (OB_FAIL(copier.add_replaced_expr(expr, source_expr))) {
      LOG_WARN("failed to add replace pair", K(ret));
    } else {
      target_columns.at(i) = static_cast<ObColumnRefRawExpr*>(target_expr);
      values_exprs.at(i) = source_expr;
    }
  }

  // 2. add sum aggr
  for (int64_t i = 0; OB_SUCC(ret) && i < select_items.count(); ++i) {
    ObRawExpr *source_count = NULL;
    ObRawExpr *target_count = NULL;
    if (OB_ISNULL(expr = select_items.at(i).expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected select item", K(ret), K(i), K(select_items));
    } else if (NULL != values_exprs.at(i) || T_FUN_SUM != expr->get_expr_type()) {
      /* do nothing */
    } else if (OB_FAIL(create_simple_column_expr(source_table->get_table_name(), select_items.at(i).alias_name_, source_table->table_id_, source_expr))) {
      LOG_WARN("failed to create simple column exprs", K(ret));
    } else if (OB_FAIL(get_dependent_aggr_of_fun_sum(expr, select_items, target_columns, values_exprs,
                                                     target_count, source_count))) {
      LOG_WARN("failed to get dependent aggr of fun sum", K(ret));
    } else if (OB_FAIL(gen_calc_expr_for_insert_clause_sum(source_count, source_expr, values_exprs.at(i)))) {
      LOG_WARN("failed to gen calc expr for aggr sum", K(ret));
    } else if (OB_FAIL(copier.add_replaced_expr(expr, values_exprs.at(i)))) {
      LOG_WARN("failed to add replace pair", K(ret));
    }
  }

  // 3. add other select exprs
  if (OB_SUCC(ret)) {
    const ObIArray<std::pair<ObAggFunRawExpr*, ObRawExpr*>> &expand_aggrs = mv_checker_.get_expand_aggrs();
    for (int64_t i = 0; OB_SUCC(ret) && i < expand_aggrs.count(); ++i) {
      if (OB_FAIL(copier.copy_on_replace(expand_aggrs.at(i).second, expr))) {
        LOG_WARN("failed to generate group by exprs", K(ret));
      } else if (OB_FAIL(copier.add_replaced_expr(expand_aggrs.at(i).first, expr))) {
        LOG_WARN("failed to add replace pair", K(ret));
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < select_items.count(); ++i) {
      if (NULL == values_exprs.at(i) &&
          OB_FAIL(copier.copy_on_replace(select_items.at(i).expr_, values_exprs.at(i)))) {
        LOG_WARN("failed to generate group by exprs", K(ret));
      }
    }
  }
  return ret;
}

//  sum(c3)
//  --> d_cnt_c3 and d_sum_c3 is output from merge source_table view
//  case when d_cnt_c3 = 0 then null else d_sum_c3 end
int ObMVPrinter::gen_calc_expr_for_insert_clause_sum(ObRawExpr *source_count,
                                                     ObRawExpr *source_sum,
                                                     ObRawExpr *&calc_sum)
{
  int ret = OB_SUCCESS;
  calc_sum = NULL;
  ObRawExpr *equal_expr = NULL;
  if (OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_EQ, source_count, exprs_.int_zero_, equal_expr))) {
      LOG_WARN("failed to build null safe equal expr", K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_case_when_expr(expr_factory_, equal_expr, exprs_.null_expr_, source_sum, calc_sum))) {
      LOG_WARN("failed to build case when expr", K(ret));
  }
  return ret;
}

//  sum(c3)
//  --> d_cnt_c3 and d_sum_c3 is output from merge source_table view
//  case when d_cnt_c3 = 0 then null else d_sum_c3 end
//  sum_c3 = case when cnt_c3 + d_cnt_c3 = 0 then null
//                when cnt_c3 = 0 then d_sum_c3
//                when d_cnt_c3 = 0 then sum_c3
//                else d_sum_c3 + sum_c3 end,
int ObMVPrinter::gen_calc_expr_for_update_clause_sum(ObRawExpr *target_count,
                                                     ObRawExpr *source_count,
                                                     ObRawExpr *target_sum,
                                                     ObRawExpr *source_sum,
                                                     ObRawExpr *&calc_sum)
{
  int ret = OB_SUCCESS;
  calc_sum = NULL;
  ObOpRawExpr *add_expr = NULL;
  ObRawExpr *when_expr = NULL;
  ObCaseOpRawExpr *case_when_expr = NULL;
  if (OB_ISNULL(target_count) || OB_ISNULL(source_count) || OB_ISNULL(target_sum) || OB_ISNULL(source_sum)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected params", K(ret), K(target_count), K(source_count), K(target_sum), K(source_sum));
  } else if (OB_FAIL(expr_factory_.create_raw_expr(T_OP_CASE, case_when_expr))) {
    LOG_WARN("create add expr failed", K(ret));
  } else if (OB_ISNULL(case_when_expr) || OB_ISNULL(exprs_.null_expr_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(case_when_expr));
  } else if (OB_FAIL(ObRawExprUtils::build_add_expr(expr_factory_, target_count, source_count, add_expr))
             || OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_EQ, add_expr, exprs_.int_zero_, when_expr))
             || OB_FAIL(case_when_expr->add_when_param_expr(when_expr))
             || OB_FAIL(case_when_expr->add_then_param_expr(exprs_.null_expr_))) {
    LOG_WARN("failed to build and add when/then exprs", K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_EQ, target_count, exprs_.int_zero_, when_expr))
             || OB_FAIL(case_when_expr->add_when_param_expr(when_expr))
             || OB_FAIL(case_when_expr->add_then_param_expr(source_sum))) {
    LOG_WARN("failed to build and add when/then exprs", K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_EQ, source_count, exprs_.int_zero_, when_expr))
             || OB_FAIL(case_when_expr->add_when_param_expr(when_expr))
             || OB_FAIL(case_when_expr->add_then_param_expr(target_sum))) {
    LOG_WARN("failed to build and add when/then exprs", K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_add_expr(expr_factory_, target_sum, source_sum, add_expr))) {
    LOG_WARN("failed to build add expr", K(ret));
  } else {
    case_when_expr->set_default_param_expr(add_expr);
    calc_sum = case_when_expr;
  }
  return ret;
}

int ObMVPrinter::get_dependent_aggr_of_fun_sum(const ObRawExpr *expr,
                                               const ObIArray<SelectItem> &select_items,
                                               const ObIArray<ObColumnRefRawExpr*> &target_columns,
                                               const ObIArray<ObRawExpr*> &values_exprs,
                                               ObRawExpr *&target_count,
                                               ObRawExpr *&source_count)
{
  int ret = OB_SUCCESS;
  target_count = NULL;
  source_count = NULL;
  const ObAggFunRawExpr *dep_aggr = NULL;
  if (OB_FAIL(ObMVChecker::get_dependent_aggr_of_fun_sum(mv_checker_.get_stmt(), static_cast<const ObAggFunRawExpr*>(expr), dep_aggr))) {
    LOG_WARN("failed to get dependent aggr of fun sum", K(ret));
  } else {
    int64_t idx = OB_INVALID_INDEX;
    for (int64_t i = 0; OB_INVALID_INDEX == idx && OB_SUCC(ret) && i < select_items.count(); ++i) {
      if (select_items.at(i).expr_ == dep_aggr) {
        idx = i;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_UNLIKELY(0 > idx || values_exprs.count() <= idx || target_columns.count() <= idx)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected idx", K(ret), K(idx), K(values_exprs.count()), K(target_columns.count()));
    } else if (OB_ISNULL(target_count = target_columns.at(idx))
               || OB_ISNULL(source_count = values_exprs.at(idx))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected NULL", K(ret), K(target_count), K(source_count));
    }
  }
  return ret;
}

// call gen_update_assignments after gen_insert_values_and_desc,
// target_columns and values_exprs is needed for this function
// generate update values in 3 steps:
// 1. count aggr
// 2. sum aggr
// 3. other select exprs except group by exprs
int ObMVPrinter::gen_update_assignments(const ObIArray<ObColumnRefRawExpr*> &target_columns,
                                        const ObIArray<ObRawExpr*> &values_exprs,
                                        const TableItem *source_table,
                                        ObIArray<ObAssignment> &assignments)
{
  int ret = OB_SUCCESS;
  assignments.reuse();
  ObRawExprCopier copier(expr_factory_);
  ObRawExpr *expr = NULL;
  ObOpRawExpr *op_expr = NULL;
  ObRawExpr *source_count = NULL;
  ObRawExpr *target_count = NULL;
  ObRawExpr *source_sum = NULL;
  const ObIArray<ObRawExpr*> &group_by_exprs = mv_checker_.get_stmt().get_group_exprs();
  const ObIArray<SelectItem> &select_items = mv_checker_.get_stmt().get_select_items();
  ObAssignment assign;
  if (OB_ISNULL(source_table) ||
      OB_UNLIKELY(target_columns.count() != select_items.count()
                  || target_columns.count() != values_exprs.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected params", K(ret), K(source_table), K(target_columns.count()),
                                          K(values_exprs.count()), K(select_items.count()));
  }
  // 1. add count aggr
  for (int64_t i = 0; OB_SUCC(ret) && i < select_items.count(); ++i) {
    assign.column_expr_ = target_columns.at(i);
    if (OB_ISNULL(expr = select_items.at(i).expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected select item", K(ret), K(i), K(select_items));
    } else if (ObOptimizerUtil::find_item(group_by_exprs, expr)) {
      if (OB_FAIL(copier.add_replaced_expr(expr, target_columns.at(i)))) {
        LOG_WARN("failed to add replace pair", K(ret));
      }
    } else if (T_FUN_COUNT != expr->get_expr_type()) {
      /* do nothing */
    } else if (OB_FAIL(ObRawExprUtils::build_add_expr(expr_factory_, target_columns.at(i), values_exprs.at(i), op_expr))) {
      LOG_WARN("failed to build add expr", K(ret));
    } else if (OB_FALSE_IT(assign.expr_ = op_expr)) {
    } else if (OB_FAIL(assignments.push_back(assign))) {
      LOG_WARN("failed to push back", K(ret));
    } else if (OB_FAIL(copier.add_replaced_expr(expr, assign.expr_))) {
      LOG_WARN("failed to add replace pair", K(ret));
    }
  }

  // 2. add sum aggr
  for (int64_t i = 0; OB_SUCC(ret) && i < select_items.count(); ++i) {
    assign.column_expr_ = target_columns.at(i);
    if (OB_ISNULL(expr = select_items.at(i).expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected select item", K(ret), K(i), K(select_items));
    } else if (T_FUN_SUM != expr->get_expr_type()) {
      /* do nothing */
    } else if (OB_FAIL(create_simple_column_expr(source_table->get_table_name(), select_items.at(i).alias_name_,
                                                 source_table->table_id_, source_sum))) {
      LOG_WARN("failed to create simple column exprs", K(ret));
    } else if (OB_FAIL(get_dependent_aggr_of_fun_sum(expr, select_items, target_columns, values_exprs,
                                                     target_count, source_count))) {
      LOG_WARN("failed to get dependent aggr of fun sum", K(ret));
    } else if (OB_FAIL(gen_calc_expr_for_update_clause_sum(target_count, source_count, target_columns.at(i),
                                                           source_sum, assign.expr_))) {
      LOG_WARN("failed to gen calc expr for aggr sum", K(ret));
    } else if (OB_FAIL(assignments.push_back(assign))) {
      LOG_WARN("failed to push back", K(ret));
    } else if (OB_FAIL(copier.add_replaced_expr(expr, assign.expr_))) {
      LOG_WARN("failed to add replace pair", K(ret));
    }
  }

  // 3. other select exprs except group by exprs
  if (OB_SUCC(ret)) {
    const ObIArray<std::pair<ObAggFunRawExpr*, ObRawExpr*>> &expand_aggrs = mv_checker_.get_expand_aggrs();
    for (int64_t i = 0; OB_SUCC(ret) && i < expand_aggrs.count(); ++i) {
      if (OB_FAIL(copier.copy_on_replace(expand_aggrs.at(i).second, expr))) {
        LOG_WARN("failed to generate group by exprs", K(ret));
      } else if (OB_FAIL(copier.add_replaced_expr(expand_aggrs.at(i).first, expr))) {
        LOG_WARN("failed to add replace pair", K(ret));
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < select_items.count(); ++i) {
      assign.column_expr_ = target_columns.at(i);
      if (OB_ISNULL(expr = select_items.at(i).expr_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected select item", K(ret), K(i), K(select_items));
      } else if (T_FUN_COUNT == expr->get_expr_type()
                 || T_FUN_SUM == expr->get_expr_type()
                 || ObOptimizerUtil::find_item(group_by_exprs, expr)) {
        /* do nothing */
      } else if (OB_FAIL(copier.copy_on_replace(expr, assign.expr_))) {
        LOG_WARN("failed to generate group by exprs", K(ret));
      } else if (OB_FAIL(assignments.push_back(assign))) {
        LOG_WARN("failed to push back", K(ret));
      }
    }
  }
  return ret;
}

// call gen_merge_conds after gen_merge_insert_clause,
// merge_stmt.get_values_desc() and merge_stmt.get_values_vector() is needed for this function
int ObMVPrinter::gen_merge_conds(ObMergeStmt &merge_stmt)
{
  int ret = OB_SUCCESS;
  merge_stmt.get_match_condition_exprs().reuse();
  merge_stmt.get_insert_condition_exprs().reuse();
  merge_stmt.get_delete_condition_exprs().reuse();

  const ObIArray<ObRawExpr*> &group_by_exprs = mv_checker_.get_stmt().get_group_exprs();
  const ObIArray<SelectItem> &select_items = mv_checker_.get_stmt().get_select_items();
  const ObIArray<ObColumnRefRawExpr*> &target_columns = merge_stmt.get_values_desc();
  const ObIArray<ObRawExpr*> &values_exprs = merge_stmt.get_values_vector();
  ObRawExpr *expr = NULL;
  ObRawExpr *match_cond = NULL;
  ObRawExpr *insert_count_expr = NULL;
  ObRawExpr *delete_count_expr = NULL;
  if (OB_UNLIKELY(target_columns.count() != select_items.count() ||
                  target_columns.count() != values_exprs.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected params", K(ret), K(target_columns.count()), K(values_exprs.count()), K(select_items.count()));
  }
  // add on condition
  for (int64_t i = 0; OB_SUCC(ret) && i < select_items.count(); ++i) {
    if (OB_ISNULL(expr = select_items.at(i).expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected select item", K(ret), K(i), K(select_items));
    } else if (T_FUN_COUNT == expr->get_expr_type()) {
      if (0 == static_cast<ObAggFunRawExpr*>(expr)->get_real_param_count()) {
        delete_count_expr = target_columns.at(i);
        insert_count_expr = values_exprs.at(i);
      }
    } else if (!ObOptimizerUtil::find_item(group_by_exprs, expr)) {
      /* do nothing */
    } else if (OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_NSEQ, target_columns.at(i), values_exprs.at(i), match_cond))) {
      LOG_WARN("failed to build null safe equal expr", K(ret));
    } else if (OB_FAIL(merge_stmt.get_match_condition_exprs().push_back(match_cond))) {
      LOG_WARN("failed to push back null safe equal expr", K(ret));
    }
  }

  // add insert/delete condition
  if (OB_SUCC(ret) && !mv_checker_.get_stmt().is_scala_group_by()) {
    ObRawExpr *insert_cond = NULL;
    ObRawExpr *delete_cond = NULL;
    if (OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_NE, insert_count_expr, exprs_.int_zero_, insert_cond))
               || OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_EQ, delete_count_expr, exprs_.int_zero_, delete_cond))) {
      LOG_WARN("failed to build equal expr", K(ret));
    } else if (OB_FAIL(merge_stmt.get_insert_condition_exprs().push_back(insert_cond))
               || OB_FAIL(merge_stmt.get_delete_condition_exprs().push_back(delete_cond))) {
      LOG_WARN("failed to push back equal expr", K(ret));
    }
  }
  return ret;
}

/*
  select  c1,
          c2,
          sum(dml_factor)                                as d_cnt,
          sum(dml_factor
              * (case when c3 is null then 0 else 1))    as d_cnt_c3,
          sum(dml_factor * c3)    as d_sum_c3
  from delta_t1
  where (old_new = 'N' and seq_no = \"MAXSEQ$$\") or (old_new = 'o' and seq_no = \"MINSEQ$$\")
      and ...
  group by c1, c2;
*/
int ObMVPrinter::gen_simple_mav_delta_mv_view(ObSelectStmt *&view_stmt)
{
  int ret = OB_SUCCESS;
  view_stmt = NULL;
  ObString empty_name;
  ObSelectStmt *delta_view = NULL;
  const TableItem *source_table = NULL;
  TableItem *table_item = NULL;
  ObRawExprCopier copier(expr_factory_);
  if (OB_UNLIKELY(1 != mv_checker_.get_stmt().get_table_size() ||
      OB_ISNULL(source_table = mv_checker_.get_stmt().get_table_item(0)))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected stmt", K(ret), K(mv_checker_.get_stmt()));
  } else if (OB_FAIL(create_simple_stmt(view_stmt))) {
    LOG_WARN("failed to create simple stmt", K(ret));
  } else if (OB_FAIL(gen_delta_table_view(*source_table, delta_view))) {
    LOG_WARN("failed to gen delta table view", K(ret));
  } else if (OB_FAIL(create_simple_table_item(view_stmt, DELTA_TABLE_VIEW_NAME, table_item, delta_view))) {
    LOG_WARN("failed to create simple table item", K(ret));
  } else if (OB_FAIL(init_expr_copier_for_delta_mv_view(*table_item, copier))) {
    LOG_WARN("failed to init expr copier", K(ret));
  } else if (OB_FAIL(copier.copy_on_replace(mv_checker_.get_stmt().get_group_exprs(),
                                            view_stmt->get_group_exprs()))) {
    LOG_WARN("failed to generate group by exprs", K(ret));
  } else if (OB_FAIL(gen_simple_mav_delta_mv_select_list(copier, *table_item,
                                                         view_stmt->get_group_exprs(),
                                                         view_stmt->get_select_items()))) {
    LOG_WARN("failed to gen select list ", K(ret));
  } else if (OB_FAIL(gen_simple_mav_delta_mv_filter(copier, *table_item, view_stmt->get_condition_exprs()))) {
    LOG_WARN("failed to gen filter ", K(ret));
  }
  return ret;
}

int ObMVPrinter::init_expr_copier_for_delta_mv_view(const TableItem &table, ObRawExprCopier &copier)
{
  int ret = OB_SUCCESS;
  const ObIArray<ColumnItem> &column_items = mv_checker_.get_stmt().get_column_items();
  ObRawExpr *old_col = NULL;
  ObRawExpr *new_col = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_items.count(); ++i) {
    const ColumnItem &col_item = column_items.at(i);
    old_col = col_item.expr_;
    if (OB_FAIL(create_simple_column_expr(table.get_table_name(), col_item.column_name_, table.table_id_, new_col))) {
      LOG_WARN("failed to create simple column expr", K(ret));
    } else if (OB_FAIL(copier.add_replaced_expr(old_col, new_col))) {
      LOG_WARN("failed to add replace pair", K(ret));
    }
  }
  return ret;
}

int ObMVPrinter::gen_simple_mav_delta_mv_select_list(ObRawExprCopier &copier,
                                                     const TableItem &table,
                                                     const ObIArray<ObRawExpr*> &group_by_exprs,
                                                     ObIArray<SelectItem> &select_items)
{
  int ret = OB_SUCCESS;
  select_items.reuse();
  const ObIArray<ObRawExpr*> &orig_group_by_exprs = mv_checker_.get_stmt().get_group_exprs();
  const ObIArray<ObAggFunRawExpr*> &orig_aggr_exprs = mv_checker_.get_stmt().get_aggr_items();
  ObRawExpr *dml_factor = NULL;
  ObAggFunRawExpr *aggr_expr = NULL;
  if (OB_UNLIKELY(orig_group_by_exprs.count() != group_by_exprs.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected group by size", K(ret), K(group_by_exprs), K(orig_group_by_exprs));
  } else if (OB_FAIL(create_simple_column_expr(table.get_table_name(), DML_FACTOR_COL_NAME, table.table_id_, dml_factor))) {
    LOG_WARN("failed to create simple column expr", K(ret));
  }
  // add select list for group by
  for (int64_t i = 0; OB_SUCC(ret) && i < group_by_exprs.count(); ++i) {
    SelectItem sel_item;
    sel_item.expr_ = group_by_exprs.at(i);
    sel_item.is_real_alias_ = true;
    if (OB_FAIL(get_mv_select_item_name(orig_group_by_exprs.at(i), sel_item.alias_name_))) {
      LOG_WARN("failed to get mv select item name", K(ret));
    } else if (OB_FAIL(select_items.push_back(sel_item))) {
      LOG_WARN("failed to pushback", K(ret));
    }
  }
  // add select list for basic aggr
  for (int64_t i = 0; OB_SUCC(ret) && i < orig_aggr_exprs.count(); ++i) {
    SelectItem sel_item;
    sel_item.is_real_alias_ = true;
    if (OB_ISNULL(aggr_expr = orig_aggr_exprs.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null", K(ret), K(aggr_expr));
    } else if (!ObMVChecker::is_basic_aggr(aggr_expr->get_expr_type())) {
      /* do nothing */
    } else if (OB_FAIL(get_mv_select_item_name(aggr_expr, sel_item.alias_name_))) {
      LOG_WARN("failed to get mv select item name", K(ret));
    } else if (OB_FAIL(gen_basic_aggr_expr(copier, dml_factor, *aggr_expr, sel_item.expr_))) {
      LOG_WARN("failed to gen basic aggr expr", K(ret));
    } else if (OB_FAIL(select_items.push_back(sel_item))) {
      LOG_WARN("failed to pushback", K(ret));
    }
  }
  return ret;
}

int ObMVPrinter::get_mv_select_item_name(const ObRawExpr *expr, ObString &select_name)
{
  int ret = OB_SUCCESS;
  select_name = ObString::make_empty_string();
  const ObIArray<SelectItem> &select_items = mv_checker_.get_stmt().get_select_items();
  for (int64_t i = 0; select_name.empty() && OB_SUCC(ret) && i < select_items.count(); ++i) {
    const SelectItem &select_item = select_items.at(i);
    if (select_item.expr_ != expr) {
      /* do nothing */
    } else if (OB_UNLIKELY(!select_item.is_real_alias_ || select_item.alias_name_.empty())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected select item", K(ret), K(i), K(select_items));
    } else {
      select_name = select_item.alias_name_;
    }
  }
  if (OB_SUCC(ret) && OB_UNLIKELY(select_name.empty())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to get select item name", K(ret), KPC(expr));
  }
  return ret;
}

// count(*)   --> sum(dml_factor)
// count(expr)  --> sum(dml_factor * (case when expr is null then 0 else 1 end))
// sum(expr)  --> sum(dml_factor * expr)
int ObMVPrinter::gen_basic_aggr_expr(ObRawExprCopier &copier,
                                     ObRawExpr *dml_factor,
                                     ObAggFunRawExpr &aggr_expr,
                                     ObRawExpr *&aggr_print_expr)
{
  int ret = OB_SUCCESS;
  aggr_print_expr = NULL;
  ObAggFunRawExpr *new_aggr_expr = NULL;
  ObRawExpr *param = NULL;
  ObRawExpr *print_param = NULL;
  if (T_FUN_COUNT == aggr_expr.get_expr_type() && 0 == aggr_expr.get_real_param_count()) {
    print_param = dml_factor;
  } else if (OB_UNLIKELY(1 != aggr_expr.get_real_param_count())
             || OB_ISNULL(param = aggr_expr.get_param_expr(0))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected aggr", K(ret), K(aggr_expr));
  } else if (OB_FAIL(copier.copy_on_replace(param, param))) {
    LOG_WARN("failed to generate group by exprs", K(ret));
  } else if (T_FUN_COUNT == aggr_expr.get_expr_type()) {
    ObRawExpr *is_null = NULL;
    ObRawExpr *case_when = NULL;
    if (OB_FAIL(ObRawExprUtils::build_is_not_null_expr(expr_factory_, param, false, is_null))) {
      LOG_WARN("failed to build is null expr", K(ret));
    } else if (OB_FAIL(ObRawExprUtils::build_case_when_expr(expr_factory_, is_null, exprs_.int_zero_, exprs_.int_one_, case_when))) {
      LOG_WARN("failed to build case when expr", K(ret));
    } else if (OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_MUL, dml_factor, case_when, print_param))) {
      LOG_WARN("failed to build mul expr", K(ret));
    }
  } else if (T_FUN_SUM == aggr_expr.get_expr_type()) {
    if (OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_MUL, dml_factor, param, print_param))) {
      LOG_WARN("failed to build mul expr", K(ret));
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected aggr", K(ret), K(aggr_expr));
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(expr_factory_.create_raw_expr(T_FUN_SUM, new_aggr_expr))) {
    LOG_WARN("create ObAggFunRawExpr failed", K(ret));
  } else if (OB_ISNULL(new_aggr_expr) || OB_ISNULL(print_param)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret), K(new_aggr_expr), K(print_param));
  } else if (OB_FAIL(new_aggr_expr->add_real_param_expr(print_param))) {
    LOG_WARN("failed to add param expr to agg expr", K(ret));
  } else if (T_FUN_COUNT != aggr_expr.get_expr_type() || !mv_checker_.get_stmt().is_scala_group_by()) {
    aggr_print_expr = new_aggr_expr;
  } else if (OB_FAIL(add_nvl_default_zero_above_expr(new_aggr_expr, aggr_print_expr))) {
    LOG_WARN("failed to gen calc expr for scalar count", K(ret));
  }
  return ret;
}

//  for scalar group by, d_cnt from sum(dml_factor) may get null, need convert null to 0
//  count(*) --> nvl(d_cnt, 0)
//  count(c3) --> nvl(d_cnt_3, 0)
int ObMVPrinter::add_nvl_default_zero_above_expr(ObRawExpr *source_expr, ObRawExpr *&expr)
{
  int ret = OB_SUCCESS;
  expr = NULL;
  ObSysFunRawExpr *nvl_expr = NULL;
  if (OB_FAIL(expr_factory_.create_raw_expr(T_FUN_SYS_NVL, nvl_expr))) {
    LOG_WARN("fail to create nvl expr", K(ret));
  } else if (OB_ISNULL(source_expr) || OB_ISNULL(exprs_.int_zero_) || OB_ISNULL(nvl_expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret), K(source_expr), K(exprs_.int_zero_), K(nvl_expr));
  } else if (OB_FAIL(nvl_expr->add_param_expr(source_expr))
             || OB_FAIL(nvl_expr->add_param_expr(exprs_.int_zero_))) {
    LOG_WARN("fail to add param expr", K(ret));
  } else {
    nvl_expr->set_expr_type(T_FUN_SYS_NVL);
    nvl_expr->set_func_name(ObString::make_string(N_NVL));
    expr = nvl_expr;
  }
  return ret;
}

//(old_new = 'N' and seq_no = "MAXSEQ$$") or (old_new = 'O' and seq_no = "MINSEQ$$")
int ObMVPrinter::gen_simple_mav_delta_mv_filter(ObRawExprCopier &copier,
                                                const TableItem &table_item,
                                                ObIArray<ObRawExpr*> &filters)
{
  int ret = OB_SUCCESS;
  filters.reuse();
  ObRawExpr *old_new = NULL;
  ObRawExpr *seq_no = NULL;
  ObRawExpr *win_seq = NULL;
  ObRawExpr *equal_old_new = NULL;
  ObRawExpr *equal_seq_no = NULL;
  ObRawExpr *new_row_filter = NULL;
  ObRawExpr *old_row_filter = NULL;
  ObRawExpr *filter = NULL;
  const ObString &table_name = table_item.get_table_name();
  const uint64_t table_id = table_item.table_id_;
  if (OB_FAIL(copier.copy_on_replace(mv_checker_.get_stmt().get_condition_exprs(), filters))) {
    LOG_WARN("failed to generate conds exprs", K(ret));
  } else if (OB_FAIL(create_simple_column_expr(table_name, OLD_NEW_COL_NAME, table_id, old_new))
             ||OB_FAIL(create_simple_column_expr(table_name, SEQUENCE_COL_NAME, table_id, seq_no))) {
    LOG_WARN("failed to create simple column exprs", K(ret));
  } else if (OB_FAIL(create_simple_column_expr(table_name, WIN_MAX_SEQ_COL_NAME , table_id, win_seq))
             || OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_EQ, old_new, exprs_.str_n_, equal_old_new))
             || OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_EQ, seq_no, win_seq, equal_seq_no))
             || OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_AND, equal_old_new, equal_seq_no, new_row_filter))) {
    LOG_WARN("failed to build new row filter expr", K(ret));
  } else if (OB_FAIL(create_simple_column_expr(table_name, WIN_MIN_SEQ_COL_NAME, table_id, win_seq))
             || OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_EQ, old_new, exprs_.str_o_, equal_old_new))
             || OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_EQ, seq_no, win_seq, equal_seq_no))
             || OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_AND, equal_old_new, equal_seq_no, old_row_filter))) {
    LOG_WARN("failed to build old row filter expr", K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_OR, new_row_filter, old_row_filter, filter))) {
    LOG_WARN("failed to build or op expr", K(ret));
  } else if (OB_FAIL(filters.push_back(filter))) {
    LOG_WARN("failed to pushback", K(ret));
  }
  return ret;
}

//  select  t.*,
//          min(sequence) over (...),
//          max(sequence) over (...),
//          (case when "OLD_NEW$$" = 'N' then 1 else -1 end) dml_factor
//  from mlog_tbale
//  where scn > xxx and scn <= scn;
int ObMVPrinter::gen_delta_table_view(const TableItem &source_table, ObSelectStmt *&view_stmt)
{
  int ret = OB_SUCCESS;
  view_stmt = NULL;
  const ObTableSchema *mlog_schema = NULL;
  TableItem *table_item = NULL;
  ObSelectStmt *select_stmt = NULL;
  if (OB_FAIL(create_simple_stmt(view_stmt))) {
    LOG_WARN("failed to create simple stmt", K(ret));
  } else if (OB_FAIL(mv_checker_.get_mlog_table_schema(&source_table, mlog_schema))) {
    LOG_WARN("failed to get mlog schema", K(ret), K(source_table));
  } else if (OB_ISNULL(view_stmt) || OB_ISNULL(mlog_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret), K(view_stmt), K(mlog_schema), K(source_table));
  } else if (OB_FALSE_IT(view_stmt->set_query_ctx(stmt_factory_.get_query_ctx()))) {
  } else if (OB_FAIL(create_simple_table_item(view_stmt, mlog_schema->get_table_name_str(), table_item))) {
    LOG_WARN("failed to create simple table item", K(ret));
  } else if (OB_FAIL(gen_delta_table_view_conds(*table_item, view_stmt->get_condition_exprs()))) {
    LOG_WARN("failed to generate delta table view conds", K(ret));
  } else if (OB_FAIL(gen_delta_table_view_select_list(*table_item, source_table, *view_stmt))) {
    LOG_WARN("failed to generate delta table view select lists", K(ret));
  } else {
    table_item->database_name_ = source_table.database_name_;
    // zhanyuetodo: mlog need not flashback query
    // table_item->flashback_query_expr_ = exprs_.refresh_scn_;
    // table_item->flashback_query_type_ = TableItem::USING_SCN;
  }
  return ret;
}

int ObMVPrinter::gen_delta_table_view_conds(const TableItem &table,
                                            ObIArray<ObRawExpr*> &conds)
{
  int ret = OB_SUCCESS;
  ObRawExpr *scn_column = NULL;
  if (OB_FAIL(conds.prepare_allocate(2))) {
    LOG_WARN("failed to prepare allocate where conds", K(ret));
  } else if (OB_FAIL(create_simple_column_expr(table.get_table_name(), ObString("ORA_ROWSCN"), 0, scn_column))) {
    LOG_WARN("failed to create simple column expr", K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_GT, scn_column, exprs_.last_refresh_scn_, conds.at(0)))) {
    LOG_WARN("failed to build greater op expr", K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_LE, scn_column, exprs_.refresh_scn_, conds.at(1)))) {
    LOG_WARN("failed to build less or equal op expr", K(ret));
  }
  return ret;
}

// generate select list for mlog table:
//  1. normal column matched datatable
//  2. old_new$$, sequence$$ column
//  3. dml_factor: (CASE WHEN OLD_NEW$$ = 'N' THEN 1 ELSE -1 END)
//  4. min/max window function: min(sequence$$) over (partition by unique_keys), max(sequence$$) over (partition by unique_keys)
int ObMVPrinter::gen_delta_table_view_select_list(const TableItem &table,
                                                  const TableItem &source_table,
                                                  ObSelectStmt &stmt)
{
  int ret = OB_SUCCESS;
  ObIArray<SelectItem> &select_items = stmt.get_select_items();
  select_items.reuse();
  const ObIArray<ColumnItem> &column_items = mv_checker_.get_stmt().get_column_items();
  ObRawExpr *equal_expr = NULL;
  if (OB_ISNULL(stmt_factory_.get_query_ctx())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret), K(stmt_factory_.get_query_ctx()));
  } else if (OB_FAIL(select_items.prepare_allocate(column_items.count() + 5))) {
    LOG_WARN("failed to prepare allocate select items", K(ret));
  } else if (OB_FAIL(create_simple_column_expr(table.get_table_name(), OLD_NEW_COL_NAME, table.table_id_, select_items.at(0).expr_))) {
    LOG_WARN("failed to create simple column expr", K(ret));
  } else if (OB_FAIL(create_simple_column_expr(table.get_table_name(), SEQUENCE_COL_NAME, table.table_id_, select_items.at(1).expr_))) {
    LOG_WARN("failed to create simple column expr", K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(expr_factory_, T_OP_EQ, select_items.at(0).expr_, exprs_.str_n_, equal_expr))) {
    LOG_WARN("failed to build mul expr", K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_case_when_expr(expr_factory_, equal_expr, exprs_.int_one_, exprs_.int_neg_one_, select_items.at(2).expr_))) {
    LOG_WARN("failed to build case when expr", K(ret));
  } else if (OB_FAIL(gen_max_min_seq_window_func_exprs(stmt_factory_.get_query_ctx()->sql_schema_guard_,
                                                       table, source_table, select_items.at(1).expr_,
                                                       select_items.at(3).expr_, select_items.at(4).expr_))) {
    LOG_WARN("failed to gen max min seq window func exprs", K(ret));
  } else {
    select_items.at(0).is_real_alias_ = true;
    select_items.at(0).alias_name_ = OLD_NEW_COL_NAME;
    select_items.at(1).is_real_alias_ = true;
    select_items.at(1).alias_name_ = SEQUENCE_COL_NAME;
    select_items.at(2).is_real_alias_ = true;
    select_items.at(2).alias_name_ = DML_FACTOR_COL_NAME;
    select_items.at(3).is_real_alias_ = true;
    select_items.at(3).alias_name_ = WIN_MAX_SEQ_COL_NAME;
    select_items.at(4).is_real_alias_ = true;
    select_items.at(4).alias_name_ = WIN_MIN_SEQ_COL_NAME;
    for (int64_t i = 0; OB_SUCC(ret) && i < column_items.count(); ++i) {
      SelectItem &sel_item = select_items.at(i + 5);
      const ColumnItem &col_item = column_items.at(i);
      if (OB_FAIL(create_simple_column_expr(table.get_table_name(), col_item.column_name_, table.table_id_, sel_item.expr_))) {
        LOG_WARN("failed to create simple column expr", K(ret));
      } else {
        sel_item.is_real_alias_ = true;
        sel_item.alias_name_ = col_item.column_name_;
      }
    }
  }
  return ret;
}

int ObMVPrinter::gen_max_min_seq_window_func_exprs(const ObSqlSchemaGuard &sql_schema_guard,
                                                   const TableItem &table,
                                                   const TableItem &source_table,
                                                   ObRawExpr *sequence_expr,
                                                   ObRawExpr *&win_max_expr,
                                                   ObRawExpr *&win_min_expr)
{
  int ret = OB_SUCCESS;
  win_max_expr = NULL;
  win_min_expr = NULL;
  ObSEArray<uint64_t, 4> unique_col_ids;
  ObSEArray<ObRawExpr*, 4> unique_keys;
  const ObTableSchema *source_data_schema = NULL;
  const ObColumnSchemaV2 *rowkey_column = NULL;
  ObWinFunRawExpr *win_max = NULL;
  ObWinFunRawExpr *win_min = NULL;
  ObAggFunRawExpr *aggr_max = NULL;
  ObAggFunRawExpr *aggr_min = NULL;
  ObRawExpr *col_expr = NULL;
  if (OB_FAIL(sql_schema_guard.get_table_schema(source_table.ref_id_, source_data_schema))) {
    LOG_WARN("failed to get table schema", K(ret));
  } else if (OB_ISNULL(source_data_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret), K(source_data_schema));
  } else if (!source_data_schema->is_heap_table()) {
    if (OB_FAIL(source_data_schema->get_rowkey_column_ids(unique_col_ids))) {
      LOG_WARN("failed to get rowkey column ids", KR(ret));
    }
  } else if (OB_FAIL(create_simple_column_expr(table.get_table_name(), HEAP_TABLE_ROWKEY_COL_NAME, table.table_id_, col_expr))) {
    LOG_WARN("failed to create simple column expr", K(ret));
  } else if (OB_FAIL(unique_keys.push_back(col_expr))) {
    LOG_WARN("failed to pushback", K(ret));
  } else if (source_data_schema->get_partition_key_info().is_valid() &&
             OB_FAIL(source_data_schema->get_partition_key_info().get_column_ids(unique_col_ids))) {
    LOG_WARN("failed to add part column ids", K(ret));
  } else if (source_data_schema->get_subpartition_key_info().is_valid() &&
             OB_FAIL(source_data_schema->get_subpartition_key_info().get_column_ids(unique_col_ids))) {
    LOG_WARN("failed to add subpart column ids", K(ret));
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < unique_col_ids.count(); ++i) {
    if (OB_ISNULL(rowkey_column = source_data_schema->get_column_schema(unique_col_ids.at(i)))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null", K(ret), K(rowkey_column));
    } else if (OB_FAIL(create_simple_column_expr(table.get_table_name(), rowkey_column->get_column_name(), table.table_id_, col_expr))) {
      LOG_WARN("failed to create simple column expr", K(ret));
    } else if (OB_FAIL(unique_keys.push_back(col_expr))) {
      LOG_WARN("failed to pushback", K(ret));
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(expr_factory_.create_raw_expr(T_WINDOW_FUNCTION, win_max))
              || OB_FAIL(expr_factory_.create_raw_expr(T_WINDOW_FUNCTION, win_min))
              || OB_FAIL(expr_factory_.create_raw_expr(T_FUN_MAX, aggr_max))
              || OB_FAIL(expr_factory_.create_raw_expr(T_FUN_MIN, aggr_min))) {
    LOG_WARN("create window function expr failed", K(ret));
  } else if (OB_ISNULL(sequence_expr), OB_ISNULL(win_max) || OB_ISNULL(win_min)
             || OB_ISNULL(aggr_max) || OB_ISNULL(aggr_min)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret), K(sequence_expr), K(win_max), K(win_min), K(aggr_max), K(aggr_min));
  } else if (OB_FAIL(aggr_max->add_real_param_expr(sequence_expr))
              || OB_FAIL(aggr_min->add_real_param_expr(sequence_expr))) {
    LOG_WARN("failed to add param expr to agg expr", K(ret));
  } else if (OB_FAIL(win_max->set_partition_exprs(unique_keys))
              || OB_FAIL(win_min->set_partition_exprs(unique_keys))) {
    LOG_WARN("fail to set partition exprs", K(ret));
  } else {
    win_max->set_func_type(T_FUN_MAX);
    win_min->set_func_type(T_FUN_MIN);
    win_max->set_agg_expr(aggr_max);
    win_min->set_agg_expr(aggr_min);
    win_max_expr = win_max;
    win_min_expr = win_min;
  }
  return ret;
}

int ObMVPrinter::create_simple_column_expr(const ObString &table_name,
                                           const ObString &column_name,
                                           const uint64_t table_id,
                                           ObRawExpr *&expr)
{
  int ret = OB_SUCCESS;
  expr = NULL;
  ObColumnRefRawExpr *column_ref = NULL;
  if (OB_FAIL(expr_factory_.create_raw_expr(T_REF_COLUMN, column_ref))) {
    LOG_WARN("failed to create a new column ref expr", K(ret));
  } else if (OB_ISNULL(column_ref)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("new_column_ref should not be null", K(ret));
  } else {
    column_ref->set_table_name(table_name);
    column_ref->set_column_name(column_name);
    column_ref->set_ref_id(table_id, OB_INVALID_ID);
    expr = column_ref;
  }
  return ret;
}

int ObMVPrinter::create_simple_table_item(ObDMLStmt *stmt,
                                          const ObString &table_name,
                                          TableItem *&table_item,
                                          ObSelectStmt *view_stmt /* default null */,
                                          const bool add_to_from /* default true */)
{
  int ret = OB_SUCCESS;
  table_item = NULL;
  if (OB_ISNULL(stmt) || OB_ISNULL(stmt->get_query_ctx())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret), K(stmt));
  } else if (OB_ISNULL(table_item = stmt->create_table_item(alloc_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("create table item failed");
  } else if (OB_FAIL(stmt->get_table_items().push_back(table_item))) {
    LOG_WARN("add table item failed", K(ret));
  } else {
    table_item->table_name_ = table_name;
    table_item->table_id_ = stmt->get_query_ctx()->available_tb_id_--;
    table_item->type_ = NULL == view_stmt ? TableItem::BASE_TABLE : TableItem::GENERATED_TABLE;
    table_item->ref_query_ = view_stmt;
    if (OB_SUCC(ret) && add_to_from && OB_FAIL(stmt->add_from_item(table_item->table_id_))) {
      LOG_WARN("failed to add from item", K(ret));
    }
  }
  return ret;
}

int ObMVPrinter::init(const share::SCN &last_refresh_scn,
                      const share::SCN &refresh_scn)
{
  int ret = OB_SUCCESS;
  inited_ = false;
  ObCollationType cs_type = ObCharset::get_default_collation(ObCharset::get_default_charset());
  ObQueryCtx *query_ctx = NULL;
  const ObDatabaseSchema *db_schema = NULL;
  if (OB_ISNULL(query_ctx = stmt_factory_.get_query_ctx())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret), K(query_ctx));
  } else if (OB_FAIL(query_ctx->sql_schema_guard_.get_database_schema(mv_schema_.get_database_id(), db_schema))) {
    LOG_WARN("fail to get data base schema", K(ret), K(mv_schema_.get_database_id()));
  } else if (OB_ISNULL(db_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret), K(db_schema));
  } else if (OB_FAIL(ObRawExprUtils::build_const_int_expr(expr_factory_, ObIntType, 0, exprs_.int_zero_))
             || OB_FAIL(ObRawExprUtils::build_const_int_expr(expr_factory_, ObIntType, 1, exprs_.int_one_))
             || OB_FAIL(ObRawExprUtils::build_const_int_expr(expr_factory_, ObIntType, -1, exprs_.int_neg_one_))) {
    LOG_WARN("failed to build const int expr", K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_const_string_expr(expr_factory_, ObVarcharType, ObString("N"), cs_type, exprs_.str_n_))
             || OB_FAIL(ObRawExprUtils::build_const_string_expr(expr_factory_, ObVarcharType, ObString("O"), cs_type, exprs_.str_o_))) {
    LOG_WARN("fail to build const string expr", K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_const_uint_expr(expr_factory_, ObUInt64Type, last_refresh_scn.get_val_for_sql(), exprs_.last_refresh_scn_))
             || OB_FAIL(ObRawExprUtils::build_const_uint_expr(expr_factory_, ObUInt64Type, refresh_scn.get_val_for_sql(), exprs_.refresh_scn_))) {
    LOG_WARN("failed to build const uint expr", K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_null_expr(expr_factory_, exprs_.null_expr_))) {
    LOG_WARN("failed to create const null expr", K(ret));
  } else {
    mv_db_name_ = db_schema->get_database_name_str();
    inited_ = true;
  }
  return ret;
}

}//end of namespace sql
}//end of namespace oceanbase
