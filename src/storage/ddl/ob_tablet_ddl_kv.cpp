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

#define USING_LOG_PREFIX STORAGE

#include "storage/ddl/ob_ddl_struct.h"
#include "storage/ddl/ob_tablet_ddl_kv.h"
#include "share/scn.h"
#include "storage/blocksstable/ob_block_manager.h"
#include "storage/blocksstable/ob_block_sstable_struct.h"
#include "storage/blocksstable/ob_macro_block_struct.h"
#include "share/ob_force_print_log.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "storage/ddl/ob_tablet_ddl_kv_mgr.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "storage/meta_mem/ob_tablet_handle.h"
#include "storage/ddl/ob_ddl_merge_task.h"
#include "storage/tx_storage/ob_ls_handle.h"
#include "storage/compaction/ob_schedule_dag_func.h"
#include "storage/blocksstable/ob_datum_rowkey.h"
#include "storage/tablet/ob_tablet_create_delete_helper.h"
#include "storage/ddl/ob_direct_insert_sstable_ctx_new.h"
#include "storage/column_store/ob_column_oriented_sstable.h"

using namespace oceanbase::storage;
using namespace oceanbase::blocksstable;
using namespace oceanbase::clog;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;



/******************             ObBlockMetaTree              **********************/
ObBlockMetaTree::ObBlockMetaTree()
  : is_inited_(false), macro_blocks_(), arena_("DDL_Btree", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID()), tree_allocator_(arena_), block_tree_(tree_allocator_), datum_utils_(nullptr)
{
  macro_blocks_.set_attr(ObMemAttr(MTL_ID(), "DDL_Btree"));
}

ObBlockMetaTree::~ObBlockMetaTree()
{
  destroy();
}

int ObBlockMetaTree::init(ObTablet &tablet,
                          const ObITable::TableKey &table_key,
                          const share::SCN &ddl_start_scn,
                          const uint64_t data_format_version,
                          const ObStorageSchema *storage_schema)
{
  int ret = OB_SUCCESS;
  const ObMemAttr mem_attr(MTL_ID(), "BlockMetaTree");
  ObTableStoreIterator ddl_table_iter;
  ObITable *first_ddl_sstable = nullptr; // get compressor_type of macro block for query
  ObTabletHandle tablet_handle;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret));
  } else if (OB_UNLIKELY(!table_key.is_valid() || data_format_version <= 0 || OB_ISNULL(storage_schema))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(table_key), KP(storage_schema));
  } else if (FALSE_IT(arena_.set_attr(mem_attr))) {
  } else if (OB_FAIL(block_tree_.init())) {
    LOG_WARN("init block tree failed", K(ret));
  } else if (OB_FAIL(tablet.get_ddl_sstables(ddl_table_iter))) {
    LOG_WARN("get ddl sstable handles failed", K(ret));
  } else if (ddl_table_iter.count() > 0 && OB_FAIL(ddl_table_iter.get_boundary_table(false/*is_last*/, first_ddl_sstable))) {
    LOG_WARN("failed to get boundary table", K(ret));
  } else if (OB_FAIL(ObTabletDDLUtil::prepare_index_data_desc(tablet,
                                                              table_key.is_column_store_sstable() ? table_key.get_column_group_id() : -1/*negative value means row_store*/,
                                                              table_key.get_snapshot_version(),
                                                              data_format_version,
                                                              static_cast<ObSSTable *>(first_ddl_sstable),
                                                              table_key.get_end_scn(),
                                                              storage_schema,
                                                              data_desc_))) {
    LOG_WARN("prepare data store desc failed", K(ret), K(table_key), K(data_format_version));
  } else {
    if (data_desc_.get_desc().is_cg()) {
      schema::ObColDesc int_col_desc;
      int_col_desc.col_id_ = 0;
      int_col_desc.col_order_ = ObOrderType::ASC;
      int_col_desc.col_type_.set_int();
      ObSEArray<schema::ObColDesc, 1> col_descs;
      col_descs.set_attr(ObMemAttr(MTL_ID(), "DDL_Btree_descs"));
      const bool is_column_store = true;
      if (OB_FAIL(col_descs.push_back(int_col_desc))) {
        LOG_WARN("push back col desc failed", K(ret));
      } else if (OB_FAIL(row_id_datum_utils_.init(col_descs, col_descs.count(), lib::is_oracle_mode(), arena_, is_column_store))) {
        LOG_WARN("init row id datum utils failed", K(ret), K(col_descs));
      } else {
        datum_utils_ = &row_id_datum_utils_;
        LOG_INFO("block meta tree sort with row id", K(table_key));
      }
    } else {
      datum_utils_ = const_cast<blocksstable::ObStorageDatumUtils *>(&data_desc_.get_desc().get_datum_utils());
      LOG_INFO("block meta tree sort with row key", K(table_key));
    }
    is_inited_ = true;
  }
  return ret;
}

