import os
from depoly_tools import exec_cmd

target=exec_cmd("hostname")
hdds=('sda', 'sdb', 'sdc', 'sdd', 'sde', 'sdf', 'sdh', 'sdi', 'sdj', 'sdk')
nvmes=('nvme0n1', 'nvme1n1')

devices={"hdds": hdds, 'nvmes':nvmes}
cluster="ceph"
group_num=2
#容量 G
nvme_capacity=4000

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

# 创建 osd 秘钥
'''
exec_cmd("rm -rf /var/lib/ceph/bootstrap-osd/{cluster_name}.keyring".format(cluster_name=cluster) )
exec_cmd("ceph-authtool --create-keyring /var/lib/ceph/bootstrap-osd/{cluster_name}.keyring "
        "--gen-key -n client.bootstrap-osd --cap mon 'profile bootstrap-osd' "
        "--cap mgr 'allow r'".format(cluster_name=cluster) )
'''

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
for i in range( len(devices['hdds']) ):
    disksinfo={}
    disksinfo['uuid'] = exec_cmd("uuidgen")
    #$(ceph osd create [{uuid} [{id}]])
    #disksinfo['osd_id'] = exec_cmd( "ceph osd create {uuid}".format(uuid=disksinfo['uuid']) )
    disksinfo['osd_id'] = i
    disksinfo['group'] = i % group_num
    disksinfo['hdd'] = devices['hdds'][i]
    exec_cmd( "lvcreate -l 100%FREE -n lv_{group_name}_{osd_id} ceph_vg_block_{disk_name}".format(
        disk_name=disksinfo['hdd'], group_name=disksinfo['group'], osd_id=disksinfo['osd_id']) )
    disksinfo['nvme']=devices['nvmes'][ disksinfo['group'] ]
    capacity= nvme_capacity *(1000*1000*1000) // (5*1024*1024*1024)
    exec_cmd( "lvcreate -L {disk_capacity}G -n lv_{group_name}_{osd_id} ceph_vg_db_{disk_name}".format(
        disk_capacity=capacity,
        disk_name=disksinfo['nvme'], group_name=disksinfo['group'],
        osd_id=disksinfo['osd_id']) )
    ready_disks.append(disksinfo)
# 检查创建成功
exec_cmd("lvdisplay")

# 准备 osd磁盘
'''
for disk in ready_disks:
    # ceph-volume lvm create --help
    exec_cmd("ceph-volume lvm create --bluestore "
            "--data ceph_vg_block_{hdd_name}/lv_{group_name}_{osd_id} "
            "--block.db ceph_vg_db_{nvme_name}/lv_{group_name}_{osd_id} "
            "--osd-id {osd_id} "
            "--osd-fsid {uuid}".format(
            hdd_name=disk['hdd'],
            group_name=disk['group'], 
            osd_id=disk['osd_id'],
            nvme_name=disk['nvme'],
            uuid=disk['uuid']) )
'''

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

exec_cmd("ceph-volume lvm activate --bluestore --all")

'''
# 激活 osd磁盘
for disk in ready_disks:
    exec_cmd("ceph-volume lvm activate --bluestore {osd_id} {uuid}".format( 
        osd_id=disk['osd_id'],
        uuid=disk['uuid']) )

for i in range( len(devices['hdds']) ):
    disksinfo['uuid'] = exec_cmd("uuidgen")
    #$(ceph osd create [{uuid} [{id}]])
    disksinfo['osd_id'] = exec_cmd( "ceph osd create {uuid}".format(uuid=disksinfo['uuid']) )
'''
