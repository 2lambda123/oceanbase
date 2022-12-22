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
#include "storage/compaction/ob_medium_compaction_mgr.h"
#include "storage/tablet/ob_tablet_meta.h"
#include "storage/tablet/ob_tablet.h"
#include "logservice/ob_log_base_header.h"

namespace oceanbase
{
using namespace storage;

namespace compaction
{

/*
 * ObParallelMergeInfo
 * */

void ObParallelMergeInfo::destroy()
{
  if (list_size_ > 0 && nullptr != parallel_end_key_list_ && nullptr != allocator_) {
    for (int i = 0; i < list_size_; ++i) {
      parallel_end_key_list_[i].destroy(*allocator_);
    }
    list_size_ = 0;
    allocator_->free(parallel_end_key_list_);
    parallel_end_key_list_ = nullptr;
    allocator_ = nullptr;
  }
  parallel_info_ = 0;
}

int ObParallelMergeInfo::serialize(char *buf, const int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(nullptr == buf || buf_len <= 0 || pos < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(buf), K(buf_len), K(pos));
  } else if (0 == list_size_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("no need to serialize parallel_merge_info", K(ret), K(list_size_));
  } else {
    LST_DO_CODE(OB_UNIS_ENCODE,
        parallel_info_);
    for (int i = 0; OB_SUCC(ret) && i < list_size_; ++i) {
      if (OB_FAIL(parallel_end_key_list_[i].serialize(buf, buf_len, pos))) {
        LOG_WARN("failed to encode concurrent cnt", K(ret), K(i), K(list_size_), K(parallel_end_key_list_[i]));
      }
    }
  }
  return ret;
}

int ObParallelMergeInfo::deserialize(
    common::ObIAllocator &allocator,
    const char *buf,
    const int64_t data_len,
    int64_t &pos)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(nullptr == buf || data_len <= 0 || pos < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(buf), K(data_len), K(pos));
  } else {
    LST_DO_CODE(OB_UNIS_DECODE, parallel_info_);
    if (OB_FAIL(ret)) {
    } else if (0 == list_size_) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("list size is invalid", K(ret), K(list_size_));
    } else {
      allocator_ = &allocator;
      void *alloc_buf = nullptr;
      if (OB_ISNULL(alloc_buf = allocator.alloc(sizeof(ObStoreRowkey) * list_size_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to alloc store rowkey array", K(ret), K(list_size_));
      } else {
        parallel_end_key_list_ = new(alloc_buf) ObStoreRowkey[list_size_];
      }
      for (int i = 0; OB_SUCC(ret) && i < list_size_; ++i) {
        if (OB_FAIL(parallel_end_key_list_[i].deserialize(allocator, buf, data_len, pos))) {
          LOG_WARN("failed to encode concurrent cnt", K(ret), K(i), K(list_size_), K(data_len), K(pos));
        }
      }
      if (OB_FAIL(ret)) {
        destroy(); // free parallel_end_key_list_ in destroy
      }
    }
  }
  return ret;
}

int64_t ObParallelMergeInfo::get_serialize_size() const
{
  int64_t len = 0;
  if (list_size_ > 0) {
    len += serialization::encoded_length_vi32(parallel_info_);
    for (int i = 0; i < list_size_; ++i) {
      len += parallel_end_key_list_[i].get_serialize_size();
    }
  }
  return len;
}