int ObDDLMemtable::init_sstable_param(
    ObTablet &tablet,
    const ObITable::TableKey &table_key,
    const share::SCN &ddl_start_scn,
    ObTabletCreateSSTableParam &sstable_param)
{
  int ret = OB_SUCCESS;
  ObStorageSchema *storage_schema_ptr = nullptr;
  ObLSService *ls_service = MTL(ObLSService *);
  ObArenaAllocator allocator("DDL_MMT", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
  ObTabletHandle tablet_handle;
  if (OB_UNLIKELY(!table_key.is_valid() || !ddl_start_scn.is_valid_and_not_min())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(table_key), K(ddl_start_scn));
  } else if (OB_ISNULL(ls_service)) {
    ret = OB_ERR_SYS;
    LOG_WARN("ls service is null", K(ret), K(table_key));
  } else if (OB_FAIL(tablet.load_storage_schema(allocator, storage_schema_ptr))) {
    LOG_WARN("fail to get storage schema", K(ret));
  } else {
    int64_t column_count = 0;
    const ObStorageSchema &storage_schema = *storage_schema_ptr;
    const int64_t root_block_size = sizeof(ObBlockMetaTree);
    const ObDataStoreDesc &data_desc = block_meta_tree_.get_data_desc();
    if (OB_FAIL(storage_schema.get_stored_column_count_in_sstable(column_count))) {
      LOG_WARN("fail to get stored column count in sstable", K(ret));
    } else {
      sstable_param.table_key_ = table_key;
      if (table_key.is_column_store_sstable()) {
        if (table_key.is_normal_cg_sstable()) {
          sstable_param.table_key_.table_type_ = ObITable::TableType::DDL_MEM_CG_SSTABLE;
          sstable_param.rowkey_column_cnt_ = 0;
          sstable_param.column_cnt_ = 1;
        } else { // co sstable with all cg or rowkey cg
          sstable_param.table_key_.table_type_ = ObITable::TableType::DDL_MEM_CO_SSTABLE;
          sstable_param.rowkey_column_cnt_ = storage_schema.get_rowkey_column_num() + ObMultiVersionRowkeyHelpper::get_extra_rowkey_col_cnt();

          // calculate column count
          const ObIArray<ObStorageColumnGroupSchema> &cg_schemas = storage_schema.get_column_groups();
          const int64_t cg_idx = sstable_param.table_key_.get_column_group_id();
          if (cg_idx < 0 || cg_idx >= cg_schemas.count()) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unexpected column group index", K(ret), K(cg_idx));
          } else if (cg_schemas.at(cg_idx).is_rowkey_column_group()) {
            column_count = storage_schema.get_rowkey_column_num() + ObMultiVersionRowkeyHelpper::get_extra_rowkey_col_cnt();
          } else {
            if (OB_FAIL(storage_schema.get_stored_column_count_in_sstable(column_count))) {
              LOG_WARN("fail to get stored column count in sstable", K(ret));
            }
          }
          if (OB_SUCC(ret)) {
            sstable_param.column_cnt_ = column_count;
          }
        }
      } else {
        sstable_param.table_key_.table_type_ = ObITable::TableType::DDL_MEM_SSTABLE;
        sstable_param.rowkey_column_cnt_ = storage_schema.get_rowkey_column_num() + ObMultiVersionRowkeyHelpper::get_extra_rowkey_col_cnt();
        sstable_param.column_cnt_ = column_count;
      }
      sstable_param.is_ready_for_read_ = true;
      sstable_param.table_mode_ = storage_schema.get_table_mode_struct();
      sstable_param.index_type_ = storage_schema.get_index_type();
      sstable_param.schema_version_ = storage_schema.get_schema_version();
      sstable_param.latest_row_store_type_ = storage_schema.get_row_store_type();
      sstable_param.create_snapshot_version_ = table_key.get_snapshot_version();
      sstable_param.max_merged_trans_version_ = table_key.get_snapshot_version();
      sstable_param.ddl_scn_ = ddl_start_scn;
      sstable_param.root_row_store_type_ = data_desc.get_row_store_type(); // for root block, not used for ddl memtable
      sstable_param.data_index_tree_height_ = 2; // fixed tree height, because there is only one root block
      sstable_param.contain_uncommitted_row_ = false; // ddl build major sstable with committed rows only
      sstable_param.compressor_type_ = data_desc.get_compressor_type();
      sstable_param.encrypt_id_ = data_desc.get_encrypt_id();
      sstable_param.master_key_id_ = data_desc.get_master_key_id();
      MEMCPY(sstable_param.encrypt_key_, data_desc.get_encrypt_key(), share::OB_MAX_TABLESPACE_ENCRYPT_KEY_LENGTH);
      sstable_param.use_old_macro_block_count_ = 0; // all new, no reuse
      sstable_param.index_blocks_cnt_ = 0; // index macro block count, the index is in memory, so be 0.
      sstable_param.other_block_ids_.reset(); // other blocks contains only index macro blocks now, so empty.
    }

    if (OB_SUCC(ret)) {
      // set root block for data tree
      if (OB_FAIL(sstable_param.root_block_addr_.set_mem_addr(0/*offset*/, root_block_size/*size*/))) {
        LOG_WARN("set root block address for data tree failed", K(ret));
      } else {
        sstable_param.root_block_data_.type_ = ObMicroBlockData::DDL_BLOCK_TREE;
        sstable_param.root_block_data_.buf_ = reinterpret_cast<char *>(&block_meta_tree_);
        sstable_param.root_block_data_.size_ = root_block_size;
      }
    }

    if (OB_SUCC(ret)) {
      // set root block for secondary meta tree
      if (OB_FAIL(sstable_param.data_block_macro_meta_addr_.set_mem_addr(0/*offset*/, root_block_size/*size*/))) {
        LOG_WARN("set root block address for secondary meta tree failed", K(ret));
      } else {
        sstable_param.data_block_macro_meta_.type_ = ObMicroBlockData::DDL_BLOCK_TREE;
        sstable_param.data_block_macro_meta_.buf_ = reinterpret_cast<char *>(&block_meta_tree_);
        sstable_param.data_block_macro_meta_.size_ = root_block_size;
      }
    }
  }
  ObTabletObjLoadHelper::free(allocator, storage_schema_ptr);
  return ret;
}

void ObBlockMetaTree::destroy()
{
  is_inited_ = false;
  macro_blocks_.reset();
  destroy_tree_value();
  block_tree_.destroy(false /*is_batch_destroy*/);
  tree_allocator_.reset();
  data_desc_.reset();
  row_id_datum_utils_.reset();
  datum_utils_ = nullptr;
  arena_.reset();
}

void ObBlockMetaTree::destroy_tree_value()
{
  int ret = OB_SUCCESS;
  const int64_t version = INT64_MAX;
  blocksstable::DDLBtreeIterator tmp_iter;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(block_tree_.set_key_range(tmp_iter,
                                               ObDatumRowkeyWrapper(&ObDatumRowkey::MIN_ROWKEY, datum_utils_),
                                               false,
                                               ObDatumRowkeyWrapper(&ObDatumRowkey::MAX_ROWKEY, datum_utils_),
                                               false,
                                               version))) {
    LOG_WARN("locate range failed", K(ret));
  } else {
    while (OB_SUCC(ret)) {
      ObDatumRowkeyWrapper rowkey_wrapper;
      ObBlockMetaTreeValue *tree_value  = nullptr;
      if (OB_FAIL(tmp_iter.get_next(rowkey_wrapper, tree_value))) {
        if (OB_ITER_END != ret) {
          LOG_WARN("get next failed", K(ret));
        } else {
          ret = OB_SUCCESS;
          break;
        }
      } else if (OB_NOT_NULL(tree_value)) {
        // destruct meta before free
        tree_value->block_meta_->~ObDataMacroBlockMeta();
        tree_value->~ObBlockMetaTreeValue();
      }
    }
  }
}

