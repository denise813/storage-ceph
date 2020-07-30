import os
from depoly_tools import exec_cmd

target=exec_cmd("hostname")
cluster='ceph'
cluster_id='6ead0d81-8278-4385-9edc-fb71b0771637'
public_ip='192.168.10.163'
# 安装软件
# rpm -Uvh
#ceph-base-15.2.4-0.el8.x86_64.rpm
#ceph-common-15.2.4-0.el8.x86_64.rpm
#ceph-mon-15.2.4-0.el8.x86_64.rpm
#librados2-15.2.4-0.el8.x86_64.rpm
#libradosstriper1-15.2.4-0.el8.x86_64.rpm
#librbd1-15.2.4-0.el8.x86_64.rpm
#python3-ceph-argparse-15.2.4-0.el8.x86_64.rpm
#python3-rados-15.2.4-0.el8.x86_64.rpm

rpm_version="15.2.4-0.el8.x86_64.rpm"
bin_dir="mon"
exec_cmd("yum install -y {rpm_dir}/ceph-mon-{version} {rpm_dir}/ceph-common-{version} "
    "{rpm_dir}/ceph-base-{version} "
    "{rpm_dir}/librados2-{version} "
    "{rpm_dir}/libradosstriper1-{version} "
    "{rpm_dir}/librbd1-{version} "
    "{rpm_dir}/python3-ceph-argparse-{version} "
    "{rpm_dir}/python3-rados-{version} "
    "{rpm_dir}/python3-rbd-{version}".format(version=rpm_version, rpm_dir=bin_dir) )

# 设置集群名称
#exec_cmd("touch /etc/sysconfig/ceph")

# 为此集群创建密钥环、并生成mon密钥
exec_cmd("rm -rf /tmp/{cluster_name}.mon.keyring".format(cluster_name=cluster) )
#exec_cmd("scp /tmp/{cluster_name}.mon.keyring root@{host}:/tmp/{cluster_name}.mon.keyring".format(
#    cluster_name=cluster, host=target) )

exec_cmd("ceph-authtool --create-keyring /tmp/{cluster_name}.mon.keyring "
        "--gen-key -n mon. --cap mon 'allow *'".format(cluster_name=cluster) )
exec_cmd("sleep 2")

# 生成管理员密钥环
exec_cmd("rm -rf /etc/ceph/{cluster_name}.client.admin.keyring".format(cluster_name=cluster) )
exec_cmd("ceph-authtool --create-keyring --gen-key -n client.admin "
        "--cap mon 'allow *' --cap osd 'allow *' --cap mds 'allow *' --cap mgr 'allow *' "
        "/etc/ceph/{cluster_name}.client.admin.keyring".format(cluster_name=cluster) )
exec_cmd("sleep 2")

exec_cmd("ceph-authtool --create-keyring /var/lib/ceph/bootstrap-osd/{cluster_name}.keyring "
        "--gen-key -n client.bootstrap-osd --cap mon 'profile bootstrap-osd'"
        " --cap mgr 'allow r'".format(cluster_name=cluster) )

# 加入 ceph.mon.keyring
exec_cmd("ceph-authtool /tmp/{cluster_name}.mon.keyring "
        "--import-keyring /etc/ceph/{cluster_name}.client.admin.keyring".format(cluster_name=cluster) )
exec_cmd("ceph-authtool /tmp/{cluster_name}.mon.keyring "
        "--import-keyring /var/lib/ceph/bootstrap-osd/{cluster_name}.keyring".format(cluster_name=cluster) )


# 生成一个监视器图
exec_cmd("rm -rf /tmp/{cluster_name}_monmap.1223".format(cluster_name=cluster) )
exec_cmd("monmaptool --create --clobber "
        "--addv {host} [v2:{public_add}:3300,v1:{public_add}:6798] "
        "--fsid {cid} --print /tmp/{cluster_name}_monmap.1223".format(
            cluster_name=cluster, host=target, cid=cluster_id, public_add=public_ip) )
exec_cmd("sleep 2")

# 创建数据目录
exec_cmd("rm -rf /var/lib/ceph/mon/{cluster_name}-{host}".format(cluster_name=cluster, host=target) )
exec_cmd("mkdir -p /var/lib/ceph/mon/{cluster_name}-{host}".format(cluster_name=cluster, host=target) )

# 拷贝对应的配置文件
exec_cmd("cp ./ceph.conf /etc/ceph/{cluster_name}.conf".format(cluster_name=cluster) )
exec_cmd("cat /etc/ceph/{cluster_name}.conf".format(cluster_name=cluster) )
exec_cmd("sleep 1")

# 初始数据
exec_cmd("ceph-mon --cluster {cluster_name} --mkfs -i {host} "
        "-c /etc/ceph/{cluster_name}.conf "
        "--monmap /tmp/{cluster_name}_monmap.1223 "
        "--keyring=/tmp/{cluster_name}.mon.keyring".format(cluster_name=cluster, host=target) )
#标志创建完成
exec_cmd("sleep 5")
exec_cmd("touch /var/lib/ceph/mon/{cluster_name}-{host}/done".format(cluster_name=cluster, host=target) )

#exec_cmd("ceph-mon --cluster {cluster_name} -i {host} -c /etc/ceph/{cluster_name}.conf "
#        "--keyring=/tmp/{cluster_name}.mon.keyring".format(cluster_name=cluster, host=target) )
#systemctl stop ceph-mon@${target}
exec_cmd("sleep 1")
exec_cmd("systemctl stop ceph-mon@{host}".format(host=target) )
exec_cmd("sleep 1")
exec_cmd("systemctl start ceph-mon@{host}".format(host=target) )
exec_cmd("sleep 1")
exec_cmd("systemctl enable ceph-mon@{host}".format(host=target) )
exec_cmd("sleep 1")
exec_cmd("systemctl status ceph-mon@{host}".format(host=target) )
exec_cmd("sleep 1")
exec_cmd("ceph mon getmap -o monmap --cluster {cluster_name}".format(cluster_name=cluster) )
exec_cmd("monmaptool --print monmap")
exec_cmd("sleep 1")
exec_cmd("ceph auth get mon. --cluster {cluster_name}".format(cluster_name=cluster) )
exec_cmd("sleep 1")

# 删除默认的rule
exec_cmd("ceph osd crush rule rm replicated_rule")
exec_cmd("ceph osd crush remove default")