int ObParallelMergeInfo::generate_from_range_array(
    ObIAllocator &allocator,
    ObArrayArray<ObStoreRange> &paral_range)
{
  int ret = OB_SUCCESS;
  void *buf = nullptr;
  if (OB_UNLIKELY(0 != list_size_ || nullptr != parallel_end_key_list_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("parallel merge info is not empty", K(ret), KPC(this));
  } else {
    int64_t sum_range_cnt = 0;
    for (int64_t i = 0; i < paral_range.count(); ++i) {
      sum_range_cnt += paral_range.at(i).count();
    }
    if (sum_range_cnt <= VALID_CONCURRENT_CNT || sum_range_cnt > UINT8_MAX) {
      // do nothing
    } else if (FALSE_IT(list_size_ = sum_range_cnt - 1)) {
    } else if (OB_ISNULL(buf = allocator.alloc(sizeof(ObStoreRowkey) * list_size_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate", K(ret), K(paral_range));
    } else {
      allocator_ = &allocator;
      parallel_end_key_list_ = new(buf) ObStoreRowkey[list_size_];
      int64_t cnt = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < paral_range.count() && cnt < list_size_; ++i) {
        const ObIArray<ObStoreRange> &range_array = paral_range.at(i);
        for (int64_t j = 0; OB_SUCC(ret) && j < range_array.count() && cnt < list_size_; ++j) {
          if (OB_FAIL(range_array.at(j).get_end_key().deep_copy(parallel_end_key_list_[cnt++], allocator))) {
            LOG_WARN("failed to deep copy end key", K(ret), K(i), K(range_array), K(j), K(cnt));
          }
        }
      } // end of loop array
    }
  }
  LOG_DEBUG("parallel range info", K(ret), KPC(this), K(paral_range), K(paral_range.count()), K(paral_range.at(0)));

  if (OB_FAIL(ret)) {
    destroy();
  } else if (get_serialize_size() > MAX_PARALLEL_RANGE_SERIALIZE_LEN) {
    ret = OB_SIZE_OVERFLOW;
    LOG_DEBUG("parallel range info is too large to sync", K(ret), KPC(this));
    destroy();
  }
  return ret;
}

int ObParallelMergeInfo::init(
    common::ObIAllocator &allocator,
    const ObParallelMergeInfo &other)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!other.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("other parallel info is invalid", K(ret), K(other));
  } else {
    list_size_ = other.list_size_;
    allocator_ = &allocator;
    if (list_size_ > 0) {
      void *buf = nullptr;
      if (OB_ISNULL(buf = allocator.alloc(sizeof(ObStoreRowkey) * list_size_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to allocate", K(ret), K(other));
      } else {
        parallel_end_key_list_ = new (buf) ObStoreRowkey[list_size_];
        for (int i = 0; OB_SUCC(ret) && i < list_size_; ++i) {
          if (OB_FAIL(other.parallel_end_key_list_[i].deep_copy(parallel_end_key_list_[i], allocator))) {
            LOG_WARN("failed to deep copy end key", K(ret), K(i), K(other.parallel_end_key_list_[i]));
          }
        }
        if (OB_FAIL(ret)) {
          destroy();
        }
      } // else
    }
  }
  return ret;
}

int64_t ObParallelMergeInfo::to_string(char* buf, const int64_t buf_len) const
{
  int64_t pos = 0;
  if (OB_ISNULL(buf) || buf_len <= 0) {
  } else {
    J_OBJ_START();
    J_KV(K_(list_size));
    J_COMMA();
    for (int i = 0; i < list_size_; ++i) {
      J_KV(K(i), "key", parallel_end_key_list_[i]);
      J_COMMA();
    }
    J_OBJ_END();
  }
  return pos;
}

/*
 * ObMediumCompactionInfo
 * */
const char *ObMediumCompactionInfo::ObCompactionTypeStr[] = {
    "MEDIUM_COMPACTION",
    "MAJOR_COMPACTION",
};

const char *ObMediumCompactionInfo::get_compaction_type_str(enum ObCompactionType type)
{
  const char *str = "";
  if (type >= COMPACTION_TYPE_MAX || type < MEDIUM_COMPACTION) {
    str = "invalid_type";
  } else {
    str = ObCompactionTypeStr[type];
  }
  return str;
}

ObMediumCompactionInfo::ObMediumCompactionInfo()
  : ObIMultiSourceDataUnit(),
    medium_compat_version_(MEIDUM_COMPAT_VERSION),
    compaction_type_(COMPACTION_TYPE_MAX),
    contain_parallel_range_(false),
    medium_merge_reason_(ObAdaptiveMergePolicy::NONE),
    reserved_(0),
    cluster_id_(0),
    data_version_(0),
    medium_snapshot_(0),
    storage_schema_(),
    parallel_merge_info_()
{
  STATIC_ASSERT(static_cast<int64_t>(COMPACTION_TYPE_MAX) == ARRAYSIZEOF(ObCompactionTypeStr), "compaction type str len is mismatch");
}

ObMediumCompactionInfo::~ObMediumCompactionInfo()
{
  reset();
}

int ObMediumCompactionInfo::init(
    ObIAllocator &allocator,
    const ObMediumCompactionInfo &medium_info)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!medium_info.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(medium_info));
  } else if (OB_FAIL(storage_schema_.init(allocator, medium_info.storage_schema_))) {
    LOG_WARN("failed to init storage schema", K(ret), K(medium_info));
  } else if (OB_FAIL(parallel_merge_info_.init(allocator, medium_info.parallel_merge_info_))) {
    LOG_WARN("failed to init parallel merge info", K(ret), K(medium_info));
  } else {
    info_ = medium_info.info_;
    cluster_id_ = medium_info.cluster_id_;
    medium_snapshot_ = medium_info.medium_snapshot_;
    data_version_ = medium_info.data_version_;
  }
  return ret;
}

bool ObMediumCompactionInfo::is_valid() const
{
  return COMPACTION_TYPE_MAX != compaction_type_
      && medium_snapshot_ > 0
      && data_version_ > 0
      && storage_schema_.is_valid()
      && parallel_merge_info_.is_valid();
}

void ObMediumCompactionInfo::reset()
{
  info_ = 0;
  medium_compat_version_ = 0;
  compaction_type_ = COMPACTION_TYPE_MAX;
  cluster_id_ = 0;
  medium_snapshot_ = 0;
  data_version_ = 0;
  storage_schema_.reset();
  parallel_merge_info_.destroy();
}

int ObMediumCompactionInfo::deep_copy(const ObIMultiSourceDataUnit *src, ObIAllocator *allocator)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(nullptr == src || nullptr == allocator)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(src), KP(allocator));
  } else if (OB_UNLIKELY(memtable::MultiSourceDataUnitType::MEDIUM_COMPACTION_INFO != src->type())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), "type", src->type(), KP(allocator));
  } else {
    ret = init(*allocator, *static_cast<const ObMediumCompactionInfo *>(src));
  }
  return ret;
}