int ObBlockMetaTree::insert_macro_block(const ObDDLMacroHandle &macro_handle,
                                        const blocksstable::ObDatumRowkey *rowkey,
                                        const blocksstable::ObDataMacroBlockMeta *meta,
                                        const int64_t co_sstable_row_offset)
{
  int ret = OB_SUCCESS;
  ObDataMacroBlockMeta *insert_meta = const_cast<ObDataMacroBlockMeta *>(meta);
  void *buf = nullptr;
  ObBlockMetaTreeValue *tree_value = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!macro_handle.is_valid() || nullptr == rowkey || nullptr == meta)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(macro_handle), K(rowkey), KP(meta));
  } else if (OB_FAIL(macro_blocks_.push_back(macro_handle))) {
    LOG_WARN("push back macro handle failed", K(ret), K(macro_handle));
  } else if (OB_ISNULL(buf = arena_.alloc(sizeof(ObBlockMetaTreeValue)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("allocate memory failed", K(ret), K(sizeof(ObBlockMetaTreeValue)));
  } else {
    tree_value = new (buf) ObBlockMetaTreeValue(insert_meta, rowkey);

    tree_value->co_sstable_row_offset_ = co_sstable_row_offset;
    tree_value->header_.version_ = ObIndexBlockRowHeader::INDEX_BLOCK_HEADER_V1;
    tree_value->header_.row_store_type_ = static_cast<uint8_t>(data_desc_.get_desc().get_row_store_type());
    tree_value->header_.compressor_type_ = static_cast<uint8_t>(data_desc_.get_desc().get_compressor_type());
    tree_value->header_.is_data_index_ = true;
    tree_value->header_.is_data_block_ = false;
    tree_value->header_.is_leaf_block_ = true;
    tree_value->header_.is_macro_node_ = true;
    tree_value->header_.is_major_node_ = true;
    tree_value->header_.is_deleted_ = insert_meta->val_.is_deleted_;
    tree_value->header_.contain_uncommitted_row_ = insert_meta->val_.contain_uncommitted_row_;
    tree_value->header_.macro_id_ = insert_meta->val_.macro_id_;
    tree_value->header_.block_offset_ = insert_meta->val_.block_offset_;
    tree_value->header_.block_size_ = insert_meta->val_.block_size_;
    tree_value->header_.macro_block_count_ = 1;
    tree_value->header_.micro_block_count_ = insert_meta->val_.micro_block_count_;
    tree_value->header_.master_key_id_ = data_desc_.get_desc().get_master_key_id();
    tree_value->header_.encrypt_id_ = data_desc_.get_desc().get_encrypt_id();
    MEMCPY(tree_value->header_.encrypt_key_, data_desc_.get_desc().get_encrypt_key(), sizeof(tree_value->header_.encrypt_key_));
    tree_value->header_.schema_version_ = data_desc_.get_desc().get_schema_version();
    tree_value->header_.row_count_ = insert_meta->val_.row_count_;
    if (OB_UNLIKELY(!tree_value->header_.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("Built an invalid index block row", K(ret), K(tree_value->header_), KPC(insert_meta));
    } else if (OB_FAIL(block_tree_.insert(ObDatumRowkeyWrapper(tree_value->rowkey_, datum_utils_), tree_value))) {
      LOG_WARN("insert block tree failed", K(ret), K(rowkey), KPC(meta));
    }
  }
  return ret;
}

int ObBlockMetaTree::get_sorted_meta_array(ObIArray<const ObDataMacroBlockMeta *> &meta_array)
{
  int ret = OB_SUCCESS;
  meta_array.reset();
  const int64_t version = INT64_MAX;
  blocksstable::DDLBtreeIterator tmp_iter;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_FAIL(block_tree_.set_key_range(tmp_iter,
                                               ObDatumRowkeyWrapper(&ObDatumRowkey::MIN_ROWKEY, datum_utils_),
                                               false,
                                               ObDatumRowkeyWrapper(&ObDatumRowkey::MAX_ROWKEY, datum_utils_),
                                               false,
                                               version))) {
    LOG_WARN("locate range failed", K(ret));
  } else if (OB_FAIL(meta_array.reserve(macro_blocks_.count()))) {
    LOG_WARN("reserve meta array failed", K(ret), K(macro_blocks_.count()));
  } else {
    while (OB_SUCC(ret)) {
      ObDatumRowkeyWrapper rowkey_wrapper;
      ObBlockMetaTreeValue *tree_value  = nullptr;
      if (OB_FAIL(tmp_iter.get_next(rowkey_wrapper, tree_value))) {
        if (OB_ITER_END != ret) {
          LOG_WARN("get next failed", K(ret));
        } else {
          ret = OB_SUCCESS;
          break;
        }
      } else if (OB_ISNULL(tree_value)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("tree_value is null", K(ret), KP(tree_value));
      } else if (((uint64_t)(tree_value) & 7ULL) != 0) {
        ret = OB_ERR_UNEXPECTED;
        LOG_ERROR("invalid btree value", K(ret), K(tree_value));
      } else if (OB_FAIL(meta_array.push_back(tree_value->block_meta_))) {
        LOG_WARN("push back block meta failed", K(ret), K(*tree_value->block_meta_));
      }
    }
    if (OB_SUCC(ret)) {
      if (meta_array.count() != macro_blocks_.count()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("meta array count not euqal with macro_block count", K(ret), K(meta_array.count()), K(macro_blocks_.count()));
      }
    }
  }
  return ret;
}

int ObBlockMetaTree::exist(const blocksstable::ObDatumRowkey *rowkey, bool &is_exist)
{
  int ret = OB_SUCCESS;
  is_exist = false;
  ObBlockMetaTreeValue *tree_value  = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_ISNULL(rowkey)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(rowkey));
  } else if (OB_FAIL(block_tree_.get(ObDatumRowkeyWrapper(rowkey, datum_utils_), tree_value))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("get value from block meta tree failed", K(ret), KPC(rowkey));
    } else {
      is_exist = false;
      ret = OB_SUCCESS;
    }
  } else {
    is_exist = true;
  }
  return ret;
}

bool ObBlockMetaTree::CompareFunctor::operator ()(const IndexItem &item,
                                                  const blocksstable::ObDatumRowkey &rowkey)
{
  int cmp_ret = 0;
  item.rowkey_->compare(rowkey, datum_utils_, cmp_ret);
  return cmp_ret < 0;
}

bool ObBlockMetaTree::CompareFunctor::operator ()(const blocksstable::ObDatumRowkey &rowkey,
                                                  const IndexItem &item)
{
  int cmp_ret = 0;
  item.rowkey_->compare(rowkey, datum_utils_, cmp_ret);
  return cmp_ret > 0;
}

int ObBlockMetaTree::lower_bound(const blocksstable::ObDatumRowkey *target_rowkey,
                                 const blocksstable::ObStorageDatumUtils &datum_utils,
                                 blocksstable::ObDatumRowkey *&rowkey,
                                 ObBlockMetaTreeValue *&tree_value) const
{
  int ret = OB_SUCCESS;
  rowkey = nullptr;
  tree_value = nullptr;
  const int64_t version = INT64_MAX;
  bool find = false;
  blocksstable::DDLBtreeIterator tmp_iter;
  if (OB_ISNULL(target_rowkey)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("target rowkey is null", K(ret));
  } else if (OB_FAIL(block_tree_.set_key_range(tmp_iter,
                                               ObDatumRowkeyWrapper(target_rowkey, &datum_utils),
                                               false,
                                               ObDatumRowkeyWrapper(&ObDatumRowkey::MAX_ROWKEY, &datum_utils),
                                               false,
                                               version))) {
    LOG_WARN("locate range failed", K(ret));
  } else {
    while (OB_SUCC(ret)) {
      ObDatumRowkeyWrapper rowkey_wrapper;
      ObBlockMetaTreeValue *tmp_tree_value  = nullptr;
      if (find) {
        break;
      } else if (OB_FAIL(tmp_iter.get_next(rowkey_wrapper, tmp_tree_value))) {
        if (OB_ITER_END != ret) {
          LOG_WARN("get next failed", K(ret));
        } else if (!find) {
          ret = OB_BEYOND_THE_RANGE;
        }
      } else {
        int cmp_ret = 0;
        if (!find) {
          rowkey_wrapper.rowkey_->compare(*target_rowkey, datum_utils, cmp_ret);
          if (cmp_ret >= 0) {
            rowkey = const_cast<blocksstable::ObDatumRowkey *>(rowkey_wrapper.rowkey_);
            tree_value = tmp_tree_value;
            find = true;
          }
        }
      }
    }
  }
  return ret;
}

