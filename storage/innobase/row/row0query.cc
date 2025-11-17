/*****************************************************************************

Copyright (c) 2025, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file row/row0query.cc
General Query Executor

Created 2025/10/30
*******************************************************/

#include "row0query.h"
#include "pars0pars.h"
#include "dict0dict.h"
#include "row0ins.h"
#include "row0upd.h"
#include "row0row.h"
#include "row0vers.h"
#include "mem0mem.h"
#include "que0que.h"
#include "lock0lock.h"
#include "rem0rec.h"
#include "btr0pcur.h"
#include "btr0cur.h"

QueryExecutor::QueryExecutor(trx_t *trx)
  : m_trx(trx), m_mtr(trx), m_mtr_active(false)
{
  m_heap= mem_heap_create(256);
  m_thr= pars_complete_graph_for_exec(nullptr, m_trx, m_heap, nullptr);
  btr_pcur_init(&m_pcur);
}

QueryExecutor::~QueryExecutor()
{
  btr_pcur_close(&m_pcur);
  if (m_heap) mem_heap_free(m_heap);
}

dberr_t QueryExecutor::insert_record(dict_table_t *table,
                                     dtuple_t *tuple) noexcept
{
  dict_index_t* index= dict_table_get_first_index(table);
  return row_ins_clust_index_entry(index, tuple, m_thr, 0);
}

dberr_t QueryExecutor::lock_table(dict_table_t *table, lock_mode mode) noexcept
{
  trx_start_if_not_started(m_trx, true);
  return ::lock_table(table, nullptr, mode, m_thr);
}

dberr_t QueryExecutor::handle_wait(dberr_t err, bool table_lock) noexcept
{
  m_trx->error_state= err;
  if (table_lock) m_thr->lock_state= QUE_THR_LOCK_TABLE;
  else m_thr->lock_state= QUE_THR_LOCK_ROW;
  if (m_trx->lock.wait_thr)
  {
    dberr_t wait_err= lock_wait(m_thr);
    if (wait_err == DB_LOCK_WAIT_TIMEOUT) err= wait_err;
    if (wait_err == DB_SUCCESS)
    {
      m_thr->lock_state= QUE_THR_LOCK_NOLOCK;
      return DB_SUCCESS;
    }
  }
  return err;
}

dberr_t QueryExecutor::delete_record(dict_table_t *table,
                                     dtuple_t *tuple) noexcept
{
  dict_index_t *index= dict_table_get_first_index(table);
  btr_pcur_t pcur;
  mtr_t mtr(m_trx);
  ulint deleted_count= 0;

  mtr.start();
  mtr.set_named_space(table->space);

  pcur.btr_cur.page_cur.index= index;
  dberr_t err= btr_pcur_open(tuple, PAGE_CUR_GE, BTR_MODIFY_LEAF,
                             &pcur, &mtr);
  if (err != DB_SUCCESS)
  {
    mtr.commit();
    return err;
  }
  while (!btr_pcur_is_after_last_on_page(&pcur) &&
         !btr_pcur_is_after_last_in_tree(&pcur))
  {
    rec_t* rec= btr_pcur_get_rec(&pcur);
    if (!rec) break;

    if (rec_get_deleted_flag(rec, dict_table_is_comp(table)))
    {
      if (!btr_pcur_move_to_next(&pcur, &mtr)) break;
      continue;
    }

    rec_offs* offsets= rec_get_offsets(rec, index, nullptr,
                                       index->n_core_fields,
                                       ULINT_UNDEFINED, &m_heap);

    uint16_t matched_fields= 0;
    int cmp= cmp_dtuple_rec_with_match(tuple, rec, index,
                                       offsets, &matched_fields);
    if (cmp != 0) break;
    err= lock_clust_rec_read_check_and_lock(
           0, btr_pcur_get_block(&pcur), rec, index, offsets, LOCK_X,
           LOCK_REC_NOT_GAP, m_thr);
    if (err == DB_LOCK_WAIT)
    {
      mtr.commit();
      err= handle_wait(err, false);
      if (err != DB_SUCCESS) return err;
      mtr.start();
      continue;
    }
    else if (err != DB_SUCCESS && err != DB_SUCCESS_LOCKED_REC)
    {
      mtr.commit();
      return err;
    }

    err= btr_cur_del_mark_set_clust_rec(btr_pcur_get_block(&pcur),
                                        rec, index, offsets, m_thr,
                                        nullptr, &mtr);
    if (err != DB_SUCCESS) break;
    deleted_count++;
    if (!btr_pcur_move_to_next(&pcur, &mtr)) break;
  }
  mtr.commit();
  return (deleted_count > 0) ? DB_SUCCESS : DB_RECORD_NOT_FOUND;
}

