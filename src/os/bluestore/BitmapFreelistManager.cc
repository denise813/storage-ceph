// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "BitmapFreelistManager.h"
#include "kv/KeyValueDB.h"
#include "os/kv.h"
#include "include/stringify.h"

#include "common/debug.h"

#define dout_context cct
#define dout_subsys ceph_subsys_bluestore
#undef dout_prefix
#define dout_prefix *_dout << "freelist "

void make_offset_key(uint64_t offset, std::string *key)
{
  key->reserve(10);
  _key_encode_u64(offset, key);
}

struct XorMergeOperator : public KeyValueDB::MergeOperator {
/** comment by hy 2020-04-24
 * # old_value不存在，那么new_value直接赋值为rdata
 */
  void merge_nonexistent(
    const char *rdata, size_t rlen, std::string *new_value) override {
    *new_value = std::string(rdata, rlen);
  }
/** comment by hy 2020-04-24
 * # old_value存在，则与rdata逐位异或xor
 */
  void merge(
    const char *ldata, size_t llen,
    const char *rdata, size_t rlen,
    std::string *new_value) override {
    ceph_assert(llen == rlen);
    *new_value = std::string(ldata, llen);
    for (size_t i = 0; i < rlen; ++i) {
      (*new_value)[i] ^= rdata[i];
    }
  }
  // We use each operator name and each prefix to construct the
  // overall RocksDB operator name for consistency check at open time.
  const char *name() const override {
    return "bitwise_xor";
  }
};

void BitmapFreelistManager::setup_merge_operator(KeyValueDB *db, string prefix)
{
  std::shared_ptr<XorMergeOperator> merge_op(new XorMergeOperator);
  db->set_merge_operator(prefix, merge_op);
}

BitmapFreelistManager::BitmapFreelistManager(CephContext* cct,
					     string meta_prefix,
					     string bitmap_prefix)
  : FreelistManager(cct),
    meta_prefix(meta_prefix),
    bitmap_prefix(bitmap_prefix),
    enumerate_bl_pos(0)
{
}

/** comment by hy 2020-02-27
 * # 通过mkfs接口创建Manager
 */
int BitmapFreelistManager::create(uint64_t new_size, uint64_t granularity,
				  KeyValueDB::Transaction txn)
{
  bytes_per_block = granularity;
  ceph_assert(isp2(bytes_per_block));
  size = p2align(new_size, bytes_per_block);
  blocks_per_key = cct->_conf->bluestore_freelist_blocks_per_key;
/** comment by hy 2020-04-24
 * # create/init 均会调用下面这个函数，初始化block/key的掩码
 */
  _init_misc();

  blocks = size_2_block_count(size);
  if (blocks * bytes_per_block > size) {
    dout(10) << __func__ << " rounding blocks up from 0x" << std::hex << size
	     << " to 0x" << (blocks * bytes_per_block)
	     << " (0x" << blocks << " blocks)" << std::dec << dendl;
    // set past-eof blocks as allocated
    _xor(size, blocks * bytes_per_block - size, txn);
  }
  dout(10) << __func__
	   << " size 0x" << std::hex << size
	   << " bytes_per_block 0x" << bytes_per_block
	   << " blocks 0x" << blocks
	   << " blocks_per_key 0x" << blocks_per_key
	   << std::dec << dendl;
/** comment by hy 2020-09-13
 * # 设立的前缀 为 B
 */
  {
    bufferlist bl;
    encode(bytes_per_block, bl);
    txn->set(meta_prefix, "bytes_per_block", bl);
  }
  {
    bufferlist bl;
    encode(blocks_per_key, bl);
    txn->set(meta_prefix, "blocks_per_key", bl);
  }
  {
    bufferlist bl;
    encode(blocks, bl);
    txn->set(meta_prefix, "blocks", bl);
  }
  {
    bufferlist bl;
    encode(size, bl);
    txn->set(meta_prefix, "size", bl);
  }
  return 0;
}