int ObBlockMetaTree::upper_bound(const blocksstable::ObDatumRowkey *target_rowkey,
                                 const blocksstable::ObStorageDatumUtils &datum_utils,
                                 blocksstable::ObDatumRowkey *&rowkey,
                                 ObBlockMetaTreeValue *&tree_value) const
{
  int ret = OB_SUCCESS;
  rowkey = nullptr;
  tree_value = nullptr;
  const int64_t version = INT64_MAX;
  bool find = false;
  blocksstable::DDLBtreeIterator tmp_iter;
  if (OB_ISNULL(target_rowkey)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("target rowkey is null", K(ret));
  } else if (OB_FAIL(block_tree_.set_key_range(tmp_iter,
                                           ObDatumRowkeyWrapper(target_rowkey, &datum_utils),
                                           true,
                                           ObDatumRowkeyWrapper(&ObDatumRowkey::MAX_ROWKEY, &datum_utils),
                                           false,
                                           version))) {
    LOG_WARN("locate range failed", K(ret));
  } else {
    while (OB_SUCC(ret)) {
      ObDatumRowkeyWrapper rowkey_wrapper;
      ObBlockMetaTreeValue *tmp_tree_value  = nullptr;
      if (find) {
        break;
      } else if (OB_FAIL(tmp_iter.get_next(rowkey_wrapper, tmp_tree_value))) {
        if (OB_ITER_END != ret) {
          LOG_WARN("get next failed", K(ret));
        } else if (!find) {
          ret = OB_BEYOND_THE_RANGE;
        }
      } else {
        int cmp_ret = 0;
        if (!find) {
          rowkey_wrapper.rowkey_->compare(*target_rowkey, datum_utils, cmp_ret);
          if (cmp_ret > 0) {
            rowkey = const_cast<blocksstable::ObDatumRowkey *>(rowkey_wrapper.rowkey_);
            tree_value = tmp_tree_value;
            find = true;
          }
        }
      }
    }
  }
  return ret;
}

int ObBlockMetaTree::locate_key(const blocksstable::ObDatumRange &range,
                                const blocksstable::ObStorageDatumUtils &datum_utils,
                                blocksstable::DDLBtreeIterator &iter,
                                ObBlockMetaTreeValue *&cur_tree_value) const
{
  int ret = OB_SUCCESS;
  cur_tree_value = nullptr;
  const int64_t version = INT64_MAX;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else {
    ObDatumRowkey *rowkey = nullptr;
    ObBlockMetaTreeValue *tree_value = nullptr;
    //locate key must be lower_bound
    if (OB_FAIL(lower_bound(&range.get_start_key(), datum_utils, rowkey, tree_value))) {
      LOG_WARN("lower bound failed", K(ret), K(range.get_start_key()));
    } else {
      //todo qilu: not set_key_range again
      iter.reset();
      if (OB_FAIL(block_tree_.set_key_range(iter,
                                            ObDatumRowkeyWrapper(rowkey, &datum_utils),
                                            true,
                                            ObDatumRowkeyWrapper(rowkey, &datum_utils),
                                            true,
                                            version))) {
        LOG_WARN("locate range failed", K(ret));
      } else {
        cur_tree_value = tree_value;
      }
    }
  }
  return ret;
}

int ObBlockMetaTree::locate_range(const blocksstable::ObDatumRange &range,
                                  const blocksstable::ObStorageDatumUtils &datum_utils,
                                  const bool is_left_border,
                                  const bool is_right_border,
                                  const bool is_reverse_scan,
                                  blocksstable::DDLBtreeIterator &iter,
                                  ObBlockMetaTreeValue *&cur_tree_value) const
{
  int ret = OB_SUCCESS;
  cur_tree_value = nullptr;
  const int64_t version = INT64_MAX;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else {
    //pre check range
    ObDatumRowkey *start_rowkey = nullptr;
    ObDatumRowkey *end_rowkey = nullptr;
    ObBlockMetaTreeValue *start_tree_value = nullptr;
    ObBlockMetaTreeValue *end_tree_value = nullptr;
    bool right_border_beyond_range = false;
    if (!is_left_border || range.get_start_key().is_min_rowkey()) {
      start_rowkey = &ObDatumRowkey::MIN_ROWKEY;
    } else {
      if (range.is_left_closed()) {
        if (OB_FAIL(lower_bound(&range.get_start_key(), datum_utils, start_rowkey, start_tree_value))) {
          LOG_WARN("lower bound failed", K(ret), K(range.get_start_key()));
        }
      } else {
        if (OB_FAIL(upper_bound(&range.get_start_key(), datum_utils, start_rowkey, start_tree_value))) {
          LOG_WARN("upper bound failed", K(ret), K(range.get_start_key()));
        }
      }
      // maybe OB_BEYOND_THE_RANGE
    }
    if (OB_SUCC(ret)) {
      if (!is_right_border || range.get_end_key().is_max_rowkey()) {
        end_rowkey = &ObDatumRowkey::MAX_ROWKEY;
      } else {
        if (OB_FAIL(lower_bound(&range.get_end_key(), datum_utils, end_rowkey, end_tree_value))) {
          if (OB_BEYOND_THE_RANGE == ret) {
            ret = OB_SUCCESS;
            right_border_beyond_range = true;
            end_rowkey = &ObDatumRowkey::MAX_ROWKEY;
          } else {
            LOG_WARN("lower bound failed", K(ret), K(range.get_end_key()));
          }
        }
      }
    }

    //locate real range
    if (OB_SUCC(ret)) {
      iter.reset();
      if (!is_reverse_scan) {
        if (!is_left_border || range.get_start_key().is_min_rowkey()) {
          ObDatumRowkeyWrapper rowkey_wrapper_left;
          ObBlockMetaTreeValue *tree_value_left = nullptr;
          if (OB_FAIL(block_tree_.set_key_range(iter,
                                                ObDatumRowkeyWrapper(start_rowkey /*ObDatumRowkey::MIN_ROWKEY*/, &datum_utils),
                                                false,
                                                ObDatumRowkeyWrapper(end_rowkey, &datum_utils),
                                                false,
                                                version))) {
            LOG_WARN("locate range failed", K(ret));
          } else if (OB_FAIL(iter.get_next(rowkey_wrapper_left, tree_value_left))) {
            if (OB_ITER_END != ret) {
              LOG_WARN("get next failed", K(ret));
            } else {
              ret = OB_BEYOND_THE_RANGE;
              LOG_WARN("beyond range", K(ret), K(range));
            }
          } else {
            cur_tree_value = tree_value_left;
          }
        } else {
          int cmp_ret = 0;
          if (OB_FAIL(start_rowkey->compare(*end_rowkey, datum_utils, cmp_ret))) {
            LOG_WARN("fail to compare rowkey", K(ret), KPC(end_rowkey), KPC(start_rowkey), K(datum_utils));
          } else if (cmp_ret > 0) {
            ret = OB_ERR_UNEXPECTED;
            LOG_ERROR("start row key is bigger than right rowkey", K(ret), KPC(start_rowkey), KPC(end_rowkey));
          } else if (OB_FAIL(block_tree_.set_key_range(iter,
                                                      ObDatumRowkeyWrapper(start_rowkey, &datum_utils),
                                                      true,
                                                      ObDatumRowkeyWrapper(end_rowkey, &datum_utils),
                                                      false,
                                                      version))) {
            LOG_WARN("locate range failed", K(ret));
          } else {
            cur_tree_value = start_tree_value;
          }
        }
      } else {
        if (right_border_beyond_range || !is_right_border || range.get_end_key().is_max_rowkey()) {
          ObDatumRowkeyWrapper rowkey_wrapper_right;
          ObBlockMetaTreeValue *tree_value_right = nullptr;
          if (OB_FAIL(block_tree_.set_key_range(iter,
                                                ObDatumRowkeyWrapper(end_rowkey /*ObDatumRowkey::MAX_ROWKEY*/, &datum_utils),
                                                false,
                                                ObDatumRowkeyWrapper(start_rowkey, &datum_utils),
                                                false,
                                                version))) {
            LOG_WARN("locate range failed", K(ret));
          } else if (OB_FAIL(iter.get_next(rowkey_wrapper_right, tree_value_right))) {
            if (OB_ITER_END != ret) {
              LOG_WARN("get next failed", K(ret));
            } else {
              ret = OB_BEYOND_THE_RANGE;
              LOG_WARN("beyond range", K(ret), K(range));
            }
          } else {
            cur_tree_value = tree_value_right;
          }
        } else {
          int cmp_ret = 0;
          if (OB_FAIL(start_rowkey->compare(*end_rowkey, datum_utils, cmp_ret))) {
            LOG_WARN("fail to compare rowkey", K(ret), KPC(end_rowkey), KPC(start_rowkey), K(datum_utils));
          } else if (cmp_ret > 0) {
            ret = OB_ERR_UNEXPECTED;
            LOG_ERROR("start row key is bigger than right rowkey", K(ret), KPC(start_rowkey), KPC(end_rowkey));
          } else if (OB_FAIL(block_tree_.set_key_range(iter,
                                                       ObDatumRowkeyWrapper(end_rowkey, &datum_utils),
                                                       true,
                                                       ObDatumRowkeyWrapper(start_rowkey, &datum_utils),
                                                       false,
                                                       version))) {
            LOG_WARN("locate range failed", K(ret));
          } else {
            cur_tree_value = end_tree_value;
          }
        }
      }
    }
  }
  return ret;
}