dberr_t QueryExecutor::delete_all(dict_table_t *table) noexcept
{
  dict_index_t *index= dict_table_get_first_index(table);
  btr_pcur_t pcur;
  mtr_t mtr(m_trx);
  mtr.start();
  mtr.set_named_space(table->space);

  dberr_t err= pcur.open_leaf(true, index, BTR_MODIFY_LEAF, &mtr);
  if (err == DB_SUCCESS) btr_pcur_move_to_next(&pcur, &mtr);
  if (err != DB_SUCCESS)
  {
    mtr.commit();
    return err;
  }

  while (!btr_pcur_is_after_last_on_page(&pcur) &&
         !btr_pcur_is_after_last_in_tree(&pcur))
  {
    rec_t* rec= btr_pcur_get_rec(&pcur);
    if (!rec) break;
    if (rec_get_deleted_flag(rec, dict_table_is_comp(table)))
    {
      if (!btr_pcur_move_to_next(&pcur, &mtr)) break;
      continue;
    }

    if (rec_get_info_bits(
          rec, dict_table_is_comp(table)) & REC_INFO_MIN_REC_FLAG)
    {
      if (!btr_pcur_move_to_next(&pcur, &mtr)) break;
      continue;
    }
    rec_offs* offsets= rec_get_offsets(rec, index, nullptr,
                                       index->n_core_fields,
                                       ULINT_UNDEFINED, &m_heap);
    err= lock_clust_rec_read_check_and_lock(
      0, btr_pcur_get_block(&pcur), rec, index, offsets, LOCK_X,
      LOCK_REC_NOT_GAP, m_thr);

    if (err == DB_LOCK_WAIT)
    {
      mtr.commit();
      err= handle_wait(err, false);
      if (err != DB_SUCCESS) return err;
      mtr.start();
      continue;
    }
    else if (err != DB_SUCCESS && err != DB_SUCCESS_LOCKED_REC)
    {
      mtr.commit();
      return err;
    }

    err= btr_cur_del_mark_set_clust_rec(btr_pcur_get_block(&pcur),
                                        const_cast<rec_t*>(rec), index,
                                        offsets, m_thr, nullptr, &mtr);
    if (err || !btr_pcur_move_to_next(&pcur, &mtr)) break;
  }

  mtr.commit();
  return err;
}

dberr_t QueryExecutor::select_for_update(dict_table_t *table,
                                         dtuple_t *search_tuple,
                                         RecordCallback *callback) noexcept
{
  dict_index_t *index= dict_table_get_first_index(table);
  if (m_mtr_active)
  {
    m_mtr.commit();
    m_mtr_active= false;
  }

  m_mtr.start();
  m_mtr.set_named_space(table->space);
  m_mtr_active= true;

  if (m_trx && !m_trx->read_view.is_open())
  {
    trx_start_if_not_started(m_trx, false);
    m_trx->read_view.open(m_trx);
  }
  m_pcur.btr_cur.page_cur.index= index;
  dberr_t err= btr_pcur_open(search_tuple, PAGE_CUR_GE, BTR_MODIFY_LEAF,
                             &m_pcur, &m_mtr);
  if (err != DB_SUCCESS)
  {
    m_mtr.commit();
    m_mtr_active= false;
    return err;
  }

  if (btr_pcur_is_after_last_on_page(&m_pcur) ||
      btr_pcur_is_after_last_in_tree(&m_pcur))
  {
    m_mtr.commit();
    m_mtr_active= false;
    return DB_RECORD_NOT_FOUND;
  }
  rec_t* rec= btr_pcur_get_rec(&m_pcur);
  if (!rec)
  {
    m_mtr.commit();
    m_mtr_active= false;
    return DB_RECORD_NOT_FOUND;
  }
  
  rec_offs* offsets= rec_get_offsets(rec, index, nullptr,
                                     index->n_core_fields,
                                     ULINT_UNDEFINED, &m_heap);

  if (m_trx && m_trx->read_view.is_open())
  {
    trx_id_t rec_trx_id= row_get_rec_trx_id(rec, index, offsets);
    if (rec_trx_id && !m_trx->read_view.changes_visible(rec_trx_id))
    {
      m_mtr.commit();
      m_mtr_active= false;
      return DB_RECORD_NOT_FOUND;
    }
  }
  uint16_t matched_fields= 0;
  int cmp= cmp_dtuple_rec_with_match(search_tuple, rec, index,
                                     offsets, &matched_fields);
  if (cmp != 0)
  {
    m_mtr.commit();
    m_mtr_active= false;
    return DB_RECORD_NOT_FOUND;
  }

  err= lock_clust_rec_read_check_and_lock(
    0, btr_pcur_get_block(&m_pcur), rec, index, offsets, LOCK_X,
    LOCK_REC_NOT_GAP, m_thr);

  if (err == DB_LOCK_WAIT)
  {
    m_mtr.commit();
    m_mtr_active= false;
    err= handle_wait(err, false);
    if (err != DB_SUCCESS) return err;
    return DB_LOCK_WAIT;
  }
  else if (err != DB_SUCCESS && err != DB_SUCCESS_LOCKED_REC)
  {
    m_mtr.commit();
    m_mtr_active= false;
    return err;
  }

  if (callback)
  {
    RecordCompareAction action=
      callback->compare_record(search_tuple, rec, index, offsets);
    if (action == RecordCompareAction::PROCESS)
      callback->process_record(rec, index, offsets);
    m_mtr.commit();
    m_mtr_active= false;
    if (action == RecordCompareAction::SKIP)
      return DB_RECORD_NOT_FOUND;
  }
  return DB_SUCCESS;
}

