import os
from depoly_tools import exec_cmd
from depoly_tools import rpm_top_dir
from depoly_tools import rpm_version

target=exec_cmd("hostname")
hdds=('sda', 'sdb', 'sdc', 'sdd', 'sde', 'sdf', 'sdh', 'sdi', 'sdj', 'sdk')
nvmes=('nvme0n1', 'nvme1n1')

devices={"hdds": hdds, 'nvmes':nvmes}
cluster="ceph"

bin_dir=rpm_top_dir
'''exec_cmd("yum install -y {rpm_dir}/ceph-osd-{version} {rpm_dir}/ceph-common-{version} "
    "{rpm_dir}/ceph-base-{version} "
    "{rpm_dir}/librados2-{version} "
    "{rpm_dir}/libradosstriper1-{version} "
    "{rpm_dir}/librbd1-{version} "
    "{rpm_dir}/python3-ceph-argparse-{version} "
    "{rpm_dir}/python3-rados-{version} "
    "{rpm_dir}/python3-rbd-{version}".format(version=rpm_version, rpm_dir=bin_dir) )
'''

# 清理磁盘 osd磁盘
for i in range( len(devices['nvmes']) ):
 exec_cmd( "/usr/bin/dd if=/dev/zero of=/dev/{disk_name} bs=1M count=10 conv=fsync".format(
     disk_name=devices['nvmes'][i]) )

for i in range( len(devices['hdds']) ):
    exec_cmd( "/usr/bin/dd if=/dev/zero of=/dev/{disk_name} bs=1M count=10 conv=fsync".format(
        disk_name=devices['hdds'][i]) )

# 进行分区
for i in range( len(devices['nvmes']) ):
    exec_cmd( "parted /dev/{disk_name} --script mktable gpt".format(disk_name=devices['nvmes'][i]) )
    exec_cmd( "sleep 1")

ready_disks=[]
for i in range( len(devices['nvmes']) ):
    # 进行分区
    internal = 200
    for index in range(5):
        offt_start = index * internal
        offt_end = (index + 1) * internal
        exec_cmd( "parted /dev/{disk_name} --script mkpart primary {start}G {end}G".format(
            disk_name=devices['nvmes'][i], start=offt_start, end=offt_end))
    exec_cmd( "sleep 1")

# 检查创建成功
exec_cmd( "sleep 1")
exec_cmd("parted -l")

# 准备 osd磁盘
for i in range( len(devices['hdds']) ):
    lv_index = (i % 5) + 1
    disk_index = i % 2
    exec_cmd("ceph-volume raw prepare --bluestore "
        "--data /dev/{hdd_name} "
        "--block.db /dev/{nvme_name}p{lv}".format(
            hdd_name=devices['hdds'][i],
            nvme_name=devices['nvmes'][disk_index],
            lv=lv_index) )
    exec_cmd("sleep 2")

# 激活 osd磁盘
# ceph-volume lvm activate -h
for i in range( len(devices['hdds']) ):
    lv_index = (i % 5) + 1
    disk_index = i % 2
    exec_cmd("ceph-volume raw activate "
        "--device /dev/{hdd_name} "
        "--block.db /dev/{nvme_name}p{lv} --no-tmpfs --no-systemd".format(
            hdd_name=devices['hdds'][i],
            nvme_name=devices['nvmes'][disk_index],
            lv=lv_index) )
    exec_cmd("sleep 2")
    exec_cmd("systemctl start ceph-osd@{osd_id}".format(osd_id=i) )
    exec_cmd("sleep 2")