int BitmapFreelistManager::_expand(uint64_t old_size, KeyValueDB* db)
{
  assert(old_size < size);
  ceph_assert(isp2(bytes_per_block));

  KeyValueDB::Transaction txn;
  txn = db->get_transaction();

  auto blocks0 = size_2_block_count(old_size);
  if (blocks0 * bytes_per_block > old_size) {
    dout(10) << __func__ << " rounding1 blocks up from 0x" << std::hex
             << old_size << " to 0x" << (blocks0 * bytes_per_block)
	     << " (0x" << blocks0 << " blocks)" << std::dec << dendl;
    // reset past-eof blocks to unallocated
/** comment by hy 2020-06-22
 * # 新的空间设置bitmap 位,靠 merge 来进行疑惑
 */
    _xor(old_size, blocks0 * bytes_per_block - old_size, txn);
  }

  size = p2align(size, bytes_per_block);
  blocks = size_2_block_count(size);

  if (blocks * bytes_per_block > size) {
    dout(10) << __func__ << " rounding2 blocks up from 0x" << std::hex
             << size << " to 0x" << (blocks * bytes_per_block)
	     << " (0x" << blocks << " blocks)" << std::dec << dendl;
    // set past-eof blocks as allocated
    _xor(size, blocks * bytes_per_block - size, txn);
  }

  dout(10) << __func__
	   << " size 0x" << std::hex << size
	   << " bytes_per_block 0x" << bytes_per_block
	   << " blocks 0x" << blocks
	   << " blocks_per_key 0x" << blocks_per_key
	   << std::dec << dendl;
  {
    bufferlist bl;
    encode(blocks, bl);
    txn->set(meta_prefix, "blocks", bl);
  }
  {
    bufferlist bl;
    encode(size, bl);
    txn->set(meta_prefix, "size", bl);
  }
  db->submit_transaction_sync(txn);

  return 0;
}

/** comment by hy 2020-02-27
 * # 初始化
 */
int BitmapFreelistManager::read_size_meta_from_db(KeyValueDB* kvdb,
  uint64_t* res)
{
  bufferlist v;
  int r = kvdb->get(meta_prefix, "size", &v);
  if (r < 0) {
    derr << __func__ << " missing size meta in DB" << dendl;
    return ENOENT;
  } else {
    auto p = v.cbegin();
    decode(*res, p);
    r = 0;
  }
  return r;
}

void BitmapFreelistManager::_load_from_db(KeyValueDB* kvdb)
{
  KeyValueDB::Iterator it = kvdb->get_iterator(meta_prefix);
  it->lower_bound(string());

  // load meta
  while (it->valid()) {
    string k = it->key();
    if (k == "bytes_per_block") {
      bufferlist bl = it->value();
      auto p = bl.cbegin();
      decode(bytes_per_block, p);
      dout(10) << __func__ << " bytes_per_block 0x" << std::hex
        << bytes_per_block << std::dec << dendl;
    } else if (k == "blocks") {
      bufferlist bl = it->value();
      auto p = bl.cbegin();
      decode(blocks, p);
      dout(10) << __func__ << " blocks 0x" << std::hex << blocks << std::dec
        << dendl;
    } else if (k == "size") {
      bufferlist bl = it->value();
      auto p = bl.cbegin();
      decode(size, p);
      dout(10) << __func__ << " size 0x" << std::hex << size << std::dec
        << dendl;
    } else if (k == "blocks_per_key") {
      bufferlist bl = it->value();
      auto p = bl.cbegin();
      decode(blocks_per_key, p);
      dout(10) << __func__ << " blocks_per_key 0x" << std::hex << blocks_per_key
        << std::dec << dendl;
    } else {
      derr << __func__ << " unrecognized meta " << k << dendl;
    }
    it->next();
  }
}


int BitmapFreelistManager::init(const bluestore_bdev_label_t& label,
  KeyValueDB *kvdb,
  bool db_in_read_only)
{
  dout(1) << __func__ << dendl;
  int r = _init_from_label(label);
  if (r != 0) {
    dout(1) << __func__ << " fall back to legacy meta repo" << dendl;
    _load_from_db(kvdb);
  }
  _sync(kvdb, db_in_read_only);

  dout(10) << __func__ << std::hex
	   << " size 0x" << size
	   << " bytes_per_block 0x" << bytes_per_block
	   << " blocks 0x" << blocks
	   << " blocks_per_key 0x" << blocks_per_key
	   << std::dec << dendl;
  _init_misc();
  return 0;
}