dberr_t QueryExecutor::update_record(dict_table_t *table,
                                     const upd_t *update) noexcept
{
  if (!m_mtr_active) return DB_ERROR;
  dict_index_t *index= dict_table_get_first_index(table);
  rec_t *rec= btr_pcur_get_rec(&m_pcur);
  if (!rec) return DB_RECORD_NOT_FOUND;
  mtr_x_lock_index(index, &m_mtr);
  rec_offs *offsets= rec_get_offsets(rec, index, nullptr,
                                     index->n_core_fields,
                                     ULINT_UNDEFINED, &m_heap);

  dberr_t err= DB_SUCCESS;
  ulint cmpl_info= UPD_NODE_NO_ORD_CHANGE | UPD_NODE_NO_SIZE_CHANGE;
  for (ulint i = 0; i < update->n_fields; i++)
  {
    const upd_field_t *upd_field= &update->fields[i];
    ulint field_no= upd_field->field_no;
    if (field_no < rec_offs_n_fields(offsets))
    {
      ulint old_len= rec_offs_nth_size(offsets, field_no);
      ulint new_len= upd_field->new_val.len;
      if (new_len != UNIV_SQL_NULL && new_len != old_len)
      {
        cmpl_info &= ~UPD_NODE_NO_SIZE_CHANGE;
        err= DB_OVERFLOW;
        break;
      }
    }
  }

  if (cmpl_info & UPD_NODE_NO_SIZE_CHANGE)
    err= btr_cur_update_in_place(BTR_NO_LOCKING_FLAG,
                                 btr_pcur_get_btr_cur(&m_pcur),
                                 offsets, const_cast<upd_t*>(update), 0,
                                 m_thr, m_trx->id, &m_mtr);
  if (err == DB_OVERFLOW)
  {
    big_rec_t *big_rec= nullptr;
    err= btr_cur_optimistic_update(BTR_NO_LOCKING_FLAG,
                                   btr_pcur_get_btr_cur(&m_pcur),
                                   &offsets, &m_heap,
                                   const_cast<upd_t*>(update),
                                   cmpl_info, m_thr, m_trx->id, &m_mtr);

    if (err == DB_OVERFLOW || err == DB_UNDERFLOW)
    {
      mem_heap_t* offsets_heap= nullptr;
      err= btr_cur_pessimistic_update(BTR_NO_LOCKING_FLAG,
                                      btr_pcur_get_btr_cur(&m_pcur),
                                      &offsets, &offsets_heap, m_heap,
                                      &big_rec, const_cast<upd_t*>(update),
                                      cmpl_info, m_thr, m_trx->id, &m_mtr);

      if (err == DB_SUCCESS && big_rec)
      {
        err= btr_store_big_rec_extern_fields(&m_pcur, offsets, big_rec, &m_mtr,
                                             BTR_STORE_UPDATE);
        dtuple_big_rec_free(big_rec);
      }
      if (offsets_heap) mem_heap_free(offsets_heap);
    }
  }
  return err;
}

dberr_t QueryExecutor::commit_update() noexcept
{
  if (m_mtr_active)
  {
    m_mtr.commit();
    m_mtr_active= false;
    return DB_SUCCESS;
  }
  return DB_ERROR;
}