int ObMediumCompactionInfo::save_storage_schema(
    ObIAllocator &allocator,
    const storage::ObStorageSchema &storage_schema)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!storage_schema.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(storage_schema));
  } else if (OB_FAIL(storage_schema_.init(allocator, storage_schema))) {
    LOG_WARN("failed to init storage schema", K(ret), K(storage_schema));
  }
  return ret;
}

int ObMediumCompactionInfo::gene_parallel_info(
    ObIAllocator &allocator,
    ObArrayArray<ObStoreRange> &paral_range)
{
  int ret = OB_SUCCESS;
  contain_parallel_range_ = false;
  if (OB_FAIL(parallel_merge_info_.generate_from_range_array(allocator, paral_range))) {
    if (OB_UNLIKELY(OB_SIZE_OVERFLOW != ret)) {
      LOG_WARN("failed to generate parallel merge info", K(ret), K(paral_range));
    } else {
      ret = OB_SUCCESS;
    }
  } else if (parallel_merge_info_.list_size_ > 0) {
    contain_parallel_range_ = true;
    LOG_INFO("success to gene parallel info", K(ret), K(contain_parallel_range_), K(parallel_merge_info_));
  }
  return ret;
}

int ObMediumCompactionInfo::serialize(char *buf, const int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(nullptr == buf || buf_len <= 0 || pos < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(buf), K(buf_len), K(pos));
  } else {
    LST_DO_CODE(
        OB_UNIS_ENCODE,
        info_,
        cluster_id_,
        medium_snapshot_,
        data_version_,
        storage_schema_);
    if (contain_parallel_range_) {
      LST_DO_CODE(
          OB_UNIS_ENCODE,
          parallel_merge_info_);
    }
    LOG_DEBUG("ObMediumCompactionInfo::serialize", K(ret), K(buf), K(buf_len), K(pos));
  }
  return ret;
}

int ObMediumCompactionInfo::deserialize(
    common::ObIAllocator &allocator,
    const char *buf,
    const int64_t data_len,
    int64_t &pos)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(nullptr == buf || data_len <= 0 || pos < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(buf), K(data_len), K(pos));
  } else {
    LST_DO_CODE(OB_UNIS_DECODE,
        info_,
        cluster_id_,
        medium_snapshot_,
        data_version_);
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(storage_schema_.deserialize(allocator, buf, data_len, pos))) {
      LOG_WARN("failed to deserialize storage schema", K(ret));
    } else if (contain_parallel_range_) {
      if (OB_FAIL(parallel_merge_info_.deserialize(allocator, buf, data_len, pos))) {
        LOG_WARN("failed to deserialize parallel merge info", K(ret), K(buf), K(data_len), K(pos));
      }
    } else {
      clear_parallel_range();
      LOG_DEBUG("ObMediumCompactionInfo::deserialize", K(ret), K(buf), K(data_len), K(pos));
    }
  }
  return ret;
}

int64_t ObMediumCompactionInfo::get_serialize_size() const
{
  int64_t len = 0;
  LST_DO_CODE(
      OB_UNIS_ADD_LEN,
      info_,
      cluster_id_,
      medium_snapshot_,
      data_version_,
      storage_schema_);
  if (contain_parallel_range_) {
    LST_DO_CODE(OB_UNIS_ADD_LEN, parallel_merge_info_);
  }
  return len;
}

void ObMediumCompactionInfo::gene_info(
    char* buf, const int64_t buf_len, int64_t &pos) const
{
  J_KV("compaction_type", ObMediumCompactionInfo::get_compaction_type_str((ObCompactionType)compaction_type_),
      K(medium_snapshot_), K_(parallel_merge_info));
}

/*
 * ObTabletMediumCompactionInfoRecorder
 * */

ObTabletMediumCompactionInfoRecorder::ObTabletMediumCompactionInfoRecorder()
  : ObIStorageClogRecorder(),
    is_inited_(false),
    ignore_medium_(false),
    ls_id_(),
    tablet_id_(),
    tablet_handle_ptr_(nullptr),
    medium_info_(nullptr),
    allocator_(nullptr)
{
#if defined(__x86_64__)
  STATIC_ASSERT(sizeof(ObTabletMediumCompactionInfoRecorder) <= 96, "size of medium recorder is oversize");
#endif
}

ObTabletMediumCompactionInfoRecorder::~ObTabletMediumCompactionInfoRecorder()
{
  destroy();
}

void ObTabletMediumCompactionInfoRecorder::destroy()
{
  is_inited_ = false;
  ignore_medium_ = false;
  ObIStorageClogRecorder::destroy();
  free_allocated_info();
  ls_id_.reset();
  tablet_id_.reset();
}

void ObTabletMediumCompactionInfoRecorder::reset()
{
  if (is_inited_) {
    ObIStorageClogRecorder::reset();
  }
}

