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

#define USING_LOG_PREFIX RS

#include "rootserver/ddl_task/ob_ddl_task.h"
#include "lib/allocator/page_arena.h"
#include "lib/mysqlclient/ob_mysql_result.h"
#include "lib/mysqlclient/ob_mysql_proxy.h"
#include "share/ob_define.h"
#include "share/ob_srv_rpc_proxy.h"
#include "share/ob_debug_sync.h"
#include "share/ob_common_rpc_proxy.h"
#include "share/location_cache/ob_location_struct.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_struct.h"
#include "share/schema/ob_part_mgr_util.h"
#include "share/ob_common_rpc_proxy.h"
#include "share/config/ob_server_config.h"
#include "share/ob_index_builder_util.h"
#include "share/ob_thread_mgr.h"
#include "share/ob_ddl_checksum.h"
#include "share/ob_ddl_error_message_table_operator.h"
#include "share/ob_max_id_fetcher.h"
#include "share/ob_freeze_info_proxy.h"
#include "share/scheduler/ob_sys_task_stat.h"
#include "rootserver/ob_server_manager.h"
#include "rootserver/ob_zone_manager.h"
#include "rootserver/ob_ddl_service.h"
#include "rootserver/ob_root_service.h"
#include "rootserver/ob_snapshot_info_manager.h"
#include "storage/tx/ob_ts_mgr.h"
#include "observer/ob_server_struct.h"

namespace oceanbase
{
using namespace common;
using namespace common::sqlclient;
using namespace obrpc;
using namespace share;
using namespace share::schema;
using namespace sql;

namespace rootserver
{

ObDDLTaskKey::ObDDLTaskKey()
  : object_id_(OB_INVALID_ID), schema_version_(0)
{
}

ObDDLTaskKey::ObDDLTaskKey(const int64_t object_id, const int64_t schema_version)
  : object_id_(object_id), schema_version_(schema_version)
{
}

uint64_t ObDDLTaskKey::hash() const
{
  uint64_t hash_val = murmurhash(&object_id_, sizeof(object_id_), 0);
  hash_val = murmurhash(&schema_version_, sizeof(schema_version_), hash_val);
  return hash_val;
}

bool ObDDLTaskKey::operator==(const ObDDLTaskKey &other) const
{
  return object_id_ == other.object_id_ && schema_version_ == other.schema_version_;
}

int ObDDLTaskKey::assign(const ObDDLTaskKey &other)
{
  int ret = OB_SUCCESS;
  object_id_ = other.object_id_;
  schema_version_ = other.schema_version_;
  return ret;
}

ObCreateDDLTaskParam::ObCreateDDLTaskParam()
  : tenant_id_(OB_INVALID_ID), object_id_(OB_INVALID_ID), schema_version_(0), parallelism_(0), parent_task_id_(0),
    type_(DDL_INVALID), src_table_schema_(nullptr), dest_table_schema_(nullptr), ddl_arg_(nullptr), allocator_(nullptr)
{
}

ObCreateDDLTaskParam::ObCreateDDLTaskParam(const uint64_t tenant_id,
                                           const share::ObDDLType &type,
                                           const ObTableSchema *src_table_schema,
                                           const ObTableSchema *dest_table_schema,
                                           const int64_t object_id,
                                           const int64_t schema_version,
                                           const int64_t parallelism,
                                           ObIAllocator *allocator,
                                           const obrpc::ObDDLArg *ddl_arg,
                                           const int64_t parent_task_id)
  : tenant_id_(tenant_id), object_id_(object_id), schema_version_(schema_version), parallelism_(parallelism),
    parent_task_id_(parent_task_id), type_(type), src_table_schema_(src_table_schema), dest_table_schema_(dest_table_schema),
    ddl_arg_(ddl_arg), allocator_(allocator)
{
}

int ObDDLTask::get_ddl_type_str(const int64_t ddl_type, const char *&ddl_type_str)
{
  int ret = OB_SUCCESS;
  switch (ddl_type) {
    case DDL_CREATE_INDEX:
      ddl_type_str =  "create index";
      break;
    case DDL_MODIFY_COLUMN:
      ddl_type_str = "modify column";
      break;
    case DDL_CHECK_CONSTRAINT:
      ddl_type_str = "add or modify check constraint";
      break;
    case DDL_FOREIGN_KEY_CONSTRAINT:
      ddl_type_str = "add or modify foreign key";
      break;
    case DDL_ADD_PRIMARY_KEY:
      ddl_type_str = "add primary key";
      break;
    case DDL_DROP_PRIMARY_KEY:
      ddl_type_str = "drop primary key";
      break;
    case DDL_ALTER_PRIMARY_KEY:
      ddl_type_str = "alter primary key";
      break;
    case DDL_ALTER_PARTITION_BY:
      ddl_type_str = "alter partition by";
      break;
    case DDL_DROP_COLUMN:
      ddl_type_str = "drop column";
      break;
    case DDL_NORMAL_TYPE:
      ddl_type_str = "normal ddl";
      break;
    case DDL_ADD_NOT_NULL_COLUMN:
      ddl_type_str = "add not null column";
      break;
    case DDL_CONVERT_TO_CHARACTER:
      ddl_type_str = "convert to character";
      break;
    case DDL_ADD_COLUMN_ONLINE:
      ddl_type_str = "add column online";
      break;
    case DDL_ADD_COLUMN_OFFLINE:
      ddl_type_str = "add column offline";
      break;
    case DDL_COLUMN_REDEFINITION:
      ddl_type_str = "column redefinition";
      break;
    case DDL_TABLE_REDEFINITION:
      ddl_type_str = "table redefinition";
      break;
    case DDL_MODIFY_AUTO_INCREMENT:
      ddl_type_str = "modify auto increment";
      break;
    case DDL_DROP_DATABASE:
      ddl_type_str = "drop database";
      break;
    case DDL_DROP_TABLE:
      ddl_type_str = "drop table";
      break;
    case DDL_TRUNCATE_TABLE:
      ddl_type_str = "truncate table";
      break;
    case DDL_DROP_PARTITION:
      ddl_type_str = "drop table";
      break;
    case DDL_DROP_SUB_PARTITION:
      ddl_type_str = "drop sub partition";
      break;
    case DDL_TRUNCATE_PARTITION:
      ddl_type_str = "truncate partition";
      break;
    case DDL_TRUNCATE_SUB_PARTITION:
      ddl_type_str = "truncate sub partition";
      break;
    case DDL_DROP_INDEX:
      ddl_type_str = "drop index";
      break;
    default:
      ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int ObDDLTask::deep_copy_table_arg(common::ObIAllocator &allocator, const ObDDLArg &source_arg, ObDDLArg &dest_arg)
{
  int ret = OB_SUCCESS;
  const int64_t serialize_size = source_arg.get_serialize_size();
  char *buf = nullptr;
  int64_t pos = 0;
  if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(serialize_size)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("allocate memory failed", K(ret), K(serialize_size));
  } else if (OB_FAIL(source_arg.serialize(buf, serialize_size, pos))) {
    LOG_WARN("serialize alter table arg", K(ret));
  } else if (FALSE_IT(pos = 0)) {
  } else if (OB_FAIL(dest_arg.deserialize(buf, serialize_size, pos))) {
    LOG_WARN("deserialize alter table arg failed", K(ret));
  }
  if (OB_FAIL(ret) && nullptr != buf) {
    allocator.free(buf);
  }
  return ret;
}

int ObDDLTask::fetch_new_task_id(ObMySQLProxy &sql_proxy, int64_t &new_task_id)
{
  int ret = OB_SUCCESS;
  uint64_t tmp_task_id = OB_INVALID_ID;
  ObMaxIdFetcher id_fetcher(sql_proxy);
  if (OB_FAIL(id_fetcher.fetch_new_max_id(OB_SYS_TENANT_ID,
          OB_MAX_USED_DDL_TASK_ID_TYPE, tmp_task_id, 1L/*ddl start id*/))) {
    LOG_WARN("fetch_new_max_id failed", K(ret), "id_type", OB_MAX_USED_DDL_TASK_ID_TYPE);
  } else {
    new_task_id = tmp_task_id;
  }
  return ret;
}

int ObDDLTask::set_ddl_stmt_str(const ObString &ddl_stmt_str)
{
  int ret = OB_SUCCESS;
  if (!ddl_stmt_str.empty()) {
    char *buf = nullptr;
    if (OB_ISNULL(buf = static_cast<char *>(allocator_.alloc(ddl_stmt_str.length())))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("allocate memory failed", K(ret), "request_size", ddl_stmt_str.length(), K(ddl_stmt_str));
    } else {
      MEMCPY(buf, ddl_stmt_str.ptr(), ddl_stmt_str.length());
      ddl_stmt_str_.assign(buf, ddl_stmt_str.length());
    }
  }
  return ret;
}

int ObDDLTask::convert_to_record(
    ObDDLTaskRecord &task_record,
    common::ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  const int64_t serialize_param_size = get_serialize_param_size();
  int64_t pos = 0;
  task_record.tenant_id_ = get_tenant_id();
  task_record.object_id_ = get_object_id();
  task_record.target_object_id_ = get_target_object_id();
  task_record.schema_version_ = get_schema_version();
  task_record.ddl_type_ = get_task_type();
  task_record.trace_id_ = get_trace_id();
  task_record.task_status_ = get_task_status();
  task_record.task_id_ = get_task_id();
  task_record.parent_task_id_ = get_parent_task_id();
  task_record.task_version_ = get_task_version();
  task_record.execution_id_ = get_execution_id();
  task_record.ret_code_ = get_ret_code();
  const ObString &ddl_stmt_str = get_ddl_stmt_str();
  if (serialize_param_size > 0) {
    char *buf = nullptr;
    if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(serialize_param_size)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("allocate memory failed", K(ret), K(serialize_param_size));
    } else if (OB_FAIL(serialize_params_to_message(buf, serialize_param_size, pos))) {
      LOG_WARN("serialize params to message failed", K(ret));
    } else {
      task_record.message_.assign(buf, serialize_param_size);
    }
  }
  if (OB_SUCC(ret) && ddl_stmt_str.length() > 0) {
    char *buf = nullptr;
    if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(ddl_stmt_str.length())))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("allocate memory failed", K(ret), "request_size", ddl_stmt_str.length(), K(ddl_stmt_str));
    } else {
      MEMCPY(buf, ddl_stmt_str.ptr(), ddl_stmt_str.length());
      task_record.ddl_stmt_str_.assign(buf, ddl_stmt_str.length());
    }
  }
  return ret;
}

