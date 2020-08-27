import os
from depoly_tools import exec_cmd
from depoly_tools import rpm_top_dir
from depoly_tools import rpm_version

target=exec_cmd("hostname")
hdds=('sda', 'sdb', 'sdc', 'sdd', 'sde', 'sdf', 'sdh', 'sdi', 'sdj', 'sdk')
nvmes=('nvme0n1', 'nvme1n1')

devices={"hdds": hdds, 'nvmes':nvmes}
cluster="ceph"

bin_dir= rpm_top_dir
'''
exec_cmd("yum install -y {rpm_dir}/ceph-osd-{version} {rpm_dir}/ceph-common-{version} "
    "{rpm_dir}/ceph-base-{version} "
    "{rpm_dir}/librados2-{version} "
    "{rpm_dir}/libradosstriper1-{version} "
    "{rpm_dir}/librbd1-{version} "
    "{rpm_dir}/python3-ceph-argparse-{version} "
    "{rpm_dir}/python3-rados-{version} "
    "{rpm_dir}/python3-rbd-{version}".format(version=rpm_version, rpm_dir=bin_dir) )
'''

# 清除磁盘格式
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
lv_num = len(devices['hdds']) / len(devices['nvmes'])
for i in range( len(devices['hdds']) ):
    disksinfo={}
    group = i % len(devices['nvmes'])
    disk_nvme = devices['nvmes'][group]
    disk_hdd = devices['hdds'][i]

    capacity = 100 / (lv_num - (i % lv_num) )
    exec_cmd( "lvcreate -l 100%FREE -n lv_{group_name}_{osd_id} ceph_vg_block_{disk_name}".format(
        disk_name=disk_hdd,
        group_name=group,
        osd_id=i) )
    exec_cmd( "lvcreate -L 20G -n lv_wal_{group_name}_{osd_id} ceph_vg_db_{disk_name}".format(
        disk_capacity=capacity,
        disk_name=disk_nvme,
        group_name=group,
        osd_id=i) )
    exec_cmd( "lvcreate -L 300G -n lv_db_{group_name}_{osd_id} ceph_vg_db_{disk_name}".format(
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

#exec_cmd("rm -rf /usr/lib64/ceph/")
# 准备 osd磁盘
for disk in ready_disks:
    exec_cmd("ceph-volume lvm prepare --bluestore "
        "--data ceph_vg_block_{hdd_name}/lv_{group_name}_{osd_id} "
        "--block.wal ceph_vg_db_{nvme_name}/lv_wal_{group_name}_{osd_id} "
        "--block.db ceph_vg_db_{nvme_name}/lv_db_{group_name}_{osd_id} "

        "".format(
            hdd_name=disk['hdd'],
            group_name=disk['group'], 
            osd_id=disk['osd_id'],
            nvme_name=disk['nvme']) )
    exec_cmd("sleep 1")

# 激活 osd磁盘
# ceph-volume lvm activate --bluestore {osd_id} {uuid}
exec_cmd("ceph-volume lvm activate --bluestore --all")
