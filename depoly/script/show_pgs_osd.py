import os
from depoly_tools import exec_cmd
from depoly_tools import rpm_top_dir
from depoly_tools import rpm_version

target=exec_cmd("hostname")
exec_cmd("sleep 1")
nice="min"
exec_cmd("ceph pg dump|grep -e '^10\.'|awk '{print $15}'|tr -d '['|tr -d ']' > ./pgs.txt'")
exec_cmd("awk -F ',' '{sum[$1]+=1}{sum[$2]+=1} END {for(k in sum) print k ":" sum[k]}' ./pgs.txt |sort -n|awk -F ':' '{print "osd."$1"\t:\t"$2}'")
exec_cmd("awk -F ',' '{sum[$1]+=1}{sum[$2]+=1} END {for(k in sum) print sum[k]}' ./pgs.txt|awk '{total+=$1}END{print "total:\t"total}'")
exec_cmd("rm -f ./pgs.txt")