int ObDDLTask::switch_status(ObDDLTaskStatus new_status, const int ret_code)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  bool is_cancel = false;
  int real_ret_code = ret_code;
  bool is_tenant_dropped = false;
  const ObDDLTaskStatus old_status = task_status_;
  if (OB_TMP_FAIL(SYS_TASK_STATUS_MGR.is_task_cancel(trace_id_, is_cancel))) {
    LOG_WARN("check task is canceled", K(tmp_ret), K(trace_id_));
  } else if (is_cancel) {
    real_ret_code = (OB_SUCCESS == ret_code || is_error_need_retry(ret_code)) ? OB_CANCELED : ret_code;
  } else if (SUCCESS == old_status || (OB_SUCCESS != ret_code && is_error_need_retry(ret_code))) {
    LOG_INFO("error code found, but execute again", K(ret_code), K(ret_code_), K(old_status), K(new_status), K(err_code_occurence_cnt_));
    ret_code_ = OB_SUCCESS;
    new_status = old_status;
    real_ret_code = OB_SUCCESS;
  }
  ret_code_ = OB_SUCCESS == ret_code_ ? real_ret_code : ret_code_;
  ObDDLTaskStatus real_new_status = ret_code_ != OB_SUCCESS ? FAIL : new_status;
  ObMySQLTransaction trans;
  ObRootService *root_service = nullptr;
  if (OB_ISNULL(root_service = GCTX.root_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("error unexpected, root service must not be nullptr", K(ret));
  } else if (OB_FAIL(root_service->get_schema_service().check_if_tenant_has_been_dropped(tenant_id_, is_tenant_dropped))) {
    LOG_WARN("check if tenant has been dropped failed", K(ret), K(tenant_id_));
  } else if (is_tenant_dropped) {
    need_retry_ = false;
    LOG_INFO("tenant has been dropped, exit anyway", K(ret), K(tenant_id_));
  } else if (OB_FAIL(trans.start(&root_service->get_sql_proxy(), tenant_id_))) {
    LOG_WARN("start transaction failed", K(ret));
  } else {
    int64_t table_task_status = 0;
    int64_t execution_id = 0;
    if (OB_FAIL(ObDDLTaskRecordOperator::select_for_update(trans, tenant_id_, task_id_, table_task_status, execution_id))) {
      LOG_WARN("select for update failed", K(ret), K(task_id_));
    } else if (old_status != task_status_) {
      ret = OB_EAGAIN;
      LOG_WARN("task status has changed", K(ret));
    } else if (table_task_status == FAIL && old_status != table_task_status) {
      // task failed marked by user
      real_new_status = FAIL;
      ret_code_ = OB_CANCELED;
    } else if (table_task_status == SUCCESS && old_status != table_task_status) {
      real_new_status = SUCCESS;
    } else if (old_status == new_status) {
      // do nothing.
    } else if (OB_FAIL(ObDDLTaskRecordOperator::update_task_status(
            trans, tenant_id_, task_id_, static_cast<int64_t>(real_new_status)))) {
      LOG_WARN("update task status failed", K(ret), K(task_id_), K(new_status));
    } else if (OB_FAIL(ObDDLTaskRecordOperator::update_ret_code(trans, tenant_id_, task_id_, ret_code_))) {
      LOG_WARN("failed to update ret code", K(ret));
    }

    bool commit = (OB_SUCCESS == ret);
    int tmp_ret = trans.end(commit);
    if (OB_SUCCESS != tmp_ret) {
      ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
    }
    if (OB_SUCC(ret) && old_status != real_new_status) {
      ROOTSERVICE_EVENT_ADD("ddl_scheduler", "switch_state", K_(tenant_id), K_(task_id), K_(object_id), K_(target_object_id),
          "new_state", real_new_status, K_(snapshot_version), ret_code_);
      task_status_ = real_new_status;
    }
  }
  return ret;
}

int ObDDLTask::refresh_status()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(switch_status(task_status_, OB_SUCCESS))) {
    LOG_WARN("refresh status failed", K(ret));
  }
  return ret;
}

int ObDDLTask::refresh_schema_version()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLTask has not been inited", K(ret));
  } else if (schema_version_ > 0 && schema_version_ != UINT64_MAX) {
    ObMultiVersionSchemaService &schema_service = ObMultiVersionSchemaService::get_instance();
    int64_t refreshed_schema_version = 0;
    if (OB_FAIL(schema_service.async_refresh_schema(tenant_id_, schema_version_))) {
      LOG_WARN("async refresh schema version failed", K(ret), K(tenant_id_), K(schema_version_));
    } else if (OB_FAIL(schema_service.get_tenant_refreshed_schema_version(tenant_id_, refreshed_schema_version))) {
      LOG_WARN("get refreshed schema version failed", K(ret), K(tenant_id_));
    } else if (refreshed_schema_version < schema_version_) {
      ret = OB_SCHEMA_EAGAIN;
      if (REACH_TIME_INTERVAL(1000L * 1000L)) {
        LOG_INFO("tenant schema not refreshed to the target version", K(ret), K(tenant_id_), K(schema_version_), K(refreshed_schema_version));
      }
    }
  }
  return ret;
}

int ObDDLTask::remove_task_record()
{
  int ret = OB_SUCCESS;
  ObRootService *root_service = GCTX.root_service_;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLTask has not been inited", K(ret));
  } else if (OB_ISNULL(root_service)) {
    ret = OB_ERR_SYS;
    LOG_WARN("error sys, root service must not be nullptr", K(ret));
  } else if (OB_FAIL(ObDDLTaskRecordOperator::delete_record(root_service->get_sql_proxy(),
                                                            tenant_id_,
                                                            task_id_))) {
    LOG_WARN("delete record failed", K(ret), K(task_id_));
  }
  return ret;
}

// Notice that,
// to solve rows affected by drop database, `affected_rows` is introduced,
// to solve forward user error msg for ddl_retry_task, `forward_user_message` is introduced.
int ObDDLTask::report_error_code(const ObString &forward_user_message, const int64_t affected_rows)
{
  int ret = OB_SUCCESS;
  bool is_oracle_mode = false;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObIndexBuildTask has not been inited", K(ret));
  } else if (OB_FAIL(ObCompatModeGetter::check_is_oracle_mode_with_table_id(tenant_id_, object_id_, is_oracle_mode))) {
    LOG_WARN("check if oracle mode failed", K(ret), K(object_id_));
  } else {
    ObDDLErrorMessageTableOperator::ObBuildDDLErrorMessage error_message;
    error_message.affected_rows_ = affected_rows;
    const bool is_ddl_retry_task = is_drop_schema_block_concurrent_trans(task_type_);
    if (OB_SUCCESS != ret_code_) {
      if (OB_FAIL(ObDDLErrorMessageTableOperator::load_ddl_user_error(tenant_id_, task_id_, object_id_,
          schema_version_, *GCTX.sql_proxy_, error_message))) {
        LOG_WARN("load ddl user error failed", K(ret), K(object_id_), K(schema_version_), K(error_message));
        if (OB_ITER_END == ret) {     // no single replica error message found, use ret_code_
          ret = OB_SUCCESS;
          if (is_oracle_mode && DDL_CREATE_INDEX != task_type_ && OB_ERR_DUPLICATED_UNIQUE_KEY == ret_code_) {
            ret_code_ = OB_ERR_PRIMARY_KEY_DUPLICATE;
          }
          const char *ddl_type_str = nullptr;
          const char *str_user_error = ob_errpkt_str_user_error(ret_code_, is_oracle_mode);
          const char *str_error = ob_errpkt_strerror(ret_code_, is_oracle_mode);
          const int64_t buf_size = is_ddl_retry_task ? forward_user_message.length() + 1 : OB_MAX_ERROR_MSG_LEN;
          error_message.ret_code_ = ret_code_;
          error_message.ddl_type_ = task_type_;
          if (OB_FAIL(get_ddl_type_str(task_type_, ddl_type_str))) {
            LOG_WARN("ddl type to string failed", K(ret));
          } else if (OB_FAIL(databuff_printf(error_message.dba_message_, OB_MAX_ERROR_MSG_LEN, "ddl_type:%s", ddl_type_str))) {
            LOG_WARN("print ddl dba message failed", K(ret));
          } else if (OB_FAIL(error_message.prepare_user_message_buf(buf_size))) {
            LOG_WARN("failed to prepare user message buf", K(ret));
          } else if (is_ddl_retry_task) {
            // databuff_printf will ignore characters after '\0', thus use memcpy here.
            MEMCPY(error_message.user_message_, forward_user_message.ptr(), forward_user_message.length());
          } else if (OB_FAIL(databuff_printf(error_message.user_message_, buf_size, "%s", str_error))) {
            LOG_WARN("print ddl user message failed", K(ret));
          }
        }
      } else if (is_oracle_mode && DDL_CREATE_INDEX != task_type_ && OB_ERR_DUPLICATED_UNIQUE_KEY == error_message.ret_code_) {
        error_message.ret_code_ = OB_ERR_PRIMARY_KEY_DUPLICATE;
        const char *str_user_error = ob_errpkt_str_user_error(ret_code_, is_oracle_mode);
        const char *str_error = ob_errpkt_strerror(error_message.ret_code_, is_oracle_mode);
        const int64_t buf_size = OB_MAX_ERROR_MSG_LEN;
        if (OB_FAIL(error_message.prepare_user_message_buf(buf_size))) {
          LOG_WARN("failed to prepare user message buf", K(ret));
        } else if (OB_FAIL(databuff_printf(error_message.user_message_, buf_size, "%s", str_error))) {
          LOG_WARN("print to buffer failed", K(ret), K(str_error));
        }
      }
    } else {
      const int64_t buf_size = is_ddl_retry_task ? forward_user_message.length() + 1: OB_MAX_ERROR_MSG_LEN;
      error_message.ret_code_ = ret_code_;
      error_message.ddl_type_ = task_type_;
      if (OB_FAIL(databuff_printf(error_message.dba_message_, OB_MAX_ERROR_MSG_LEN, "%s", "Successful ddl"))) {
        LOG_WARN("print ddl dba message failed", K(ret));
      } else if (OB_FAIL(error_message.prepare_user_message_buf(buf_size))) {
        LOG_WARN("failed to prepare user message buf", K(ret));
      } else if (is_ddl_retry_task) {
        MEMCPY(error_message.user_message_, forward_user_message.ptr(), forward_user_message.length());
      } else if (OB_FAIL(databuff_printf(error_message.user_message_, buf_size, "%s", "Successful ddl"))) {
        LOG_WARN("print ddl user message failed", K(ret));
      }
    }

    if (OB_SUCC(ret)) {
      if (OB_FAIL(ObDDLErrorMessageTableOperator::report_ddl_error_message(error_message, tenant_id_, task_id_,
          target_object_id_, schema_version_, -1/*object id*/, GCTX.self_addr(), GCTX.root_service_->get_sql_proxy()))) {
        LOG_WARN("report ddl error message failed", K(ret));
      }
    }
  }
  return ret;
}

