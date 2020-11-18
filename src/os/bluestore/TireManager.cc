// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "FreelistManager.h"
#include "BitmapFreelistManager.h"

TireFreelistManager::TireFreelistManager(CephContext* cct,
		       std::string meta_prefix,
		       std::string info_prefix)
{

}

static void TireFreelistManager::setup_merge_operator(KeyValueDB *db,
    std::string prefix){
}

int TireFreelistManager::create(uint64_t size,
	     uint64_t granularity,
	     KeyValueDB::Transaction txn){

  // 下盘 磁盘大小
  bufferlist bl;
  encode(size, bl);
  txn->set(meta_prefix, "size", bl);
  // 下盘 最小分配大小

  // 下盘 zone 信息 
  // 一个
}

int TireFreelistManager::init(const bluestore_bdev_label_t& label,
    KeyValueDB *kvdb,
    bool db_in_read_only) {

  dout(1) << __func__ << dendl;
  bfm = new BitmapFreelistManager(cct, "B", "b");
  int r = bfm->_init_from_label(label);
  if (r != 0) {
    dout(1) << __func__ << " fall back to legacy meta repo" << dendl;
    bfm->_load_from_db(kvdb);
  }
  bfm->_sync(kvdb, db_in_read_only);

  dout(10) << __func__ << std::hex
	   << " size 0x" << size
	   << " bytes_per_block 0x" << bytes_per_block
	   << " blocks 0x" << blocks
	   << " blocks_per_key 0x" << blocks_per_key
	   << std::dec << dendl;
  bfm->_init_misc();
}

void TireFreelistManager::shutdown()
{
   bfm->shutdown();
   delete bfm;
}
void TireFreelistManager::sync(KeyValueDB* kvdb)
{
  bfm->sync(kvdb);
}
void TireFreelistManager::dump(KeyValueDB *kvdb)
{
  bfm->dump(kvdb);
}

void TireFreelistManager::enumerate_reset()
{
  bfm->enumerate_reset();
}

bool TireFreelistManager::enumerate_next(KeyValueDB *kvdb,
		      uint64_t *offset,
		      uint64_t *length)
{
  bfm->enumerate_next (kvdb, offset, length);
}

void TireFreelistManager::allocate(uint64_t offset,
		uint64_t length,
		KeyValueDB::Transaction txn)
{
}

void TireFreelistManager::release(uint64_t offset,
	       uint64_t length,
	       KeyValueDB::Transaction txn)
{
  
}

void TireFreelistManager::get_meta(uint64_t target_size,
    std::vector<std::pair<string, string>>* res)
{
  bfm->get_meta(target_size, res);
}