int ObBlockMetaTree::skip_to_next_valid_position(const blocksstable::ObDatumRowkey &rowkey,
                                                 const blocksstable::ObStorageDatumUtils &datum_utils,
                                                 blocksstable::DDLBtreeIterator &iter,
                                                 ObBlockMetaTreeValue *&tree_value) const
{
  int ret = OB_SUCCESS;
  tree_value = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else {
    int cmp_ret = 0;
    while (OB_SUCC(ret)) {
      ObDatumRowkeyWrapper rowkey_wrapper;
      ObBlockMetaTreeValue *tmp_tree_value  = nullptr;
      if (OB_FAIL(iter.get_next(rowkey_wrapper, tmp_tree_value))) {
        if (OB_ITER_END != ret) {
          LOG_WARN("get next failed", K(ret));
        }
        // just return ITER_END
      } else if (OB_FAIL(rowkey_wrapper.rowkey_->compare(rowkey, datum_utils, cmp_ret, false/*need_compare_datum_cnt*/))) {
        LOG_WARN("fail to cmp rowkey", K(ret), K(rowkey), K(rowkey_wrapper));
      } else if(cmp_ret >= 0) { //lower bound
        if (OB_ISNULL(tmp_tree_value)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("tree_value is null", K(ret), KP(tmp_tree_value));
        } else {
          tree_value = tmp_tree_value;
        }
        break;
      }
    }
  }
  return ret;
}

int ObBlockMetaTree::get_next_tree_value(blocksstable::DDLBtreeIterator &iter,
                                         const int64_t step,
                                         ObBlockMetaTreeValue *&tree_value) const
{
  int ret = OB_SUCCESS;
  tree_value = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(step <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(step));
  } else {
    ObBlockMetaTreeValue *tmp_tree_value  = nullptr;
    for (int64_t i = 0; OB_SUCC(ret) && i < step; ++i) {
      ObDatumRowkeyWrapper rowkey_wrapper;
      if (OB_FAIL(iter.get_next(rowkey_wrapper, tmp_tree_value))) {
        if (OB_ITER_END != ret) {
          LOG_WARN("get next failed", K(ret));
        }
        // just return ITER_END
      }
    }
    if (OB_FAIL(ret)) {
      // do nothing
    } else if (OB_ISNULL(tmp_tree_value)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("tree_value is null", K(ret), KP(tmp_tree_value));
    } else if (((uint64_t)(tmp_tree_value) & 7ULL) != 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("invalid btree value", K(ret), KP(tmp_tree_value));
    } else {
      tree_value = tmp_tree_value;
    }
  }
  return ret;
}

int ObBlockMetaTree::get_last_rowkey(const ObDatumRowkey *&last_rowkey)
{
  int ret = OB_SUCCESS;
  const int64_t version = INT64_MAX;
  last_rowkey = nullptr;
  blocksstable::DDLBtreeIterator tmp_iter;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
    //always forward
  } else if (OB_FAIL(block_tree_.set_key_range(tmp_iter,
                                               ObDatumRowkeyWrapper(&ObDatumRowkey::MIN_ROWKEY, datum_utils_),
                                               false,
                                               ObDatumRowkeyWrapper(&ObDatumRowkey::MAX_ROWKEY, datum_utils_),
                                               false,
                                               version))) {
    LOG_WARN("locate range failed", K(ret));
  } else {
    bool find = false;
    ObDatumRowkeyWrapper rowkey_wrapper;
    ObBlockMetaTreeValue *tree_value  = nullptr;
    while (OB_SUCC(ret)) {
      if (OB_FAIL(tmp_iter.get_next(rowkey_wrapper, tree_value))) {
        if (OB_ITER_END != ret) {
          LOG_WARN("get next failed", K(ret));
        } else {
          ret = OB_SUCCESS;
          find = true;
          break;
        }
      }
    }
    if (find) {
      last_rowkey = rowkey_wrapper.rowkey_;
    } else {
      last_rowkey = &ObDatumRowkey::MAX_ROWKEY;
    }
  }
  return ret;
}

int64_t ObBlockMetaTree::get_memory_used() const
{
  return arena_.total();
}

/******************             ObDDLKV              **********************/

ObDDLMemtable::ObDDLMemtable()
  : is_inited_(false), allocator_("ddl_mem_sst", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID()), block_meta_tree_()
{

}

ObDDLMemtable::~ObDDLMemtable()
{

}

