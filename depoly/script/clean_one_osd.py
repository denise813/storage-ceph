import os
from depoly_tools import exec_cmd

hdds=('sda', 'sdb', 'sdc', 'sdd', 'sde', 'sdf', 'sdh', 'sdi', 'sdj', 'sdk')
nvmes=('nvme0n1', 'nvme1n1')
cluster="ceph"

remote_exec_cmd(" systemctl stop ceph-osd.target".format(osd_id=i))
remote_exec_cmd("sleep 2")
exec_cmd("umount /var/lib/ceph/osd/ceph-{osd_id}".format(osd_id=i))
exec_cmd("sleep 2")
# 删除 lvm

# 清理磁盘


exec_cmd("rm -rf /var/lib/ceph/osd/ceph-{osd_id}".format(osd_id=i))
exec_cmd("sleep 2")

#lvremove -y /dev/ceph-3aa53c5f-1cec-4be8-93d2-e74c4c8cadc7/osd-block-8962ee76-8f69-42b4-ae5c-66284ced7152
#vgremove -y ceph-c04b176c-f77d-4335-badf-d9d7f4cc4938
#pvremove -y /dev/sdck

vgscan --cache
pvscan --cache
lvscan --cache