// wait trans end, but not hold snapshot.
int ObDDLTask::wait_trans_end(
    ObDDLWaitTransEndCtx &wait_trans_ctx,
    const ObDDLTaskStatus next_task_status)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  int64_t tmp_snapshot_version = 0;;
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *data_table_schema = nullptr;
  ObDDLTaskStatus new_status = PREPARE;
  ObRootService *root_service = GCTX.root_service_;
  ObMultiVersionSchemaService &schema_service = ObMultiVersionSchemaService::get_instance();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLRedefinitionTask has not been inited", K(ret));
  } else if (OB_ISNULL(root_service)) {
    ret = OB_ERR_SYS;
    LOG_WARN("error sys, root service must not be nullptr", K(ret));
  } else if (OB_FAIL(schema_service.get_tenant_schema_guard(tenant_id_, schema_guard))) {
    LOG_WARN("get schema guard failed", K(ret), K(tenant_id_));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, object_id_, data_table_schema))) {
    LOG_WARN("get data table schema failed", K(ret), K(object_id_));
  } else if (OB_ISNULL(data_table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("get table schema failed", K(ret), K(object_id_));
  }

  if (OB_SUCC(ret) && new_status != next_task_status && !wait_trans_ctx.is_inited()) {
    if (OB_FAIL(wait_trans_ctx.init(tenant_id_, object_id_,
      ObDDLWaitTransEndCtx::WAIT_SCHEMA_TRANS, data_table_schema->get_schema_version()))) {
      LOG_WARN("fail to init wait trans ctx", K(ret));
    }
  }
  // try wait transaction end
  if (OB_SUCC(ret) && new_status != next_task_status && tmp_snapshot_version <= 0) {
    bool is_trans_end = false;
    if (OB_FAIL(wait_trans_ctx.try_wait(is_trans_end, tmp_snapshot_version, true /*need_wait_trans_end */))) {
      if (OB_EAGAIN != ret) {
        LOG_WARN("fail to try wait transaction", K(ret));
      } else {
        ret = OB_SUCCESS;
      }
    }
  }
  DEBUG_SYNC(DDL_REDEFINITION_WAIT_TRANS_END);
  if (OB_SUCC(ret) && new_status != next_task_status && tmp_snapshot_version > 0) {
    new_status = next_task_status;
    wait_trans_ctx.reset();
  }

  // overwrite ret
  if (new_status == next_task_status || OB_FAIL(ret)) {
    if (OB_FAIL(switch_status(new_status, ret))) {
      LOG_WARN("fail to switch task status", K(ret));
    }
  }
  return ret;
}

// For idempotence, release snapshot version and update snapshot_version 0 
// in ALL_DDL_TASK_STATUS should be done in single trans.
int ObDDLTask::batch_release_snapshot(
      const int64_t snapshot_version, 
      const common::ObIArray<common::ObTabletID> &tablet_ids)
{
  int ret = OB_SUCCESS;
  bool need_commit = false;
  ObMySQLTransaction trans;
  ObRootService *root_service = GCTX.root_service_;
  if (OB_ISNULL(root_service)) {
    ret = OB_ERR_SYS;
    LOG_WARN("error sys, root service must not be nullptr", K(ret));
  } else if (OB_FAIL(trans.start(&root_service->get_ddl_service().get_sql_proxy(), tenant_id_))) {
    LOG_WARN("fail to start trans", K(ret));
  } else if (OB_FAIL(root_service->get_ddl_service().get_snapshot_mgr().batch_release_snapshot_in_trans(
          trans, SNAPSHOT_FOR_DDL, tenant_id_, schema_version_, snapshot_version, tablet_ids))) {
    LOG_WARN("batch release snapshot failed", K(ret), K(tablet_ids));
  } else if (OB_FAIL(ObDDLTaskRecordOperator::update_snapshot_version(trans,
                                                                      tenant_id_,
                                                                      task_id_,
                                                                      0 /* snapshot_version */))) {
    LOG_WARN("update snapshot version 0 failed", K(ret), K(task_id_));
  } else {
    need_commit = true;
  }
  int tmp_ret = trans.end(need_commit);
  if (OB_SUCCESS != tmp_ret) {
    LOG_WARN("fail to end trans", K(tmp_ret));
  }
  ret = OB_SUCC(ret) ? tmp_ret : ret;
  if (OB_SUCC(ret)) {
    snapshot_version_ = 0;
  }
  return ret;
}

int ObDDLTask::push_execution_id()
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  ObRootService *root_service = nullptr;
  int64_t table_task_status = 0;
  int64_t table_execution_id = 0;
  if (OB_ISNULL(root_service = GCTX.root_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("error unexpected, root service must not be nullptr", K(ret));
  } else if (OB_FAIL(trans.start(&root_service->get_sql_proxy(), tenant_id_))) {
    LOG_WARN("start transaction failed", K(ret));
  } else {
    if (OB_FAIL(ObDDLTaskRecordOperator::select_for_update(trans, tenant_id_, task_id_, table_task_status, table_execution_id))) {
      LOG_WARN("select for update failed", K(ret), K(task_id_));
    } else if (OB_FAIL(ObDDLTaskRecordOperator::update_execution_id(trans, tenant_id_, task_id_, table_execution_id + 1))) {
      LOG_WARN("update task status failed", K(ret));
    } else {
      execution_id_ = table_execution_id + 1;
    }
    bool commit = (OB_SUCCESS == ret);
    int tmp_ret = trans.end(commit);
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("fail to end trans", K(tmp_ret));
      ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
    }
  }
  return ret;
}

void ObDDLTask::calc_next_schedule_ts(int ret_code)
{
  if (OB_TIMEOUT == ret_code) {
    const int64_t SEC = 1000000;
    delay_schedule_time_ = std::min(delay_schedule_time_ * 6/5 + SEC/10, 30*SEC);
    const int64_t max_dt = delay_schedule_time_;
    const int64_t min_dt = std::max(0L, max_dt - 3*SEC);
    next_schedule_ts_ = ObTimeUtility::current_time() + ObRandom::rand(min_dt, max_dt);
  } else {
    delay_schedule_time_ = 0;
  }
  return;
}

// check if the current replica build task should be scheduled again.
bool ObDDLTask::is_replica_build_need_retry(
    const int ret_code)
{
  int ret = OB_SUCCESS;
  bool need_retry = true;
  bool is_table_exist = false;
  ObSchemaGetterGuard schema_guard;
  if (ObIDDLTask::in_ddl_retry_white_list(ret_code)
    || OB_REPLICA_NOT_READABLE == ret_code
    || OB_ERR_INSUFFICIENT_PX_WORKER == ret_code) {
    // need retry.
  } else if (OB_TABLE_NOT_EXIST == ret_code) {
    // Sometimes, the tablet leader has not refreshed the latest schema.
    // Thus, check whether the table really does not exist.
    const ObTableSchema *table_schema = nullptr;
    if (OB_FAIL(ObMultiVersionSchemaService::get_instance().get_tenant_schema_guard(tenant_id_, schema_guard))) {
      LOG_WARN("get tenant schema guard failed", K(ret), K_(tenant_id));
    } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, object_id_, table_schema))) {
      LOG_WARN("get table schema failed", K(ret), K(tenant_id_), K(object_id_));
    } else if (OB_ISNULL(table_schema)) {
      ret = OB_TABLE_NOT_EXIST;
      LOG_INFO("table schema not exist", K(ret), K(tenant_id_), K(object_id_));
    } else {
      if (ObDDLType::DDL_CHECK_CONSTRAINT == task_type_ || ObDDLType::DDL_ADD_NOT_NULL_COLUMN == task_type_) {
        // need retry.
      } else if (ObDDLType::DDL_FOREIGN_KEY_CONSTRAINT == task_type_) {
        // check whether the parent/child table does not exist.
        bool found = false;
        const ObIArray<ObForeignKeyInfo> &fk_infos = table_schema->get_foreign_key_infos();
        for (int64_t i = 0; OB_SUCC(ret) && !found && i < fk_infos.count(); ++i) {
          if (target_object_id_ != fk_infos.at(i).foreign_key_id_) {
          } else {
            found = true;
            if (OB_FAIL(schema_guard.check_table_exist(tenant_id_, fk_infos.at(i).parent_table_id_, is_table_exist))) {
              LOG_WARN("check schema exist failed", K(ret), K(tenant_id_), K(fk_infos.at(i)));
            } else if (!is_table_exist) {
              ret = OB_TABLE_NOT_EXIST;
              LOG_INFO("table schema not exist", K(ret), K(tenant_id_), K(object_id_), K(fk_infos.at(i)));
            } else if (OB_FAIL(schema_guard.check_table_exist(tenant_id_, fk_infos.at(i).child_table_id_, is_table_exist))) {
              LOG_WARN("check schema exist failed", K(ret), K(tenant_id_), K(fk_infos.at(i)));
            } else if (!is_table_exist) {
              ret = OB_TABLE_NOT_EXIST;
              LOG_INFO("table schema not exist", K(ret), K(tenant_id_), K(object_id_), K(fk_infos.at(i)));
            }
          }
        }
      } else if (OB_FAIL(schema_guard.check_table_exist(tenant_id_, target_object_id_, is_table_exist))) {
        LOG_WARN("check table exist failed", K(ret), K(tenant_id_), K(target_object_id_));
      } else if (!is_table_exist) {
        ret = OB_TABLE_NOT_EXIST;
        LOG_WARN("not exist", K(ret), K(tenant_id_), K(target_object_id_));
      }
    }
  } else {
    // ret_code is not in some predefined error code list.
    need_retry = false;
  }
  need_retry = OB_TABLE_NOT_EXIST == ret ? false : need_retry;
  return need_retry;
}

#ifdef ERRSIM
int ObDDLTask::check_errsim_error()
{
  int ret = OB_SUCCESS;
  int64_t to_fail_status = E(EventTable::EN_DDL_TASK_PROCESS_FAIL_STATUS) 1;
  if (to_fail_status * -1 == task_status_) {
    ret = E(EventTable::EN_DDL_TASK_PROCESS_FAIL_ERROR) OB_SUCCESS;
  }
  return ret;
}
#endif

/******************           ObDDLWaitTransEndCtx         *************/

ObDDLWaitTransEndCtx::ObDDLWaitTransEndCtx()
{
  reset();
}

ObDDLWaitTransEndCtx::~ObDDLWaitTransEndCtx()
{

}

int ObDDLWaitTransEndCtx::init(
    const uint64_t tenant_id,
    const uint64_t table_id,
    const WaitTransType wait_trans_type,
    const int64_t wait_version)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret), K(is_inited_));
  } else if (OB_UNLIKELY(OB_INVALID_ID == tenant_id
        || table_id <= 0
        || !is_wait_trans_type_valid(wait_trans_type)
        || wait_version <= 0)) {
    LOG_WARN("invalid argument", K(ret), K(tenant_id), K(table_id), K(wait_trans_type), K(wait_version));
  } else if (OB_FALSE_IT(tablet_ids_.reset())) {
  } else if (OB_FAIL(ObDDLUtil::get_tablets(tenant_id, table_id, tablet_ids_))) {
    LOG_WARN("get table partitions failed", K(ret));
  } else if (OB_UNLIKELY(tablet_ids_.count() <= 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid partition array or pg array", K(ret), K(table_id), K(tablet_ids_.count()));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < tablet_ids_.count(); ++i) {
      if (OB_FAIL(snapshot_array_.push_back(0))) {
        LOG_WARN("push back snapshot array failed", K(ret), K(i));
      }
    }
    if (OB_SUCC(ret)) {
      tenant_id_ = tenant_id;
      table_id_ = table_id;
      wait_type_ = wait_trans_type;
      wait_version_ = wait_version;
      is_trans_end_ = false;
      is_inited_ = true;
    }
  }
  return ret;
}

