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

#include <gtest/gtest.h>
#define protected public
#define private public
#include "storage/checkpoint/ob_freeze_checkpoint.h"
#include "storage/checkpoint/ob_checkpoint_executor.h"
#include "storage/ls/ob_ls.h"
#include "storage/checkpoint/ob_data_checkpoint.h"
#include "mtlenv/mock_tenant_module_env.h"
#include "share/rc/ob_tenant_base.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "logservice/ob_log_base_type.h"
#include "storage/init_basic_struct.h"
#include "storage/mock_ob_log_handler.h"
#include "observer/ob_safe_destroy_thread.h"


using namespace oceanbase;
using namespace oceanbase::lib;
using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace oceanbase::obrpc;


namespace oceanbase
{
namespace storage
{

using namespace checkpoint;

int ObTableHandleV2::set_table(
    ObITable *table,
    ObTenantMetaMemMgr *t3m,
    const ObITable::TableType table_type)
{
  int ret = OB_SUCCESS;
  reset();
  table_ = table;
  table_->inc_ref();
  t3m_ = t3m;
  table_type_ = table_type;
  return ret;
}

void ObTableHandleV2::reset()
{
  if (nullptr != table_) {
    if (nullptr == t3m_) {
    } else {
      if (0 == table_->dec_ref()) {
        // just for debug
        if (ObITable::TX_DATA_MEMTABLE == table_type_ || ObITable::TX_CTX_MEMTABLE == table_type_) {
          STORAGE_LOG(INFO, "push memtable into gc queue", K(table_type_), K(lbt()), KPC(this));
        }
        t3m_->push_table_into_gc_queue(table_, table_type_);
      }
      table_ = nullptr;
      t3m_ = nullptr;
    }
  }
}

int ObFreezer::get_ls_weak_read_ts(int64_t &weak_read_ts)
{
  int ret = OB_SUCCESS;
  weak_read_ts = 100;
  return ret;
}

class TestMemtable : public memtable::ObMemtable
{
public:
  TestMemtable()
    : rec_log_ts_(INT64_MAX),
      ready_for_flush_(true),
      is_frozen_(true)
  {}
  ~TestMemtable()
  {
    ObFreezeCheckpoint::remove_from_data_checkpoint(true);
  }

  void set_rec_log_ts(int64_t rec_log_ts)
  {
    if (rec_log_ts < rec_log_ts_) {
      rec_log_ts_ = rec_log_ts;
      check_can_move_to_active();
    }
  }

  int64_t get_rec_log_ts() { return rec_log_ts_; }

  bool rec_log_ts_is_stable() { return true; }

  bool ready_for_flush() { return ready_for_flush_; }

  bool is_frozen_checkpoint() const { return is_frozen_; }

  bool is_active_checkpoint() const { return !is_frozen_; }

  ObTabletID get_tablet_id() const { return ObTabletID(111111111111); }

  void set_is_frozen(bool is_frozen) { is_frozen_ = is_frozen; }

  void set_ready_for_flush(bool ready) { ready_for_flush_ = ready; }

  int flush(share::ObLSID ls_id)
  {
    UNUSED(ls_id);
    return OB_SUCCESS;
  }

private:
  int64_t rec_log_ts_;
  bool ready_for_flush_;
  bool is_frozen_;
};

class TestService : public logservice::ObICheckpointSubHandler
{
public:
  TestService()
    : rec_log_ts_(INT64_MAX)
  {}
  int64_t get_rec_log_ts() override { return rec_log_ts_; }

  int flush(int64_t rec_log_ts) override
  {
    if (rec_log_ts_ <= rec_log_ts) {
      rec_log_ts_ = rec_log_ts_ + 2;
    }
    return OB_SUCCESS;
  }
private:
  int64_t rec_log_ts_;
};

class TestDataCheckpoint : public ::testing::Test
{
public:
  TestDataCheckpoint();
  virtual ~TestDataCheckpoint() = default;

