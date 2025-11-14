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
@file fts/fts0exec.cc

Created 2025/11/05
*******************************************************/

#include "fts0exec.h"
#include "row0query.h"
#include "fts0fts.h"
#include "fts0types.h"
#include "fts0vlc.h"
#include "fts0priv.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "dict0dict.h"
#include "row0ins.h"
#include "row0upd.h"
#include "row0sel.h"
#include "pars0pars.h"
#include "eval0eval.h"
#include "que0que.h"
#include "trx0trx.h"
#include "lock0lock.h"
#include "rem0cmp.h"
#include "ha_prototypes.h"

/** Defined in fts0fts.cc */
extern const char* fts_common_tables[];

/** Find common table index by name */
uint8_t find_common_table(const char* tbl_name)
{
  for (uint8_t i= 0; fts_common_tables[i]; i++)
    if (!strcmp(tbl_name, fts_common_tables[i])) return i;
  return UINT8_MAX;
}

FTSQueryExecutor::FTSQueryExecutor(
  trx_t *trx, const dict_index_t *fts_index, const dict_table_t *fts_table,
  bool dict_locked) : m_executor(new QueryExecutor(trx)),
                      m_dict_locked(dict_locked), m_fts_index(fts_index),
                      m_fts_table(fts_table)
{
  for (uint8_t i = 0; i < FTS_NUM_AUX_INDEX; i++)
    m_aux_tables[i] = nullptr;

  for (uint8_t i = 0; i < FTS_NUM_AUX_INDEX - 1; i++)
    m_common_tables[i] = nullptr;
}

FTSQueryExecutor::~FTSQueryExecutor()
{
  for (uint8_t i = 0; i < FTS_NUM_AUX_INDEX; i++)
    if (m_aux_tables[i]) m_aux_tables[i]->release();

  for (uint8_t i = 0; i < FTS_NUM_AUX_INDEX - 1; i++)
    if (m_common_tables[i]) m_common_tables[i]->release();
  delete m_executor;
}

dberr_t FTSQueryExecutor::open_aux_table(uint8_t aux_index) noexcept
{
  if (m_aux_tables[aux_index]) return DB_SUCCESS;
  fts_table_t fts_table;
  FTS_INIT_INDEX_TABLE(&fts_table, nullptr, FTS_INDEX_TABLE, m_fts_index);
  fts_table.suffix= fts_get_suffix(aux_index);

  char table_name[MAX_FULL_NAME_LEN];
  fts_get_table_name(&fts_table, table_name, m_dict_locked);

  m_aux_tables[aux_index]= dict_table_open_on_name(
    table_name, m_dict_locked, DICT_ERR_IGNORE_TABLESPACE);
  return m_aux_tables[aux_index] ? DB_SUCCESS : DB_TABLE_NOT_FOUND;
}

dberr_t FTSQueryExecutor::open_common_table(const char *tbl_name) noexcept
{
  uint8_t index= find_common_table(tbl_name);
  if (index == UINT8_MAX) return DB_ERROR;
  if (m_common_tables[index]) return DB_SUCCESS;
  fts_table_t fts_table;
  FTS_INIT_FTS_TABLE(&fts_table, nullptr, FTS_COMMON_TABLE, m_fts_table);
  fts_table.suffix= tbl_name;
  char table_name[MAX_FULL_NAME_LEN];
  fts_get_table_name(&fts_table, table_name, m_dict_locked);

  m_common_tables[index]= dict_table_open_on_name(
    table_name, m_dict_locked, DICT_ERR_IGNORE_TABLESPACE);
  return m_common_tables[index] ? DB_SUCCESS : DB_TABLE_NOT_FOUND;
}

dberr_t FTSQueryExecutor::lock_aux_tables(uint8_t aux_index,
                                          lock_mode mode) noexcept
{
  dict_table_t *table= m_aux_tables[aux_index];
  if (table == nullptr) return DB_TABLE_NOT_FOUND;
  dberr_t err= m_executor->lock_table(table, mode);
  if (err == DB_LOCK_WAIT) err= m_executor->handle_wait(err, true);
  return err;
}