void ObDDLWaitTransEndCtx::reset()
{
  is_inited_ = false;
  tenant_id_ = OB_INVALID_ID;
  table_id_ = 0;
  is_trans_end_ = false;
  wait_type_ = WaitTransType::MIN_WAIT_TYPE;
  wait_version_ = 0;
  tablet_ids_.reset();
  snapshot_array_.reset();
}

struct SendItem final
{
public:
  bool operator < (const SendItem &other) const { return leader_addr_ <  other.leader_addr_; }
  TO_STRING_KV(K_(leader_addr), K_(ls_id), K_(tablet_id), KP_(other_info));
public:
  ObAddr leader_addr_;
  ObLSID ls_id_;
  ObTabletID tablet_id_;
  void *other_info_;
};

int group_tablets_leader_addr(const uint64_t tenant_id, const ObIArray<ObTabletID> &tablet_ids, ObLocationService *location_service, ObArray<SendItem> &group_items)
{
  int ret = OB_SUCCESS;
  group_items.reuse();
  if (OB_UNLIKELY(OB_INVALID_ID == tenant_id || nullptr == location_service)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id), K(tablet_ids.count()));
  } else {
    const int64_t rpc_timeout = max(GCONF.rpc_timeout, 1000L * 1000L * 9L);
    if (OB_FAIL(group_items.reserve(tablet_ids.count()))) {
      LOG_WARN("reserve send array failed", K(ret), K(tablet_ids.count()));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < tablet_ids.count(); ++i) {
      const ObTabletID &tablet_id = tablet_ids.at(i);
      SendItem item;
      if (OB_FAIL(ObDDLUtil::get_tablet_leader_addr(location_service,
                                                    tenant_id,
                                                    tablet_id,
                                                    rpc_timeout,
                                                    item.ls_id_,
                                                    item.leader_addr_))) {
        LOG_WARN("get tablet leader addr failed", K(ret));
      } else if (FALSE_IT(item.tablet_id_ = tablet_id)) {
      } else if (OB_FAIL(group_items.push_back(item))) {
        LOG_WARN("push back send item failed", K(ret), K(item));
      }
    }
  }
  return ret;
}

template<typename Proxy, typename Arg, typename Res>
int check_trans_end(const ObArray<SendItem> &send_array,
                    Proxy &proxy,
                    Arg &arg,
                    Res *res,
                    ObIArray<int> &ret_array,
                    ObIArray<int64_t> &snapshot_array)
{
  int ret = OB_SUCCESS;
  ret_array.reuse();
  snapshot_array.reuse();
  hash::ObHashMap<obrpc::ObLSTabletPair, obrpc::ObCheckTransElapsedResult> result_map;
  ObArray<SendItem> tmp_send_array;
  if (OB_UNLIKELY(send_array.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret));
  } else if (OB_FAIL(tmp_send_array.assign(send_array))) {
    LOG_WARN("copy send array failed", K(ret), K(send_array.count()));
  } else if (OB_FAIL(result_map.create(send_array.count(), "check_trans_map"))) {
    LOG_WARN("create return code map failed", K(ret));
  } else {
    // group by leader addr and send batch rpc
    std::sort(tmp_send_array.begin(), tmp_send_array.end());
    const int64_t rpc_timeout = max(GCONF.rpc_timeout, 1000L * 1000L * 9L);
    ObAddr last_addr;
    for (int64_t i = 0; OB_SUCC(ret) && i < tmp_send_array.count(); ++i) {
      const SendItem &send_item = tmp_send_array.at(i);
      if (send_item.leader_addr_ != last_addr) {
        if (arg.tablets_.count() > 0) {
          if (OB_FAIL(proxy.call(last_addr, rpc_timeout, arg.tenant_id_, arg))) {
            LOG_WARN("send rpc failed", K(ret), K(arg), K(last_addr), K(arg.tenant_id_));
          }
        }
        if (OB_SUCC(ret)) {
          arg.tablets_.reuse();
          last_addr = send_item.leader_addr_;
        }
      }
      if (OB_SUCC(ret)) {
        ObLSTabletPair ls_tablet_pair;
        ls_tablet_pair.ls_id_ = send_item.ls_id_;
        ls_tablet_pair.tablet_id_ = send_item.tablet_id_;
        if (OB_FAIL(arg.tablets_.push_back(ls_tablet_pair))) {
          LOG_WARN("push back send item failed", K(ret), K(i), K(send_item));
        }
      }
    }
    if (OB_SUCC(ret) && arg.tablets_.count() > 0) {
      if (OB_FAIL(proxy.call(last_addr, rpc_timeout, arg.tenant_id_, arg))) {
        LOG_WARN("send rpc failed", K(ret), K(arg), K(last_addr), K(arg.tenant_id_));
      }
    }

    // collect result
    int tmp_ret = OB_SUCCESS;
    common::ObArray<int> tmp_ret_array;
    if (OB_SUCCESS != (tmp_ret = proxy.wait_all(tmp_ret_array))) {
      LOG_WARN("rpc proxy wait failed", K(tmp_ret));
      ret = OB_SUCCESS == ret ? tmp_ret : ret;
    } else if (OB_SUCC(ret)) {
      const ObIArray<const Res *> &result_array = proxy.get_results();
      const ObIArray<Arg> &arg_array = proxy.get_args();
      const ObIArray<ObAddr> &dest_array = proxy.get_dests();
      for (int64_t i = 0; OB_SUCC(ret) && i < result_array.count(); ++i) {
        const Res *cur_result = result_array.at(i);
        const Arg &cur_arg = arg_array.at(i);
        const ObAddr &cur_dest_addr = dest_array.at(i);
        if (OB_ISNULL(cur_result)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("result it null", K(ret), K(i), KP(cur_result));
        } else if (OB_FAIL(tmp_ret_array.at(i))) {
          LOG_WARN("check shema trans elapsed failed", K(ret), K(i), K(cur_dest_addr), K(cur_arg), KPC(cur_result));
        } else if (cur_arg.tablets_.count() != cur_result->results_.count()) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("the result count does not match the argument", K(ret), K(cur_arg), KPC(cur_result));
        } else {
          for (int64_t j = 0; OB_SUCC(ret) && j < cur_result->results_.count(); ++j) {
            const obrpc::ObLSTabletPair &send_item = cur_arg.tablets_.at(j);
            const obrpc::ObCheckTransElapsedResult &result_item = cur_result->results_.at(j);
            if (OB_FAIL(result_map.set_refactored(send_item, result_item))) {
              LOG_WARN("insert into result map failed", K(ret));
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(ret_array.reserve(send_array.count()))) {
          LOG_WARN("reserve return code array failed", K(ret), K(send_array.count()));
        } else if (OB_FAIL(snapshot_array.reserve(send_array.count()))) {
          LOG_WARN("reserve snapshot array failed", K(ret), K(send_array.count()));
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < send_array.count(); ++i) {
          const SendItem &send_item = send_array.at(i);
          ObLSTabletPair ls_tablet_pair;
          ls_tablet_pair.ls_id_ = send_item.ls_id_;
          ls_tablet_pair.tablet_id_ = send_item.tablet_id_;
          obrpc::ObCheckTransElapsedResult result_item;
          if (OB_FAIL(result_map.get_refactored(ls_tablet_pair, result_item))) {
            LOG_WARN("get result failed", K(ret), K(send_item));
          } else if (OB_FAIL(ret_array.push_back(result_item.ret_code_))) {
            LOG_WARN("push back return code failed", K(ret), K(send_item), K(result_item));
          } else if (OB_FAIL(snapshot_array.push_back(result_item.snapshot_))) {
            LOG_WARN("push back snapshot failed", K(ret), K(send_item), K(result_item));
          }
        }
      }
    }
  }
  return ret;
}

int ObDDLWaitTransEndCtx::check_schema_trans_end(
    const int64_t schema_version,
    const common::ObIArray<common::ObTabletID> &tablet_ids,
    common::ObIArray<int> &ret_array,
    common::ObIArray<int64_t> &snapshot_array,
    const uint64_t tenant_id,
    obrpc::ObSrvRpcProxy *rpc_proxy,
    ObLocationService *location_service,
    const bool need_wait_trans_end)
{
  int ret = OB_SUCCESS;
  ret_array.reset();
  snapshot_array.reset();
  ObArray<SendItem> send_array;
  if (OB_UNLIKELY(schema_version <= 0 || tablet_ids.count() <= 0 || OB_INVALID_ID == tenant_id
      || nullptr == rpc_proxy || nullptr == location_service)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(schema_version), K(tablet_ids.count()), K(tenant_id), KP(rpc_proxy), KP(location_service));
  } else if (OB_FAIL(group_tablets_leader_addr(tenant_id, tablet_ids, location_service, send_array))) {
    LOG_WARN("group tablet by leader addr failed", K(ret), K(tenant_id), K(tablet_ids.count()));
  } else {
    ObCheckSchemaVersionElapsedProxy proxy(*rpc_proxy, &obrpc::ObSrvRpcProxy::check_schema_version_elapsed);
    obrpc::ObCheckSchemaVersionElapsedArg arg;
    obrpc::ObCheckSchemaVersionElapsedResult *res = nullptr;
    arg.tenant_id_ = tenant_id;
    arg.schema_version_ = schema_version;
    arg.need_wait_trans_end_ = need_wait_trans_end;
    if (OB_FAIL(check_trans_end(send_array, proxy, arg, res, ret_array, snapshot_array))) {
      LOG_WARN("check trans end failed", K(ret));
    }
  }
  return ret;
}

int ObDDLWaitTransEndCtx::check_sstable_trans_end(
    const uint64_t tenant_id,
    const int64_t sstable_exist_ts,
    const common::ObIArray<common::ObTabletID> &tablet_ids,
    obrpc::ObSrvRpcProxy *rpc_proxy,
    ObLocationService *location_service,
    common::ObIArray<int> &ret_array,
    common::ObIArray<int64_t> &snapshot_array)
{
  int ret = OB_SUCCESS;
  ret_array.reset();
  snapshot_array.reset();
  ObArray<SendItem> send_array;
  if (OB_UNLIKELY(OB_INVALID_ID == tenant_id || sstable_exist_ts <= 0 || tablet_ids.count() <= 0
      || nullptr == rpc_proxy || nullptr == location_service)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id), K(sstable_exist_ts), K(tablet_ids.count()),
        KP(rpc_proxy), KP(location_service));
  } else if (OB_FAIL(group_tablets_leader_addr(tenant_id, tablet_ids, location_service, send_array))) {
    LOG_WARN("group tablet by leader addr failed", K(ret), K(tenant_id), K(tablet_ids.count()));
  } else {
    ObCheckCtxCreateTimestampElapsedProxy proxy(*rpc_proxy, &obrpc::ObSrvRpcProxy::check_modify_time_elapsed);
    obrpc::ObCheckModifyTimeElapsedArg arg;
    obrpc::ObCheckModifyTimeElapsedResult *res = nullptr;
    arg.tenant_id_ = tenant_id;
    arg.sstable_exist_ts_ = sstable_exist_ts;
    if (OB_FAIL(check_trans_end(send_array, proxy, arg, res, ret_array, snapshot_array))) {
      LOG_WARN("check trans end failed", K(ret));
    }
  }
  return ret;
}