dberr_t QueryExecutor::rollback_update() noexcept
{
  if (m_mtr_active)
  {
    m_mtr.commit();
    m_mtr_active= false;
    return DB_SUCCESS;
  }
  return DB_ERROR;
}

dberr_t QueryExecutor::replace_record(
   dict_table_t *table, dtuple_t *search_tuple,
   const upd_t *update, dtuple_t *insert_tuple) noexcept
{
retry_again:
  dberr_t err= select_for_update(table, search_tuple);
  if (err == DB_SUCCESS)
  {
    err= update_record(table, update);
    if (err == DB_SUCCESS) err= commit_update();
    else rollback_update();
    return err;
  }
  else if (err == DB_RECORD_NOT_FOUND)
  {
    if (m_mtr_active)
    {
      m_mtr.commit();
      m_mtr_active= false;
    }
    err= insert_record(table, insert_tuple);
    return err;
  }
  else if (err == DB_LOCK_WAIT)
    goto retry_again;
  return err;
}

dberr_t QueryExecutor::read(dict_table_t *table, dtuple_t *tuple,
                            page_cur_mode_t mode,
                            RecordCallback& callback) noexcept
{
  ut_ad(table);
  dict_index_t *index= dict_table_get_first_index(table);
  if (!index) return DB_ERROR;

  m_mtr.start();
  if (m_trx && !m_trx->read_view.is_open())
  {
    trx_start_if_not_started(m_trx, false);
    m_trx->read_view.open(m_trx);
  }
  m_pcur.btr_cur.page_cur.index= index;
  dberr_t err= DB_SUCCESS;
  if (tuple) err= btr_pcur_open(tuple, mode, BTR_SEARCH_LEAF, &m_pcur, &m_mtr);
  else
  {
    err= m_pcur.open_leaf(true, index, BTR_SEARCH_LEAF, &m_mtr);
    if (err == DB_SUCCESS) btr_pcur_move_to_next(&m_pcur, &m_mtr);
  }
  if (err != DB_SUCCESS)
  {
    m_mtr.commit();
    return err;
  }
  ulint match_count= 0;
  while (btr_pcur_is_on_user_rec(&m_pcur))
  {
    const rec_t *rec= btr_pcur_get_rec(&m_pcur);
    rec_offs* offsets= rec_get_offsets(rec, index, nullptr,
                                       index->n_core_fields,
                                       ULINT_UNDEFINED, &m_heap);
    RecordCompareAction action= callback.compare_record(
      tuple, rec, index, offsets);
    if (action == RecordCompareAction::PROCESS)
    {
      bool continue_processing= process_record_with_mvcc(table, index, rec,
                                                         offsets, callback,
                                                         &m_mtr, match_count);
      if (!continue_processing) break;
    }
    else if (action == RecordCompareAction::STOP)
      break;
    if (!btr_pcur_move_to_next(&m_pcur, &m_mtr)) break;
  }
  m_mtr.commit();
  return (match_count > 0 || !tuple) ? DB_SUCCESS : DB_RECORD_NOT_FOUND;
}

dberr_t QueryExecutor::read_by_index(dict_table_t *table,
                                     dict_index_t *sec_index,
                                     dtuple_t *search_tuple,
                                     page_cur_mode_t mode,
                                     RecordCallback& callback) noexcept
{
  ut_ad(table);
  ut_ad(sec_index);
  ut_ad(sec_index->table == table);
  ut_ad(!dict_index_is_clust(sec_index));

  dict_index_t *clust_index= dict_table_get_first_index(table);
  if (!clust_index) return DB_ERROR;

  m_mtr.start();
  if (m_trx && !m_trx->read_view.is_open())
  {
    trx_start_if_not_started(m_trx, false);
    m_trx->read_view.open(m_trx);
  }
  m_pcur.btr_cur.page_cur.index= sec_index;

  dberr_t err= DB_SUCCESS;
  if (search_tuple)
    err= btr_pcur_open(search_tuple, mode, BTR_SEARCH_LEAF, &m_pcur, &m_mtr);
  else
  {
    err= m_pcur.open_leaf(true, sec_index, BTR_SEARCH_LEAF, &m_mtr);
    if (err == DB_SUCCESS) btr_pcur_move_to_next(&m_pcur, &m_mtr);
  }

  if (err != DB_SUCCESS)
  {
    m_mtr.commit();
    return err;
  }

  ulint match_count= 0;
  while (btr_pcur_is_on_user_rec(&m_pcur))
  {
    const rec_t *sec_rec= btr_pcur_get_rec(&m_pcur);
    rec_offs* sec_offsets= rec_get_offsets(sec_rec, sec_index, nullptr,
                                           sec_index->n_core_fields,
                                           ULINT_UNDEFINED, &m_heap);
    /* Check if secondary record matches our search criteria */
    RecordCompareAction action= callback.compare_record(search_tuple, sec_rec,
                                                        sec_index, sec_offsets);
    if (action == RecordCompareAction::PROCESS)
    {
      /* Lookup clustered record and process it */
      if (!lookup_clustered_record(table, sec_index, clust_index,
                                   sec_rec, callback, match_count))
        break;
    }
    else if (action == RecordCompareAction::STOP)
      break;
    if (!btr_pcur_move_to_next(&m_pcur, &m_mtr)) break;
  }
  m_mtr.commit();
  return (match_count > 0 || !search_tuple) ? DB_SUCCESS : DB_RECORD_NOT_FOUND;
}

