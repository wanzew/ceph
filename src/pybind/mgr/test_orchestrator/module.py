import errno
import json
import re
import os
import threading
import functools
from subprocess import check_output, CalledProcessError
try:
    from typing import Callable, List, Tuple
except ImportError:
    pass  # type checking

import six

from ceph.deployment import inventory
from ceph.deployment.drive_group import DriveGroupSpec
from mgr_module import CLICommand, HandleCommandResult
from mgr_module import MgrModule

import orchestrator


class TestCompletion(orchestrator.Completion):
    def evaluate(self):
        self.finalize(None)


def deferred_read(f):
    # type: (Callable) -> Callable[..., TestCompletion]
    """
    Decorator to make methods return
    a completion object that executes themselves.
    """

    @functools.wraps(f)
    def wrapper(*args, **kwargs):
        return TestCompletion(on_complete=lambda _: f(*args, **kwargs))

    return wrapper


def deferred_write(message):
    def inner(f):
        # type: (Callable) -> Callable[..., TestCompletion]

        @functools.wraps(f)
        def wrapper(self, *args, **kwargs):
            return TestCompletion.with_progress(
                message=message,
                mgr=self,
                on_complete=lambda _: f(self, *args, **kwargs),
            )

        return wrapper
    return inner


class TestOrchestrator(MgrModule, orchestrator.Orchestrator):
    """
    This is an orchestrator implementation used for internal testing. It's meant for
    development environments and integration testing.

    It does not actually do anything.

    The implementation is similar to the Rook orchestrator, but simpler.
    """

    def process(self, completions):
        # type: (List[TestCompletion]) -> None
        if completions:
            self.log.info("process: completions={0}".format(orchestrator.pretty_print(completions)))

            for p in completions:
                p.evaluate()

    @CLICommand('test_orchestrator load_data', '', 'load dummy data into test orchestrator', 'w')
    def _load_data(self, inbuf):
        try:
            data = json.loads(inbuf)
            self._init_data(data)
            return HandleCommandResult()
        except json.decoder.JSONDecodeError as e:
            msg = 'Invalid JSON file: {}'.format(e)
            return HandleCommandResult(retval=-errno.EINVAL, stderr=msg)
        except orchestrator.OrchestratorValidationError as e:
            return HandleCommandResult(retval=-errno.EINVAL, stderr=str(e))

    def available(self):
        return True, ""

    def __init__(self, *args, **kwargs):
        super(TestOrchestrator, self).__init__(*args, **kwargs)

        self._initialized = threading.Event()
        self._shutdown = threading.Event()
        self._init_data({})
        self.all_progress_references = list()  # type: List[orchestrator.ProgressReference]

    def shutdown(self):
        self._shutdown.set()

    def serve(self):

        self._initialized.set()

        while not self._shutdown.is_set():
            # XXX hack (or is it?) to kick all completions periodically,
            # in case we had a caller that wait()'ed on them long enough
            # to get persistence but not long enough to get completion

            self.all_progress_references = [p for p in self.all_progress_references if not p.effective]
            for p in self.all_progress_references:
                p.update()

            self._shutdown.wait(5)

    def _init_data(self, data=None):
        self._inventory = [orchestrator.InventoryNode.from_json(inventory_node)
                           for inventory_node in data.get('inventory', [])]
        self._daemons = [orchestrator.DaemonDescription.from_json(service)
                          for service in data.get('daemons', [])]

    @deferred_read
    def get_inventory(self, node_filter=None, refresh=False):
        """
        There is no guarantee which devices are returned by get_inventory.
        """
        if node_filter and node_filter.nodes is not None:
            assert isinstance(node_filter.nodes, list)

        if self._inventory:
            if node_filter:
                return list(filter(lambda node: node.name in node_filter.nodes,
                                   self._inventory))
            return self._inventory

        try:
            c_v_out = check_output(['ceph-volume', 'inventory', '--format', 'json'])
        except OSError:
            cmd = """
            . {tmpdir}/ceph-volume-virtualenv/bin/activate
            ceph-volume inventory --format json
            """
            try:
                c_v_out = check_output(cmd.format(tmpdir=os.environ.get('TMPDIR', '/tmp')), shell=True)
            except (OSError, CalledProcessError):
                c_v_out = check_output(cmd.format(tmpdir='.'),shell=True)

        for out in c_v_out.splitlines():
            self.log.error(out)
            devs = inventory.Devices.from_json(json.loads(out))
            return [orchestrator.InventoryNode('localhost', devs)]
        self.log.error('c-v failed: ' + str(c_v_out))
        raise Exception('c-v failed')

    @deferred_read
    def list_daemons(self, daemon_type=None, daemon_id=None, node_name=None, refresh=False):
        """
        There is no guarantee which daemons are returned by describe_service, except that
        it returns the mgr we're running in.
        """
        if daemon_type:
            daemon_types = ("mds", "osd", "mon", "rgw", "mgr", "iscsi")
            assert daemon_type in daemon_types, daemon_type + " unsupported"

        if self._daemons:
            if node_name:
                return list(filter(lambda svc: svc.nodename == node_name, self._daemons))
            return self._daemons

        out = map(str, check_output(['ps', 'aux']).splitlines())
        types = (daemon_type, ) if daemon_type else ("mds", "osd", "mon", "rgw", "mgr")
        assert isinstance(types, tuple)
        processes = [p for p in out if any([('ceph-' + t in p) for t in types])]

        result = []
        for p in processes:
            sd = orchestrator.DaemonDescription()
            sd.nodename = 'localhost'
            res = re.search('ceph-[^ ]+', p)
            assert res
            sd.daemon_id = res.group()
            result.append(sd)

        return result

    def create_osds(self, drive_groups):
        # type: (List[DriveGroupSpec]) -> TestCompletion
        """ Creates OSDs from a drive group specification.

        Caveat: Currently limited to a single DriveGroup.
        The orchestrator_cli expects a single completion which
        ideally represents a set of operations. This orchestrator
        doesn't support this notion, yet. Hence it's only accepting
        a single DriveGroup for now.
        You can work around it by invoking:

        $: ceph orch osd create -i <dg.file>

        multiple times. The drivegroup file must only contain one spec at a time.
        """
        drive_group = drive_groups[0]

        def run(all_hosts):
            # type: (List[orchestrator.HostSpec]) -> None
            drive_group.validate([h.hostname for h in all_hosts])
        return self.get_hosts().then(run).then(
            on_complete=orchestrator.ProgressReference(
                message='create_osds',
                mgr=self,
            )

        )


    @deferred_write("remove_daemons")
    def remove_daemons(self, names):
        assert isinstance(names, list)

    @deferred_write("remove_service")
    def remove_service(self, service_name):
        assert isinstance(service_name, str)

    @deferred_write("blink_device_light")
    def blink_device_light(self, ident_fault, on, locations):
        assert ident_fault in ("ident", "fault")
        assert len(locations)
        return ''

    @deferred_write("service_action")
    def service_action(self, action, service_name):
        pass

    @deferred_write("Adding NFS service")
    def add_nfs(self, spec):
        # type: (orchestrator.NFSServiceSpec) -> None
        assert isinstance(spec.pool, str)

    @deferred_write("update_nfs")
    def update_nfs(self, spec):
        pass

    @deferred_write("add_mds")
    def add_mds(self, spec):
        pass

    @deferred_write("add_rgw")
    def add_rgw(self, spec):
        pass

    @deferred_read
    def get_hosts(self):
        if self._inventory:
            return [orchestrator.HostSpec(i.name, i.addr, i.labels) for i in self._inventory]
        return [orchestrator.HostSpec('localhost')]

    @deferred_write("add_host")
    def add_host(self, spec):
        # type: (orchestrator.HostSpec) -> None
        host = spec.hostname
        if host == 'raise_no_support':
            raise orchestrator.OrchestratorValidationError("MON count must be either 1, 3 or 5")
        if host == 'raise_bug':
            raise ZeroDivisionError()
        if host == 'raise_not_implemented':
            raise NotImplementedError()
        if host == 'raise_no_orchestrator':
            raise orchestrator.NoOrchestrator()
        if host == 'raise_import_error':
            raise ImportError("test_orchestrator not enabled")
        assert isinstance(host, six.string_types)

    @deferred_write("remove_host")
    def remove_host(self, host):
        assert isinstance(host, six.string_types)

    @deferred_write("update_mgrs")
    def update_mgrs(self, spec):
        # type: (orchestrator.ServiceSpec) -> None

        assert not spec.placement.hosts or len(spec.placement.hosts) == spec.placement.count
        assert all([isinstance(h, str) for h in spec.placement.hosts])

    @deferred_write("update_mons")
    def update_mons(self, spec):
        # type: (orchestrator.ServiceSpec) -> None

        assert not spec.placement.hosts or len(spec.placement.hosts) == spec.placement.count
        assert all([isinstance(h[0], str) for h in spec.placement.hosts])
        assert all([isinstance(h[1], str) or h[1] is None for h in spec.placement.hosts])
