import re
import os
from depoly_tools import remote_exec_cmd
from depoly_tools import local_exec_cmd

target=remote_exec_cmd("hostname")
data_disks=['sda', 'sdb', 'sdc', 'sdd', 'sde', 'sdf', 'sdh', 'sdi', 'sdj', 'sdk']
meta_disks=['nvme0n1', 'nvme1n1']

devices={'data':data_disks, 'meta':meta_disks, 'cache':cache_disks}

osd_id = 0
disk_id = 0
data_disk_len = len(devices['data'])
meta_disk_len = len(devices['meta'])
cache_disk_len = len(devices['cache'])
cache_zone_len = data_disk_len // cache_disk_len

# 关闭osd 服务
for i in range(data_disk_len):
    osd_id = i;
    remote_exec_cmd("ceph osd down osd.{osd_id}".format(osd_id=osd_id))
    remote_exec_cmd("ceph osd out osd.{osd_id}".format(osd_id=osd_id))
    remote_exec_cmd("ceph osd crush remove osd.{osd_id}".format(osd_id=osd_id))
    remote_exec_cmd("ceph auth del osd.{osd_id}".format(osd_id=osd_id))
    remote_exec_cmd("ceph osd rm osd.{osd_id}".format(osd_id=osd_id))
    remote_exec_cmd("systemctl stop ceph-osd@{osd_id}".format(osd_id=osd_id))
    local_exec_cmd("sleep 1")
    remote_exec_cmd("systemctl disable ceph-osd@{osd_id}".format(osd_id=osd_id))
    local_exec_cmd("sleep 2")

osd_id = 0
disk_id = 0
for i in range(data_disk_len):
    osd_id = i
    remote_exec_cmd("rm -rf /var/lib/ceph/osd/ceph-{osd_id}".format(osd_id=osd_id))
    local_exec_cmd("sleep 10")

# 清理 缓存盘
disk_id = 0
for i in range(cache_disk_len):
    cset_out = remote_exec_cmd('bcache-super-show /dev/{disk_name}|grep "cset.uuid" '.format(
        disk_name=devices['cache'][i]))
    tmp_cset_uuid = re.findall(r'\S+-\S+-\S+-\S+-*', cset_out)
    cset_uuid = tmp_cset_uuid[0]
    for index in range(cache_zone_len):
        remote_exec_cmd('echo "{uuid}" >/sys/block/bcache{cache_id}/bcache/detach'.format(
            uuid=cset_uuid, cache_id=disk_id))
        disk_id = disk_id + 1
        local_exec_cmd("sleep 2")

    remote_exec_cmd('echo 1 > /sys/fs/bcache/{uuid}/unregister'.format(uuid=cset_uuid))
    local_exec_cmd("sleep 2")
    remote_exec_cmd('wipefs -a /dev/{disk_name}'.format(disk_name=devices['cache'][i]))

# 清理数据
disk_id = 0
for i in range(data_disk_len):
    if cache_disk_len != 0:
        remote_exec_cmd('echo 1 > /sys/block/bcache{cache_id}/bcache/stop'.format(cache_id=disk_id))
    local_exec_cmd("sleep 2")
    remote_exec_cmd('wipefs -a /dev/{disk_name}'.format(disk_name=devices['data'][i]))
    remote_exec_cmd( "/usr/bin/dd if=/dev/zero of=/dev/{disk_name} bs=1M count=10 conv=fsync".format(
        disk_name=devices['data'][i]))
    disk_id = disk_id + 1

# 清理元数据
for i in range(meta_disk_len):
    remote_exec_cmd( "/usr/bin/dd if=/dev/zero of=/dev/{disk_name} bs=1M count=10 conv=fsync".format(
        disk_name=devices['meta'][i]) )
    local_exec_cmd("sleep 5")