dberr_t FTSQueryExecutor::lock_common_tables(uint8_t index,
                                             lock_mode mode) noexcept
{
  dict_table_t *table= m_common_tables[index];
  if (table == nullptr) return DB_TABLE_NOT_FOUND;
  dberr_t err = m_executor->lock_table(table, mode);
  if (err == DB_LOCK_WAIT) err= m_executor->handle_wait(err, true);
  return err;
}

dberr_t FTSQueryExecutor::insert_aux_record(
  uint8_t aux_index, const fts_aux_data_t* aux_data) noexcept
{
  if (aux_index >= FTS_NUM_AUX_INDEX) return DB_ERROR;

  dberr_t err= open_aux_table(aux_index);
  if (err != DB_SUCCESS) return err;
  err= lock_aux_tables(aux_index, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_aux_tables[aux_index];
  dict_index_t* index= dict_table_get_first_index(table);

  if (index->n_fields != 7 || index->n_uniq != 2)
    return DB_ERROR;

  byte sys_buf[DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN]= {0};
  dfield_t fields[7];
  doc_id_t first_doc_id, last_doc_id;

  dtuple_t tuple{0, 7, 2, 0, fields, nullptr
#ifdef UNIV_DEBUG
                 , DATA_TUPLE_MAGIC_N
#endif
                 };
  dict_index_copy_types(&tuple, index, 7);
  /* Field 0: word (VARCHAR) */
  dfield_t *field= dtuple_get_nth_field(&tuple, 0);
  dfield_set_data(field, aux_data->word, aux_data->word_len);

  /* Field 1: first_doc_id (INT) */
  field= dtuple_get_nth_field(&tuple, 1);
  fts_write_doc_id(&first_doc_id, aux_data->first_doc_id);
  dfield_set_data(field, &first_doc_id, sizeof(doc_id_t));

  /* Field 2: trx_id (DB_TRX_ID) */
  field= dtuple_get_nth_field(&tuple, 2);
  dfield_set_data(field, sys_buf, DATA_TRX_ID_LEN);

  /* Field 3: roll_ptr (DB_ROLL_PTR) */
  field= dtuple_get_nth_field(&tuple, 3);
  dfield_set_data(field, sys_buf + DATA_TRX_ID_LEN, DATA_ROLL_PTR_LEN);

  /* Field 4: last_doc_id (UNSIGNED INT) */
  field= dtuple_get_nth_field(&tuple, 4);
  fts_write_doc_id(&last_doc_id, aux_data->last_doc_id);
  dfield_set_data(field, &last_doc_id, sizeof(doc_id_t));

  /* Field 5: doc_count (UINT32_T) */
  byte doc_count[4];
  mach_write_to_4(doc_count, aux_data->doc_count);
  field= dtuple_get_nth_field(&tuple, 5);
  dfield_set_data(field, doc_count, sizeof(doc_count));

  /* Field 6: ilist (VARBINARY) */
  field= dtuple_get_nth_field(&tuple, 6);
  dfield_set_data(field, aux_data->ilist, aux_data->ilist_len);

  return m_executor->insert_record(table, &tuple);
}

dberr_t FTSQueryExecutor::insert_common_record(
  const char *tbl_name, doc_id_t doc_id) noexcept
{
  dberr_t err= open_common_table(tbl_name);
  if (err != DB_SUCCESS) return err;
  uint8_t index_no= find_common_table(tbl_name);
  err= lock_common_tables(index_no, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[index_no];
  dict_index_t* index= dict_table_get_first_index(table);

  if (index->n_fields != 3 || index->n_uniq != 1)
    return DB_ERROR;

  byte sys_buf[DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN]= {0};
  dfield_t fields[3];

  dtuple_t tuple{0, 3, 1, 0, fields, nullptr
#ifdef UNIV_DEBUG
                 , DATA_TUPLE_MAGIC_N
#endif
                 };
  dict_index_copy_types(&tuple, index, 3);
  /* Field 0: doc_id (INT) */
  dfield_t *field= dtuple_get_nth_field(&tuple, 0);
  doc_id_t write_doc_id;
  fts_write_doc_id(&write_doc_id, doc_id);
  dfield_set_data(field, &write_doc_id, sizeof(doc_id_t));

  /* Field 1: trx_id (DB_TRX_ID) */
  field= dtuple_get_nth_field(&tuple, 1);
  dfield_set_data(field, sys_buf, DATA_TRX_ID_LEN);

  /* Field 2: roll_ptr (DB_ROLL_PTR) */
  field= dtuple_get_nth_field(&tuple, 2);
  dfield_set_data(field, sys_buf + DATA_TRX_ID_LEN, DATA_ROLL_PTR_LEN);

  return m_executor->insert_record(table, &tuple);
}

dberr_t FTSQueryExecutor::insert_config_record(
  const char *key, const char *value) noexcept
{
  dberr_t err= open_common_table("CONFIG");
  if (err != DB_SUCCESS) return err;

  uint8_t index_no= find_common_table("CONFIG");
  err= lock_common_tables(index_no, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[index_no];
  dict_index_t* index= dict_table_get_first_index(table);

  if (index->n_fields != 4 || index->n_uniq != 1)
    return DB_ERROR;

  byte sys_buf[DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN]= {0};
  dfield_t fields[4];

  dtuple_t tuple{0, 4, 1, 0, fields, nullptr
#ifdef UNIV_DEBUG
                 , DATA_TUPLE_MAGIC_N
#endif
                 };
  dict_index_copy_types(&tuple, index, 4);
  /* Field 0: key (CHAR(50)) */
  dfield_t *field= dtuple_get_nth_field(&tuple, 0);
  dfield_set_data(field, key, strlen(key));

  /* Field 1: trx_id (DB_TRX_ID) */
  field= dtuple_get_nth_field(&tuple, 1);
  dfield_set_data(field, sys_buf, DATA_TRX_ID_LEN);

  /* Field 2: roll_ptr (DB_ROLL_PTR) */
  field= dtuple_get_nth_field(&tuple, 2);
  dfield_set_data(field, sys_buf + DATA_TRX_ID_LEN, DATA_ROLL_PTR_LEN);

  /* Field 3: value (CHAR(200)) */
  field= dtuple_get_nth_field(&tuple, 3);
  dfield_set_data(field, value, strlen(value));

  return m_executor->insert_record(table, &tuple);
}

dberr_t FTSQueryExecutor::update_config_record(
  const char *key, const char *value) noexcept
{
  dberr_t err= open_common_table("CONFIG");
  if (err != DB_SUCCESS) return err;

  uint8_t index_no= find_common_table("CONFIG");
  err= lock_common_tables(index_no, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[index_no];
  dict_index_t* index= dict_table_get_first_index(table);

  if (index->n_fields != 4 || index->n_uniq != 1)
    return DB_ERROR;

  byte sys_buf[DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN]= {0};
  dfield_t search_fields[1];
  dfield_t insert_fields[4];

  dtuple_t search_tuple{0, 1, 1, 0, search_fields, nullptr
#ifdef UNIV_DEBUG
                        , DATA_TUPLE_MAGIC_N
#endif
                        };
  dict_index_copy_types(&search_tuple, index, 1);
  dfield_t *field= dtuple_get_nth_field(&search_tuple, 0);
  dfield_set_data(field, key, strlen(key));

  dtuple_t insert_tuple{0, 4, 1, 0, insert_fields, nullptr
#ifdef UNIV_DEBUG
                        , DATA_TUPLE_MAGIC_N
#endif
                        };
  dict_index_copy_types(&insert_tuple, index, 4);

  /* Field 0: key (CHAR(50)) */
  field= dtuple_get_nth_field(&insert_tuple, 0);
  dfield_set_data(field, key, strlen(key));

  /* Field 1: trx_id (DB_TRX_ID) */
  field= dtuple_get_nth_field(&insert_tuple, 1);
  dfield_set_data(field, sys_buf, DATA_TRX_ID_LEN);

  /* Field 2: roll_ptr (DB_ROLL_PTR) */
  field= dtuple_get_nth_field(&insert_tuple, 2);
  dfield_set_data(field, sys_buf + DATA_TRX_ID_LEN, DATA_ROLL_PTR_LEN);

  /* Field 3: value (CHAR(200)) */
  field= dtuple_get_nth_field(&insert_tuple, 3);
  dfield_set_data(field, value, strlen(value));

  upd_field_t upd_field;
  upd_field.field_no = 3;
  upd_field.orig_len = 0;
  upd_field.exp = nullptr;
  dfield_set_data(&upd_field.new_val, value, strlen(value));
  dict_col_copy_type(dict_index_get_nth_col(index, 3),
                     dfield_get_type(&upd_field.new_val));

  upd_t update;
  update.heap = nullptr;
  update.info_bits = 0;
  update.old_vrow = nullptr;
  update.n_fields = 1;
  update.fields = &upd_field;

  return m_executor->replace_record(table, &search_tuple, &update,
                                    &insert_tuple);
}

dberr_t FTSQueryExecutor::delete_aux_record(
  uint8_t aux_index, const fts_aux_data_t* aux_data) noexcept
{
  if (aux_index >= FTS_NUM_AUX_INDEX) return DB_ERROR;

  dberr_t err= open_aux_table(aux_index);
  if (err != DB_SUCCESS) return err;
  err= lock_aux_tables(aux_index, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_aux_tables[aux_index];
  dict_index_t* index= dict_table_get_first_index(table);

  if (dict_table_get_next_index(index) != nullptr)
    return DB_ERROR;

  dfield_t fields[1];
  dtuple_t tuple{0, 1, 1, 0, fields, nullptr
#ifdef UNIV_DEBUG
                 , DATA_TUPLE_MAGIC_N
#endif
                 };
  dict_index_copy_types(&tuple, index, 1);
  /* Field 0: word (VARCHAR) */
  dfield_t *field= dtuple_get_nth_field(&tuple, 0);
  dfield_set_data(field, aux_data->word, aux_data->word_len);

  return m_executor->delete_record(table, &tuple);
}

dberr_t FTSQueryExecutor::delete_common_record(
  const char *table_name, doc_id_t doc_id) noexcept
{
  dberr_t err= open_common_table(table_name);
  if (err != DB_SUCCESS) return err;

  uint8_t cached_index= find_common_table(table_name);
  err= lock_common_tables(cached_index, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[cached_index];
  dict_index_t* index= dict_table_get_first_index(table);

  dfield_t fields[1];
  dtuple_t tuple{0, 1, 1, 0, fields, nullptr
#ifdef UNIV_DEBUG
                 , DATA_TUPLE_MAGIC_N
#endif
                 };
  dict_index_copy_types(&tuple, index, 1);
  /* Field 0: doc_id */
  dfield_t *field= dtuple_get_nth_field(&tuple, 0);
  doc_id_t write_doc_id;
  fts_write_doc_id(&write_doc_id, doc_id);
  dfield_set_data(field, &write_doc_id, sizeof(doc_id_t));

  return m_executor->delete_record(table, &tuple);
}

dberr_t FTSQueryExecutor::delete_all_common_records(
  const char *table_name) noexcept
{
  dberr_t err= open_common_table(table_name);
  if (err != DB_SUCCESS) return err;

  uint8_t cached_index= find_common_table(table_name);
  err= lock_common_tables(cached_index, LOCK_X);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[cached_index];
  return m_executor->delete_all(table);
}

dberr_t FTSQueryExecutor::delete_config_record(
  const char *key) noexcept
{
  dberr_t err= open_common_table("CONFIG");
  if (err != DB_SUCCESS) return err;

  uint8_t index_no= find_common_table("CONFIG");
  err= lock_common_tables(index_no, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[index_no];
  dict_index_t* index= dict_table_get_first_index(table);

  dfield_t fields[1];

  dtuple_t tuple{0, 1, 1, 0, fields, nullptr
#ifdef UNIV_DEBUG
                 , DATA_TUPLE_MAGIC_N
#endif
                 };
  dict_index_copy_types(&tuple, index, 1);
  /* Field 0: key (CHAR(50)) */
  dfield_t *field= dtuple_get_nth_field(&tuple, 0);
  dfield_set_data(field, key, strlen(key));

  return m_executor->delete_record(table, &tuple);
}

dberr_t FTSQueryExecutor::read_all_config(RecordCallback& callback) noexcept
{
  dberr_t err= open_common_table("CONFIG");
  if (err != DB_SUCCESS) return err;

  uint8_t index_no= find_common_table("CONFIG");
  err= lock_common_tables(index_no, LOCK_IS);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[index_no];
  return m_executor->read(table, nullptr, PAGE_CUR_GE, callback);
}

dberr_t FTSQueryExecutor::read_config(const char *key,
                                      RecordCallback& callback) noexcept
{
  dberr_t err= open_common_table("CONFIG");
  if (err != DB_SUCCESS) return err;

  uint8_t index_no= find_common_table("CONFIG");
  err= lock_common_tables(index_no, LOCK_IS);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[index_no];
  dict_index_t* index= dict_table_get_first_index(table);

  dfield_t fields[1];
  dtuple_t tuple{0, 1, 1, 0, fields, nullptr
#ifdef UNIV_DEBUG
                 , DATA_TUPLE_MAGIC_N
#endif
                 };
  dict_index_copy_types(&tuple, index, 1);
  /* Field 0: key (CHAR(50)) */
  dfield_t *field= dtuple_get_nth_field(&tuple, 0);
  dfield_set_data(field, key, strlen(key));

  return m_executor->read(table, &tuple, PAGE_CUR_GE, callback);
}

dberr_t FTSQueryExecutor::read_config_with_lock(const char *key,
                                               RecordCallback& callback) noexcept
{
  dberr_t err= open_common_table("CONFIG");
  if (err != DB_SUCCESS) return err;

  uint8_t index_no= find_common_table("CONFIG");

  err= lock_common_tables(index_no, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[index_no];
  dict_index_t* index= dict_table_get_first_index(table);

  dfield_t fields[1];
  dtuple_t tuple{0, 1, 1, 0, fields, nullptr
#ifdef UNIV_DEBUG
                 , DATA_TUPLE_MAGIC_N
#endif
                 };
  dict_index_copy_types(&tuple, index, 1);
  /* Field 0: key (CHAR(50)) */
  dfield_t *field= dtuple_get_nth_field(&tuple, 0);
  dfield_set_data(field, key, strlen(key));

  return m_executor->select_for_update(table, &tuple, &callback);
}

dberr_t FTSQueryExecutor::read_aux(uint8_t aux_index,
                                   const char *word,
                                   page_cur_mode_t mode,
                                   RecordCallback& callback) noexcept
{
  if (aux_index >= FTS_NUM_AUX_INDEX) return DB_ERROR;
  dberr_t err= open_aux_table(aux_index);
  if (err != DB_SUCCESS) return err;

  err= lock_aux_tables(aux_index, LOCK_IS);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_aux_tables[aux_index];
  dict_index_t* index= dict_table_get_first_index(table);

  dfield_t fields[1];
  dtuple_t tuple{0, 1, 1, 0, fields, nullptr
#ifdef UNIV_DEBUG
                 , DATA_TUPLE_MAGIC_N
#endif
                 };
  dict_index_copy_types(&tuple, index, 1);
  /* Field 0: word (VARCHAR) */
  dfield_t *field= dtuple_get_nth_field(&tuple, 0);
  dfield_set_data(field, word, strlen(word));

  return m_executor->read(table, &tuple, mode, callback);
}

dberr_t FTSQueryExecutor::read_aux_all(uint8_t aux_index, RecordCallback& callback) noexcept
{
  if (aux_index >= FTS_NUM_AUX_INDEX) return DB_ERROR;
  dberr_t err= open_aux_table(aux_index);
  if (err != DB_SUCCESS) return err;

  err= lock_aux_tables(aux_index, LOCK_IS);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_aux_tables[aux_index];
  return m_executor->read(table, nullptr, PAGE_CUR_GE, callback);
}

dberr_t FTSQueryExecutor::read_all_common(const char *tbl_name,
                                          RecordCallback& callback) noexcept
{
  dberr_t err= open_common_table(tbl_name);
  if (err != DB_SUCCESS) return err;

  uint8_t index_no= find_common_table(tbl_name);
  err= lock_common_tables(index_no, LOCK_IS);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[index_no];
  return m_executor->read(table, nullptr, PAGE_CUR_GE, callback);
}