int ObDDLWaitTransEndCtx::try_wait(bool &is_trans_end, int64_t &snapshot_version, const bool need_wait_trans_end)
{
  int ret = OB_SUCCESS;
  is_trans_end = false;
  snapshot_version = 0;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), K(is_inited_));
  } else if (is_trans_end_) {
    // do nothing
  } else {
    ObArray<common::ObTabletID> need_check_tablets;
    ObArray<int64_t> tablet_pos_indexes;
    if (OB_FAIL(get_snapshot_check_list(need_check_tablets, tablet_pos_indexes))) {
      LOG_WARN("get snapshot check list failed", K(ret));
    } else if (need_check_tablets.empty()) {
      is_trans_end_ = true;
    } else {
      const int64_t check_count = need_check_tablets.count();
      ObArray<int> ret_codes;
      ObArray<int64_t> tmp_snapshots;
      switch (wait_type_) {
        case WaitTransType::WAIT_SCHEMA_TRANS: {
          if (OB_FAIL(check_schema_trans_end(
              wait_version_, need_check_tablets, ret_codes, tmp_snapshots, tenant_id_,
              GCTX.srv_rpc_proxy_, GCTX.location_service_, need_wait_trans_end))) {
            LOG_WARN("check schema transactions elapsed failed", K(ret), K(wait_type_), K(wait_version_));
          }
          break;
        }
        case WaitTransType::WAIT_SSTABLE_TRANS: {
          if (OB_FAIL(check_sstable_trans_end(
              tenant_id_, wait_version_, need_check_tablets, GCTX.srv_rpc_proxy_,
              GCTX.location_service_, ret_codes, tmp_snapshots))) {
            LOG_WARN("check sstable transactions elapsed failed", K(ret), K(wait_type_), K(wait_version_));
          }
          break;
        }
        default: {
          ret = OB_NOT_SUPPORTED;
          LOG_WARN("not supported wait_trans_type", K(ret), K(wait_type_));
          break;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (ret_codes.count() != check_count || tmp_snapshots.count() != check_count) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("check count not match", K(ret),
            K(check_count), K(ret_codes.count()), K(tmp_snapshots.count()));
      } else {
        int64_t succ_count = 0;
        for (int64_t i = 0; OB_SUCC(ret) && i < check_count; ++i) {
          if (OB_SUCCESS == ret_codes.at(i) && tmp_snapshots.at(i) > 0) {
            snapshot_array_.at(tablet_pos_indexes.at(i)) = tmp_snapshots.at(i);
            ++succ_count;
          } else if (ObIDDLTask::in_ddl_retry_white_list(ret_codes.at(i))) {
            // need retry
          } else if (OB_SUCCESS != ret_codes.at(i)) {
            ret = ret_codes.at(i);
            LOG_WARN("failed return code", K(ret), K(i), K(need_check_tablets.at(i)));
          }
        }
        if (OB_SUCC(ret) && check_count == succ_count) {
          is_trans_end_ = true;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (!need_wait_trans_end && !is_trans_end_) {
        // No need to wait trans end at the phase of obtain_snapshot,
        // and meanwhile the snapshot version can be obtained is sured.
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("error unexpected", K(ret), K(need_check_tablets), K(ret_codes), K(tmp_snapshots));
      }
    }
  }
  if (OB_SUCC(ret) && is_trans_end_) {
    if (OB_FAIL(get_snapshot(snapshot_version))) {
      LOG_WARN("get snapshot version failed", K(ret));
    }
  }
  is_trans_end = is_trans_end_;
  return ret;
}

int ObDDLWaitTransEndCtx::get_snapshot(int64_t &snapshot_version)
{
  int ret = OB_SUCCESS;
  snapshot_version = 0;
  ObRootService *root_service = nullptr;
  ObFreezeInfoProxy freeze_info_proxy(tenant_id_);
  ObSimpleFrozenStatus frozen_status;
  const int64_t timeout = 10 * 1000 * 1000;//  10s
  int64_t curr_ts = 0;
  bool is_external_consistent = false;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), K(is_inited_));
  } else if (!is_trans_end_) {
    ret = OB_STATE_NOT_MATCH;
    LOG_WARN("not all transactions are end", K(ret));
  } else if (OB_ISNULL(root_service = GCTX.root_service_)) {
    ret = OB_ERR_SYS;
    LOG_WARN("root service is null", K(ret), KP(root_service));
  } else {
    {
      MAKE_TENANT_SWITCH_SCOPE_GUARD(tenant_guard);
      // ignore return, MTL is only used in get_ts_sync, which will handle switch failure.
      // for performance, everywhere calls get_ts_sync should ensure using correct tenant ctx
      tenant_guard.switch_to(tenant_id_);
      if (OB_FAIL(OB_TS_MGR.get_ts_sync(tenant_id_,
                                        timeout,
                                        curr_ts,
                                        is_external_consistent))) {
        LOG_WARN("fail to get gts sync", K(ret), K(tenant_id_), K(timeout), K(curr_ts), K(is_external_consistent));
      }
    }
    if (OB_SUCC(ret)) {
      int64_t max_snapshot = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < snapshot_array_.count(); ++i) {
        int64_t cur_snapshot = snapshot_array_.at(i);
        if (0 >= cur_snapshot) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("current snapshot is invalid", K(ret), K(cur_snapshot));
        } else {
          max_snapshot = max(max_snapshot, cur_snapshot);
        }
      }
      if (OB_SUCC(ret)) {
        int tmp_ret = OB_SUCCESS;
        snapshot_version = max(max_snapshot, curr_ts - INDEX_SNAPSHOT_VERSION_DIFF);
        if (OB_SUCCESS != (tmp_ret = freeze_info_proxy.get_freeze_info(
            root_service->get_sql_proxy(), 0L/*major version*/, frozen_status))) {
          LOG_WARN("get freeze info failed", K(ret));
        } else {
          snapshot_version = max(snapshot_version, frozen_status.frozen_scn_);
        }
      }
    }
  }
  return ret;
}

bool ObDDLWaitTransEndCtx::is_wait_trans_type_valid(const WaitTransType wait_trans_type)
{
  return wait_trans_type > WaitTransType::MIN_WAIT_TYPE
    && wait_trans_type < WaitTransType::MAX_WAIT_TYPE;
}

int ObDDLWaitTransEndCtx::get_snapshot_check_list(
    ObIArray<common::ObTabletID> &need_check_tablets,
    ObIArray<int64_t> &tablet_pos_indexes)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), K(is_inited_));
  } else {
    const int64_t tablet_count = tablet_ids_.count();
    for (int64_t i = 0; OB_SUCC(ret) && i < tablet_count; ++i) {
      if (0 == snapshot_array_.at(i)) {
        const ObTabletID &tablet_id = tablet_ids_.at(i);
        if (OB_FAIL(need_check_tablets.push_back(tablet_id))) {
          LOG_WARN("push back tablet id failed", K(ret), K(i), K(tablet_id));
        } else if (OB_FAIL(tablet_pos_indexes.push_back(i))) {
          LOG_WARN("push back tablet index failed", K(ret), K(i), K(tablet_id));
        }
      }
    }
  }
  return ret;
}


/***************         ObDDLWaitColumnChecksumCtx        *************/

ObDDLWaitColumnChecksumCtx::ObDDLWaitColumnChecksumCtx()
{
  reset();
}

ObDDLWaitColumnChecksumCtx::~ObDDLWaitColumnChecksumCtx()
{

}

int ObDDLWaitColumnChecksumCtx::init(
    const int64_t task_id,
    const uint64_t tenant_id,
    const uint64_t source_table_id,
    const uint64_t target_table_id,
    const int64_t schema_version,
    const int64_t snapshot_version,
    const uint64_t execution_id,
    const int64_t timeout_us)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret), K(is_inited_));
  } else if (OB_UNLIKELY(
        task_id <= 0
        || OB_INVALID_ID == tenant_id
        || OB_INVALID_ID == source_table_id
        || OB_INVALID_ID == target_table_id
        || schema_version <= 0
        || snapshot_version <= 0
        || OB_INVALID_ID == execution_id
        || timeout_us <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(task_id), K(tenant_id), K(source_table_id), K(target_table_id),
        K(schema_version), K(snapshot_version), K(execution_id));
  } else {
    ObArray<ObTabletID> tablet_ids;
    PartitionColChecksumStat tmp_stat;
    const int64_t NEED_CALC_CHECKSUM_COUNT =  2;  // source table and target table
    tmp_stat.col_checksum_stat_ = CCS_INVALID;
    tmp_stat.execution_id_ = execution_id;
    tmp_stat.snapshot_ = -1;
    for (int64_t i = 0; OB_SUCC(ret) && i < NEED_CALC_CHECKSUM_COUNT; ++i) {
      const uint64_t cur_table_id =  0 == i ? source_table_id : target_table_id;
      tablet_ids.reset();
      if (OB_UNLIKELY(cur_table_id <= 0)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("invalid table id", K(ret), K(i), K(cur_table_id));
      } else if (OB_FAIL(ObDDLUtil::get_tablets(tenant_id, cur_table_id, tablet_ids))) {
        LOG_WARN("get table partition failed", K(ret), K(tenant_id), K(cur_table_id));
      } else if (OB_UNLIKELY(tablet_ids.count() <= 0)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get invalid tablet ids", K(ret), K(tablet_ids.count()));
      } else {
        for (int64_t j = 0; OB_SUCC(ret) && j < tablet_ids.count(); ++j) {
          tmp_stat.tablet_id_ = tablet_ids.at(j);
          tmp_stat.table_id_ = cur_table_id;
          if (OB_FAIL(stat_array_.push_back(tmp_stat))) {
            LOG_WARN("push batck column checksum status array failed", K(ret), K(tmp_stat));
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      source_table_id_ = source_table_id;
      target_table_id_ = target_table_id;
      schema_version_ = schema_version;
      snapshot_version_ = snapshot_version;
      execution_id_ = execution_id;
      timeout_us_ = timeout_us;
      task_id_ = task_id;
      tenant_id_ = tenant_id;
      is_inited_ = true;
    }
  }
  return ret;
}

void ObDDLWaitColumnChecksumCtx::reset()
{
  is_inited_ = false;
  is_calc_done_ = false;
  source_table_id_ = OB_INVALID_ID;
  target_table_id_ = OB_INVALID_ID;
  schema_version_ = 0;
  snapshot_version_ = 0;
  execution_id_ = OB_INVALID_ID;
  timeout_us_ = 0;
  last_drive_ts_ = 0;
  stat_array_.reset();
  task_id_ = 0;
  tenant_id_ = OB_INVALID_ID;
}

int ObDDLWaitColumnChecksumCtx::try_wait(bool &is_column_checksum_ready)
{
  is_column_checksum_ready = false;
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), K(is_inited_));
  } else if (is_calc_done_) {
    // do nothing
  } else {
    SpinRLockGuard guard(lock_);
    const int64_t check_count = stat_array_.count();
    int64_t success_count = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < check_count; ++i) {
      const PartitionColChecksumStat &item = stat_array_.at(i);
      if (!item.is_valid()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("item is invalid", K(ret), K(item));
      } else if (item.snapshot_ <= 0) {
        // calc rpc not send, by pass
      } else if (CCS_FAILED == item.col_checksum_stat_) {
        is_calc_done_ = true;
        ret = item.ret_code_;
        LOG_WARN("current column checksum status failed", K(ret), K(item));
      } else if (item.col_checksum_stat_ == ColChecksumStat::CCS_SUCCEED) {
        ++success_count;
      }
    }
    if (check_count == success_count) {
      is_calc_done_ = true;
    }
  }
  if (OB_SUCC(ret) && !is_calc_done_) {
    int64_t send_succ_count = 0;
    if (0 != last_drive_ts_  && last_drive_ts_ + timeout_us_ < ObTimeUtility::current_time()) {
      // wait too long, refresh to retry send rpc
      if (OB_FAIL(refresh_zombie_task())) {
        LOG_WARN("refresh zombie task failed", K(ret));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(send_calc_rpc(send_succ_count))) {
      LOG_WARN("send column checksum calculation request failed", K(ret), K(send_succ_count));
    } else if (send_succ_count > 0) {
      last_drive_ts_ = ObTimeUtility::current_time();
    }
  }
  is_column_checksum_ready = is_calc_done_;
  return ret;
}

