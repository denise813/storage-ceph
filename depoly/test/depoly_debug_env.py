import os
from depoly_tools import exec_cmd

target=exec_cmd("hostname")

#exec_cmd("ceph osd pool create debugpool 32 32")
#exec_cmd("ceph osd pool set debugpool size 1")
exec_cmd("sleep 1")

#exec_cmd("rbd create rbd1 size 50T")
for i in range(10):
    exec_cmd("ceph osd crush add osd.{osd_id} 1 root=root{osd_id}".format(osd_id=i))
    exec_cmd("ceph osd crush rule create-replicated rule{osd_id} root{osd_id} osd".format(
        osd_id=i))
    exec_cmd("ceph osd pool create pool{osd_id} 16 16 replicated rule{osd_id}".format(
        osd_id=i))
    exec_cmd("ceph osd pool set pool{osd_id} pg_autoscale_mode off".format(
        osd_id=i))
    exec_cmd("ceph osd pool set pool{osd_id} size 1".format(
        osd_id=i))
    exec_cmd("rbd create pool{osd_id}/rbd1 --size 500G".format(
        osd_id=i))

for i in range(5):
    exec_cmd("ceph osd crush add osd.{osd_id} 1 root=rootx".format(osd_id=i))
exec_cmd("ceph osd crush rule create-replicated rulex rootx osd")
exec_cmd("ceph osd pool create poolx 128 128 replicated rulex")
exec_cmd("ceph osd pool set poolx pg_autoscale_mode off")
exec_cmd("ceph osd pool set poolx size 1")
exec_cmd("rbd create poolx/rbd1 --size 500G")
for i in range(5):
    exec_cmd("ceph osd crush add osd.{osd_id} 1 root=rooty".format(osd_id=i+5))
exec_cmd("ceph osd crush rule create-replicated ruley rooty osd")
exec_cmd("ceph osd pool create pooly 128 128 replicated ruley")
exec_cmd("ceph osd pool set pooly pg_autoscale_mode off")
exec_cmd("ceph osd pool set pooly size 1")
exec_cmd("rbd create pooly/rbd1 --size 500G")
#exec_cmd("ceph osd pool create pool001 32 32")
