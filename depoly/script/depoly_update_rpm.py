import os
from depoly_tools import exec_cmd
from depoly_tools import rpm_top_dir
from depoly_tools import rpm_version

target=exec_cmd("hostname")
cluster='ceph'
cluster_id='6ead0d81-8278-4385-9edc-fb71b0771637'
public_ip='192.168.100.163'
# 安装软件
# rpm -Uvh
#ceph-base-15.2.4-0.el8.x86_64.rpm
#ceph-common-15.2.4-0.el8.x86_64.rpm
#ceph-mon-15.2.4-0.el8.x86_64.rpm
#librados2-15.2.4-0.el8.x86_64.rpm
#libradosstriper1-15.2.4-0.el8.x86_64.rpm
#librbd1-15.2.4-0.el8.x86_64.rpm
#python3-ceph-argparse-15.2.4-0.el8.x86_64.rpm
#python3-rados-15.2.4-0.el8.x86_64.rpmA
rpm_array = [
"x86_64/ceph-mon-15.2.4-0.el8.x86_64.rpm",
"x86_64/ceph-osd-15.2.4-0.el8.x86_64.rpm",
"x86_64/libcephfs2-15.2.4-0.el8.x86_64.rpm",
"x86_64/librados2-15.2.4-0.el8.x86_64.rpm",
"x86_64/librados-devel-15.2.4-0.el8.x86_64.rpm",
"x86_64/libradospp-devel-15.2.4-0.el8.x86_64.rpm",
"x86_64/libradosstriper1-15.2.4-0.el8.x86_64.rpm",
"x86_64/libradosstriper-devel-15.2.4-0.el8.x86_64.rpm",
"x86_64/librbd1-15.2.4-0.el8.x86_64.rpm",
"x86_64/python3-cephfs-15.2.4-0.el8.x86_64.rpm",
"x86_64/python3-rados-15.2.4-0.el8.x86_64.rpm",
]

for i in rpm_array:
    exec_cmd("scp root@10.112.88.1://root/rpmbuild/RPMS/{rpm_name} /root/".format(rpm_name=i) )