int ObDDLWaitColumnChecksumCtx::update_status(const common::ObTabletID &tablet_id, const int ret_code)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), K(is_inited_));
  } else if (OB_UNLIKELY(!tablet_id.is_valid() || ret_code > 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tablet_id), K(ret_code));
  } else {
    SpinWLockGuard guard(lock_);
    bool found = false;
    for (int64_t i = 0; OB_SUCC(ret) && !found && i < stat_array_.count(); ++i) {
      PartitionColChecksumStat &item = stat_array_.at(i);
      if (tablet_id == item.tablet_id_) {
        found = true;
        if (OB_SUCCESS == ret_code) {
          item.col_checksum_stat_ = ColChecksumStat::CCS_SUCCEED;
        } else if (OB_NOT_MASTER == ret_code || OB_PARTITION_NOT_EXIST == ret_code) {
          item.col_checksum_stat_ = CCS_NOT_MASTER;
        } else {
          item.col_checksum_stat_ = CCS_FAILED;
          item.ret_code_ = ret_code;
          LOG_WARN("column checksum calc failed", K(ret_code), K(item));
        }
      }
    }
    if (!found) {
      ret = OB_ENTRY_NOT_EXIST;
      LOG_WARN("column_checksum_stat not found", K(ret), K(tablet_id));
    }
  }
  return ret;
}

int ObDDLWaitColumnChecksumCtx::refresh_zombie_task()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), K(is_inited_));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < stat_array_.count(); ++i) {
      PartitionColChecksumStat &item = stat_array_.at(i);
      if (snapshot_version_ == item.snapshot_ && CCS_INVALID == item.col_checksum_stat_) {
        item.snapshot_ = -1;
      }
    }
  }
  return ret;
}

int send_batch_calc_rpc(obrpc::ObSrvRpcProxy &rpc_proxy,
                        const ObAddr &leader_addr,
                        const ObCalcColumnChecksumRequestArg &arg,
                        ObCalcColumnChecksumRequestRes &res,
                        ObIArray<SendItem> &send_array,
                        const int64_t group_start_idx,
                        const int64_t group_end_idx,
                        common::SpinRWLock &item_lock,
                        int64_t &send_succ_count)
{
  int ret = OB_SUCCESS;
  const int64_t rpc_timeout = max(GCONF.rpc_timeout, 1000L * 1000L * 9L);
  if (OB_FAIL(rpc_proxy.to(leader_addr)
                       .by(arg.tenant_id_)
                       .timeout(rpc_timeout)
                       .calc_column_checksum_request(arg, res))) {
    LOG_WARN("send rpc failed", K(ret), K(arg), K(leader_addr), K(arg.tenant_id_));
  } else if (res.ret_codes_.count() != arg.calc_items_.count() || res.ret_codes_.count() != (group_end_idx - group_start_idx)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("return codes count not match the argument", K(ret), K(arg.calc_items_.count()),
        K(res.ret_codes_.count()), "group_count", group_end_idx - group_start_idx);
  } else {
    LOG_INFO("send checksum validation task", K(arg));
    SpinWLockGuard guard(item_lock);
    for (int64_t j = group_start_idx, k = 0; j < group_end_idx; ++j, ++k) { // ignore ret
      PartitionColChecksumStat *item = reinterpret_cast<PartitionColChecksumStat *>(send_array.at(j).other_info_);
      int ret_code = res.ret_codes_.at(k);
      if (OB_SUCCESS == ret_code) {
        item->snapshot_ = arg.snapshot_version_;
        item->col_checksum_stat_ = CCS_INVALID;
        ++send_succ_count;
      } else if (OB_EAGAIN == ret_code || OB_HASH_EXIST == ret_code) { // ignore
        LOG_INFO("send checksum rpc not success", K(ret), KPC(item));
      } else {
        ret = OB_SUCCESS == ret ? ret_code : ret; // keep first error code
        LOG_WARN("fail to calc column checksum request", K(ret_code), K(arg), KPC(item));
      }
    }
  }
  return ret;
}

int ObDDLWaitColumnChecksumCtx::send_calc_rpc(int64_t &send_succ_count)
{
  int ret = OB_SUCCESS;
  send_succ_count = 0;
  ObRootService *root_service = nullptr;
  share::ObLocationService *location_service = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), K(is_inited_));
  } else if (is_calc_done_) {
    // do nothing
  } else if (OB_ISNULL(root_service = GCTX.root_service_)
      || OB_ISNULL(location_service = GCTX.location_service_)) {
    ret = OB_ERR_SYS;
    LOG_WARN("root service or location_cache is null", K(ret), KP(root_service), KP(location_service));
  } else {
    ObLSID ls_id;
    const int64_t rpc_timeout = max(GCONF.rpc_timeout, 1000L * 1000L * 9L);
    ObArray<SendItem> send_array;
    for (int64_t i = 0; OB_SUCC(ret) && i < stat_array_.count(); ++i) {
      PartitionColChecksumStat &item = stat_array_.at(i);
      ObAddr leader_addr;
      if (!item.is_valid()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pkey invalid", K(ret), K(item));
      } else if (item.snapshot_ <= 0 || CCS_NOT_MASTER == item.col_checksum_stat_) {
        // only send rpc for the request not send or not master
        if (OB_FAIL(ObDDLUtil::get_tablet_leader_addr(location_service, tenant_id_, item.tablet_id_, rpc_timeout, ls_id, leader_addr))) {
          LOG_WARN("get tablet leader addr failed", K(ret));
        } else {
          SendItem send_item;
          send_item.leader_addr_ = leader_addr;
          send_item.ls_id_ = ls_id;
          send_item.tablet_id_ = item.tablet_id_;
          send_item.other_info_ = reinterpret_cast<void *>(&item);
          if (OB_FAIL(send_array.push_back(send_item))) {
            LOG_WARN("push send array failed", K(ret));
          }
        }
      }
    }

    if (OB_SUCC(ret)) {
      // group by leader addr and send batch rpc
      std::sort(send_array.begin(), send_array.end());

      ObAddr last_addr;
      int64_t group_start_idx = 0;
      ObCalcColumnChecksumRequestArg arg;
      ObCalcColumnChecksumRequestRes res;
      arg.tenant_id_ = tenant_id_;
      arg.task_id_ = task_id_;
      arg.source_table_id_ = source_table_id_;
      arg.target_table_id_ = target_table_id_;
      arg.schema_version_ = schema_version_;
      arg.execution_id_ = execution_id_;
      arg.snapshot_version_ = snapshot_version_;
      for (int64_t i = 0; OB_SUCC(ret) && i < send_array.count(); ++i) {
        const SendItem &send_item = send_array.at(i);
        if (send_item.leader_addr_ != last_addr) {
          if (arg.calc_items_.count() > 0) {
            if (OB_FAIL(send_batch_calc_rpc(root_service->get_rpc_proxy(), last_addr,
                    arg, res, send_array, group_start_idx, i, lock_, send_succ_count))) {
              LOG_WARN("send batch calc rpc failed", K(ret));
            }
          }
          if (OB_SUCC(ret)) {
            arg.calc_items_.reuse();
            res.ret_codes_.reuse();
            last_addr = send_item.leader_addr_;
            group_start_idx = i;
          }
        }
        if (OB_SUCC(ret)) {
          ObCalcColumnChecksumRequestArg::SingleItem calc_item;
          calc_item.ls_id_ = send_item.ls_id_;
          calc_item.tablet_id_ = send_item.tablet_id_;
          calc_item.calc_table_id_ = reinterpret_cast<PartitionColChecksumStat *>(send_item.other_info_)->table_id_;
          if (OB_FAIL(arg.calc_items_.push_back(calc_item))) {
            LOG_WARN("push back send item failed", K(ret), K(i), K(send_item));
          }
        }
      }
      if (OB_SUCC(ret) && arg.calc_items_.count() > 0) {
        if (OB_FAIL(send_batch_calc_rpc(root_service->get_rpc_proxy(), last_addr,
                arg, res, send_array, group_start_idx, send_array.count(), lock_, send_succ_count))) {
          LOG_WARN("send batch calc rpc failed", K(ret));
        }
      }
    }
  }
  return ret;
}

/*****************          ObDDLTaskRecord             ****************/

bool ObDDLTaskRecord::is_valid() const
{
  bool is_valid = task_id_ >= 0
    && ddl_type_ != ObDDLType::DDL_INVALID
    && !trace_id_.is_invalid()
    && task_status_ >= 0
    && tenant_id_ > 0
    && task_version_ > 0
    && OB_INVALID_ID != object_id_
    && schema_version_ > 0
    && ret_code_ >= 0
    && execution_id_ >= 0;
  return is_valid;
}

void ObDDLTaskRecord::reset()
{
  task_id_ = 0;
  parent_task_id_ = 0;
  ddl_type_ = ObDDLType::DDL_INVALID;
  trace_id_.reset();
  task_status_ = 0;
  tenant_id_ = 0;
  object_id_ = OB_INVALID_ID;
  schema_version_ = 0;
  target_object_id_ = OB_INVALID_ID;
  snapshot_version_ = 0;
  message_.reset();
  task_version_ = 0;
  ret_code_ = OB_SUCCESS;
  execution_id_ = 0;
}