int BitmapFreelistManager::_init_from_label(const bluestore_bdev_label_t& label)
{
  dout(1) << __func__ << dendl;

  int r = ENOENT;
  string err;

  auto it = label.meta.find("bfm_size");
  auto end = label.meta.end();
  if (it != end) {
    size = strict_iecstrtoll(it->second.c_str(), &err);
    if (!err.empty()) {
      derr << __func__ << " Failed to parse - "
        << it->first << ":" << it->second
        << ", error: " << err << dendl;
      return r;
    }
  } else {
    // this is expected for legacy deployed OSDs
    dout(0) << __func__ << " bfm_size not found in bdev meta" << dendl;
    return r;
  }

  it = label.meta.find("bfm_blocks");
  if (it != end) {
    blocks = strict_iecstrtoll(it->second.c_str(), &err);
    if (!err.empty()) {
      derr << __func__ << " Failed to parse - "
        << it->first << ":" << it->second
        << ", error: " << err << dendl;
      return r;
    }
  } else {
    derr << __func__ << " bfm_blocks not found in bdev meta" << dendl;
    return r;
  }

  it = label.meta.find("bfm_bytes_per_block");
  if (it != end) {
    bytes_per_block = strict_iecstrtoll(it->second.c_str(), &err);
    if (!err.empty()) {
      derr << __func__ << " Failed to parse - "
        << it->first << ":" << it->second
        << ", error: " << err << dendl;
      return r;
    }
  } else {
    derr << __func__ << " bfm_bytes_per_block not found in bdev meta" << dendl;
    return r;
  }
  it = label.meta.find("bfm_blocks_per_key");
  if (it != end) {
    blocks_per_key = strict_iecstrtoll(it->second.c_str(), &err);
    if (!err.empty()) {
      derr << __func__ << " Failed to parse - "
        << it->first << ":" << it->second
        << ", error: " << err << dendl;
      return r;
    }
  } else {
    derr << __func__ << " bfm_blocks_per_key not found in bdev meta" << dendl;
    return r;
  }
  r = 0;
  return 0;
}

void BitmapFreelistManager::_init_misc()
{
/** comment by hy 2020-04-22
 * # 128 >> 3 = 16，即一个key的value(段)
     对应128个block，每个block用1个bit表示，需要16字节
 */
  bufferptr z(blocks_per_key >> 3);
  memset(z.c_str(), 0xff, z.length());
  all_set_bl.clear();
  all_set_bl.append(z);

/** comment by hy 2020-04-22
 * # x FFFF FFFF FFFF F000
 */
  block_mask = ~(bytes_per_block - 1);

  bytes_per_key = bytes_per_block * blocks_per_key;
/** comment by hy 2020-04-22
 * # 0xFFFF FFFF FFF8 0000
 */
  key_mask = ~(bytes_per_key - 1);
  dout(10) << __func__ << std::hex << " bytes_per_key 0x" << bytes_per_key
	   << ", key_mask 0x" << key_mask << std::dec
	   << dendl;
}

void BitmapFreelistManager::sync(KeyValueDB* kvdb)
{
  _sync(kvdb, true);
}

void BitmapFreelistManager::_sync(KeyValueDB* kvdb, bool read_only)
{
  dout(10) << __func__ << " checks if size sync is needed" << dendl;
  uint64_t size_db = 0;
  int r = read_size_meta_from_db(kvdb, &size_db);
  ceph_assert(r >= 0);
/** comment by hy 2020-06-22
 * # 记录的 size 小于 设备空间
 */
  if (!read_only && size_db < size) {
    dout(1) << __func__ << " committing new size 0x" << std::hex << size
      << std::dec << dendl;
/** comment by hy 2020-06-22
 * # 更新容量
 */
    r = _expand(size_db, kvdb);
    ceph_assert(r == 0);
  } else if (size_db > size) {
    // this might hapen when OSD passed the following sequence:
    // upgrade -> downgrade -> expand -> upgrade
    // One needs to run expand once again to syncup
    dout(1) << __func__ << " fall back to legacy meta repo" << dendl;
    _load_from_db(kvdb);
  }
}

