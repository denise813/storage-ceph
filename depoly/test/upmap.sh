pool=$1
ceph osd getmap >./osdmap.bin
osdmaptool --upmap-pool ${pool} ./osdmap.bin --upmap ./upmap.bin
source ./upmap.bin 
ceph osd df

