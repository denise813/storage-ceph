import os
from depoly_tools import exec_cmd
from depoly_tools import rpm_top_dir
from depoly_tools import rpm_version

target=exec_cmd("hostname")
exec_cmd("sleep 1")
exec_cmd("ceph osd pool application enable default.rgw.control  mon")
