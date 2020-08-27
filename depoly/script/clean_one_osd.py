import os
from depoly_tools import exec_cmd
from depoly_tools import rpm_top_dir
from depoly_tools import rpm_version

hdds=('sda', 'sdb', 'sdc', 'sdd', 'sde', 'sdf', 'sdh', 'sdi', 'sdj', 'sdk')
nvmes=('nvme0n1', 'nvme1n1')
cluster="ceph"

for i in range(10):
    exec_cmd("ceph osd out {osd_id}".format(osd_id=i))
    exec_cmd("sleep 2")
    exec_cmd(" systemctl stop ceph-osd@{osd_id}.service".format(osd_id=i))
    exec_cmd("sleep 2")
    exec_cmd("ceph osd crush remove osd.{osd_id}".format(osd_id=i))
    exec_cmd("sleep 2")
    exec_cmd("ceph auth del osd.{osd_id}".format(osd_id=i))
    exec_cmd("sleep 2")
    exec_cmd("ceph osd rm {osd_id}".format(osd_id=i))
    exec_cmd("sleep 2")

    exec_cmd("umount /var/lib/ceph/osd/ceph-{osd_id}".format(osd_id=i))
    exec_cmd("sleep 2")
    exec_cmd("rm -rf /var/lib/ceph/osd/ceph-{osd_id}".format(osd_id=i))

#lvremove -y /dev/ceph-3aa53c5f-1cec-4be8-93d2-e74c4c8cadc7/osd-block-8962ee76-8f69-42b4-ae5c-66284ced7152
#vgremove -y ceph-c04b176c-f77d-4335-badf-d9d7f4cc4938
#pvremove -y /dev/sdck

vgscan --cache
pvscan --cache
lvscan --cache
