import os

def exec_cmd(cmd):
    out_stream = os.popen(cmd)
    _out = out_stream.read()
    out_stream.close()
    print( "cmd={cmd_str}, out={out_str}".format(cmd_str=cmd, out_str=_out) )
    out = _out.replace('\n', '')
    return out

target=exec_cmd("hostname")
hdds=('sda', 'sdb', 'sdc', 'sdd', 'sde', 'sdf', 'sdh', 'sdi', 'sdj', 'sdk')
nvmes=('nvme0n1', 'nvme1n1')
cluster="ceph"
group_num=2

# 创建 osd 秘钥
exec_cmd("rm -rf /var/lib/ceph/bootstrap-osd/{cluster_name}.keyring ".format(
    cluster_name=cluster))

# 元数据进行分区
for disk in hdds:
    exec_cmd( "pvcreate /dev/{disk_name}".format(disk_name=disk) )

for disk in nvmes:
    exec_cmd( "pvcreate /dev/{disk_name}".format(disk_name=disk) )

# 检查创建成功
exec_cmd("pvs")

#创建卷组
for i in range( len(hdds) ):
    disk=hdds[i]
    exec_cmd( "vgcreate ceph_vg_block_{disk_name} /dev/{disk_name}".format(disk_name=disk) )

for i in range( len(nvmes) ):
    disk=nvmes[i]
    exec_cmd( "vgcreate ceph_vg_db_{disk_name} /dev/{disk_name}".format(disk_name=disk) )
# 检查创建成功
exec_cmd("vgdisplay")

ready_disks=[]

# 创建逻辑卷
for i in range( len(hdds) ):
    disksinfo={}
    disksinfo['uuid'] = exec_cmd("uuidgen")
    #$(ceph osd create [{uuid} [{id}]])
    disksinfo['osd_id'] = exec_cmd( "ceph osd create {uuid)".format(uuid=disksinfo['uuid']) )
    disksinfo['group'] = i % group_num
    disksinfo['hdd'] = hdds[i]
    exec_cmd( "lvcreate -n ceph_vg_block_{disk_name} lv_{group_name}_{osd_id}".format(
        disk_name=disksinfo['hdd'], group_name=disksinfo['group'], osd_id=disksinfo['osd_id']) )
    disksinfo['nvme']=nvmes[group_id]
    persent=20
    exec_cmd( "lvcreate -l {persent}%FREE -n ceph_vg_db_{disk_name} lv_{group_name}_osd_id}".format(
        disk_name=disksinfo['nvme'], group_name=disksinfo['group'], osd_id=disksinfo['osd_id']) )
    ready_disks.append(disksinfo)
# 检查创建成功
exec_cmd("lvdisplay")

# 创建 osd磁盘
for disk in ready_disks:

    exec_cmd("ceph-volume lvm create --bluestore "
            "--data /ceph_vg_block_{hdd_name} /lv_{group_name}_{osd_id} "
            "--block.db /ceph_vg_db_{nvme_name}/lv_${group_name}_{osd_id}".format(
            hdd_name=disk['hdd'],
            group_name=disk['group'], 
            osd_id=disk['osd_id'],
            nvme_name=disks['nvme']) )

    '''
    # /usr/bin/ceph-authtool --gen-print-key
    # ceph --cluster ceph --name client.bootstrap-osd --keyring /var/lib/ceph/bootstrap-osd/ceph.keyring -i - osd new 2edc2bd5-3d88-4bec-bdda-25aed9551772
    exec_cmd("ceph --cluster ${cluster_name} --name client.bootstrap-osd "
            "--keyring /var/lib/ceph/bootstrap-osd/{cluster}.keyring -i - osd new {uuid}".format(
                cluster_name=cluster,
                uuid=ready_disks[i].['uuid']) )

    # ceph --cluster ceph --name client.bootstrap-osd --keyring /var/lib/ceph/bootstrap-osd/ceph.keyring mon getmap -o /var/lib/ceph/osd/ceph-0/activate.monmap
    exec_cmd("ceph --cluster {cluster_name} --name client.bootstrap-osd "
            "--keyring /var/lib/ceph/bootstrap-osd/{cluster_name}.keyring mon getmap -o "
            "/var/lib/ceph/osd/{cluster_name}-{osd_id}/activate.monmap".format(
                cluster_name=cluster,
                osd_id=ready_disks[i]['osd_id']) )

    # ceph-authtool /var/lib/ceph/osd/ceph-0/keyring --create-keyring --name osd.0 --add-key AQAv4M1eIlloAhAArIcBoA+gQZ0qPBOGuvsI5g==
    exec_cmd("ceph-authtool /var/lib/ceph/osd/${cluster_name}-{osd_id}/keyring --create-keyring "
            "--name osd.{osd_id} --add-key {key}".format(
                cluster_name=cluster,
                osd_id=ready_disks[i]['osd_id'],
                key=key_context) )
    # ceph-osd --cluster ceph --osd-objectstore bluestore --mkfs -i 0 --monmap /var/lib/ceph/osd/ceph-0/activate.monmap --keyfile - --osd-data /var/lib/ceph/osd/ceph-0/ --osd-uuid 2edc2bd5-3d88-4bec-bdda-25aed9551772 --setuser ceph --setgroup ceph
    exec_cmd("ceph-osd --cluster ${cluster_name} --osd-objectstore bluestore --mkfs -i 0 "
            "--monmap /var/lib/ceph/osd/${cluster_name}-{osd_id}/activate.monmap "
            "--keyfile - --osd-data /var/lib/ceph/osd/{cluster_name}-{osd_id}/ "
            "--osd-uuid {uuid}".format(
                ) )

    # ceph-bluestore-tool --cluster=ceph prime-osd-dir --dev /dev/ceph-e8272e7a-54de-47cb-8a62-7534a953ef09/osd-block-2edc2bd5-3d88-4bec-bdda-25aed9551772 --path /var/lib/ceph/osd/ceph-0 --no-mon-config
    # ln -snf /dev/ceph-e8272e7a-54de-47cb-8a62-7534a953ef09/osd-block-2edc2bd5-3d88-4bec-bdda-25aed9551772 /var/lib/ceph/osd/ceph-0/block
    '''

    exec_cmd("ceph-osd -i {osd_id} --mkfs --mkkey --osd-uuid {uuid}".format(osd_id=ready_disks[i]['osd_id'], ) )
