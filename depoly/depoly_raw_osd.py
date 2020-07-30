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
# python3-ceph-common-15.2.4-0.el8.x86_64.rpm
# python3-rados-15.2.4-0.el8.x86_64.rpm
# python3-rbd-15.2.4-0.el8.x86_64.rpm
rpm_version="15.2.4-0.el8.x86_64.rpm"
bin_dir="osd"
exec_cmd("yum install -y {rpm_dir}/ceph-osd-{version} {rpm_dir}/ceph-common-{version} "
    "{rpm_dir}/ceph-base-{version} "
    "{rpm_dir}/librados2-{version} "
    "{rpm_dir}/libradosstriper1-{version} "
    "{rpm_dir}/librbd1-{version} "
    "{rpm_dir}/python3-ceph-argparse-{version} "
    "{rpm_dir}/python3-rados-{version} "
    "{rpm_dir}/python3-rbd-{version}".format(version=rpm_version, rpm_dir=bin_dir) )

# 清理磁盘 osd磁盘
for i in range( len(devices['nvmes']) ):
 exec_cmd( "/usr/bin/dd if=/dev/zero of=/dev/{disk_name} bs=1M count=10 conv=fsync".format(
     disk_name=devices['nvmes'][i]) )

for i in range( len(devices['hdds']) ):
    exec_cmd( "/usr/bin/dd if=/dev/zero of=/dev/{disk_name} bs=1M count=10 conv=fsync".format(
        disk_name=devices['hdds'][i]) )

# 元数据进行分区
for i in range( len(devices['nvmes']) ):
    exec_cmd( "parted /dev/{disk_name} --script mktable gpt".format(disk_name=devices['nvmes'][i]) )
    exec_cmd( "sleep 1")

ready_disks=[]
lv_num = len(devices['hdds']) / len(devices['nvmes'])
for i in range( len(devices['hdds']) ):
    disksinfo={}
    # 将磁盘平均分布在 nvme 磁盘中
    group = i % len(devices['nvmes'])
    disk_nvme = devices['nvmes'][group]
    interval = 100 / lv_num

    offt_start = int( (i % lv_num) * interval)
    offt_end = int(((i  % lv_num) + 1) * interval)

    # 进行分区
    exec_cmd( "parted /dev/{disk_name} --script mkpart primary {start}% {end}%".format(
        disk_name=devices['nvmes'][group], start=offt_start, end=offt_end))
    exec_cmd( "sleep 1")

    # 记录磁盘信息
    disksinfo['osd_id'] = i
    disksinfo['uuid'] = exec_cmd("uuidgen")
    disksinfo['hdd'] = devices['hdds'][i]
    disksinfo['nvme'] = disk_nvme
    disksinfo['lv'] = int((i % lv_num) + 1)
    disksinfo["group"] = group
    ready_disks.append(disksinfo)

# 检查创建成功
exec_cmd("parted -l")

# 准备 osd磁盘
for disk in ready_disks:
    exec_cmd("ceph-volume raw prepare --bluestore "
        "--data /dev/{hdd_name} "
        "--block.db /dev/{nvme_name}p{lv}".format(
            hdd_name=disk['hdd'],
            nvme_name=disk['nvme'],
            lv=disk['lv']) )
    exec_cmd("sleep 2")

# 激活 osd磁盘
# ceph-volume lvm activate -h
for disk in ready_disks:
    exec_cmd("ceph-volume raw activate --device /dev/{hdd_name} --block.db /dev/{nvme_name}p{lv} "
        "--no-systemd".format(
            hdd_name=disk['hdd'],
            nvme_name=disk['nvme'],
            lv=disk['lv']) )
    exec_cmd("sleep 1")
    exec_cmd("systemctl stop ceph-osd@{osd_id}".format(osd_id=disk['osd_id']) )
    exec_cmd("sleep 1")
    exec_cmd("systemctl enable ceph-osd@{osd_id}".format(osd_id=disk['osd_id']) )
    exec_cmd("sleep 1")
    exec_cmd("systemctl start ceph-osd@{osd_id}".format(osd_id=disk['osd_id']) )
    exec_cmd("sleep 1")
    exec_cmd("systemctl status ceph-osd@{osd_id}".format(osd_id=disk['osd_id']) )
    exec_cmd("sleep 1")

