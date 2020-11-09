import errno
import json
import re
import os
import threading
import functools
import itertools
import six
from threading import Event
from subprocess import check_output, CalledProcessError
from typing import Callable, List, Sequence, Tuple

from ceph.deployment.service_spec import ServiceSpec, NFSServiceSpec, IscsiServiceSpec
from ceph.deployment import inventory
from ceph.deployment.drive_group import DriveGroupSpec

from mgr_module import CLICommand, HandleCommandResult
from mgr_module import MgrModule
import orchestrator

logger = logging.getLogger(__name__)
CEPH_DATEFMT = '%Y-%m-%dT%H:%M:%S.%fZ'

class SSHCompletion(orchestrator.Completion[T]):
    # 由orchestrator接口而来
    def evaluate(self):
        self.finalize(None)


def trivial_completion(f: Callable[..., T]) -> Callable[..., SSHCompletion[T]]:
    """
    Decorator to make CephadmCompletion methods return
    a completion object that executes themselves.
    """

    @wraps(f)
    def wrapper(*args, **kwargs):
        return SSHCompletion(on_complete=lambda _: f(*args, **kwargs))
    return wrapper

def local_exec_cmd(cmd):
    out_stream = os.popen(cmd)
    _out = ""
    _out = out_stream.read()
    out_stream.close()
    out = _out
    out = _out.replace('\n', '')
    #print( "cmd={cmd_str} end, out={out_str}".format(cmd_str=cmd, out_str=_out))
    return out

def remote_exec_cmd(target, cmd, passwd='1'):
    cmdstr = ('''sshpass -p {pwd} ssh root@{remote} "{comand}"'''.format(
        remote=target, pwd=passwd, comand=cmd))
    return local_exec_cmd(cmdstr)