/*****************          ObDDLTaskRecordOperator             ****************/
int ObDDLTaskRecordOperator::update_task_status(
    common::ObISQLClient &proxy,
    const uint64_t tenant_id,
    const int64_t task_id,
    const int64_t task_status)
{
  int ret = OB_SUCCESS;
  ObSqlString sql_string;
  int64_t affected_rows = 0;
  if (OB_UNLIKELY(task_id <= 0 || tenant_id <= 0 || task_status <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(task_id), K(tenant_id), K(task_status));
  } else if (OB_FAIL(sql_string.assign_fmt(" UPDATE %s SET status = %ld WHERE task_id = %lu",
          OB_ALL_DDL_TASK_STATUS_TNAME, task_status, task_id))) {
    LOG_WARN("assign sql string failed", K(ret), K(task_status), K(task_id));
  } else if (OB_FAIL(proxy.write(tenant_id, sql_string.ptr(), affected_rows))) {
    LOG_WARN("update status of ddl task record failed", K(ret), K(sql_string));
  } else if (OB_UNLIKELY(affected_rows < 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected_rows", K(ret), K(affected_rows));
  }
  return ret;
}

int ObDDLTaskRecordOperator::update_snapshot_version(
    common::ObISQLClient &sql_client,
    const uint64_t tenant_id,
    const int64_t task_id,
    const int64_t snapshot_version)
{
  int ret = OB_SUCCESS;
  ObSqlString sql_string;
  int64_t affected_rows = 0;
  if (OB_ISNULL(sql_client.get_pool()) || OB_UNLIKELY(task_id <= 0 || tenant_id <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(tenant_id), K(task_id));
  } else if (OB_FAIL(sql_string.assign_fmt(" UPDATE %s SET snapshot_version=%lu WHERE task_id=%lu ",
          OB_ALL_DDL_TASK_STATUS_TNAME, snapshot_version < 0 ? 0 : snapshot_version, task_id))) {
    LOG_WARN("assign sql string failed", K(ret), K(snapshot_version), K(task_id));
  } else if (OB_FAIL(sql_client.write(tenant_id, sql_string.ptr(), affected_rows))) {
    LOG_WARN("update snapshot_version of ddl task record failed", K(ret), K(sql_string));
  } else if (OB_UNLIKELY(affected_rows < 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected_rows", K(ret), K(affected_rows));
  }
  return ret;
}

int ObDDLTaskRecordOperator::update_ret_code(
    common::ObISQLClient &sql_client,
    const uint64_t tenant_id,
    const int64_t task_id,
    const int64_t ret_code)
{
  int ret = OB_SUCCESS;
  ObSqlString sql_string;
  int64_t affected_rows = 0;
  if (OB_ISNULL(sql_client.get_pool()) || OB_UNLIKELY(task_id <= 0 || tenant_id <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(tenant_id), K(task_id));
  } else if (OB_FAIL(sql_string.assign_fmt(" UPDATE %s SET ret_code=%lu WHERE task_id=%lu ",
          OB_ALL_DDL_TASK_STATUS_TNAME, ret_code, task_id))) {
    LOG_WARN("assign sql string failed", K(ret), K(ret_code), K(task_id));
  } else if (OB_FAIL(sql_client.write(tenant_id, sql_string.ptr(), affected_rows))) {
    LOG_WARN("update snapshot_version of ddl task record failed", K(ret), K(sql_string));
  } else if (OB_UNLIKELY(affected_rows < 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected_rows", K(ret), K(affected_rows));
  }
  return ret;
}

int ObDDLTaskRecordOperator::update_execution_id(
    common::ObISQLClient &sql_client,
    const uint64_t tenant_id,
    const int64_t task_id,
    const int64_t execution_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql_string;
  int64_t affected_rows = 0;
  if (OB_ISNULL(sql_client.get_pool()) || OB_UNLIKELY(task_id <= 0 || tenant_id <= 0 || execution_id <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(tenant_id), K(task_id));
  } else if (OB_FAIL(sql_string.assign_fmt(" UPDATE %s SET execution_id=%lu WHERE task_id=%lu ",
          OB_ALL_DDL_TASK_STATUS_TNAME, execution_id, task_id))) {
    LOG_WARN("assign sql string failed", K(ret), K(execution_id), K(task_id));
  } else if (OB_FAIL(sql_client.write(tenant_id, sql_string.ptr(), affected_rows))) {
    LOG_WARN("update snapshot_version of ddl task record failed", K(ret), K(sql_string));
  } else if (OB_UNLIKELY(affected_rows < 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected_rows", K(ret), K(affected_rows));
  }
  return ret;
}

int ObDDLTaskRecordOperator::update_message(
    common::ObISQLClient &proxy,
    const uint64_t tenant_id,
    const int64_t task_id,
    const ObString &message)
{
  int ret = OB_SUCCESS;
  ObSqlString sql_string;
  ObSqlString message_string;
  int64_t affected_rows = 0;
  if (OB_UNLIKELY(message.empty()
        || tenant_id <= 0 || task_id <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id), K(task_id), K(message));
  } else if (OB_FAIL(to_hex_str(message, message_string))) {
    LOG_WARN("append hex escaped string failed", K(ret));
  } else if (OB_FAIL(sql_string.assign_fmt(" UPDATE %s SET message=\"%.*s\" WHERE task_id=%lu",
          OB_ALL_DDL_TASK_STATUS_TNAME, static_cast<int>(message_string.length()), message_string.ptr(), task_id))) {
    LOG_WARN("assign sql string failed", K(ret), K(message_string));
  } else if (OB_FAIL(proxy.write(tenant_id, sql_string.ptr(), affected_rows))) {
    LOG_WARN("update message of ddl task record failed", K(ret), K(sql_string), K(message_string));
  } else if (OB_UNLIKELY(affected_rows < 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected_rows", K(ret), K(affected_rows));
  }
  return ret;
}

int ObDDLTaskRecordOperator::delete_record(common::ObMySQLProxy &proxy, const uint64_t tenant_id, const int64_t task_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql_string;
  int64_t affected_rows = 0;
  if (OB_UNLIKELY(!proxy.is_inited() || task_id <= 0 || tenant_id <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(proxy.is_inited()), K(tenant_id), K(task_id));
  } else if (OB_FAIL(sql_string.assign_fmt(" DELETE FROM %s WHERE task_id=%lu",
          OB_ALL_DDL_TASK_STATUS_TNAME, task_id))) {
    LOG_WARN("assign sql string failed", K(ret), K(task_id));
  } else if (OB_FAIL(proxy.write(tenant_id, sql_string.ptr(), affected_rows))) {
    LOG_WARN("delete ddl task record failed", K(ret), K(sql_string));
  } else if (OB_UNLIKELY(affected_rows < 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected_rows", K(ret), K(affected_rows));
  }
  return ret;
}

// check if is adding check constraint, foreign key, not null constraint
int ObDDLTaskRecordOperator::check_is_adding_constraint(
    common::ObMySQLProxy *proxy,
    common::ObIAllocator &allocator,
    const uint64_t object_id,
    bool &is_building)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(nullptr == proxy || !proxy->is_inited())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret));
  } else {
    ObSqlString sql_string;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(sql_string.assign_fmt(" SELECT tenant_id, task_id, object_id, target_object_id, ddl_type, "
          "schema_version, parent_task_id, trace_id, status, snapshot_version, task_version, execution_id, "
          "UNHEX(ddl_stmt_str) as ddl_stmt_str_unhex, ret_code, UNHEX(message) as message_unhex FROM %s "
          "WHERE object_id = %" PRIu64 " && ddl_type IN (%d, %d, %d)", OB_ALL_VIRTUAL_DDL_TASK_STATUS_TNAME,
          object_id, DDL_CHECK_CONSTRAINT, DDL_FOREIGN_KEY_CONSTRAINT, DDL_ADD_NOT_NULL_COLUMN))) {
        LOG_WARN("assign sql string failed", K(ret));
      } else if (OB_FAIL(proxy->read(res, sql_string.ptr()))) {
        LOG_WARN("query ddl task record failed", K(ret), K(sql_string));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get sql result", K(ret), KP(result));
      } else {
        ObDDLTaskRecord task_record;
        if (OB_SUCC(ret) && OB_SUCC(result->next())) {
          if (OB_FAIL(fill_task_record(result, allocator, task_record))) {
            LOG_WARN("fill index task failed", K(ret), K(result));
          } else if (!task_record.is_valid()) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("task record is invalid", K(ret), K(task_record));
          } else { // succeed to read at least 1 row
            is_building = true;
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

// check whether there are some long running ddl on the specified table.
int ObDDLTaskRecordOperator::check_has_long_running_ddl(
    common::ObMySQLProxy *proxy,
    const uint64_t tenant_id,
    const uint64_t table_id,
    bool &has_long_running_ddl)
{
  int ret = OB_SUCCESS;
  has_long_running_ddl = false;
  if (OB_UNLIKELY(nullptr == proxy || !proxy->is_inited() 
    || OB_INVALID_ID == tenant_id
    || OB_INVALID_ID == table_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), KP(proxy), K(tenant_id), K(table_id));
  } else {
    ObSqlString sql_string;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(sql_string.assign_fmt(" SELECT * FROM %s "
          "WHERE tenant_id = %lu AND object_id = %lu", OB_ALL_VIRTUAL_DDL_TASK_STATUS_TNAME,
          tenant_id, table_id))) {
        LOG_WARN("assign sql string failed", K(ret));
      } else if (OB_FAIL(proxy->read(res, sql_string.ptr()))) {
        LOG_WARN("query ddl task record failed", K(ret), K(sql_string));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get sql result", K(ret), KP(result));
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        } else {
          LOG_WARN("result next failed", K(ret), K(tenant_id), K(table_id));
        }
      } else {
        has_long_running_ddl = true;
      }
    }
  }
  return ret;
}

