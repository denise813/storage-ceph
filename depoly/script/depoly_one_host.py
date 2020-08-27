import os
from depoly_tools import exec_cmd
from depoly_tools import rpm_top_dir
from depoly_tools import rpm_version

target=exec_cmd("hostname")
cluster='ceph'
cluster_id='6ead0d81-8278-4385-9edc-fb71b0771637'
public_ip='192.168.100.163'
roles=['mon', 'osd', 'client']
# 安装软件
# rpm -Uvh

# 清理遗留垃圾
exec_cmd("rm -rf /tmp/{cluster_name}.mon.keyring".format(cluster_name=cluster) )
exec_cmd("rm -rf /etc/ceph/{cluster_name}.client.admin.keyring".format(cluster_name=cluster) )
exec_cmd("rm -rf /tmp/{cluster_name}_monmap.1223".format(cluster_name=cluster) )
exec_cmd("rm -rf /var/lib/ceph/mon/{cluster_name}-{host}".format(cluster_name=cluster, host=target) )
exec_cmd("rm -rf /var/lib/ceph/mgr/{cluster_name}-{host}".format(cluster_name=cluster, host=target) )
exec_cmd("rm -rf /usr/lib64/ceph/*")

bin_dir = rpm_top_dir
exec_cmd("yum install -y {rpm_dir}/ceph-common-{version} "
    "{rpm_dir}/ceph-base-{version} "
    "{rpm_dir}/librados2-{version} "
    "{rpm_dir}/libradosstriper1-{version} "
    "{rpm_dir}/librbd1-{version} "
    "{rpm_dir}/python3-ceph-argparse-{version} "
    "{rpm_dir}/python3-rados-{version} "
    "{rpm_dir}/python3-rbd-{version} "
    "{rpm_dir}/ceph-mgr-{version}".format(version=rpm_version, rpm_dir=bin_dir) )

for i in roles:
    if i == 'mon':
        exec_cmd("yum install -y {rpm_dir}/ceph-mon-{version} {rpm_dir}/ceph-mgr-{version}".format(version=rpm_version, rpm_dir=bin_dir) )
    elif i == 'osd':
        exec_cmd("yum install -y {rpm_dir}/ceph-osd-{version}".format(version=rpm_version, rpm_dir=bin_dir) )
    elif i == 'client':
        pass
    else:
        pass

# 拷贝对应的配置文件
exec_cmd("cp {source}/ceph.conf /etc/ceph/{cluster_name}.conf".format(source=rpm_top_dir, cluster_name=cluster) )
exec_cmd("cat /etc/ceph/{cluster_name}.conf".format(cluster_name=cluster) )