  virtual void SetUp() override;
  virtual void TearDown() override;
  static void SetUpTestCase();
  static void TearDownTestCase();

private:
  int64_t tenant_id_;

};

TestDataCheckpoint::TestDataCheckpoint() {}

void TestDataCheckpoint::SetUp()
{
  tenant_id_ = MTL_ID();
}
void TestDataCheckpoint::TearDown()
{
}
void TestDataCheckpoint::SetUpTestCase()
{
  EXPECT_EQ(OB_SUCCESS, MockTenantModuleEnv::get_instance().init());
  SAFE_DESTROY_INSTANCE.init();
  SAFE_DESTROY_INSTANCE.start();
  ObServerCheckpointSlogHandler::get_instance().is_started_ = true;
}

void TestDataCheckpoint::TearDownTestCase()
{
  SAFE_DESTROY_INSTANCE.stop();
  SAFE_DESTROY_INSTANCE.wait();
  SAFE_DESTROY_INSTANCE.destroy();
  MockTenantModuleEnv::get_instance().destroy();
}

TEST_F(TestDataCheckpoint, dlink_base)
{
  EXPECT_EQ(OB_SYS_TENANT_ID, MTL_ID());
  ObCreateLSArg arg;
  ObLSHandle handle;
  ObLSID ls_id(100);
  ASSERT_EQ(OB_SUCCESS, gen_create_ls_arg(tenant_id_, ls_id, arg));
  ASSERT_EQ(OB_SUCCESS, MTL(ObLSService*)->create_ls(arg));
  EXPECT_EQ(OB_SUCCESS, MTL(ObLSService*)->get_ls(ls_id, handle, ObLSGetMod::STORAGE_MOD));
  ObLS * ls = handle.get_ls();
  ASSERT_NE(nullptr, ls);
  checkpoint::ObDataCheckpoint *data_checkpoint = ls->get_data_checkpoint();
  TestMemtable memtable1;
  memtable1.add_to_data_checkpoint(data_checkpoint);
  TestMemtable memtable2;
  memtable2.add_to_data_checkpoint(data_checkpoint);
  TestMemtable memtable3;
  memtable3.add_to_data_checkpoint(data_checkpoint);
  ASSERT_EQ(3, data_checkpoint->new_create_list_.checkpoint_list_.get_size());
  memtable1.set_rec_log_ts(1);
  memtable2.set_rec_log_ts(2);
  memtable3.set_rec_log_ts(3);
  ASSERT_EQ(0, data_checkpoint->new_create_list_.checkpoint_list_.get_size());
  ASSERT_EQ(3, data_checkpoint->active_list_.checkpoint_list_.get_size());

  // for exist
  handle.reset();
  //ASSERT_EQ(OB_SUCCESS, ls->disable_palf(true));
  ASSERT_EQ(OB_SUCCESS, MTL(ObLSService*)->remove_ls(ls_id, false));
}

TEST_F(TestDataCheckpoint, ls_freeze)
{
  EXPECT_EQ(OB_SYS_TENANT_ID, MTL_ID());
  ObCreateLSArg arg;
  ObLSHandle handle;
  ObLSID ls_id(101);
  ASSERT_EQ(OB_SUCCESS, gen_create_ls_arg(tenant_id_, ls_id, arg));
  ASSERT_EQ(OB_SUCCESS, MTL(ObLSService*)->create_ls(arg));
  EXPECT_EQ(OB_SUCCESS, MTL(ObLSService*)->get_ls(ls_id, handle, ObLSGetMod::STORAGE_MOD));
  ObLS * ls = handle.get_ls();
  ASSERT_NE(nullptr, ls);
  ObDataCheckpoint *data_checkpoint = ls->get_data_checkpoint();

  TestMemtable memtable1;
  memtable1.add_to_data_checkpoint(data_checkpoint);
  TestMemtable memtable2;
  memtable2.add_to_data_checkpoint(data_checkpoint);
  TestMemtable memtable3;
  memtable3.add_to_data_checkpoint(data_checkpoint);
  ASSERT_EQ(3, data_checkpoint->new_create_list_.checkpoint_list_.get_size());
  memtable1.set_rec_log_ts(1);
  memtable2.set_rec_log_ts(2);
  memtable3.set_rec_log_ts(3);
  memtable3.set_ready_for_flush(false);
  memtable3.set_is_frozen(false);

  TestService service1;
  service1.rec_log_ts_ = 4;
  ObCheckpointExecutor *checkpoint_executor = ls->get_checkpoint_executor();
  checkpoint_executor->register_handler(TIMESTAMP_LOG_BASE_TYPE, &service1);
  MockObLogHandler mock_log_handler_;
  ASSERT_EQ(OB_SUCCESS, checkpoint_executor->init(ls, &mock_log_handler_));
  checkpoint_executor->start();

  ASSERT_EQ(OB_SUCCESS, checkpoint_executor->advance_checkpoint_by_flush(2));
  ASSERT_EQ(OB_SUCCESS, ls->get_data_checkpoint()->flush(INT64_MAX, false));
  usleep(60L * 1000L);  // 60ms
  ASSERT_EQ(0, data_checkpoint->new_create_list_.checkpoint_list_.get_size());
  ASSERT_EQ(1, data_checkpoint->active_list_.checkpoint_list_.get_size());
  ASSERT_EQ(2, data_checkpoint->prepare_list_.checkpoint_list_.get_size());
  ASSERT_EQ(true, data_checkpoint->ls_freeze_finished());

  ASSERT_EQ(OB_SUCCESS, checkpoint_executor->advance_checkpoint_by_flush(4));
  ASSERT_EQ(OB_SUCCESS, ls->get_data_checkpoint()->flush(INT64_MAX, false));
  usleep(60L * 1000L);  // 60ms
  ASSERT_EQ(2, data_checkpoint->prepare_list_.checkpoint_list_.get_size());
  ASSERT_EQ(6, service1.rec_log_ts_);

  // for exist
  handle.reset();
  //ASSERT_EQ(OB_SUCCESS, ls->disable_palf(true));
  ASSERT_EQ(OB_SUCCESS, MTL(ObLSService*)->remove_ls(ls_id, false));
}

}
}

int main(int argc, char **argv)
{
  oceanbase::common::ObLogger::get_logger().set_file_name("test_data_checkpoint.log", true);
  oceanbase::common::ObLogger::get_logger().set_log_level("INFO");
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