bool QueryExecutor::lookup_clustered_record(dict_table_t *table,
                                            dict_index_t *sec_index,
                                            dict_index_t *clust_index,
                                            const rec_t *sec_rec,
                                            RecordCallback& callback,
                                            ulint& match_count) noexcept
{
  /* Extract primary key from secondary index record */
  dtuple_t *clust_tuple= row_build_row_ref(ROW_COPY_DATA, sec_index,
                                           sec_rec, m_heap);
  if (!clust_tuple) return false;
  /* Now lookup the complete row using clustered index */
  btr_pcur_t clust_pcur;
  clust_pcur.btr_cur.page_cur.index= clust_index;

  mtr_t clust_mtr{m_trx};
  clust_mtr.start();
  bool continue_processing= true;
  bool record_processed= false;
  dberr_t clust_err= btr_pcur_open(clust_tuple, PAGE_CUR_LE,
                                   BTR_SEARCH_LEAF, &clust_pcur,
                                   &clust_mtr);
  if (clust_err == DB_SUCCESS && btr_pcur_is_on_user_rec(&clust_pcur))
  {
    const rec_t *clust_rec= btr_pcur_get_rec(&clust_pcur);
    rec_offs* clust_offsets= rec_get_offsets(clust_rec, clust_index,
                                             nullptr,
                                             clust_index->n_core_fields,
                                             ULINT_UNDEFINED, &m_heap);
    /* Verify this is the exact record we want */
    if (!cmp_dtuple_rec(clust_tuple, clust_rec, clust_index, clust_offsets))
    {
      ulint prev_match_count= match_count;
      continue_processing= process_record_with_mvcc(
        table, clust_index, clust_rec, clust_offsets, callback, &clust_mtr,
        match_count);
      record_processed= (match_count > prev_match_count);
    }
  }
  clust_mtr.commit();
  return record_processed ? continue_processing : true;
}

bool QueryExecutor::process_record_with_mvcc(dict_table_t *table,
                                             dict_index_t *index,
                                             const rec_t *rec,
                                             rec_offs *offsets,
                                             RecordCallback& callback,
                                             mtr_t *mtr,
                                             ulint& match_count) noexcept
{
  bool is_deleted= rec_get_deleted_flag(rec, dict_table_is_comp(table));
  rec_t* version_rec= const_cast<rec_t*>(rec);
  rec_offs* version_offsets= offsets;
  mem_heap_t* version_heap= nullptr;
  bool should_process_record= false;
  bool result= true;
  if (m_trx && m_trx->read_view.is_open())
  {
    trx_id_t rec_trx_id= row_get_rec_trx_id(rec, index, offsets);
    if (rec_trx_id && !m_trx->read_view.changes_visible(rec_trx_id))
    {
      version_heap= mem_heap_create(1024);
      dberr_t vers_err= row_vers_build_for_consistent_read(
        rec, mtr, index, &offsets, &m_trx->read_view, &version_heap,
	version_heap, &version_rec, nullptr);
      if (vers_err == DB_SUCCESS && version_rec)
      {
        version_offsets= rec_get_offsets(version_rec, index, nullptr,
                                         index->n_core_fields,
                                         ULINT_UNDEFINED, &version_heap);
        is_deleted= rec_get_deleted_flag(version_rec, dict_table_is_comp(table));
        should_process_record= !is_deleted;
      }
    }
    else should_process_record= !is_deleted;
  }
  else should_process_record= !is_deleted && version_rec;

  if (should_process_record)
  {
    result= callback.process_record(version_rec, index, version_offsets);
    match_count++;
  }

  if (version_heap) mem_heap_free(version_heap);
  return result;
}