int ObDDLTaskRecordOperator::check_has_conflict_ddl(
    common::ObMySQLProxy *proxy,
    const uint64_t tenant_id,
    const uint64_t table_id,
    const int64_t task_id,
    const ObDDLType ddl_type,
    bool &has_conflict_ddl)
{
  int ret = OB_SUCCESS;
  has_conflict_ddl = false;
  if (OB_UNLIKELY(nullptr == proxy || !proxy->is_inited() 
    || OB_INVALID_ID == tenant_id
    || OB_INVALID_ID == table_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), KP(proxy), K(tenant_id), K(table_id));
  } else {
    ObSqlString sql_string;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(sql_string.assign_fmt("SELECT tenant_id, task_id, object_id, target_object_id, ddl_type,"
          "schema_version, parent_task_id, trace_id, status, snapshot_version, task_version, execution_id,"
          "UNHEX(ddl_stmt_str) as ddl_stmt_str_unhex, ret_code, UNHEX(message) as message_unhex FROM %s "
          "WHERE tenant_id = %lu AND object_id = %lu", OB_ALL_VIRTUAL_DDL_TASK_STATUS_TNAME,
          tenant_id, table_id))) {
        LOG_WARN("assign sql string failed", K(ret));
      } else if (OB_FAIL(proxy->read(res, sql_string.ptr()))) {
        LOG_WARN("query ddl task record failed", K(ret), K(sql_string));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get sql result", K(ret), KP(result));
      } else {
        ObDDLTaskRecord task_record;
        ObArenaAllocator allocator("DdlTaskRec");
        while (OB_SUCC(ret) && !has_conflict_ddl && OB_SUCC(result->next())) {
          allocator.reuse();
          if (OB_FAIL(fill_task_record(result, allocator, task_record))) {
            LOG_WARN("failed to fill task record", K(ret));
          } else if (task_record.task_id_ != task_id) {
            switch (ddl_type) {
            case ObDDLType::DDL_DROP_TABLE: {
              if (task_record.ddl_type_ == ObDDLType::DDL_DROP_INDEX && task_record.target_object_id_ != task_record.object_id_) {
                LOG_WARN("conflict with ddl", K(task_record));
                has_conflict_ddl = true;
              } 
              break;
            }
            default: {
              // do nothing
            }
            }
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

int ObDDLTaskRecordOperator::get_all_record(
    common::ObMySQLProxy &proxy,
    common::ObIAllocator &allocator,
    common::ObIArray<ObDDLTaskRecord> &records)
{
  int ret = OB_SUCCESS;
  records.reset();
  if (OB_UNLIKELY(!proxy.is_inited())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(proxy.is_inited()));
  } else {
    ObSqlString sql_string;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(sql_string.assign_fmt(" SELECT tenant_id, task_id, object_id, target_object_id, ddl_type, "
          "schema_version, parent_task_id, trace_id, status, snapshot_version, task_version, execution_id, "
          "UNHEX(ddl_stmt_str) as ddl_stmt_str_unhex, ret_code, UNHEX(message) as message_unhex FROM %s ", OB_ALL_VIRTUAL_DDL_TASK_STATUS_TNAME))) {
        LOG_WARN("assign sql string failed", K(ret));
      } else if (OB_FAIL(proxy.read(res, sql_string.ptr()))) {
        LOG_WARN("query ddl task record failed", K(ret), K(sql_string));
      } else if (OB_ISNULL((result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get sql result", K(ret), KP(result));
      } else {
        ObDDLTaskRecord task_record;
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          if (OB_FAIL(fill_task_record(result, allocator, task_record))) {
            LOG_WARN("fill index task failed", K(ret), K(result));
          } else if (!task_record.is_valid()) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("task record is invalid", K(ret), K(task_record));
          } else if (OB_FAIL(records.push_back(task_record))) {
            LOG_WARN("push back task record failed", K(ret), K(task_record));
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

int ObDDLTaskRecordOperator::to_hex_str(const ObString &src, ObSqlString &dst)
{
  int ret = OB_SUCCESS;
  const int64_t need_len = dst.length() + src.length() * 2;

  if (OB_FAIL(dst.reserve(need_len))) {
    LOG_WARN("reserve sql failed, ", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < src.length(); ++i) {
      if (OB_FAIL(dst.append_fmt("%02X", static_cast<uint8_t>(src.ptr()[i])))) {
        LOG_WARN("append string failed", K(ret), K(i));
      }
    }
  }
  return ret;
}

int ObDDLTaskRecordOperator::insert_record(
    common::ObISQLClient &proxy,
    const ObDDLTaskRecord &record)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!record.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(record));
  } else {
    ObSqlString sql_string;
    ObSqlString ddl_stmt_string;
    ObSqlString message_string;
    int64_t affected_rows = 0;
    char trace_id_str[64] = { 0 };
    int64_t pos = 0;
    if (OB_UNLIKELY(0 > (pos = record.trace_id_.to_string(trace_id_str, sizeof(trace_id_str))))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get task id string failed", K(ret), K(record), K(pos));
    } else if (OB_FAIL(to_hex_str(record.ddl_stmt_str_, ddl_stmt_string))) {
      LOG_WARN("append hex escaped ddl stmt string failed", K(ret));
    } else if (OB_FAIL(to_hex_str(record.message_, message_string))) {
      LOG_WARN("append hex escaped string failed", K(ret));
    } else if (OB_FAIL(sql_string.assign_fmt(
            " INSERT INTO %s (task_id, parent_task_id, tenant_id, object_id, schema_version, target_object_id, ddl_type, trace_id, status, task_version, execution_id, ret_code, ddl_stmt_str, message) "
            " VALUES (%lu, %lu, %lu, %lu, %lu, %lu, %d, '%s', %ld, %lu, %lu, %lu, '%.*s', \"%.*s\") ",
            OB_ALL_DDL_TASK_STATUS_TNAME, record.task_id_, record.parent_task_id_,
            ObSchemaUtils::get_extract_tenant_id(record.tenant_id_, record.tenant_id_), record.object_id_, record.schema_version_,
            get_record_id(record.ddl_type_, record.target_object_id_), record.ddl_type_, trace_id_str, record.task_status_, record.task_version_, record.execution_id_, record.ret_code_,
            static_cast<int>(ddl_stmt_string.length()), ddl_stmt_string.ptr(), static_cast<int>(message_string.length()), message_string.ptr()))) {
      LOG_WARN("assign sql string failed", K(ret), K(record));
    } else if (OB_FAIL(proxy.write(record.tenant_id_, sql_string.ptr(), affected_rows))) {
      LOG_WARN("insert ddl task record failed", K(ret), K(sql_string), K(record));
    } else if (OB_UNLIKELY(1 != affected_rows)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected affected_rows", K(ret), K(affected_rows));
    }
  }
  return ret;
}

int ObDDLTaskRecordOperator::fill_task_record(
    const common::sqlclient::ObMySQLResult *result_row,
    common::ObIAllocator &allocator,
    ObDDLTaskRecord &task_record)
{
  int ret = OB_SUCCESS;
  task_record.reset();
  if (OB_ISNULL(result_row)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(result_row));
  } else {
    ObString trace_id_str;
    ObString task_message;
    ObString ddl_stmt_str;
    char *buf_ddl_stmt_str = nullptr;
    char *buf_task_message = nullptr;
    EXTRACT_INT_FIELD_MYSQL(*result_row, "task_id", task_record.task_id_, uint64_t);
    EXTRACT_INT_FIELD_MYSQL(*result_row, "parent_task_id", task_record.parent_task_id_, uint64_t);
    EXTRACT_INT_FIELD_MYSQL(*result_row, "tenant_id", task_record.tenant_id_, uint64_t);
    EXTRACT_INT_FIELD_MYSQL(*result_row, "object_id", task_record.object_id_, uint64_t);
    EXTRACT_INT_FIELD_MYSQL(*result_row, "schema_version", task_record.schema_version_, uint64_t);
    EXTRACT_INT_FIELD_MYSQL(*result_row, "target_object_id", task_record.target_object_id_, uint64_t);
    EXTRACT_INT_FIELD_MYSQL(*result_row, "ddl_type", task_record.ddl_type_, ObDDLType);
    EXTRACT_VARCHAR_FIELD_MYSQL(*result_row, "trace_id", trace_id_str);
    EXTRACT_INT_FIELD_MYSQL(*result_row, "status", task_record.task_status_, int64_t);
    EXTRACT_UINT_FIELD_MYSQL(*result_row, "snapshot_version", task_record.snapshot_version_, uint64_t);
    EXTRACT_INT_FIELD_MYSQL(*result_row, "task_version", task_record.task_version_, int64_t);
    EXTRACT_INT_FIELD_MYSQL(*result_row, "ret_code", task_record.ret_code_, int64_t);
    EXTRACT_INT_FIELD_MYSQL(*result_row, "execution_id", task_record.execution_id_, int64_t);
    EXTRACT_VARCHAR_FIELD_MYSQL(*result_row, "message_unhex", task_message);
    EXTRACT_VARCHAR_FIELD_MYSQL(*result_row, "ddl_stmt_str_unhex", ddl_stmt_str);
    if (OB_SUCC(ret)) {
      if (OB_FAIL(task_record.trace_id_.parse_from_buf(trace_id_str.ptr()))) {
        LOG_WARN("failed to parse trace id from buf", K(ret));
      } else if (!ddl_stmt_str.empty()) {
        if (OB_ISNULL(buf_ddl_stmt_str = static_cast<char *>(allocator.alloc(ddl_stmt_str.length())))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("allocate memory failed", K(ret));
        } else {
          MEMCPY(buf_ddl_stmt_str, ddl_stmt_str.ptr(), ddl_stmt_str.length());
          task_record.ddl_stmt_str_.assign(buf_ddl_stmt_str, ddl_stmt_str.length());
        }
      }
    }
    if (OB_SUCC(ret) && !task_message.empty()) {
      if (OB_ISNULL(buf_task_message = static_cast<char *>(allocator.alloc(task_message.length())))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("allocate memory failed", K(ret));
      } else {
        MEMCPY(buf_task_message, task_message.ptr(), task_message.length());
        task_record.message_.assign(buf_task_message, task_message.length());
      }
      if (OB_FAIL(ret) && nullptr != buf_ddl_stmt_str) {
        allocator.free(buf_ddl_stmt_str);
        buf_ddl_stmt_str = nullptr;
      }
      if (OB_FAIL(ret) && nullptr != buf_task_message) {
        allocator.free(buf_task_message);
        buf_task_message = nullptr;
      }
    }
  }
  return ret;
}

int64_t ObDDLTaskRecordOperator::get_record_id(share::ObDDLType ddl_type, int64_t origin_id)
{
  UNUSED(ddl_type);
  return origin_id;
}

int ObDDLTaskRecordOperator::select_for_update(
    common::ObMySQLTransaction &trans,
    const uint64_t tenant_id,
    const int64_t task_id,
    int64_t &task_status,
    int64_t &execution_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql_string;
  task_status = 0;
  execution_id = 0;
  if (OB_UNLIKELY(task_id <= 0 || tenant_id <= 0)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), K(tenant_id), K(task_id));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(sql_string.assign_fmt("SELECT status, execution_id FROM %s WHERE task_id = %lu FOR UPDATE",
          OB_ALL_DDL_TASK_STATUS_TNAME, task_id))) {
        LOG_WARN("assign sql string failed", K(ret), K(task_id), K(tenant_id));
      } else if (OB_FAIL(trans.read(res, tenant_id, sql_string.ptr()))) {
        LOG_WARN("update status of ddl task record failed", K(ret), K(sql_string));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get sql result", K(ret), KP(result));
      } else if (OB_FAIL(result->next())) {
        LOG_WARN("fail to get next row", K(ret));
      } else {
        EXTRACT_INT_FIELD_MYSQL(*result, "status", task_status, int64_t);
        EXTRACT_INT_FIELD_MYSQL(*result, "execution_id", execution_id, int64_t);
      }
    }
  }
  return ret;
}

} // end namespace rootserver
} // end namespace oceanbase
