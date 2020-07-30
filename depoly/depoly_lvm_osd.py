import os
from depoly_tools import exec_cmd

target=exec_cmd("hostname")
hdds=('sda', 'sdb', 'sdc', 'sdd', 'sde', 'sdf', 'sdh', 'sdi', 'sdj', 'sdk')
nvmes=('nvme0n1', 'nvme1n1')

devices={"hdds": hdds, 'nvmes':nvmes}
cluster="ceph"

# ceph-base-15.2.4-0.el8.x86_64.rpm
# ceph-common-15.2.4-0.el8.x86_64.rpm
# ceph-osd-15.2.4-0.el8.x86_64.rpm
# librados2-15.2.4-0.el8.x86_64.rpm
# libradosstriper1-15.2.4-0.el8.x86_64.rpm
# python3-ceph-argparse-15.2.4-0.el8.x86_64.rpm
# 67468 Jul 24 04:43 python3-ceph-common-15.2.4-0.el8.x86_64.rpm
# python3-rados-15.2.4-0.el8.x86_64.rpm
# python3-rbd-15.2.4-0.el8.x86_64.rpm

# 清理磁盘 osd磁盘
for disk in devices['hdds']:
    exec_cmd( "ceph-volume lvm zap /dev/{disk_name} --destroy".format(disk_name=disk) )

for disk in devices['nvmes']:
    exec_cmd( "ceph-volume lvm zap /dev/{disk_name} --destroy".format(disk_name=disk) )

# 元数据进行分区
for disk in devices['hdds']:
    exec_cmd( "pvcreate /dev/{disk_name}".format(disk_name=disk) )

for disk in devices['nvmes']:
    exec_cmd( "pvcreate /dev/{disk_name}".format(disk_name=disk) )

# 检查创建成功
exec_cmd("pvs")

#创建卷组
for i in range( len(devices['hdds']) ):
    disk=devices['hdds'][i]
    exec_cmd( "vgcreate ceph_vg_block_{disk_name} /dev/{disk_name}".format(disk_name=disk) )

for i in range( len(devices['nvmes']) ):
    disk=devices['nvmes'][i]
    exec_cmd( "vgcreate ceph_vg_db_{disk_name} /dev/{disk_name}".format(disk_name=disk) )
# 检查创建成功
exec_cmd("vgdisplay")

ready_disks=[]

# 创建逻辑卷
group_num=2
for i in range( len(devices['hdds']) ):
    disksinfo={}
    group = i % group_num
    disk_nvme = devices['nvme'][group]
    disk_hdd = devices['hdds'][i]

    capacity = 100 * len(devices['nvmes']) / ( len(devices['hdds']) - (i % len(devices['nvmes'])) )
    exec_cmd( "lvcreate -l 100%FREE -n lv_{group_name}_{osd_id} ceph_vg_block_{disk_name}".format(
        disk_name=disk_hdd,
        group_name=group,
        osd_id=i) )
    exec_cmd( "lvcreate -L {disk_capacity}%FREE -n lv_{group_name}_{osd_id} ceph_vg_db_{disk_name}".format(
        disk_capacity=capacity,
        disk_name=disk_nvme,
        group_name=group,
        osd_id=i) )


    # 记录
    disksinfo['uuid'] = exec_cmd("uuidgen")
    disksinfo['osd_id'] = i
    disksinfo['group'] = group
    disksinfo['hdd'] = devices['hdds'][i]
    disksinfo['nvme']=disk_nvme
    ready_disks.append(disksinfo)

# 检查创建成功
exec_cmd("lvdisplay")

# 准备 osd磁盘
for disk in ready_disks:
    exec_cmd("ceph-volume lvm prepare --bluestore "
        "--data ceph_vg_block_{hdd_name}/lv_{group_name}_{osd_id} "
        "--block.db ceph_vg_db_{nvme_name}/lv_{group_name}_{osd_id} "
        "".format(
            hdd_name=disk['hdd'],
            group_name=disk['group'], 
            osd_id=disk['osd_id'],
            nvme_name=disk['nvme']) )
    exec_cmd("sleep 1")

# 激活 osd磁盘
# ceph-volume lvm activate --bluestore {osd_id} {uuid}
exec_cmd("ceph-volume lvm activate --bluestore --all")