int ObDDLMemtable::init(
    ObTablet &tablet,
    const ObITable::TableKey &table_key,
    const share::SCN &ddl_start_scn,
    const uint64_t data_format_version)
{

  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret), KP(this));
  } else if (OB_UNLIKELY(!table_key.is_valid()
        || !ddl_start_scn.is_valid_and_not_min()
        || data_format_version <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(table_key), K(ddl_start_scn), K(data_format_version));
  } else {
    ObTabletCreateSSTableParam sstable_param;
    ObStorageSchema *storage_schema = nullptr;
    ObArenaAllocator arena("init_ddl_memt", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
    if (OB_FAIL(tablet.load_storage_schema(arena, storage_schema))) {
      LOG_WARN("load storage schema failed", K(ret), KPC(this));
    } else if (OB_FAIL(block_meta_tree_.init(tablet, table_key, ddl_start_scn, data_format_version, storage_schema))) {
      LOG_WARN("init mem index sstable failed", K(ret), K(table_key), K(ddl_start_scn));
    } else if (OB_FAIL(init_sstable_param(tablet, table_key, ddl_start_scn, sstable_param))) {
      LOG_WARN("init sstable param failed", K(ret));
    } else if (OB_FAIL(ObSSTable::init(sstable_param, &allocator_))) {
      LOG_WARN("init sstable failed", K(ret));
    } else {
      is_inited_ = true;
    }
    ObTabletObjLoadHelper::free(arena, storage_schema);
  }
  return ret;
}

void ObDDLMemtable::reset()
{
  is_inited_ = false;
  ObSSTable::reset();
  block_meta_tree_.destroy();
  allocator_.reset();
}

void ObDDLMemtable::set_scn_range(
    const share::SCN &start_scn,
    const share::SCN &end_scn)
{
  key_.scn_range_.start_scn_ = start_scn;
  key_.scn_range_.end_scn_ = end_scn;
}

int ObDDLMemtable::get_sorted_meta_array(
    ObIArray<const blocksstable::ObDataMacroBlockMeta *> &meta_array)
{
  int ret = OB_SUCCESS;
  meta_array.reset();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), KP(this));
  } else if (OB_FAIL(block_meta_tree_.get_sorted_meta_array(meta_array))) {
    LOG_WARN("get sorted array failed", K(ret));
  }
  return ret;
}

int ObDDLMemtable::init_ddl_index_iterator(const blocksstable::ObStorageDatumUtils *datum_utils,
                                           const bool is_reverse_scan,
                                           blocksstable::ObDDLIndexBlockRowIterator *ddl_kv_index_iter)
{
  int ret = OB_SUCCESS;
  const bool is_co_sst = is_co_sstable();
  if (OB_ISNULL(datum_utils) || OB_UNLIKELY(!datum_utils->is_valid()) || OB_ISNULL(ddl_kv_index_iter)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguement", K(ret), KP(ddl_kv_index_iter), KPC(datum_utils));
  } else if (OB_FAIL(ddl_kv_index_iter->set_iter_param(datum_utils, is_reverse_scan, &block_meta_tree_, is_co_sst))) {
    LOG_WARN("fail to set ddl iter param", K(ret));
  }
  return ret;
}

ObDDLKV::ObDDLKV()
  : is_inited_(false), is_closed_(false), ref_cnt_(0), lock_(), arena_allocator_("DDL_CONTAINER", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID()),
    ls_id_(), tablet_id_(), ddl_start_scn_(SCN::min_scn()), snapshot_version_(0), data_format_version_(0),
    is_freezed_(false), last_freezed_scn_(SCN::min_scn()),
    min_scn_(SCN::max_scn()), max_scn_(SCN::min_scn()), freeze_scn_(SCN::max_scn()), pending_cnt_(0),
    macro_block_count_(0)
{

}

ObDDLKV::~ObDDLKV()
{
  reset();
}

void ObDDLKV::inc_ref()
{
  ATOMIC_AAF(&ref_cnt_, 1);
  // FLOG_INFO("DDLKV inc_ref", K(ref_cnt_), KP(this), K(tablet_id_));
}


int ObDDLKV::init(const ObLSID &ls_id,
                  const ObTabletID &tablet_id,
                  const SCN &ddl_start_scn,
                  const int64_t snapshot_version,
                  const SCN &last_freezed_scn,
                  const uint64_t data_format_version)

{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret), KP(this));
  } else if (OB_UNLIKELY(!ls_id.is_valid()
        || !tablet_id.is_valid()
        || !ddl_start_scn.is_valid_and_not_min()
        || snapshot_version <= 0
        || !last_freezed_scn.is_valid_and_not_min()
        || data_format_version <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(ls_id), K(tablet_id), K(ddl_start_scn), K(snapshot_version), K(last_freezed_scn), K(data_format_version));
  } else {
    ls_id_ = ls_id;
    tablet_id_ = tablet_id;
    ddl_start_scn_ = ddl_start_scn;
    snapshot_version_ = snapshot_version;
    data_format_version_ = data_format_version;
    last_freezed_scn_ = last_freezed_scn;
    is_inited_ = true;
    LOG_INFO("ddl kv init success", K(ret), KP(this), K(*this));
  }
  return ret;
}

void ObDDLKV::reset()
{
  FLOG_INFO("ddl kv reset", KP(this), K(*this));
  is_inited_ = false;
  is_closed_ = false;
  ls_id_.reset();
  tablet_id_.reset();
  ddl_start_scn_ = SCN::min_scn();
  snapshot_version_ = 0;
  data_format_version_ = 0;

  is_freezed_ = false;
  last_freezed_scn_ = SCN::min_scn();
  min_scn_ = SCN::max_scn();
  max_scn_ = SCN::min_scn();
  freeze_scn_ = SCN::max_scn();
  pending_cnt_ = 0;

  for (int64_t i = 0; i < ddl_memtables_.count(); ++i) {
    if (OB_NOT_NULL(ddl_memtables_.at(i))) {
      ddl_memtables_.at(i)->reset();
      ddl_memtables_.at(i) = nullptr;
    }
  }
  macro_block_count_ = 0;
  ddl_memtables_.reset();
  arena_allocator_.reset();
}

int ObDDLKV::create_ddl_memtable(ObTablet &tablet, const ObITable::TableKey &table_key, ObDDLMemtable *&ddl_memtable)
{
  int ret = OB_SUCCESS;
  ddl_memtable = nullptr;
  void *buf = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!table_key.is_valid() || table_key.tablet_id_ != tablet_id_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(table_key), K(tablet_id_));
  } else if (OB_ISNULL(buf = arena_allocator_.alloc(sizeof(ObDDLMemtable)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("allocate memory failed", K(ret), K(sizeof(ObDDLMemtable)));
  } else {
    ddl_memtable = new (buf) ObDDLMemtable;
    if (OB_FAIL(ddl_memtable->init(tablet, table_key, ddl_start_scn_, data_format_version_))) {
      LOG_WARN("init ddl memtable failed", K(ret), K(table_key));
    } else if (OB_FAIL(ddl_memtables_.push_back(ddl_memtable))) {
      LOG_WARN("push back ddl memtable failed", K(ret));
    }
    if (OB_FAIL(ret) && nullptr != ddl_memtable) {
      ddl_memtable->~ObDDLMemtable();
      ddl_memtable = nullptr;
      arena_allocator_.free(ddl_memtable);
    }
  }
  return ret;
}

int ObDDLKV::get_ddl_memtable(const int64_t cg_idx, ObDDLMemtable *&ddl_memtable)
{
  int ret = OB_SUCCESS;
  ddl_memtable = nullptr;
  bool have_found = false;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(cg_idx < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(cg_idx));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && !have_found && i < ddl_memtables_.count(); ++i) {
      ObDDLMemtable *cur_ddl_memtable = ddl_memtables_.at(i);
      if (OB_ISNULL(cur_ddl_memtable)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("current ddl memtable is null", K(ret), K(i), K(cur_ddl_memtable));
      } else if (cur_ddl_memtable->get_column_group_id() == cg_idx) {
        ddl_memtable = cur_ddl_memtable;
        have_found = true;
      }
    }
  }
  if (OB_SUCC(ret) && !have_found) {
    ddl_memtable = nullptr;
    ret = OB_ENTRY_NOT_EXIST;
  }
  return ret;
}