int ObTabletMediumCompactionInfoRecorder::init(
    const share::ObLSID &ls_id,
    const ObTabletID &tablet_id,
    const int64_t max_saved_version,
    logservice::ObLogHandler *log_handler)
{
  int ret = OB_SUCCESS;

  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret));
  } else if (OB_ISNULL(log_handler)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(log_handler));
  } else if (OB_FAIL(ObIStorageClogRecorder::init(max_saved_version, log_handler))) {
    LOG_WARN("failed to init ObIStorageClogRecorder", K(ret), K(log_handler));
  } else {
    ignore_medium_ = tablet_id.is_special_merge_tablet();
    ls_id_ = ls_id;
    tablet_id_ = tablet_id;
    is_inited_ = true;
    LOG_INFO("success to init", K(ret), K_(ls_id), K_(tablet_id), K(max_saved_version));
  }
  return ret;
}
// this func is protected by lock in reserved_snapshot_map
int ObTabletMediumCompactionInfoRecorder::submit_medium_compaction_info(
    ObMediumCompactionInfo &medium_info,
    ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  int64_t table_id = 0;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret));
  } else if (ignore_medium_) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("not support to submit medium compaction clog", K(ret), K_(tablet_id));
  } else if (OB_UNLIKELY(!medium_info.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(medium_info));
  } else if (FALSE_IT(medium_info_ = &medium_info)) {
  } else if (OB_FAIL(try_update_for_leader(medium_info.medium_snapshot_, &allocator))) {
    LOG_WARN("failed to update for leader", K(ret), K(medium_info));
  }
  medium_info_ = nullptr;
  if (OB_ALLOCATE_MEMORY_FAILED == ret || OB_BLOCK_FROZEN == ret) {
    ret = OB_EAGAIN;
  }
  return ret;
}

void ObTabletMediumCompactionInfoRecorder::free_allocated_info()
{
  if (OB_NOT_NULL(allocator_)) {
    if (OB_NOT_NULL(logcb_ptr_)) {
      tablet_handle_ptr_->reset();
      tablet_handle_ptr_->~ObTabletHandle();
      allocator_->free(logcb_ptr_);
      logcb_ptr_ = nullptr;
      tablet_handle_ptr_ = nullptr;
    }
    allocator_ = nullptr;
  }
}

int ObTabletMediumCompactionInfoRecorder::replay_medium_compaction_log(
    const share::SCN &scn,
    const char *buf,
    const int64_t size,
    int64_t &pos)
{
  int ret = OB_SUCCESS;
  int64_t update_version = OB_INVALID_VERSION;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret));
  } else if (ignore_medium_) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("not support to replay medium compaction clog", K(ret), K_(tablet_id));
  } else if (OB_FAIL(serialization::decode_i64(buf, size, pos, &update_version))) {
    LOG_WARN("fail to deserialize table_version", K(ret), K_(tablet_id));
  } else if (OB_FAIL(ObIStorageClogRecorder::replay_clog(update_version, scn, buf, size, pos))) {
    LOG_WARN("failed to replay clog", K(ret), K(scn), K_(tablet_id), K(update_version));
  }
  return ret;
}

int ObTabletMediumCompactionInfoRecorder::inner_replay_clog(
    const int64_t update_version,
    const share::SCN &scn,
    const char *buf,
    const int64_t size,
    int64_t &pos)
{
  UNUSED(update_version);
  int ret = OB_SUCCESS;
  ObArenaAllocator tmp_allocator;
  ObMediumCompactionInfo replay_medium_info;
  ObTabletHandle tmp_tablet_handle;
  if (OB_FAIL(replay_get_tablet_handle(ls_id_, tablet_id_, scn, tmp_tablet_handle))) {
    LOG_WARN("failed to get tablet handle", K(ret), K_(tablet_id), K(scn));
  } else if (OB_FAIL(replay_medium_info.deserialize(tmp_allocator, buf, size, pos))) {
    LOG_WARN("failed to deserialize medium compaction info", K(ret));
  } else if (!replay_medium_info.from_cur_cluster()
      && replay_medium_info.is_medium_compaction()) {
    // throw medium compaction clog from other cluster
  } else if (FALSE_IT(replay_medium_info.set_sync_finish(true))) {
  } else if (OB_FAIL(tmp_tablet_handle.get_obj()->save_multi_source_data_unit(&replay_medium_info,
      scn, true/*for replay*/, memtable::MemtableRefOp::NONE))) {
    LOG_WARN("failed to save medium info", K(ret), K_(tablet_id), K(replay_medium_info));
  } else {
    tmp_tablet_handle.reset();
    FLOG_INFO("success to save medium info", K(ret), K_(tablet_id), K(replay_medium_info), K(max_saved_version_));
  }
  return ret;
}