class SSHOrchestrator(orchestrator.Orchestrator, MgrModule):

    def process(self, completions):
        """
        Does nothing, as completions are processed in another thread.
        """
        if completions:
            self.log.debug("process: completions={0}".format(
                orchestrator.pretty_print(completions)))

            for p in completions:
                p.evaluate()

    def __init__(self, *args, **kwargs):
        super(SSHOrchestrator, self).__init__(*args, **kwargs)
        # for serve()
        self.run = True
        self.event = Event()
        self._worker_pool = multiprocessing.pool.ThreadPool(10)

    def serve(self) -> None:
        """
        The main loop of ssh.

        A command handler will typically change the declarative state
        of cephadm. This loop will then attempt to apply this new state.
        """
        self.log.debug("serve starting")
        while self.run:
                # 轮询休眠
                self._serve_sleep()
                # 主从端都进行刷新 主机以及 服务的信息
                # refresh daemons
                # 刷新服务和对应的主机状态
                self._refresh_hosts_and_daemons()             

        # 退出
        self.log.debug("serve exit")


    def available(self):
        """
        The ssh orchestrator is always available.
        """
        # 检查条件,标识位可用
        return True, ''

    def process(self, completions):
        """
        Does nothing, as completions are processed in another thread.
        """
        if completions:
            self.log.debug("process: completions={0}".format(
                orchestrator.pretty_print(completions)))

            for p in completions:
                p.evaluate()

    @trivial_completion
    def add_host(self, spec: HostSpec) -> str:
       # 将本地的 repo 推送到该主机节点
       # 获取本地节点信息
        local_exec_cmd("scp /etc/yum/yum.repo/ceph.repo root@{host}://etc/yum/yum.repo/ceph.repo", format(host=spec.host))
        # 将信息存储到 mgr 数据库中
        self.log.info('Added label %s to host %s' % (label, host))
        return "added host '{}'".format(spec)

    @trivial_completion
    def remove_host(self, host):
        # 删除该节点的repo
        remote_exec_cmd("rm -rf /etc/yum/yum.repo/ceph.repo", format(host=spec.host))
        self.log.info('Added label %s to host %s' % (label, host))
        return "Removed host '{}'".format(host)

    @trivial_completion
    def update_host_addr(self, host, addr) -> str:
         pass

    @trivial_completion
    def get_hosts(self):
        # type: () -> List[orchestrator.HostSpec]
        """
        Return a list of hosts managed by the orchestrator.

        Notes:
          - skip async: manager reads from cache.
        """
        # 从数据库中获取信息
        self.list_hosts()

    @trivial_completion
    def add_host_label(self, host, label) -> str:
        self.log.info('Added label %s to host %s' % (label, host))
        return 'Added label %s to host %s' % (label, host)

    @trivial_completion
    def remove_host_label(self, host, label) -> str:
        self.log.info('Removed label %s to host %s' % (label, host))
        return 'Removed label %s from host %s' % (label, host)

    @trivial_completion
    def host_ok_to_stop(self, hostname: str):
        pass

    @trivial_completion
    def describe_service(self, service_type: Optional[str] = None, service_name: Optional[str] = None,
                         refresh: bool = False) -> List[orchestrator.ServiceDescription]:
       pass

    @trivial_completion
    def list_daemons(self,
                     service_name: Optional[str] = None,
                     daemon_type: Optional[str] = None,
                     daemon_id: Optional[str] = None,
                     host: Optional[str] = None,
                     refresh: bool = False) -> List[orchestrator.DaemonDescription]:
        pass

    @trivial_completion
    def service_action(self, action, service_name) -> List[str]:
        pass

    @trivial_completion
    def daemon_action(self, action: str, daemon_name: str, image: Optional[str] = None) -> str:
        pass

    @trivial_completion
    def remove_daemons(self, names):
        # type: (List[str]) -> List[str]
        pass

    @trivial_completion
    def remove_service(self, service_name) -> str:
        self.log.info('Remove service %s' % service_name)
        pass

    @trivial_completion
    def get_inventory(self, host_filter: Optional[orchestrator.InventoryFilter] = None, refresh=False) -> List[orchestrator.InventoryHost]:
        """
        Return the storage inventory of hosts matching the given filter.

        :param host_filter: host filter

        TODO:
          - add filtering by label
        """
        pass

    @trivial_completion
    def zap_device(self, host, path) -> str:
        self.log.info('Zap device %s:%s' % (host, path))
        pass

    @trivial_completion
    def blink_device_light(self, ident_fault: str, on: bool, locs: List[orchestrator.DeviceLightLoc]) -> List[str]:
        """
        Blink a device light. Calling something like::

          lsmcli local-disk-ident-led-on --path $path

        If you must, you can customize this via::

          ceph config-key set mgr/cephadm/lsmcli_blink_lights_cmd '<my jinja2 template>'

        See templates/lsmcli_blink_lights_cmd.j2
        """
        pass

    @trivial_completion
    def apply_drivegroups(self, specs: List[DriveGroupSpec]) -> List[str]:
        """
        Deprecated. Please use `apply()` instead.

        Keeping this around to be compapatible to mgr/dashboard
        """
        pass

    @trivial_completion
    def create_osds(self, drive_group: DriveGroupSpec) -> str:
        pass

    @trivial_completion
    def apply_mon(self, spec) -> str:
       pass

    @trivial_completion
    def add_mon(self, spec):
        # type: (ServiceSpec) -> List[str]
       pass

    @trivial_completion
    def add_mgr(self, spec):
        # type: (ServiceSpec) -> List[str]
        pass

    @trivial_completion
    def apply(self, specs: List[GenericSpec]) -> List[str]:
        pass

    @trivial_completion
    def apply_mgr(self, spec) -> str:
        pass

    @trivial_completion
    def add_crash(self, spec):
        # type: (ServiceSpec) -> List[str]
        return self._add_daemon('crash', spec,
                                self.crash_service.prepare_create)

    @trivial_completion
    def apply_crash(self, spec) -> str:
        return self._apply(spec)

    @trivial_completion
    def remove_osds(self, osd_ids: List[str],
                    replace: bool = False,
                    force: bool = False) -> str:
        """
        Takes a list of OSDs and schedules them for removal.
        The function that takes care of the actual removal is
        process_removal_queue().
        """

        daemons: List[orchestrator.DaemonDescription] = self.cache.get_daemons_by_type('osd')
        pass