int ObDDLKV::set_macro_block(
    ObTablet &tablet,
    const ObDDLMacroBlock &macro_block,
    const int64_t snapshot_version,
    const uint64_t data_format_version,
    const bool can_freeze)
{
  int ret = OB_SUCCESS;
  const int64_t MAX_DDL_BLOCK_COUNT = 10 * 10L * 1024L * 1024L * 1024L / OB_SERVER_BLOCK_MGR.get_macro_block_size();
  const int64_t MEMORY_LIMIT = 50 * 1024 * 1024; // 50M
  int64_t freeze_block_count = MAX_DDL_BLOCK_COUNT;
#ifdef ERRSIM
  if (0 != GCONF.errsim_max_ddl_block_count) {
    freeze_block_count = GCONF.errsim_max_ddl_block_count;
    LOG_INFO("ddl set macro block count", K(freeze_block_count));
  }
#endif
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ddl kv is not init", K(ret));
  } else if (OB_UNLIKELY(!macro_block.is_valid() || data_format_version <= 0 || snapshot_version <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(macro_block), K(data_format_version), K(snapshot_version));
  } else {
    const uint64_t tenant_id = MTL_ID();
    ObUnitInfoGetter::ObTenantConfig unit;
    int tmp_ret = OB_SUCCESS;
    if (OB_TMP_FAIL(GCTX.omt_->get_tenant_unit(tenant_id, unit))) {
      LOG_WARN("get tenant unit failed", K(tmp_ret), K(tenant_id));
    } else {
      const int64_t log_allowed_block_count = unit.config_.log_disk_size() * 0.2 / OB_SERVER_BLOCK_MGR.get_macro_block_size();
      if (log_allowed_block_count <= 0) {
        tmp_ret = OB_INVALID_ARGUMENT;
        LOG_WARN("invalid macro block count by log disk size", K(tmp_ret), K(tenant_id), K(unit.config_));
      } else {
        freeze_block_count = min(freeze_block_count, log_allowed_block_count);
      }
    }
  }
  if (OB_SUCC(ret) && (get_macro_block_cnt() >= freeze_block_count || get_memory_used() >= MEMORY_LIMIT) && can_freeze) {
    ObDDLTableMergeDagParam param;
    param.direct_load_type_    = ObDirectLoadType::DIRECT_LOAD_DDL;
    param.ls_id_               = ls_id_;
    param.tablet_id_           = tablet_id_;
    param.start_scn_           = ddl_start_scn_;
    param.data_format_version_ = data_format_version;
    param.snapshot_version_    = snapshot_version;
    int tmp_ret = OB_SUCCESS;
    if (OB_TMP_FAIL(ObTabletDDLUtil::freeze_ddl_kv(param))) {
      LOG_WARN("try to freeze ddl kv failed", K(tmp_ret), K(param));
    } else if (OB_TMP_FAIL(compaction::ObScheduleDagFunc::schedule_ddl_table_merge_dag(param))) {
      LOG_WARN("try schedule ddl merge dag failed when ddl kv is full ",
          K(tmp_ret), K(param), K(get_macro_block_cnt()));
    }
  }
  if (OB_SUCC(ret)) {
    ObDataMacroBlockMeta *data_macro_meta = nullptr;
    TCWLockGuard guard(lock_);
    // For incremental direct load, ddl_start_scn is set to min_scn().
    if (macro_block.ddl_start_scn_ != ddl_start_scn_) {
      if (macro_block.ddl_start_scn_ > ddl_start_scn_) {
        ret = OB_EAGAIN;
        LOG_INFO("ddl start scn too large, retry", K(ret),
            K(ls_id_), K(tablet_id_), K(ddl_start_scn_), K(macro_block));
      } else {
        // filter out and do nothing
        LOG_INFO("ddl start scn too small, maybe from old build task, ignore", K(ret),
            K(ls_id_), K(tablet_id_), K(ddl_start_scn_), K(macro_block));
      }
    } else if (macro_block.scn_ > freeze_scn_) {
      ret = OB_EAGAIN;
      LOG_INFO("this ddl kv is freezed, retry other ddl kv", K(ret), K(ls_id_), K(tablet_id_), K(macro_block), K(freeze_scn_));
    } else if (OB_UNLIKELY(snapshot_version != snapshot_version_ || data_format_version != data_format_version_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected error", K(ret), K(snapshot_version), K(data_format_version), KPC(this));
    } else {
      ObDDLMemtable *ddl_memtable = nullptr;
      // 1. try find the ddl memtable
      if (OB_FAIL(get_ddl_memtable(macro_block.table_key_.get_column_group_id(), ddl_memtable))) {
        if (OB_ENTRY_NOT_EXIST != ret) {
          LOG_WARN("get ddl memtable failed", K(ret));
        } else {
          ret = OB_SUCCESS;
        }
      }
      // 2. if not found, create one
      if (OB_SUCC(ret) && OB_ISNULL(ddl_memtable)) {
        if (OB_FAIL(create_ddl_memtable(tablet, macro_block.table_key_, ddl_memtable))) {
          LOG_WARN("create ddl memtable failed", K(ret), K(macro_block.table_key_));
        }
      }

      // 3. set macro block into meta tree
      if (OB_FAIL(ret)) {
      } else if (OB_ISNULL(ddl_memtable)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ddl memtable is null", K(ret));
      } else if (OB_FAIL(ObIndexBlockRebuilder::get_macro_meta(
              macro_block.buf_, macro_block.size_, macro_block.get_block_id(), arena_allocator_, data_macro_meta))) {
        LOG_WARN("get macro meta failed", K(ret), K(macro_block));
      } else if (data_macro_meta->end_key_.get_datum_cnt() <= 0) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid end key of data macro block meta", K(ret), K(data_macro_meta->end_key_));
      } else if (macro_block.table_key_.is_cg_sstable()) { // for normal cg, use row id as rowkey
        if (!macro_block.is_column_group_info_valid() || !data_macro_meta->end_key_.is_valid()) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("invalid ddl macro block", K(ret), K(macro_block), K(data_macro_meta->end_key_));
        } else {
          data_macro_meta->end_key_.datums_[0].set_int(macro_block.end_row_id_);
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(ddl_memtable->insert_block_meta_tree(macro_block.block_handle_, data_macro_meta, macro_block.end_row_id_))) {
        LOG_WARN("insert block meta tree faield", K(ret));
      } else {
        min_scn_ = SCN::min(min_scn_, macro_block.scn_);
        max_scn_ = SCN::max(max_scn_, macro_block.scn_);
        ++macro_block_count_;
        LOG_INFO("succeed to set macro block into ddl kv", K(macro_block), K(macro_block_count_), KPC(data_macro_meta));
      }
    }
  }
  return ret;
}