int ObTabletMediumCompactionInfoRecorder::sync_clog_succ_for_leader(const int64_t update_version)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(medium_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("medium info is invalid", K(ret), K_(clog_scn), KP_(medium_info));
  } else if (OB_UNLIKELY(medium_info_->medium_snapshot_ != update_version)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("medium snapshot not match", K(ret), KPC(medium_info_), K(update_version));
  } else if (OB_FAIL(dec_ref_on_memtable(true/*sync_finish*/))) {
    LOG_WARN("failed to dec ref on memtable", K(ret), K_(tablet_id), KPC(medium_info_));
  } else {
    FLOG_INFO("success to save medium info", K(ret), K_(tablet_id), KPC(medium_info_),
        K(max_saved_version_), K_(clog_scn));
  }
  return ret;
}

void ObTabletMediumCompactionInfoRecorder::sync_clog_failed_for_leader()
{
  dec_ref_on_memtable(false/*sync_finish*/);
}

int ObTabletMediumCompactionInfoRecorder::dec_ref_on_memtable(const bool sync_finish)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(nullptr == medium_info_
      || nullptr == tablet_handle_ptr_
      || !tablet_handle_ptr_->is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("medium info or tablet handle is unexpected null", K(ret), K_(ls_id), K_(tablet_id),
        KP_(medium_info), K_(tablet_handle_ptr));
  } else {
    medium_info_->set_sync_finish(sync_finish);
    if (OB_FAIL(tablet_handle_ptr_->get_obj()->save_multi_source_data_unit(medium_info_, clog_scn_,
        false/*for_replay*/, memtable::MemtableRefOp::DEC_REF, true/*is_callback*/))) {
      LOG_WARN("failed to save medium info", K(ret), K_(tablet_id), K(medium_info_));
    }
  }
  return ret;
}

// log_header + tablet_id + medium_snapshot + medium_compaction_info
int ObTabletMediumCompactionInfoRecorder::prepare_struct_in_lock(
    int64_t &update_version,
    ObIAllocator *allocator,
    char *&clog_buf,
    int64_t &clog_len)
{
  UNUSED(update_version);
  int ret = OB_SUCCESS;
  clog_buf = nullptr;
  clog_len = 0;
  const logservice::ObLogBaseHeader log_header(
      logservice::ObLogBaseType::MEDIUM_COMPACTION_LOG_BASE_TYPE,
      logservice::ObReplayBarrierType::PRE_BARRIER);

  int64_t pos = 0;
  char *buf = nullptr;
  char *alloc_clog_buf = nullptr;
  int64_t alloc_buf_offset = 0;
  const int64_t buf_len = log_header.get_serialize_size()
      + tablet_id_.get_serialize_size()
      + serialization::encoded_length_i64(medium_info_->medium_snapshot_)
      + medium_info_->get_serialize_size();
  const int64_t alloc_buf_size = buf_len + sizeof(ObTabletHandle) + sizeof(ObStorageCLogCb);

  if (OB_UNLIKELY(nullptr == medium_info_ || nullptr == allocator)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("medium_info or allocator is unexpected null", K(ret), KP_(medium_info), KP(allocator));
  } else if (buf_len >= common::OB_MAX_LOG_ALLOWED_SIZE) { // need be separated into several clogs
    ret = OB_ERR_DATA_TOO_LONG;
    LOG_WARN("medium info log too long", K(buf_len), LITERAL_K(common::OB_MAX_LOG_ALLOWED_SIZE));
  } else if (FALSE_IT(allocator_ = allocator)) {
  } else if (OB_ISNULL(buf = static_cast<char *>(allocator_->alloc(alloc_buf_size)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("allocate memory failed", K(ret), KPC(medium_info_));
  } else {
    logcb_ptr_ = new(buf) ObStorageCLogCb(*this);
    alloc_buf_offset += sizeof(ObStorageCLogCb);
    tablet_handle_ptr_ = new (buf + alloc_buf_offset) ObTabletHandle();
    alloc_buf_offset += sizeof(ObTabletHandle);
    alloc_clog_buf = static_cast<char*>(buf) + alloc_buf_offset;
  }

  if (FAILEDx(get_tablet_handle(ls_id_, tablet_id_, *tablet_handle_ptr_))) {
    LOG_WARN("failed to get tablet handle", K(ret), K_(ls_id), K_(tablet_id));
  } else if (OB_FAIL(log_header.serialize(alloc_clog_buf, buf_len, pos))) {
    LOG_WARN("failed to serialize log header", K(ret));
  } else if (OB_FAIL(tablet_id_.serialize(alloc_clog_buf, buf_len, pos))) {
    LOG_WARN("fail to serialize tablet_id", K(ret), K_(tablet_id));
  } else if (OB_FAIL(serialization::encode_i64(alloc_clog_buf, buf_len, pos, medium_info_->medium_snapshot_))) {
    LOG_WARN("fail to serialize schema version", K(ret), K_(tablet_id));
  } else if (OB_FAIL(medium_info_->serialize(alloc_clog_buf, buf_len, pos))) {
    LOG_WARN("failed to serialize medium compaction info", K(ret), K(buf_len), K_(tablet_id), KPC(medium_info_));
  }
  if (OB_SUCC(ret)) {
    clog_buf = alloc_clog_buf;
    clog_len = pos;
  } else if (nullptr != buf && nullptr != allocator_) {
    free_allocated_info();
  }
  return ret;
}

int ObTabletMediumCompactionInfoRecorder::submit_log(
    const int64_t update_version,
    const char *clog_buf,
    const int64_t clog_len)
{
  UNUSED(update_version);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(nullptr == medium_info_
      || nullptr == tablet_handle_ptr_
      || !tablet_handle_ptr_->is_valid()
      || nullptr == clog_buf
      || clog_len <= 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("log handler or medium info is null", K(ret), KP(medium_info_),
        KP(clog_buf), K(clog_len), K(tablet_handle_ptr_));
  } else if (FALSE_IT(medium_info_->set_sync_finish(false))) {
  } else if (OB_FAIL(tablet_handle_ptr_->get_obj()->save_multi_source_data_unit(
      medium_info_, share::SCN::max_scn(),
      false/*for_replay*/, memtable::MemtableRefOp::INC_REF))) {
    LOG_WARN("failed to save medium info", K_(tablet_id), KPC(medium_info_));
  } else if (OB_FAIL(write_clog(clog_buf, clog_len))) {
    LOG_WARN("fail to submit log", K(ret), K_(tablet_id), K(medium_info_));
    int tmp_ret = OB_SUCCESS;
    if (OB_TMP_FAIL(dec_ref_on_memtable(false))) {
      LOG_ERROR("failed to dec ref on memtable", K(tmp_ret), K_(ls_id), K_(tablet_id));
    }
  } else {
    LOG_INFO("success to submit medium log", K(ret), K_(tablet_id), K(medium_info_), K_(clog_scn),
        "max_saved_version", get_max_saved_version());
  }

  return ret;
}

/*
 * ObMediumCompactionInfoList
 * */

const int64_t ObMediumCompactionInfoList::MAX_SERIALIZE_SIZE;

ObMediumCompactionInfoList::ObMediumCompactionInfoList()
  : is_inited_(false),
    allocator_(nullptr),
    compat_(MEDIUM_LIST_VERSION),
    last_compaction_type_(0),
    reserved_(0),
    wait_check_medium_scn_(0)
{
}

ObMediumCompactionInfoList::~ObMediumCompactionInfoList()
{
  reset();
}

int ObMediumCompactionInfoList::init(common::ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret));
  } else {
    compat_ = MEDIUM_LIST_VERSION;
    allocator_ = &allocator;
    is_inited_ = true;
  }
  return ret;
}