void BitmapFreelistManager::shutdown()
{
  dout(1) << __func__ << dendl;
}

void BitmapFreelistManager::enumerate_reset()
{
  std::lock_guard l(lock);
  enumerate_offset = 0;
  enumerate_bl_pos = 0;
  enumerate_bl.clear();
  enumerate_p.reset();
}

int get_next_clear_bit(bufferlist& bl, int start)
{
  const char *p = bl.c_str();
  int bits = bl.length() << 3;
  while (start < bits) {
    // byte = start / 8 (or start >> 3)
    // bit = start % 8 (or start & 7)
    unsigned char byte_mask = 1 << (start & 7);
    if ((p[start >> 3] & byte_mask) == 0) {
      return start;
    }
    ++start;
  }
  return -1; // not found
}

int get_next_set_bit(bufferlist& bl, int start)
{
  const char *p = bl.c_str();
  int bits = bl.length() << 3;
  while (start < bits) {
    int which_byte = start / 8;
    int which_bit = start % 8;
    unsigned char byte_mask = 1 << which_bit;
    if (p[which_byte] & byte_mask) {
      return start;
    }
    ++start;
  }
  return -1; // not found
}

bool BitmapFreelistManager::enumerate_next(KeyValueDB *kvdb, uint64_t *offset, uint64_t *length)
{
  std::lock_guard l(lock);

  // initial base case is a bit awkward
  if (enumerate_offset == 0 && enumerate_bl_pos == 0) {
    dout(10) << __func__ << " start" << dendl;
/** comment by hy 2020-11-18
 * # 根据 前缀获取 cf 获取 迭代器
 */
    enumerate_p = kvdb->get_iterator(bitmap_prefix);
/** comment by hy 2020-11-18
 * # 第一个元素
 */
    enumerate_p->lower_bound(string());
    // we assert that the first block is always allocated; it's true,
    // and it simplifies our lives a bit.
    ceph_assert(enumerate_p->valid());
    string k = enumerate_p->key();
    const char *p = k.c_str();
    _key_decode_u64(p, &enumerate_offset);
    enumerate_bl = enumerate_p->value();
    ceph_assert(enumerate_offset == 0);
/** comment by hy 2020-11-18
 * # 迭代器指向第一个元素
 */
    ceph_assert(get_next_set_bit(enumerate_bl, 0) == 0);
  }

  if (enumerate_offset >= size) {
    dout(10) << __func__ << " end" << dendl;
    return false;
  }

  // skip set bits to find offset
  while (true) {
/** comment by hy 2020-11-18
 * # 从0开始
 */
    enumerate_bl_pos = get_next_clear_bit(enumerate_bl, enumerate_bl_pos);
    if (enumerate_bl_pos >= 0) {
      *offset = _get_offset(enumerate_offset, enumerate_bl_pos);
      dout(30) << __func__ << " found clear bit, key 0x" << std::hex
	       << enumerate_offset << " bit 0x" << enumerate_bl_pos
	       << " offset 0x" << *offset
	       << std::dec << dendl;
      break;
    }
    dout(30) << " no more clear bits in 0x" << std::hex << enumerate_offset
	     << std::dec << dendl;
    enumerate_p->next();
    enumerate_bl.clear();
/** comment by hy 2020-11-18
 * # 第一个偏移
 */
    if (!enumerate_p->valid()) {
      enumerate_offset += bytes_per_key;
      enumerate_bl_pos = 0;
      *offset = _get_offset(enumerate_offset, enumerate_bl_pos);
      break;
    }
    string k = enumerate_p->key();
    const char *p = k.c_str();
    uint64_t next = enumerate_offset + bytes_per_key;
    _key_decode_u64(p, &enumerate_offset);
    enumerate_bl = enumerate_p->value();
    enumerate_bl_pos = 0;
    if (enumerate_offset > next) {
      dout(30) << " no key at 0x" << std::hex << next << ", got 0x"
	       << enumerate_offset << std::dec << dendl;
      *offset = next;
      break;
    }
  }

  // skip clear bits to find the end
/** comment by hy 2020-11-18
 * # 另一个情况,异常设置
 */
  uint64_t end = 0;
  if (enumerate_p->valid()) {
    while (true) {
      enumerate_bl_pos = get_next_set_bit(enumerate_bl, enumerate_bl_pos);
      if (enumerate_bl_pos >= 0) {
	end = _get_offset(enumerate_offset, enumerate_bl_pos);
	dout(30) << __func__ << " found set bit, key 0x" << std::hex
		 << enumerate_offset << " bit 0x" << enumerate_bl_pos
		 << " offset 0x" << end << std::dec
		 << dendl;
	end = std::min(get_alloc_units() * bytes_per_block, end);
	*length = end - *offset;
        dout(10) << __func__ << std::hex << " 0x" << *offset << "~" << *length
		 << std::dec << dendl;
	return true;
      }
      dout(30) << " no more set bits in 0x" << std::hex << enumerate_offset
	       << std::dec << dendl;
      enumerate_p->next();
      enumerate_bl.clear();
      enumerate_bl_pos = 0;
      if (!enumerate_p->valid()) {
	break;
      }
      string k = enumerate_p->key();
      const char *p = k.c_str();
      _key_decode_u64(p, &enumerate_offset);
      enumerate_bl = enumerate_p->value();
    }
  }

  if (enumerate_offset < size) {
    end = get_alloc_units() * bytes_per_block;
    *length = end - *offset;
    dout(10) << __func__ << std::hex << " 0x" << *offset << "~" << *length
	     << std::dec << dendl;
    enumerate_offset = size;
    enumerate_bl_pos = blocks_per_key;
    return true;
  }

  dout(10) << __func__ << " end" << dendl;
  return false;
}

