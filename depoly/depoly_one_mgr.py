import os
from depoly_tools import exec_cmd

target=exec_cmd("hostname")
cluster="ceph"

# ceph-base-15.2.4-0.el8.x86_64.rpm
# ceph-common-15.2.4-0.el8.x86_64.rpm
# ceph-mgr-15.2.4-0.el8.x86_64.rpm
# ceph-mgr-dashboard-15.2.4-0.el8.noarch.rpm
# ceph-mgr-diskprediction-local-15.2.4-0.el8.noarch.rpm
# ceph-mgr-modules-core-15.2.4-0.el8.noarch.rpm
# python3-ceph-argparse-15.2.4-0.el8.x86_64.rpm
rpm_version="15.2.4-0.el8.x86_64.rpm"
bin_dir="mgr"
exec_cmd("yum install -y {rpm_dir}/ceph-mgr-{version} {rpm_dir}/ceph-common-{version} "
    "{rpm_dir}/ceph-base-{version} "
    "{rpm_dir}/librados2-{version} "
    "{rpm_dir}/libradosstriper1-{version} "
    "{rpm_dir}/librbd1-{version} "
    "{rpm_dir}/python3-ceph-argparse-{version} "
    "{rpm_dir}/python3-rados-{version} "
    "{rpm_dir}/python3-rbd-{version}".format(version=rpm_version, rpm_dir=bin_dir) )

# 创建 mgr key
exec_cmd("rm -rf /var/lib/ceph/mgr/{cluster_name}-{host}".format(cluster_name=cluster, host=target) )
exec_cmd("ceph auth get-or-create mgr.{host} mon 'allow profile mgr' osd 'allow *' mds 'allow *'".format(
    host=target) )
exec_cmd("mkdir /var/lib/ceph/mgr/{cluster_name}-{host}".format(cluster_name=cluster, host=target) )
exec_cmd("ceph auth get-or-create mgr.{host} -o /var/lib/ceph/mgr/ceph-{host}/keyring".format(
    host=target) )
#exec_cmd("ceph config set mgr mgr/restful/{host}/server_port 42159".format(host=target) )

# 启动服务
exec_cmd("systemctl start ceph-mgr@{host}".format(host=target) )
exec_cmd("sleep 1")
exec_cmd("systemctl status ceph-mgr@{host}".format(host=target) )
exec_cmd("sleep 1")
exec_cmd("systemctl enable ceph-mgr@{host}".format(host=target) )
exec_cmd("sleep 1")