// MINI: dump_list is from memtable
// finish_medium_scn = last_major_scn
// init_by_ha = true: need force set wait_check = finish_scn
// if wait_check=0 after restore, report_scn don't will be updated by leader
int ObMediumCompactionInfoList::init(common::ObIAllocator &allocator,
    const ObMediumCompactionInfoList *old_list,
    const ObMediumCompactionInfoList *dump_list,
    const int64_t finish_medium_scn/*= 0*/,
    const bool update_in_major_type_merge/*= false*/)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret));
  } else if (FALSE_IT(allocator_ = &allocator)) {
  } else if (nullptr != old_list && OB_FAIL(append_list_with_deep_copy(finish_medium_scn, update_in_major_type_merge, *old_list))) {
    LOG_WARN("failed to deep copy list", K(ret), K(old_list));
  } else if (nullptr != dump_list && OB_FAIL(append_list_with_deep_copy(finish_medium_scn, update_in_major_type_merge, *dump_list))) {
    LOG_WARN("failed to deep copy list", K(ret), K(dump_list));
  } else {
    // if update_in_major_type_merge = true, will update wait_check_medium_scn in delete_medium_compaction_info
    if (!update_in_major_type_merge && nullptr != old_list) {
      last_compaction_type_ = old_list->last_compaction_type_;
      wait_check_medium_scn_ = old_list->get_wait_check_medium_scn();
    }
  }
  if (OB_SUCC(ret)) {
    compat_ = MEDIUM_LIST_VERSION;
    is_inited_ = true;
    if (medium_info_list_.get_size() > 0 || wait_check_medium_scn_ > 0) {
      LOG_INFO("success to init list", K(ret), KPC(this), KPC(old_list));
    }
  } else if (OB_UNLIKELY(!is_inited_)) {
    reset();
  }
  return ret;
}

int ObMediumCompactionInfoList::init_after_check_finish(
    ObIAllocator &allocator,
    const ObMediumCompactionInfoList &old_list) // list from old_tablet
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret));
  } else if (OB_UNLIKELY(!old_list.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(old_list));
  } else if (FALSE_IT(allocator_ = &allocator)) {
  } else if (OB_FAIL(append_list_with_deep_copy(wait_check_medium_scn_, false, old_list))) {
    LOG_WARN("failed to deep copy list", K(ret), K(wait_check_medium_scn_));
  } else {
    last_compaction_type_ = old_list.last_compaction_type_;
    wait_check_medium_scn_ = 0; // update after check finished, should reset wait_check_medium_scn
    compat_ = MEDIUM_LIST_VERSION;
    is_inited_ = true;
    LOG_INFO("success to init list", K(ret), KPC(this), K(old_list));
  }
  if (OB_UNLIKELY(!is_inited_)) {
    reset();
  }
  return ret;
}