void BitmapFreelistManager::dump(KeyValueDB *kvdb)
{
  enumerate_reset();
  uint64_t offset, length;
  while (enumerate_next(kvdb, &offset, &length)) {
    dout(20) << __func__ << " 0x" << std::hex << offset << "~" << length
	     << std::dec << dendl;
  }
}

void BitmapFreelistManager::_verify_range(KeyValueDB *kvdb,
					  uint64_t offset, uint64_t length,
					  int val)
{
  unsigned errors = 0;
  uint64_t first_key = offset & key_mask;
  uint64_t last_key = (offset + length - 1) & key_mask;
  if (first_key == last_key) {
    string k;
    make_offset_key(first_key, &k);
    bufferlist bl;
    kvdb->get(bitmap_prefix, k, &bl);
    if (bl.length() > 0) {
      const char *p = bl.c_str();
      unsigned s = (offset & ~key_mask) / bytes_per_block;
      unsigned e = ((offset + length - 1) & ~key_mask) / bytes_per_block;
      for (unsigned i = s; i <= e; ++i) {
	int has = !!(p[i >> 3] & (1ull << (i & 7)));
	if (has != val) {
	  derr << __func__ << " key 0x" << std::hex << first_key << " bit 0x"
	       << i << " has 0x" << has << " expected 0x" << val
	       << std::dec << dendl;
	  ++errors;
	}
      }
    } else {
      if (val) {
	derr << __func__ << " key 0x" << std::hex << first_key
	     << " not present, expected 0x" << val << std::dec << dendl;
	++errors;
      }
    }
  } else {
    // first key
    {
      string k;
      make_offset_key(first_key, &k);
      bufferlist bl;
      kvdb->get(bitmap_prefix, k, &bl);
      if (bl.length()) {
	const char *p = bl.c_str();
	unsigned s = (offset & ~key_mask) / bytes_per_block;
	unsigned e = blocks_per_key;
	for (unsigned i = s; i < e; ++i) {
	  int has = !!(p[i >> 3] & (1ull << (i & 7)));
	  if (has != val) {
	    derr << __func__ << " key 0x" << std::hex << first_key << " bit 0x"
		 << i << " has 0x" << has << " expected 0x" << val << std::dec
		 << dendl;
	    ++errors;
	  }
	}
      } else {
	if (val) {
	  derr << __func__ << " key 0x" << std::hex << first_key
	       << " not present, expected 0x" << val << std::dec << dendl;
	  ++errors;
	}
      }
      first_key += bytes_per_key;
    }
    // middle keys
    if (first_key < last_key) {
      while (first_key < last_key) {
	string k;
	make_offset_key(first_key, &k);
	bufferlist bl;
	kvdb->get(bitmap_prefix, k, &bl);
	if (bl.length() > 0) {
	  const char *p = bl.c_str();
	  for (unsigned i = 0; i < blocks_per_key; ++i) {
	    int has = !!(p[i >> 3] & (1ull << (i & 7)));
	    if (has != val) {
	      derr << __func__ << " key 0x" << std::hex << first_key << " bit 0x"
		   << i << " has 0x" << has << " expected 0x" << val
		   << std::dec << dendl;
	      ++errors;
	    }
	  }
	} else {
	  if (val) {
	    derr << __func__ << " key 0x" << std::hex << first_key
		 << " not present, expected 0x" << val << std::dec << dendl;
	    ++errors;
	  }
	}
	first_key += bytes_per_key;
      }
    }
    ceph_assert(first_key == last_key);
    {
      string k;
      make_offset_key(first_key, &k);
      bufferlist bl;
      kvdb->get(bitmap_prefix, k, &bl);
      if (bl.length() > 0) {
	const char *p = bl.c_str();
	unsigned e = ((offset + length - 1) & ~key_mask) / bytes_per_block;
	for (unsigned i = 0; i < e; ++i) {
	  int has = !!(p[i >> 3] & (1ull << (i & 7)));
	  if (has != val) {
	    derr << __func__ << " key 0x" << std::hex << first_key << " bit 0x"
		 << i << " has 0x" << has << " expected 0x" << val << std::dec
		 << dendl;
	    ++errors;
	  }
	}
      } else {
	if (val) {
	  derr << __func__ << " key 0x" << std::hex << first_key
	       << " not present, expected 0x" << val << std::dec << dendl;
	  ++errors;
	}
      }
    }
  }
  if (errors) {
    derr << __func__ << " saw " << errors << " errors" << dendl;
    ceph_abort_msg("bitmap freelist errors");
  }
}

