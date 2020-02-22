.. _bootstrap:

========================
 Installation (cephadm)
========================

.. note:: The *cephadm* bootstrap feature is first introduced in Octopus, and is not yet recommended for production deployments.

A new Ceph cluster is deployed by bootstrapping a cluster on a single
node, and then adding additional nodes and daemons via the CLI or GUI
dashboard.

The following example installs a basic three-node cluster. Each
node will be identified by its prompt. For example, "[monitor 1]"
identifies the first monitor, "[monitor 2]" identifies the second
monitor, and "[monitor 3]" identifies the third monitor. This
information is provided in order to make clear which commands
should be issued on which systems.

"[any node]" identifies any Ceph node, and in the context
of this installation guide means that the associated command
can be run on any node.

.. highlight:: console

Get cephadm
===========

The ``cephadm`` utility is used to bootstrap a new Ceph Cluster.

Use curl to fetch the standalone script::

  [monitor 1] # curl --silent --remote-name --location https://github.com/ceph/ceph/raw/master/src/cephadm/cephadm
  [monitor 1] # chmod +x cephadm
  
You can also get the utility by installing a package provided by
your Linux distribution::

   [monitor 1] # apt install -y cephadm   # or
   [monitor 1] # dnf install -y cephadm   # or
   [monitor 1] # yum install -y cephadm   # or
   [monitor 1] # zypper install -y cephadm


Bootstrap a new cluster
=======================

To create a new cluster, you need to know:

* Which *IP address* to use for the cluster's first monitor.  This is
  normally just the IP for the first cluster node.  If there are
  multiple networks and interfaces, be sure to choose one that will be
  accessible by any hosts accessing the Ceph cluster.

To bootstrap the cluster run the following command::

  [node 1] $ sudo ./cephadm bootstrap --mon-ip *<mon-ip>*

This command does a few things:

* Creates a monitor and manager daemon for the new cluster on the
  local host.  A minimal configuration file needed to communicate with
  the new cluster is written to ``ceph.conf`` in the local directory.
* A copy of the ``client.admin`` administrative (privileged!) secret
  key is written to ``ceph.client.admin.keyring`` in the local directory.
* Generates a new SSH key, and adds the public key to the local root user's
  ``/root/.ssh/authorized_keys`` file.  A copy of the public key is written
  to ``ceph.pub`` in the local directory.

Interacting with the cluster
============================

To interact with your cluster, start up a container that has all of 
the Ceph packages installed::

  [any node] $ sudo ./cephadm shell --config ceph.conf --keyring ceph.client.admin.keyring

The ``--config`` and ``--keyring`` arguments will bind those local
files to the default locations in ``/etc/ceph`` inside the container
to allow the ``ceph`` CLI utility to work without additional
arguments.  Inside the container, you can check the cluster status with::

  [ceph: root@monitor_1_hostname /]# ceph status

In order to interact with the Ceph cluster outside of a container
(that is, from the command line), install the Ceph
client packages and install the configuration and privileged 
administrator key in a global location::

   [any node] $ sudo apt install -y ceph-common   # or,
   [any node] $ sudo dnf install -y ceph-common   # or,
   [any node] $ sudo yum install -y ceph-common

   [any node] $ sudo install -m 0644 ceph.conf /etc/ceph/ceph.conf
   [any node] $ sudo install -m 0600 ceph.keyring /etc/ceph/ceph.keyring

Adding hosts to the cluster
===========================

For each new host you'd like to add to the cluster, you need to do two things:

#. Install the cluster's public SSH key in the new host's root user's
   ``authorized_keys`` file.  For example,::

     [monitor 1] # cat ceph.pub | ssh root@*newhost* tee -a /root/.ssh/authorized_keys

#. Tell Ceph that the new node is part of the cluster::

     [monitor 1] # ceph orch host add *newhost*

Deploying additional monitors
=============================

Normally a Ceph cluster has at least three (or, preferably, five)
monitor daemons spread across different hosts.  Since we are deploying
a monitor, we again need to specify what IP address it will use,
either as a simple IP address or as a CIDR network name.

To deploy additional monitors,::

  [monitor 1] # ceph orch apply mon *<new-num-monitors>* *<host1:network1> [<host1:network2>...]*

For example, to deploy a second monitor on ``newhost`` using an IP
address in network ``10.1.2.0/24``,::

  [monitor 1] # ceph orch apply mon 2 newhost:10.1.2.0/24

Deploying OSDs
==============

To add an OSD to the cluster, you need to know the device name for the
block device (hard disk or SSD) that will be used.  Then,::

  [monitor 1] # ceph orch osd create *<host>*:*<path-to-device>*

For example, to deploy an OSD on host *newhost*'s SSD,::

  [monitor 1] # ceph orch osd create newhost:/dev/disk/by-id/ata-WDC_WDS200T2B0A-00SM50_182294800028

Deploying manager daemons
=========================

It is a good idea to have at least one backup manager daemon.  To
deploy one or more new manager daemons,::

  [monitor 1] # ceph orch apply mgr *<new-num-mgrs>* [*<host1>* ...]

Deploying MDSs
==============

One or more MDS daemons is required to use the CephFS file system.
These are created automatically if the newer ``ceph fs volume``
interface is used to create a new file system.  For more information,
see :ref:`fs-volumes-and-subvolumes`.

To deploy metadata servers,::

  # ceph orch apply mds *<fs-name>* *<num-daemons>* [*<host1>* ...]

Deploying RGWs
==============

Cephadm deploys radosgw as a collection of daemons that manage a
particular *realm* and *zone*.  (For more information about realms and
zones, see :ref:`multisite`.)  To deploy a set of radosgw daemons for
a particular realm and zone,::

  # ceph orch apply rgw *<realm-name>* *<zone-name>* *<num-daemons>* [*<host1>* ...]

Note that with cephadm, radosgw daemons are configured via the monitor
configuration database instead of via a `ceph.conf` or the command line.  If
that confiruation isn't already in place (usually in the
``client.rgw.<realmname>.<zonename>`` section), then the radosgw
daemons will start up with default settings (e.g., binding to port
80).