void ObMediumCompactionInfoList::reset_list()
{
  DLIST_REMOVE_ALL_NORET(info, medium_info_list_) {
    static_cast<ObMediumCompactionInfo *>(info)->~ObMediumCompactionInfo();
    allocator_->free(info);
  }
  medium_info_list_.reset();
}

void ObMediumCompactionInfoList::reset()
{
  if (OB_NOT_NULL(allocator_)) {
    reset_list();
    medium_info_list_.reset();
  }
  is_inited_ = false;
  info_ = 0;
  wait_check_medium_scn_ = 0;
  allocator_ = nullptr;
}

int ObMediumCompactionInfoList::add_medium_compaction_info(const ObMediumCompactionInfo &input_info)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("list is not init", K(ret));
  } else if (OB_FAIL(inner_deep_copy_node(input_info))) {
    LOG_WARN("failed to init medium info", K(ret), K(input_info));
  }
  return ret;
}

int ObMediumCompactionInfoList::get_specified_scn_info(
    const int64_t snapshot,
    const ObMediumCompactionInfo *&ret_info) const
{
  ret_info = nullptr;
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("list is not init", K(ret));
  } else if (OB_UNLIKELY(snapshot <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(snapshot));
  } else if (snapshot <= get_max_medium_snapshot()) {
    const ObMediumCompactionInfo *cur_info = nullptr;
    DLIST_FOREACH_X(info, medium_info_list_, OB_SUCC(ret)) {
      cur_info = static_cast<const ObMediumCompactionInfo *>(info);
      if (OB_UNLIKELY(!info->is_valid())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_ERROR("invalid medium info", K(ret), KPC(info));
      } else if (snapshot == cur_info->medium_snapshot_) {
        ret_info = cur_info;
        break;
      }
    }
  }
  if (OB_SUCC(ret) && nullptr == ret_info) {
    ret = OB_ENTRY_NOT_EXIST;
  }
  return ret;
}

int64_t ObMediumCompactionInfoList::get_schedule_scn(const int64_t major_compaction_scn) const
{
  int64_t ret_scn = 0;
  if (size() > 0) {
    const ObMediumCompactionInfo *first_medium_info = get_first_medium_info();
    if (first_medium_info->is_medium_compaction()
        || (first_medium_info->is_major_compaction() && major_compaction_scn >= first_medium_info->medium_snapshot_)) {
      // for standby cluster, receive several medium info, only schedule what scheduler have received
      ret_scn = first_medium_info->medium_snapshot_;
    }
  }
  return ret_scn;
}