/** comment by hy 2020-02-27
 * # 分配指定范围
 */
void BitmapFreelistManager::allocate(
  uint64_t offset, uint64_t length,
  KeyValueDB::Transaction txn)
{
  dout(10) << __func__ << " 0x" << std::hex << offset << "~" << length
	   << std::dec << dendl;
  _xor(offset, length, txn);
}

/** comment by hy 2020-02-27
 * # 释放指定范围
 */
void BitmapFreelistManager::release(
  uint64_t offset, uint64_t length,
  KeyValueDB::Transaction txn)
{
  dout(10) << __func__ << " 0x" << std::hex << offset << "~" << length
	   << std::dec << dendl;
/** comment by hy 2020-06-22
 * # 
 */
  _xor(offset, length, txn);
}

void BitmapFreelistManager::_xor(
  uint64_t offset, uint64_t length,
  KeyValueDB::Transaction txn)
{
  // must be block aligned
/** comment by hy 2020-04-22
 * # offset和length都是以block边界对齐
 */
  ceph_assert((offset & block_mask) == offset);
  ceph_assert((length & block_mask) == length);

  uint64_t first_key = offset & key_mask;
  uint64_t last_key = (offset + length - 1) & key_mask;
  dout(20) << __func__ << " first_key 0x" << std::hex << first_key
	   << " last_key 0x" << last_key << std::dec << dendl;

/** comment by hy 2020-04-22
 * # key可以理解为段
     最简单的case，此次操作对应一个段
 */
  if (first_key == last_key) {
/** comment by hy 2020-04-22
 * # 16字节大小的buffer
 */
    bufferptr p(blocks_per_key >> 3);
    p.zero();
/** comment by hy 2020-04-22
 * # 段内开始block的编号
 */
    unsigned s = (offset & ~key_mask) / bytes_per_block;
/** comment by hy 2020-04-22
 * # 段内结束block的编号
 */
    unsigned e = ((offset + length - 1) & ~key_mask) / bytes_per_block;
    for (unsigned i = s; i <= e; ++i) {
/** comment by hy 2020-04-22
 * # 生成此次操作的掩码
     i>>3 定位block对应位的字节，
     1ull<<(i&7)定位bit，然后异或将位设置位1
 */
      p[i >> 3] ^= 1ull << (i & 7);
    }
    string k;
/** comment by hy 2020-04-22
 * # 将内存内容转换为 进制的字符
 */
    make_offset_key(first_key, &k);
    bufferlist bl;
    bl.append(p);
    dout(30) << __func__ << " 0x" << std::hex << first_key << std::dec << ": ";
    bl.hexdump(*_dout, false);
    *_dout << dendl;
/** comment by hy 2020-04-22
 * # 和目前的value进行异或操作
     mergeop= XorMergeOperator
 */
    txn->merge(bitmap_prefix, k, bl);
  } else {
    // first key
/** comment by hy 2020-04-22
 * # 对应多个段，分别处理第一个段，中间段，
     和最后一个段，首尾两个段和前面情况一样
 */
/** comment by hy 2020-04-22
 * # 第一个段
 */
    {
      bufferptr p(blocks_per_key >> 3);
      p.zero();
      unsigned s = (offset & ~key_mask) / bytes_per_block;
      unsigned e = blocks_per_key;
/** comment by hy 2020-06-22
 * # 标记 key 全部数据为1
 */
      for (unsigned i = s; i < e; ++i) {
	p[i >> 3] ^= 1ull << (i & 7);
      }
      string k;
      make_offset_key(first_key, &k);
      bufferlist bl;
      bl.append(p);
      dout(30) << __func__ << " 0x" << std::hex << first_key << std::dec << ": ";
      bl.hexdump(*_dout, false);
      *_dout << dendl;
      txn->merge(bitmap_prefix, k, bl);
/** comment by hy 2020-04-22
 * # 增加key，定位下一个段
 */
      first_key += bytes_per_key;
    }
    // middle keys
/** comment by hy 2020-04-22
 * # 中间段，此时掩码就是全1，所以用all_set_bl
 */
    while (first_key < last_key) {
      string k;
      make_offset_key(first_key, &k);
      dout(30) << __func__ << " 0x" << std::hex << first_key << std::dec
      	 << ": ";
      all_set_bl.hexdump(*_dout, false);
      *_dout << dendl;
/** comment by hy 2020-04-24
 * # 和目前的value进行异或操作，all_set_bl
 */
      txn->merge(bitmap_prefix, k, all_set_bl);
      first_key += bytes_per_key;
    }
/** comment by hy 2020-06-22
 * # 尾巴部分
 */
    ceph_assert(first_key == last_key);
    {
      bufferptr p(blocks_per_key >> 3);
      p.zero();
      unsigned e = ((offset + length - 1) & ~key_mask) / bytes_per_block;
      for (unsigned i = 0; i <= e; ++i) {
	p[i >> 3] ^= 1ull << (i & 7);
      }
      string k;
      make_offset_key(first_key, &k);
      bufferlist bl;
      bl.append(p);
      dout(30) << __func__ << " 0x" << std::hex << first_key << std::dec << ": ";
      bl.hexdump(*_dout, false);
      *_dout << dendl;
/** comment by hy 2020-04-24
 * # 和目前的value进行异或操作
 */
      txn->merge(bitmap_prefix, k, bl);
    }
  }
}

uint64_t BitmapFreelistManager::size_2_block_count(uint64_t target_size) const
{
  auto target_blocks = target_size / bytes_per_block;
  if (target_blocks / blocks_per_key * blocks_per_key != target_blocks) {
    target_blocks = (target_blocks / blocks_per_key + 1) * blocks_per_key;
  }
  return target_blocks;
}

void BitmapFreelistManager::get_meta(
  uint64_t target_size,
  std::vector<std::pair<string, string>>* res) const
{
  if (target_size == 0) {
    res->emplace_back("bfm_blocks", stringify(blocks));
    res->emplace_back("bfm_size", stringify(size));
  } else {
    target_size = p2align(target_size, bytes_per_block);
    auto target_blocks = size_2_block_count(target_size);

    res->emplace_back("bfm_blocks", stringify(target_blocks));
    res->emplace_back("bfm_size", stringify(target_size));
  }
  res->emplace_back("bfm_bytes_per_block", stringify(bytes_per_block));
  res->emplace_back("bfm_blocks_per_key", stringify(blocks_per_key));
}