int ObDDLMemtable::insert_block_meta_tree(const ObDDLMacroHandle &macro_handle, blocksstable::ObDataMacroBlockMeta *data_macro_meta, const int64_t co_sstable_row_offset)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(block_meta_tree_.insert_macro_block(macro_handle, &data_macro_meta->end_key_, data_macro_meta, co_sstable_row_offset))) {
    LOG_WARN("insert macro block failed", K(ret), K(macro_handle), KPC(data_macro_meta));
  } else {
    const ObDataBlockMetaVal &meta_val = data_macro_meta->get_meta_val();
    meta_->get_basic_meta().data_macro_block_count_ += 1;
    meta_->get_basic_meta().data_micro_block_count_ += meta_val.micro_block_count_;
    meta_->get_basic_meta().max_merged_trans_version_ = max(meta_->get_basic_meta().max_merged_trans_version_, meta_val.max_merged_trans_version_);
    meta_->get_basic_meta().row_count_ += meta_val.row_count_;
    meta_->get_basic_meta().data_checksum_ = ob_crc64_sse42(meta_->get_basic_meta().data_checksum_, &meta_val.data_checksum_, sizeof(meta_val.data_checksum_));
    meta_->get_basic_meta().occupy_size_ += meta_val.occupy_size_;
    meta_->get_basic_meta().original_size_ += meta_val.original_size_;
    if (OB_FAIL(meta_cache_.init(meta_))) {
      LOG_WARN("failed to init meta cache", K(ret), KPC(meta_));
    }
  }
  return ret;
}

int ObDDLKV::freeze(const SCN &freeze_scn)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ddl kv is not init", K(ret));
  } else {
    TCWLockGuard guard(lock_);
    if (is_freezed()) {
      // do nothing
    } else {
      if (freeze_scn.is_valid_and_not_min()) {
        freeze_scn_ = freeze_scn;
      } else if (max_scn_.is_valid_and_not_min()) {
        freeze_scn_ = max_scn_;
      } else {
        ret = OB_EAGAIN;
        LOG_INFO("ddl kv not freezed, try again", K(ret), K(ls_id_), K(tablet_id_), K(get_macro_block_cnt()));
      }
      if (OB_SUCC(ret)) {
        ATOMIC_SET(&is_freezed_, true);
        LOG_INFO("ddl kv freezed", K(ret), K(ls_id_), K(tablet_id_), K(get_macro_block_cnt()));
      }
    }
  }
  return ret;
}

int ObDDLKV::prepare_sstable(const bool need_check/*=true*/)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ddl kv is not init", K(ret));
  } else if (!is_freezed()) {
    ret = OB_STATE_NOT_MATCH;
    LOG_WARN("ddl kv not freezed", K(ret), K(*this));
  } else if (ddl_memtables_.empty()) {
    // do nothing
  } else if (need_check && OB_FAIL(wait_pending())) {
    if (OB_EAGAIN != ret) {
      LOG_WARN("wait pending failed", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    TCWLockGuard guard(lock_);
    for (int64_t i = 0; OB_SUCC(ret) && i < ddl_memtables_.count(); ++i) {
      ObDDLMemtable *ddl_memtable = ddl_memtables_.at(i);
      if (OB_ISNULL(ddl_memtable)) {
        ret = OB_INVALID_ERROR;
        LOG_WARN("ddl memtable is null", K(ret));
      } else {
        ddl_memtable->set_scn_range(last_freezed_scn_, freeze_scn_);
      }
    }
  }
  return ret;
}

int ObDDLKV::close()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ddl kv is not init", K(ret));
  } else if (is_closed_) {
    // do nothing
    LOG_INFO("ddl kv already closed", K(*this));
  } else if (OB_FAIL(prepare_sstable())) {
    LOG_WARN("prepare sstable failed", K(ret));
  } else {
    is_closed_ = true; // data not dumped, just means data is complete
    LOG_INFO("ddl kv closed success", K(*this));
  }
  return ret;
}

void ObDDLKV::inc_pending_cnt()
{
  ATOMIC_INC(&pending_cnt_);
}

void ObDDLKV::dec_pending_cnt()
{
  ATOMIC_DEC(&pending_cnt_);
}

int ObDDLKV::wait_pending()
{
  int ret = OB_SUCCESS;
  ObLSService *ls_service = MTL(ObLSService *);
  ObLSHandle ls_handle;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), K(is_inited_));
  } else if (OB_UNLIKELY(!is_freezed())) {
    ret = OB_STATE_NOT_MATCH;
    LOG_WARN("ddl kv not freezed", K(ret));
  } else if (OB_FAIL(ls_service->get_ls(ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("get ls handle failed", K(ret), K(ls_id_));
  } else {
    SCN max_decided_scn;
    if (OB_FAIL(ls_handle.get_ls()->get_max_decided_scn(max_decided_scn))) {
      LOG_WARN("get max decided log ts failed", K(ret), K(ls_id_));
      if (OB_STATE_NOT_MATCH == ret) {
        ret = OB_NEED_RETRY;
      }
    } else {
      // max_decided_scn is the left border scn - 1
      // the min deciding(replay or apply) scn (aka left border) is max_decided_scn + 1
      const bool pending_finished = SCN::plus(max_decided_scn, 1) >= freeze_scn_ && !is_pending();
      if (!pending_finished) {
        ret = OB_EAGAIN;
        LOG_INFO("wait pending not finish", K(ret), K_(ls_id), K_(tablet_id), K_(freeze_scn), K_(last_freezed_scn), K_(min_scn), K_(max_scn), K(max_decided_scn));
      }
    }
  }
  return ret;
}

int64_t ObDDLKV::dec_ref()
{
  int64_t tmp_cnt = ATOMIC_SAF(&ref_cnt_, 1 /* just sub 1 */);
  if (0 == tmp_cnt) {
    MTL(ObTenantMetaMemMgr *)->release_ddl_kv(this);
  } else if (tmp_cnt < 0) {
    int ret = OB_ERR_SYS;
    LOG_ERROR("ref_cnt of ddl kv less than 0", KP(this));
  }
  return tmp_cnt;
}

int64_t ObDDLKV::get_memory_used() const
{
  int64_t total_used_memory = 0;
  TCRLockGuard guard(lock_);
  for (int64_t i = 0; i < ddl_memtables_.count(); ++i) {
    if (nullptr != ddl_memtables_.at(i)) {
      total_used_memory += ddl_memtables_.at(i)->get_memory_used();
    }
  }
  return total_used_memory;
}