int ObMediumCompactionInfoList::inner_deep_copy_node(
    const ObMediumCompactionInfo &input_info)
{
  int ret = OB_SUCCESS;
  ObMediumCompactionInfo *new_info = nullptr;
  void *alloc_buf = nullptr;

  if (get_max_medium_snapshot() >= input_info.medium_snapshot_) {
    // do nothing
  } else if (OB_ISNULL(alloc_buf = allocator_->alloc(sizeof(ObMediumCompactionInfo)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to alloc memory", K(ret));
  } else if (FALSE_IT(new_info = new (alloc_buf) ObMediumCompactionInfo())) {
  } else if (OB_FAIL(new_info->init(*allocator_, input_info))) {
    LOG_WARN("failed to init medium info", K(ret), K(input_info));
  } else if (OB_UNLIKELY(!medium_info_list_.add_last(new_info))) {
    ret = OB_ERR_SYS;
    LOG_WARN("failed to add into medium info list", K(ret), KPC(new_info));
  } else if (OB_UNLIKELY(!inner_is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("medium info list is invalid", K(ret), KPC(this));
  } else {
    LOG_INFO("success to deep copy append medium info", K(ret), KPC(new_info));
  }

  if (OB_FAIL(ret) && nullptr != new_info) {
    new_info->~ObMediumCompactionInfo();
    allocator_->free(new_info);
    new_info = nullptr;
  }
  return ret;
}

int ObMediumCompactionInfoList::serialize(char *buf, const int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;

  int64_t new_pos = pos;
  if (OB_UNLIKELY(nullptr == buf || buf_len <= 0 || pos < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(buf), K(buf_len), K(pos));
  } else if (OB_UNLIKELY(!is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("medium info list is invalid", K(ret), KPC(this));
  } else if (OB_FAIL(serialization::encode_vi64(buf, buf_len, new_pos, info_))) {
    STORAGE_LOG(WARN, "failed to serialize info", K(ret), K(buf_len), K(pos));
  } else if (OB_FAIL(serialization::encode_vi64(buf, buf_len, new_pos, wait_check_medium_scn_))) {
    STORAGE_LOG(WARN, "failed to serialize wait_check_medium_scn", K(ret), K(buf_len), K(pos));
  } else if (OB_FAIL(serialization::encode_vi64(buf, buf_len, new_pos, medium_info_list_.get_size()))) {
    LOG_WARN("failed to serialize medium status", K(ret), K(buf_len));
  } else {
    DLIST_FOREACH_X(info, medium_info_list_, OB_SUCC(ret)) {
      if (OB_FAIL(static_cast<const ObMediumCompactionInfo *>(info)->serialize(buf, buf_len, new_pos))) {
        LOG_WARN("failed to serialize medium compaction info", K(ret), K(buf), K(buf_len), K(new_pos), KPC(info));
      } else {
        LOG_DEBUG("success to serialize medium info", K(ret), KPC(info));
      }
    }
  }
  if (OB_SUCC(ret)) {
    pos = new_pos;
  }
  return ret;
}

int ObMediumCompactionInfoList::deserialize(
    common::ObIAllocator &allocator,
    const char *buf,
    const int64_t data_len,
    int64_t &pos)
{
  int ret = OB_SUCCESS;
  int64_t new_pos = pos;
  int64_t deserialize_info = 0;
  int64_t list_count = 0;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(nullptr == buf || data_len <= 0 || pos < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(buf), K(data_len), K(pos));
  } else if (OB_FAIL(serialization::decode_vi64(buf, data_len, new_pos, &deserialize_info))) {
    LOG_WARN("failed to deserialize cur medium snapshot", K(ret), K(data_len));
  } else if (0 == deserialize_info) {
    if (OB_FAIL(serialization::decode_vi64(buf, data_len, new_pos, &list_count))) {
      LOG_WARN("failed to deserialize list count", K(ret), K(data_len));
    } else if (OB_UNLIKELY(0 != list_count)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("list count should be zero in old version medium list", K(ret), K(list_count));
    }
  } else if (FALSE_IT(info_ = deserialize_info)) {
  } else if (OB_FAIL(serialization::decode_vi64(buf, data_len, new_pos, &wait_check_medium_scn_))) {
    LOG_WARN("failed to deserialize wait_check_medium_scn", K(ret), K(data_len));
  } else if (OB_FAIL(serialization::decode_vi64(buf, data_len, new_pos, &list_count))) {
    LOG_WARN("failed to deserialize list count", K(ret), K(data_len));
  } else if (OB_UNLIKELY(list_count < 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected list count", K(ret), K(list_count));
  } else if (list_count > 0) {
    void *alloc_buf = nullptr;
    ObMediumCompactionInfo *new_info = nullptr;
    for (int i = 0; OB_SUCC(ret) && i < list_count; ++i) {
      if (OB_ISNULL(alloc_buf = allocator.alloc(sizeof(ObMediumCompactionInfo)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to alloc memory", K(ret));
      } else if (FALSE_IT(new_info = new (alloc_buf) ObMediumCompactionInfo())) {
      } else if (OB_FAIL(new_info->deserialize(allocator, buf, data_len, new_pos))) {
        LOG_WARN("failed to deserialize medium info", K(ret));
      } else if (!medium_info_list_.add_last(new_info)) {
        ret = OB_ERR_SYS;
        LOG_WARN("failed to add into medium info list", K(ret), KPC(new_info));
      } else {
        LOG_DEBUG("success to deserialize medium info", K(ret), K(new_info));
      }

      if (OB_FAIL(ret) && nullptr != new_info) {
        new_info->~ObMediumCompactionInfo();
        allocator.free(new_info);
        new_info = nullptr;
      }
    } // end of for
  }
  if (OB_FAIL(ret)) {
  } else if (OB_UNLIKELY(!inner_is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("medium info list is invalid", K(ret), KPC(this));
  } else {
    allocator_ = &allocator;
    compat_ = MEDIUM_LIST_VERSION;
    is_inited_ = true;
    pos = new_pos;
  }
  return ret;
}

int64_t ObMediumCompactionInfoList::get_serialize_size() const
{
  int64_t len = 0;
  len += serialization::encoded_length_vi64(info_);
  len += serialization::encoded_length_vi64(wait_check_medium_scn_);
  len += serialization::encoded_length_vi64(medium_info_list_.get_size());
  DLIST_FOREACH_NORET(info, medium_info_list_){
    len += static_cast<const ObMediumCompactionInfo *>(info)->get_serialize_size();
  }
  return len;
}

void ObMediumCompactionInfoList::gene_info(
    char* buf, const int64_t buf_len, int64_t &pos) const
{
  if (OB_ISNULL(buf) || buf_len <= 0) {
    // do nothing
  } else {
    J_OBJ_START();
    J_KV("size", size(), K_(info), K_(wait_check_medium_scn));
    J_COMMA();
    BUF_PRINTF("info_list");
    J_COLON();
    J_OBJ_START();
    if (size() > 0) {
      int i = 0;
      DLIST_FOREACH_NORET(info, medium_info_list_) {
        BUF_PRINTF("[%d]:", i++);
        static_cast<const ObMediumCompactionInfo *>(info)->gene_info(buf, buf_len, pos);
        if (i != size()) {
          BUF_PRINTF(";");
        }
      }
    } else {
      J_EMPTY_OBJ();
    }
    J_OBJ_END();
    J_OBJ_END();
  }
}

} //namespace compaction
} // namespace oceanbase
