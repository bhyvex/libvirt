/*
 * domain_conf.c: domain XML processing
 *
 * Copyright (C) 2006-2013 Red Hat, Inc.
 * Copyright (C) 2006-2008 Daniel P. Berrange
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "internal.h"
#include "virerror.h"
#include "datatypes.h"
#include "domain_conf.h"
#include "snapshot_conf.h"
#include "viralloc.h"
#include "verify.h"
#include "virxml.h"
#include "viruuid.h"
#include "virutil.h"
#include "virbuffer.h"
#include "virlog.h"
#include "nwfilter_conf.h"
#include "virstoragefile.h"
#include "virfile.h"
#include "virbitmap.h"
#include "count-one-bits.h"
#include "secret_conf.h"
#include "netdev_vport_profile_conf.h"
#include "netdev_bandwidth_conf.h"
#include "netdev_vlan_conf.h"
#include "device_conf.h"

#define VIR_FROM_THIS VIR_FROM_DOMAIN

/* virDomainVirtType is used to set bits in the expectedVirtTypes bitmask,
 * verify that it doesn't overflow an unsigned int when shifting */
verify(VIR_DOMAIN_VIRT_LAST <= 32);


struct _virDomainObjList {
    virObjectLockable parent;

    /* uuid string -> virDomainObj  mapping
     * for O(1), lockless lookup-by-uuid */
    virHashTable *objs;
};


/* This structure holds various callbacks and data needed
 * while parsing and creating domain XMLs */
struct _virDomainXMLOption {
    virObject parent;

    /* XML parser callbacks and defaults */
    virDomainDefParserConfig config;

    /* domain private data management callbacks */
    virDomainXMLPrivateDataCallbacks privateData;

    /* XML namespace callbacks */
    virDomainXMLNamespace ns;
 };


/* Private flags used internally by virDomainSaveStatus and
 * virDomainLoadStatus. */
typedef enum {
   /* dump internal domain status information */
   VIR_DOMAIN_XML_INTERNAL_STATUS = (1<<16),
   /* dump/parse <actual> element */
   VIR_DOMAIN_XML_INTERNAL_ACTUAL_NET = (1<<17),
   /* dump/parse original states of host PCI device */
   VIR_DOMAIN_XML_INTERNAL_PCI_ORIG_STATES = (1<<18),
   VIR_DOMAIN_XML_INTERNAL_ALLOW_ROM = (1<<19),
   VIR_DOMAIN_XML_INTERNAL_ALLOW_BOOT = (1<<20),
} virDomainXMLInternalFlags;

VIR_ENUM_IMPL(virDomainTaint, VIR_DOMAIN_TAINT_LAST,
              "custom-argv",
              "custom-monitor",
              "high-privileges",
              "shell-scripts",
              "disk-probing",
              "external-launch",
              "host-cpu");

VIR_ENUM_IMPL(virDomainVirt, VIR_DOMAIN_VIRT_LAST,
              "qemu",
              "kqemu",
              "kvm",
              "xen",
              "lxc",
              "uml",
              "openvz",
              "test",
              "vmware",
              "hyperv",
              "vbox",
              "phyp",
              "parallels")

VIR_ENUM_IMPL(virDomainBoot, VIR_DOMAIN_BOOT_LAST,
              "fd",
              "cdrom",
              "hd",
              "network")

VIR_ENUM_IMPL(virDomainBootMenu, VIR_DOMAIN_BOOT_MENU_LAST,
              "default",
              "yes",
              "no")

VIR_ENUM_IMPL(virDomainFeature, VIR_DOMAIN_FEATURE_LAST,
              "acpi",
              "apic",
              "pae",
              "hap",
              "viridian",
              "privnet",
              "hyperv")

VIR_ENUM_IMPL(virDomainFeatureState, VIR_DOMAIN_FEATURE_STATE_LAST,
              "default",
              "on",
              "off")

VIR_ENUM_IMPL(virDomainHyperv, VIR_DOMAIN_HYPERV_LAST,
              "relaxed")

VIR_ENUM_IMPL(virDomainLifecycle, VIR_DOMAIN_LIFECYCLE_LAST,
              "destroy",
              "restart",
              "rename-restart",
              "preserve")

VIR_ENUM_IMPL(virDomainLifecycleCrash, VIR_DOMAIN_LIFECYCLE_CRASH_LAST,
              "destroy",
              "restart",
              "rename-restart",
              "preserve",
              "coredump-destroy",
              "coredump-restart")

VIR_ENUM_IMPL(virDomainLockFailure, VIR_DOMAIN_LOCK_FAILURE_LAST,
              "default",
              "poweroff",
              "restart",
              "pause",
              "ignore")

VIR_ENUM_IMPL(virDomainPMState, VIR_DOMAIN_PM_STATE_LAST,
              "default",
              "yes",
              "no")

VIR_ENUM_IMPL(virDomainDevice, VIR_DOMAIN_DEVICE_LAST,
              "none",
              "disk",
              "lease",
              "filesystem",
              "interface",
              "input",
              "sound",
              "video",
              "hostdev",
              "watchdog",
              "controller",
              "graphics",
              "hub",
              "redirdev",
              "smartcard",
              "chr",
              "memballoon",
              "rng")

VIR_ENUM_IMPL(virDomainDeviceAddress, VIR_DOMAIN_DEVICE_ADDRESS_TYPE_LAST,
              "none",
              "pci",
              "drive",
              "virtio-serial",
              "ccid",
              "usb",
              "spapr-vio",
              "virtio-s390",
              "ccw")

VIR_ENUM_IMPL(virDomainDisk, VIR_DOMAIN_DISK_TYPE_LAST,
              "block",
              "file",
              "dir",
              "network",
              "volume")

VIR_ENUM_IMPL(virDomainDiskDevice, VIR_DOMAIN_DISK_DEVICE_LAST,
              "disk",
              "cdrom",
              "floppy",
              "lun")

VIR_ENUM_IMPL(virDomainDiskGeometryTrans, VIR_DOMAIN_DISK_TRANS_LAST,
              "default",
              "none",
              "auto",
              "lba")

VIR_ENUM_IMPL(virDomainDiskBus, VIR_DOMAIN_DISK_BUS_LAST,
              "ide",
              "fdc",
              "scsi",
              "virtio",
              "xen",
              "usb",
              "uml",
              "sata")

VIR_ENUM_IMPL(virDomainDiskCache, VIR_DOMAIN_DISK_CACHE_LAST,
              "default",
              "none",
              "writethrough",
              "writeback",
              "directsync",
              "unsafe")

VIR_ENUM_IMPL(virDomainDiskErrorPolicy, VIR_DOMAIN_DISK_ERROR_POLICY_LAST,
              "default",
              "stop",
              "report",
              "ignore",
              "enospace")

VIR_ENUM_IMPL(virDomainDiskProtocol, VIR_DOMAIN_DISK_PROTOCOL_LAST,
              "nbd",
              "rbd",
              "sheepdog",
              "gluster",
              "iscsi")

VIR_ENUM_IMPL(virDomainDiskProtocolTransport, VIR_DOMAIN_DISK_PROTO_TRANS_LAST,
              "tcp",
              "unix",
              "rdma")

VIR_ENUM_IMPL(virDomainDiskSecretType, VIR_DOMAIN_DISK_SECRET_TYPE_LAST,
              "none",
              "uuid",
              "usage")

VIR_ENUM_IMPL(virDomainDiskIo, VIR_DOMAIN_DISK_IO_LAST,
              "default",
              "native",
              "threads")

VIR_ENUM_IMPL(virDomainDiskSGIO, VIR_DOMAIN_DISK_SGIO_LAST,
              "default",
              "filtered",
              "unfiltered")

VIR_ENUM_IMPL(virDomainIoEventFd, VIR_DOMAIN_IO_EVENT_FD_LAST,
              "default",
              "on",
              "off")

VIR_ENUM_IMPL(virDomainVirtioEventIdx, VIR_DOMAIN_VIRTIO_EVENT_IDX_LAST,
              "default",
              "on",
              "off")

VIR_ENUM_IMPL(virDomainDiskCopyOnRead, VIR_DOMAIN_DISK_COPY_ON_READ_LAST,
              "default",
              "on",
              "off")

VIR_ENUM_IMPL(virDomainController, VIR_DOMAIN_CONTROLLER_TYPE_LAST,
              "ide",
              "fdc",
              "scsi",
              "sata",
              "virtio-serial",
              "ccid",
              "usb")

VIR_ENUM_IMPL(virDomainControllerModelSCSI, VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LAST,
              "auto",
              "buslogic",
              "lsilogic",
              "lsisas1068",
              "vmpvscsi",
              "ibmvscsi",
              "virtio-scsi",
              "lsisas1078");

VIR_ENUM_IMPL(virDomainControllerModelUSB, VIR_DOMAIN_CONTROLLER_MODEL_USB_LAST,
              "piix3-uhci",
              "piix4-uhci",
              "ehci",
              "ich9-ehci1",
              "ich9-uhci1",
              "ich9-uhci2",
              "ich9-uhci3",
              "vt82c686b-uhci",
              "pci-ohci",
              "nec-xhci",
              "none")

VIR_ENUM_IMPL(virDomainFS, VIR_DOMAIN_FS_TYPE_LAST,
              "mount",
              "block",
              "file",
              "template",
              "ram",
              "bind")

VIR_ENUM_IMPL(virDomainFSDriverType, VIR_DOMAIN_FS_DRIVER_TYPE_LAST,
              "default",
              "path",
              "handle")

VIR_ENUM_IMPL(virDomainFSAccessMode, VIR_DOMAIN_FS_ACCESSMODE_LAST,
              "passthrough",
              "mapped",
              "squash")

VIR_ENUM_IMPL(virDomainFSWrpolicy, VIR_DOMAIN_FS_WRPOLICY_LAST,
              "default",
              "immediate")

VIR_ENUM_IMPL(virDomainNet, VIR_DOMAIN_NET_TYPE_LAST,
              "user",
              "ethernet",
              "server",
              "client",
              "mcast",
              "network",
              "bridge",
              "internal",
              "direct",
              "hostdev")

VIR_ENUM_IMPL(virDomainNetBackend, VIR_DOMAIN_NET_BACKEND_TYPE_LAST,
              "default",
              "qemu",
              "vhost")

VIR_ENUM_IMPL(virDomainNetVirtioTxMode, VIR_DOMAIN_NET_VIRTIO_TX_MODE_LAST,
              "default",
              "iothread",
              "timer")

VIR_ENUM_IMPL(virDomainNetInterfaceLinkState, VIR_DOMAIN_NET_INTERFACE_LINK_STATE_LAST,
              "default",
              "up",
              "down")

VIR_ENUM_IMPL(virDomainChrSerialTarget,
              VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_LAST,
              "isa-serial",
              "usb-serial")

VIR_ENUM_IMPL(virDomainChrChannelTarget,
              VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_LAST,
              "none",
              "guestfwd",
              "virtio")

VIR_ENUM_IMPL(virDomainChrConsoleTarget,
              VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_LAST,
              "none",
              "serial",
              "xen",
              "uml",
              "virtio",
              "lxc",
              "openvz",
              "sclp",
              "sclplm")

VIR_ENUM_IMPL(virDomainChrDevice, VIR_DOMAIN_CHR_DEVICE_TYPE_LAST,
              "parallel",
              "serial",
              "console",
              "channel")

VIR_ENUM_IMPL(virDomainChr, VIR_DOMAIN_CHR_TYPE_LAST,
              "null",
              "vc",
              "pty",
              "dev",
              "file",
              "pipe",
              "stdio",
              "udp",
              "tcp",
              "unix",
              "spicevmc")

VIR_ENUM_IMPL(virDomainChrTcpProtocol, VIR_DOMAIN_CHR_TCP_PROTOCOL_LAST,
              "raw",
              "telnet",
              "telnets",
              "tls")

VIR_ENUM_IMPL(virDomainChrSpicevmc, VIR_DOMAIN_CHR_SPICEVMC_LAST,
              "vdagent",
              "smartcard",
              "usbredir")

VIR_ENUM_IMPL(virDomainSmartcard, VIR_DOMAIN_SMARTCARD_TYPE_LAST,
              "host",
              "host-certificates",
              "passthrough")

VIR_ENUM_IMPL(virDomainSoundCodec, VIR_DOMAIN_SOUND_CODEC_TYPE_LAST,
              "duplex",
              "micro")

VIR_ENUM_IMPL(virDomainSoundModel, VIR_DOMAIN_SOUND_MODEL_LAST,
              "sb16",
              "es1370",
              "pcspk",
              "ac97",
              "ich6")

VIR_ENUM_IMPL(virDomainMemDump, VIR_DOMAIN_MEM_DUMP_LAST,
              "default",
              "on",
              "off")

VIR_ENUM_IMPL(virDomainMemballoonModel, VIR_DOMAIN_MEMBALLOON_MODEL_LAST,
              "virtio",
              "xen",
              "none")

VIR_ENUM_IMPL(virDomainSmbiosMode, VIR_DOMAIN_SMBIOS_LAST,
              "none",
              "emulate",
              "host",
              "sysinfo")

VIR_ENUM_IMPL(virDomainWatchdogModel, VIR_DOMAIN_WATCHDOG_MODEL_LAST,
              "i6300esb",
              "ib700")

VIR_ENUM_IMPL(virDomainWatchdogAction, VIR_DOMAIN_WATCHDOG_ACTION_LAST,
              "reset",
              "shutdown",
              "poweroff",
              "pause",
              "dump",
              "none")

VIR_ENUM_IMPL(virDomainVideo, VIR_DOMAIN_VIDEO_TYPE_LAST,
              "vga",
              "cirrus",
              "vmvga",
              "xen",
              "vbox",
              "qxl")

VIR_ENUM_IMPL(virDomainInput, VIR_DOMAIN_INPUT_TYPE_LAST,
              "mouse",
              "tablet")

VIR_ENUM_IMPL(virDomainInputBus, VIR_DOMAIN_INPUT_BUS_LAST,
              "ps2",
              "usb",
              "xen")

VIR_ENUM_IMPL(virDomainGraphics, VIR_DOMAIN_GRAPHICS_TYPE_LAST,
              "sdl",
              "vnc",
              "rdp",
              "desktop",
              "spice")

VIR_ENUM_IMPL(virDomainGraphicsListen, VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_LAST,
              "none",
              "address",
              "network")

VIR_ENUM_IMPL(virDomainGraphicsAuthConnected,
              VIR_DOMAIN_GRAPHICS_AUTH_CONNECTED_LAST,
              "default",
              "fail",
              "disconnect",
              "keep")

VIR_ENUM_IMPL(virDomainGraphicsSpiceChannelName,
              VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_LAST,
              "main",
              "display",
              "inputs",
              "cursor",
              "playback",
              "record",
              "smartcard",
              "usbredir");

VIR_ENUM_IMPL(virDomainGraphicsSpiceChannelMode,
              VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_LAST,
              "any",
              "secure",
              "insecure");

VIR_ENUM_IMPL(virDomainGraphicsSpiceImageCompression,
              VIR_DOMAIN_GRAPHICS_SPICE_IMAGE_COMPRESSION_LAST,
              "default",
              "auto_glz",
              "auto_lz",
              "quic",
              "glz",
              "lz",
              "off");

VIR_ENUM_IMPL(virDomainGraphicsSpiceJpegCompression,
              VIR_DOMAIN_GRAPHICS_SPICE_JPEG_COMPRESSION_LAST,
              "default",
              "auto",
              "never",
              "always");

VIR_ENUM_IMPL(virDomainGraphicsSpiceZlibCompression,
              VIR_DOMAIN_GRAPHICS_SPICE_ZLIB_COMPRESSION_LAST,
              "default",
              "auto",
              "never",
              "always");

VIR_ENUM_IMPL(virDomainGraphicsSpicePlaybackCompression,
              VIR_DOMAIN_GRAPHICS_SPICE_PLAYBACK_COMPRESSION_LAST,
              "default",
              "on",
              "off");

VIR_ENUM_IMPL(virDomainGraphicsSpiceMouseMode,
              VIR_DOMAIN_GRAPHICS_SPICE_MOUSE_MODE_LAST,
              "default",
              "server",
              "client");

VIR_ENUM_IMPL(virDomainGraphicsSpiceStreamingMode,
              VIR_DOMAIN_GRAPHICS_SPICE_STREAMING_MODE_LAST,
              "default",
              "filter",
              "all",
              "off");

VIR_ENUM_IMPL(virDomainGraphicsSpiceClipboardCopypaste,
              VIR_DOMAIN_GRAPHICS_SPICE_CLIPBOARD_COPYPASTE_LAST,
              "default",
              "yes",
              "no");

VIR_ENUM_IMPL(virDomainHostdevMode, VIR_DOMAIN_HOSTDEV_MODE_LAST,
              "subsystem",
              "capabilities")

VIR_ENUM_IMPL(virDomainHostdevSubsys, VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_LAST,
              "usb",
              "pci")

VIR_ENUM_IMPL(virDomainHostdevCaps, VIR_DOMAIN_HOSTDEV_CAPS_TYPE_LAST,
              "storage",
              "misc",
              "net")

VIR_ENUM_IMPL(virDomainPciRombarMode,
              VIR_DOMAIN_PCI_ROMBAR_LAST,
              "default",
              "on",
              "off")

VIR_ENUM_IMPL(virDomainHub, VIR_DOMAIN_HUB_TYPE_LAST,
              "usb")

VIR_ENUM_IMPL(virDomainRedirdevBus, VIR_DOMAIN_REDIRDEV_BUS_LAST,
              "usb")

VIR_ENUM_IMPL(virDomainState, VIR_DOMAIN_LAST,
              "nostate",
              "running",
              "blocked",
              "paused",
              "shutdown",
              "shutoff",
              "crashed",
              "pmsuspended")

VIR_ENUM_IMPL(virDomainNostateReason, VIR_DOMAIN_NOSTATE_LAST,
              "unknown")

VIR_ENUM_IMPL(virDomainRunningReason, VIR_DOMAIN_RUNNING_LAST,
              "unknown",
              "booted",
              "migrated",
              "restored",
              "from snapshot",
              "unpaused",
              "migration canceled",
              "save canceled",
              "wakeup")

VIR_ENUM_IMPL(virDomainBlockedReason, VIR_DOMAIN_BLOCKED_LAST,
              "unknown")

VIR_ENUM_IMPL(virDomainPausedReason, VIR_DOMAIN_PAUSED_LAST,
              "unknown",
              "user",
              "migration",
              "save",
              "dump",
              "ioerror",
              "watchdog",
              "from snapshot",
              "shutdown",
              "snapshot")

VIR_ENUM_IMPL(virDomainShutdownReason, VIR_DOMAIN_SHUTDOWN_LAST,
              "unknown",
              "user")

VIR_ENUM_IMPL(virDomainShutoffReason, VIR_DOMAIN_SHUTOFF_LAST,
              "unknown",
              "shutdown",
              "destroyed",
              "crashed",
              "migrated",
              "saved",
              "failed",
              "from snapshot")

VIR_ENUM_IMPL(virDomainCrashedReason, VIR_DOMAIN_CRASHED_LAST,
              "unknown")

VIR_ENUM_IMPL(virDomainPMSuspendedReason, VIR_DOMAIN_PMSUSPENDED_LAST,
              "unknown")

VIR_ENUM_IMPL(virDomainSeclabel, VIR_DOMAIN_SECLABEL_LAST,
              "default",
              "none",
              "dynamic",
              "static")

VIR_ENUM_IMPL(virDomainClockOffset, VIR_DOMAIN_CLOCK_OFFSET_LAST,
              "utc",
              "localtime",
              "variable",
              "timezone");

VIR_ENUM_IMPL(virDomainClockBasis, VIR_DOMAIN_CLOCK_BASIS_LAST,
              "utc",
              "localtime");

VIR_ENUM_IMPL(virDomainTimerName, VIR_DOMAIN_TIMER_NAME_LAST,
              "platform",
              "pit",
              "rtc",
              "hpet",
              "tsc",
              "kvmclock");

VIR_ENUM_IMPL(virDomainTimerTrack, VIR_DOMAIN_TIMER_TRACK_LAST,
              "boot",
              "guest",
              "wall");

VIR_ENUM_IMPL(virDomainTimerTickpolicy, VIR_DOMAIN_TIMER_TICKPOLICY_LAST,
              "delay",
              "catchup",
              "merge",
              "discard");

VIR_ENUM_IMPL(virDomainTimerMode, VIR_DOMAIN_TIMER_MODE_LAST,
              "auto",
              "native",
              "emulate",
              "paravirt",
              "smpsafe");

VIR_ENUM_IMPL(virDomainStartupPolicy, VIR_DOMAIN_STARTUP_POLICY_LAST,
              "default",
              "mandatory",
              "requisite",
              "optional");

VIR_ENUM_IMPL(virDomainCpuPlacementMode, VIR_DOMAIN_CPU_PLACEMENT_MODE_LAST,
              "static",
              "auto");

VIR_ENUM_IMPL(virDomainDiskTray, VIR_DOMAIN_DISK_TRAY_LAST,
              "closed",
              "open");

VIR_ENUM_IMPL(virDomainRNGModel,
              VIR_DOMAIN_RNG_MODEL_LAST,
              "virtio");

VIR_ENUM_IMPL(virDomainRNGBackend,
              VIR_DOMAIN_RNG_BACKEND_LAST,
              "random",
              "egd");

#define VIR_DOMAIN_XML_WRITE_FLAGS  VIR_DOMAIN_XML_SECURE
#define VIR_DOMAIN_XML_READ_FLAGS   VIR_DOMAIN_XML_INACTIVE

static virClassPtr virDomainObjClass;
static virClassPtr virDomainObjListClass;
static virClassPtr virDomainXMLOptionClass;
static void virDomainObjDispose(void *obj);
static void virDomainObjListDispose(void *obj);
static void virDomainXMLOptionClassDispose(void *obj);

static int virDomainObjOnceInit(void)
{
    if (!(virDomainObjClass = virClassNew(virClassForObjectLockable(),
                                          "virDomainObj",
                                          sizeof(virDomainObj),
                                          virDomainObjDispose)))
        return -1;

    if (!(virDomainObjListClass = virClassNew(virClassForObjectLockable(),
                                              "virDomainObjList",
                                              sizeof(virDomainObjList),
                                              virDomainObjListDispose)))
        return -1;

    if (!(virDomainXMLOptionClass = virClassNew(virClassForObject(),
                                                "virDomainXMLOption",
                                                sizeof(virDomainXMLOption),
                                                virDomainXMLOptionClassDispose)))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(virDomainObj)


static void
virDomainXMLOptionClassDispose(void *obj)
{
    virDomainXMLOptionPtr xmlopt = obj;

    if (xmlopt->config.privFree)
        (xmlopt->config.privFree)(xmlopt->config.priv);
}


/**
 * virDomainXMLOptionNew:
 *
 * Allocate a new domain XML configuration
 */
virDomainXMLOptionPtr
virDomainXMLOptionNew(virDomainDefParserConfigPtr config,
                      virDomainXMLPrivateDataCallbacksPtr priv,
                      virDomainXMLNamespacePtr xmlns)
{
    virDomainXMLOptionPtr xmlopt;

    if (virDomainObjInitialize() < 0)
        return NULL;

    if (!(xmlopt = virObjectNew(virDomainXMLOptionClass)))
        return NULL;

    if (priv)
        xmlopt->privateData = *priv;

    if (config)
        xmlopt->config = *config;

    if (xmlns)
        xmlopt->ns = *xmlns;

    /* Technically this forbids to use one of Xerox's MAC address prefixes in
     * our hypervisor drivers. This shouldn't ever be a problem.
     *
     * Use the KVM prefix as default as it's in the privately administered
     * range */
    if (xmlopt->config.macPrefix[0] == 0 &&
        xmlopt->config.macPrefix[1] == 0 &&
        xmlopt->config.macPrefix[2] == 0) {
        xmlopt->config.macPrefix[0] = 0x52;
        xmlopt->config.macPrefix[1] = 0x54;
    }

    return xmlopt;
}

/**
 * virDomainXMLOptionGetNamespace:
 *
 * @xmlopt: XML parser configuration object
 *
 * Returns a pointer to the stored namespace structure.
 * The lifetime of the pointer is equal to @xmlopt;
 */
virDomainXMLNamespacePtr
virDomainXMLOptionGetNamespace(virDomainXMLOptionPtr xmlopt)
{
    return &xmlopt->ns;
}


void
virBlkioDeviceWeightArrayClear(virBlkioDeviceWeightPtr deviceWeights,
                               int ndevices)
{
    int i;

    for (i = 0; i < ndevices; i++)
        VIR_FREE(deviceWeights[i].path);
}

/**
 * virDomainBlkioDeviceWeightParseXML
 *
 * this function parses a XML node:
 *
 *   <device>
 *     <path>/fully/qualified/device/path</path>
 *     <weight>weight</weight>
 *   </device>
 *
 * and fills a virBlkioDeviceWeight struct.
 */
static int
virDomainBlkioDeviceWeightParseXML(xmlNodePtr root,
                                   virBlkioDeviceWeightPtr dw)
{
    char *c;
    xmlNodePtr node;

    node = root->children;
    while (node) {
        if (node->type == XML_ELEMENT_NODE) {
            if (xmlStrEqual(node->name, BAD_CAST "path") && !dw->path) {
                dw->path = (char *)xmlNodeGetContent(node);
            } else if (xmlStrEqual(node->name, BAD_CAST "weight")) {
                c = (char *)xmlNodeGetContent(node);
                if (virStrToLong_ui(c, NULL, 10, &dw->weight) < 0) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                   _("could not parse weight %s"),
                                   c);
                    VIR_FREE(c);
                    VIR_FREE(dw->path);
                    return -1;
                }
                VIR_FREE(c);
            }
        }
        node = node->next;
    }
    if (!dw->path) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("missing per-device path"));
        return -1;
    }

    return 0;
}



static void
virDomainObjListDataFree(void *payload, const void *name ATTRIBUTE_UNUSED)
{
    virDomainObjPtr obj = payload;
    virObjectUnref(obj);
}

virDomainObjListPtr virDomainObjListNew(void)
{
    virDomainObjListPtr doms;

    if (virDomainObjInitialize() < 0)
        return NULL;

    if (!(doms = virObjectLockableNew(virDomainObjListClass)))
        return NULL;

    if (!(doms->objs = virHashCreate(50, virDomainObjListDataFree))) {
        virObjectUnref(doms);
        return NULL;
    }

    return doms;
}


static void virDomainObjListDispose(void *obj)
{
    virDomainObjListPtr doms = obj;

    virHashFree(doms->objs);
}


static int virDomainObjListSearchID(const void *payload,
                                    const void *name ATTRIBUTE_UNUSED,
                                    const void *data)
{
    virDomainObjPtr obj = (virDomainObjPtr)payload;
    const int *id = data;
    int want = 0;

    virObjectLock(obj);
    if (virDomainObjIsActive(obj) &&
        obj->def->id == *id)
        want = 1;
    virObjectUnlock(obj);
    return want;
}

virDomainObjPtr virDomainObjListFindByID(const virDomainObjListPtr doms,
                                         int id)
{
    virDomainObjPtr obj;
    virObjectLock(doms);
    obj = virHashSearch(doms->objs, virDomainObjListSearchID, &id);
    if (obj)
        virObjectLock(obj);
    virObjectUnlock(doms);
    return obj;
}


virDomainObjPtr virDomainObjListFindByUUID(const virDomainObjListPtr doms,
                                           const unsigned char *uuid)
{
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    virDomainObjPtr obj;

    virObjectLock(doms);
    virUUIDFormat(uuid, uuidstr);

    obj = virHashLookup(doms->objs, uuidstr);
    if (obj)
        virObjectLock(obj);
    virObjectUnlock(doms);
    return obj;
}

static int virDomainObjListSearchName(const void *payload,
                                      const void *name ATTRIBUTE_UNUSED,
                                      const void *data)
{
    virDomainObjPtr obj = (virDomainObjPtr)payload;
    int want = 0;

    virObjectLock(obj);
    if (STREQ(obj->def->name, (const char *)data))
        want = 1;
    virObjectUnlock(obj);
    return want;
}

virDomainObjPtr virDomainObjListFindByName(const virDomainObjListPtr doms,
                                           const char *name)
{
    virDomainObjPtr obj;
    virObjectLock(doms);
    obj = virHashSearch(doms->objs, virDomainObjListSearchName, name);
    if (obj)
        virObjectLock(obj);
    virObjectUnlock(doms);
    return obj;
}


bool virDomainObjTaint(virDomainObjPtr obj,
                       enum virDomainTaintFlags taint)
{
    unsigned int flag = (1 << taint);

    if (obj->taint & flag)
        return false;

    obj->taint |= flag;
    return true;
}

static void
virDomainDeviceInfoFree(virDomainDeviceInfoPtr info)
{
    if (info) {
        virDomainDeviceInfoClear(info);
        VIR_FREE(info);
    }
}


static void
virDomainGraphicsAuthDefClear(virDomainGraphicsAuthDefPtr def)
{
    if (!def)
        return;

    VIR_FREE(def->passwd);

    /* Don't free def */
}

static void
virDomainGraphicsListenDefClear(virDomainGraphicsListenDefPtr def)
{
    if (!def)
        return;

    VIR_FREE(def->address);
    VIR_FREE(def->network);
    return;
}

void
virSecurityLabelDefFree(virSecurityLabelDefPtr def)
{
    if (!def)
        return;
    VIR_FREE(def->model);
    VIR_FREE(def->label);
    VIR_FREE(def->imagelabel);
    VIR_FREE(def->baselabel);
    VIR_FREE(def);
}


void
virSecurityDeviceLabelDefFree(virSecurityDeviceLabelDefPtr def)
{
    if (!def)
        return;
    VIR_FREE(def->model);
    VIR_FREE(def->label);
    VIR_FREE(def);
}


void virDomainGraphicsDefFree(virDomainGraphicsDefPtr def)
{
    int ii;

    if (!def)
        return;

    switch (def->type) {
    case VIR_DOMAIN_GRAPHICS_TYPE_VNC:
        VIR_FREE(def->data.vnc.socket);
        VIR_FREE(def->data.vnc.keymap);
        virDomainGraphicsAuthDefClear(&def->data.vnc.auth);
        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_SDL:
        VIR_FREE(def->data.sdl.display);
        VIR_FREE(def->data.sdl.xauth);
        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_RDP:
        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_DESKTOP:
        VIR_FREE(def->data.desktop.display);
        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_SPICE:
        VIR_FREE(def->data.spice.keymap);
        virDomainGraphicsAuthDefClear(&def->data.spice.auth);
        break;
    }

    for (ii = 0; ii < def->nListens; ii++)
        virDomainGraphicsListenDefClear(&def->listens[ii]);
    VIR_FREE(def->listens);

    VIR_FREE(def);
}

void virDomainInputDefFree(virDomainInputDefPtr def)
{
    if (!def)
        return;

    virDomainDeviceInfoClear(&def->info);
    VIR_FREE(def);
}

void virDomainLeaseDefFree(virDomainLeaseDefPtr def)
{
    if (!def)
        return;

    VIR_FREE(def->lockspace);
    VIR_FREE(def->key);
    VIR_FREE(def->path);

    VIR_FREE(def);
}

static void
virDomainDiskSourcePoolDefFree(virDomainDiskSourcePoolDefPtr def)
{
    if (!def)
        return;

    VIR_FREE(def->pool);
    VIR_FREE(def->volume);

    VIR_FREE(def);
}

void virDomainDiskDefFree(virDomainDiskDefPtr def)
{
    unsigned int i;

    if (!def)
        return;

    VIR_FREE(def->serial);
    VIR_FREE(def->src);
    virDomainDiskSourcePoolDefFree(def->srcpool);
    VIR_FREE(def->dst);
    VIR_FREE(def->driverName);
    virStorageFileFreeMetadata(def->backingChain);
    VIR_FREE(def->mirror);
    VIR_FREE(def->auth.username);
    VIR_FREE(def->wwn);
    VIR_FREE(def->vendor);
    VIR_FREE(def->product);
    if (def->auth.secretType == VIR_DOMAIN_DISK_SECRET_TYPE_USAGE)
        VIR_FREE(def->auth.secret.usage);
    virStorageEncryptionFree(def->encryption);
    virDomainDeviceInfoClear(&def->info);

    if (def->seclabels) {
        for (i = 0; i < def->nseclabels; i++)
            virSecurityDeviceLabelDefFree(def->seclabels[i]);
        VIR_FREE(def->seclabels);
    }

    for (i = 0 ; i < def->nhosts ; i++)
        virDomainDiskHostDefFree(&def->hosts[i]);
    VIR_FREE(def->hosts);

    VIR_FREE(def);
}

void virDomainDiskHostDefFree(virDomainDiskHostDefPtr def)
{
    if (!def)
        return;

    VIR_FREE(def->name);
    VIR_FREE(def->port);
    VIR_FREE(def->socket);
}

void virDomainControllerDefFree(virDomainControllerDefPtr def)
{
    if (!def)
        return;

    virDomainDeviceInfoClear(&def->info);

    VIR_FREE(def);
}

void virDomainFSDefFree(virDomainFSDefPtr def)
{
    if (!def)
        return;

    VIR_FREE(def->src);
    VIR_FREE(def->dst);
    virDomainDeviceInfoClear(&def->info);

    VIR_FREE(def);
}

void
virDomainActualNetDefFree(virDomainActualNetDefPtr def)
{
    if (!def)
        return;

    switch (def->type) {
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
        VIR_FREE(def->data.bridge.brname);
        break;
    case VIR_DOMAIN_NET_TYPE_DIRECT:
        VIR_FREE(def->data.direct.linkdev);
        break;
    case VIR_DOMAIN_NET_TYPE_HOSTDEV:
        virDomainHostdevDefClear(&def->data.hostdev.def);
        break;
    default:
        break;
    }

    VIR_FREE(def->virtPortProfile);
    virNetDevBandwidthFree(def->bandwidth);
    virNetDevVlanClear(&def->vlan);
    VIR_FREE(def);
}

void virDomainNetDefFree(virDomainNetDefPtr def)
{
    if (!def)
        return;

    VIR_FREE(def->model);

    switch (def->type) {
    case VIR_DOMAIN_NET_TYPE_ETHERNET:
        VIR_FREE(def->data.ethernet.dev);
        VIR_FREE(def->data.ethernet.ipaddr);
        break;

    case VIR_DOMAIN_NET_TYPE_SERVER:
    case VIR_DOMAIN_NET_TYPE_CLIENT:
    case VIR_DOMAIN_NET_TYPE_MCAST:
        VIR_FREE(def->data.socket.address);
        break;

    case VIR_DOMAIN_NET_TYPE_NETWORK:
        VIR_FREE(def->data.network.name);
        VIR_FREE(def->data.network.portgroup);
        virDomainActualNetDefFree(def->data.network.actual);
        break;

    case VIR_DOMAIN_NET_TYPE_BRIDGE:
        VIR_FREE(def->data.bridge.brname);
        VIR_FREE(def->data.bridge.ipaddr);
        break;

    case VIR_DOMAIN_NET_TYPE_INTERNAL:
        VIR_FREE(def->data.internal.name);
        break;

    case VIR_DOMAIN_NET_TYPE_DIRECT:
        VIR_FREE(def->data.direct.linkdev);
        break;

    case VIR_DOMAIN_NET_TYPE_HOSTDEV:
        virDomainHostdevDefClear(&def->data.hostdev.def);
        break;

    case VIR_DOMAIN_NET_TYPE_USER:
    case VIR_DOMAIN_NET_TYPE_LAST:
        break;
    }

    VIR_FREE(def->virtPortProfile);
    VIR_FREE(def->script);
    VIR_FREE(def->ifname);

    virDomainDeviceInfoClear(&def->info);

    VIR_FREE(def->filter);
    virNWFilterHashTableFree(def->filterparams);

    virNetDevBandwidthFree(def->bandwidth);
    virNetDevVlanClear(&def->vlan);

    VIR_FREE(def);
}

static void ATTRIBUTE_NONNULL(1)
virDomainChrSourceDefClear(virDomainChrSourceDefPtr def)
{
    switch (def->type) {
    case VIR_DOMAIN_CHR_TYPE_PTY:
    case VIR_DOMAIN_CHR_TYPE_DEV:
    case VIR_DOMAIN_CHR_TYPE_FILE:
    case VIR_DOMAIN_CHR_TYPE_PIPE:
        VIR_FREE(def->data.file.path);
        break;

    case VIR_DOMAIN_CHR_TYPE_UDP:
        VIR_FREE(def->data.udp.bindHost);
        VIR_FREE(def->data.udp.bindService);
        VIR_FREE(def->data.udp.connectHost);
        VIR_FREE(def->data.udp.connectService);
        break;

    case VIR_DOMAIN_CHR_TYPE_TCP:
        VIR_FREE(def->data.tcp.host);
        VIR_FREE(def->data.tcp.service);
        break;

    case VIR_DOMAIN_CHR_TYPE_UNIX:
        VIR_FREE(def->data.nix.path);
        break;
    }
}

/* Deep copies the contents of src into dest.  Return -1 and report
 * error on failure.  */
int
virDomainChrSourceDefCopy(virDomainChrSourceDefPtr dest,
                          virDomainChrSourceDefPtr src)
{
    if (!dest || !src)
        return -1;

    virDomainChrSourceDefClear(dest);

    switch (src->type) {
    case VIR_DOMAIN_CHR_TYPE_PTY:
    case VIR_DOMAIN_CHR_TYPE_DEV:
    case VIR_DOMAIN_CHR_TYPE_FILE:
    case VIR_DOMAIN_CHR_TYPE_PIPE:
        if (src->data.file.path &&
            !(dest->data.file.path = strdup(src->data.file.path))) {
            virReportOOMError();
            return -1;
        }
        break;

    case VIR_DOMAIN_CHR_TYPE_UDP:
        if (src->data.udp.bindHost &&
            !(dest->data.udp.bindHost = strdup(src->data.udp.bindHost))) {
            virReportOOMError();
            return -1;
        }

        if (src->data.udp.bindService &&
            !(dest->data.udp.bindService = strdup(src->data.udp.bindService))) {
            virReportOOMError();
            return -1;
        }

        if (src->data.udp.connectHost &&
            !(dest->data.udp.connectHost = strdup(src->data.udp.connectHost))) {
            virReportOOMError();
            return -1;
        }


        if (src->data.udp.connectService &&
            !(dest->data.udp.connectService = strdup(src->data.udp.connectService))) {
            virReportOOMError();
            return -1;
        }
        break;

    case VIR_DOMAIN_CHR_TYPE_TCP:
        if (src->data.tcp.host &&
            !(dest->data.tcp.host = strdup(src->data.tcp.host))) {
            virReportOOMError();
            return -1;
        }

        if (src->data.tcp.service &&
            !(dest->data.tcp.service = strdup(src->data.tcp.service))) {
            virReportOOMError();
            return -1;
        }
        break;

    case VIR_DOMAIN_CHR_TYPE_UNIX:
        if (src->data.nix.path &&
            !(dest->data.nix.path = strdup(src->data.nix.path))) {
            virReportOOMError();
            return -1;
        }
        break;
    }

    dest->type = src->type;

    return 0;
}

void virDomainChrSourceDefFree(virDomainChrSourceDefPtr def)
{
    if (!def)
        return;

    virDomainChrSourceDefClear(def);

    VIR_FREE(def);
}

/* virDomainChrSourceDefIsEqual:
 * @src: Source
 * @tgt: Target
 *
 * Compares source and target if they contain
 * the same information.
 */
static bool
virDomainChrSourceDefIsEqual(const virDomainChrSourceDef *src,
                             const virDomainChrSourceDef *tgt)
{
    if (tgt->type != src->type)
        return false;

    switch (src->type) {
    case VIR_DOMAIN_CHR_TYPE_PTY:
    case VIR_DOMAIN_CHR_TYPE_DEV:
    case VIR_DOMAIN_CHR_TYPE_FILE:
    case VIR_DOMAIN_CHR_TYPE_PIPE:
        return STREQ_NULLABLE(src->data.file.path, tgt->data.file.path);
        break;
    case VIR_DOMAIN_CHR_TYPE_UDP:
        return STREQ_NULLABLE(src->data.udp.bindHost, tgt->data.udp.bindHost) &&
            STREQ_NULLABLE(src->data.udp.bindService, tgt->data.udp.bindService) &&
            STREQ_NULLABLE(src->data.udp.connectHost, tgt->data.udp.connectHost) &&
            STREQ_NULLABLE(src->data.udp.connectService, tgt->data.udp.connectService);
        break;
    case VIR_DOMAIN_CHR_TYPE_TCP:
        return src->data.tcp.listen == tgt->data.tcp.listen &&
            src->data.tcp.protocol == tgt->data.tcp.protocol &&
            STREQ_NULLABLE(src->data.tcp.host, tgt->data.tcp.host) &&
            STREQ_NULLABLE(src->data.tcp.service, tgt->data.tcp.service);
        break;
    case VIR_DOMAIN_CHR_TYPE_UNIX:
        return src->data.nix.listen == tgt->data.nix.listen &&
            STREQ_NULLABLE(src->data.nix.path, tgt->data.nix.path);
        break;

    case VIR_DOMAIN_CHR_TYPE_VC:
    case VIR_DOMAIN_CHR_TYPE_STDIO:
    case VIR_DOMAIN_CHR_TYPE_SPICEVMC:
        /* nada */
        return true;
    }

    /* This should happen only on new,
     * yet unhandled type */

    return false;
}

void virDomainChrDefFree(virDomainChrDefPtr def)
{
    size_t i;

    if (!def)
        return;

    switch (def->deviceType) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
        switch (def->targetType) {
        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_GUESTFWD:
            VIR_FREE(def->target.addr);
            break;

        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO:
            VIR_FREE(def->target.name);
            break;
        }
        break;

    default:
        break;
    }

    virDomainChrSourceDefClear(&def->source);
    virDomainDeviceInfoClear(&def->info);

    if (def->seclabels) {
        for (i = 0; i < def->nseclabels; i++)
            virSecurityDeviceLabelDefFree(def->seclabels[i]);
        VIR_FREE(def->seclabels);
    }

    VIR_FREE(def);
}

void virDomainSmartcardDefFree(virDomainSmartcardDefPtr def)
{
    size_t i;
    if (!def)
        return;

    switch (def->type) {
    case VIR_DOMAIN_SMARTCARD_TYPE_HOST:
        break;

    case VIR_DOMAIN_SMARTCARD_TYPE_HOST_CERTIFICATES:
        for (i = 0; i < VIR_DOMAIN_SMARTCARD_NUM_CERTIFICATES; i++)
            VIR_FREE(def->data.cert.file[i]);
        VIR_FREE(def->data.cert.database);
        break;

    case VIR_DOMAIN_SMARTCARD_TYPE_PASSTHROUGH:
        virDomainChrSourceDefClear(&def->data.passthru);
        break;

    default:
        break;
    }

    virDomainDeviceInfoClear(&def->info);

    VIR_FREE(def);
}

void virDomainSoundCodecDefFree(virDomainSoundCodecDefPtr def)
{
    if (!def)
        return;

    VIR_FREE(def);
}

void virDomainSoundDefFree(virDomainSoundDefPtr def)
{
    if (!def)
        return;

    virDomainDeviceInfoClear(&def->info);

    int i;
    for (i = 0 ; i < def->ncodecs ; i++)
        virDomainSoundCodecDefFree(def->codecs[i]);
    VIR_FREE(def->codecs);

    VIR_FREE(def);
}

void virDomainMemballoonDefFree(virDomainMemballoonDefPtr def)
{
    if (!def)
        return;

    virDomainDeviceInfoClear(&def->info);

    VIR_FREE(def);
}

void virDomainWatchdogDefFree(virDomainWatchdogDefPtr def)
{
    if (!def)
        return;

    virDomainDeviceInfoClear(&def->info);

    VIR_FREE(def);
}

void virDomainVideoDefFree(virDomainVideoDefPtr def)
{
    if (!def)
        return;

    virDomainDeviceInfoClear(&def->info);

    VIR_FREE(def->accel);
    VIR_FREE(def);
}

virDomainHostdevDefPtr virDomainHostdevDefAlloc(void)
{
    virDomainHostdevDefPtr def = NULL;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }
    if (VIR_ALLOC(def->info) < 0) {
        virReportOOMError();
        VIR_FREE(def);
        return NULL;
    }
    return def;
}

void virDomainHostdevDefClear(virDomainHostdevDefPtr def)
{
    if (!def)
        return;

    /* Free all resources in the hostdevdef. Currently the only
     * such resource is the virDomainDeviceInfo.
     */

    /* If there is a parent device object, it will handle freeing
     * def->info.
     */
    if (def->parent.type == VIR_DOMAIN_DEVICE_NONE)
        virDomainDeviceInfoFree(def->info);

    if (def->mode == VIR_DOMAIN_HOSTDEV_MODE_CAPABILITIES) {
        switch (def->source.caps.type) {
        case VIR_DOMAIN_HOSTDEV_CAPS_TYPE_STORAGE:
            VIR_FREE(def->source.caps.u.storage.block);
            break;
        case VIR_DOMAIN_HOSTDEV_CAPS_TYPE_MISC:
            VIR_FREE(def->source.caps.u.misc.chardev);
            break;
        case VIR_DOMAIN_HOSTDEV_CAPS_TYPE_NET:
            VIR_FREE(def->source.caps.u.net.iface);
            break;
        }
    }
}

void virDomainHostdevDefFree(virDomainHostdevDefPtr def)
{
    if (!def)
        return;

    /* free all subordinate objects */
    virDomainHostdevDefClear(def);

    /* If there is a parent device object, it will handle freeing
     * the memory.
     */
    if (def->parent.type == VIR_DOMAIN_DEVICE_NONE)
        VIR_FREE(def);
}

void virDomainHubDefFree(virDomainHubDefPtr def)
{
    if (!def)
        return;

    virDomainDeviceInfoClear(&def->info);
    VIR_FREE(def);
}

void virDomainRedirdevDefFree(virDomainRedirdevDefPtr def)
{
    if (!def)
        return;

    virDomainChrSourceDefClear(&def->source.chr);
    virDomainDeviceInfoClear(&def->info);

    VIR_FREE(def);
}

void virDomainRedirFilterDefFree(virDomainRedirFilterDefPtr def)
{
    size_t i;

    if (!def)
        return;

    for (i = 0; i < def->nusbdevs; i++)
        VIR_FREE(def->usbdevs[i]);

    VIR_FREE(def->usbdevs);
    VIR_FREE(def);
}

void virDomainDeviceDefFree(virDomainDeviceDefPtr def)
{
    if (!def)
        return;

    switch ((virDomainDeviceType) def->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        virDomainDiskDefFree(def->data.disk);
        break;
    case VIR_DOMAIN_DEVICE_LEASE:
        virDomainLeaseDefFree(def->data.lease);
        break;
    case VIR_DOMAIN_DEVICE_NET:
        virDomainNetDefFree(def->data.net);
        break;
    case VIR_DOMAIN_DEVICE_INPUT:
        virDomainInputDefFree(def->data.input);
        break;
    case VIR_DOMAIN_DEVICE_SOUND:
        virDomainSoundDefFree(def->data.sound);
        break;
    case VIR_DOMAIN_DEVICE_VIDEO:
        virDomainVideoDefFree(def->data.video);
        break;
    case VIR_DOMAIN_DEVICE_HOSTDEV:
        virDomainHostdevDefFree(def->data.hostdev);
        break;
    case VIR_DOMAIN_DEVICE_WATCHDOG:
        virDomainWatchdogDefFree(def->data.watchdog);
        break;
    case VIR_DOMAIN_DEVICE_CONTROLLER:
        virDomainControllerDefFree(def->data.controller);
        break;
    case VIR_DOMAIN_DEVICE_GRAPHICS:
        virDomainGraphicsDefFree(def->data.graphics);
        break;
    case VIR_DOMAIN_DEVICE_HUB:
        virDomainHubDefFree(def->data.hub);
        break;
    case VIR_DOMAIN_DEVICE_REDIRDEV:
        virDomainRedirdevDefFree(def->data.redirdev);
        break;
    case VIR_DOMAIN_DEVICE_RNG:
        virDomainRNGDefFree(def->data.rng);
        break;
    case VIR_DOMAIN_DEVICE_NONE:
    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_CHR:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_LAST:
        break;
    }

    VIR_FREE(def);
}

static void
virDomainClockDefClear(virDomainClockDefPtr def)
{
    if (def->offset == VIR_DOMAIN_CLOCK_OFFSET_TIMEZONE)
        VIR_FREE(def->data.timezone);

    int i;
    for (i = 0; i < def->ntimers; i++)
        VIR_FREE(def->timers[i]);
    VIR_FREE(def->timers);
}

virDomainVcpuPinDefPtr *
virDomainVcpuPinDefCopy(virDomainVcpuPinDefPtr *src, int nvcpupin)
{
    int i = 0;
    virDomainVcpuPinDefPtr *ret = NULL;

    if (VIR_ALLOC_N(ret, nvcpupin) < 0) {
        goto no_memory;
    }

    for (i = 0; i < nvcpupin; i++) {
        if (VIR_ALLOC(ret[i]) < 0)
            goto no_memory;
        ret[i]->vcpuid = src[i]->vcpuid;
        if ((ret[i]->cpumask = virBitmapNewCopy(src[i]->cpumask)) == NULL)
            goto no_memory;
    }

    return ret;

no_memory:
    if (ret) {
        for (; i >= 0; --i) {
            if (ret[i]) {
                virBitmapFree(ret[i]->cpumask);
                VIR_FREE(ret[i]);
            }
        }
        VIR_FREE(ret);
    }
    virReportOOMError();

    return NULL;
}

void
virDomainVcpuPinDefFree(virDomainVcpuPinDefPtr def)
{
    if (def) {
        virBitmapFree(def->cpumask);
        VIR_FREE(def);
    }
}

void
virDomainVcpuPinDefArrayFree(virDomainVcpuPinDefPtr *def,
                             int nvcpupin)
{
    int i;

    if (!def || !nvcpupin)
        return;

    for (i = 0; i < nvcpupin; i++) {
        virDomainVcpuPinDefFree(def[i]);
    }

    VIR_FREE(def);
}

void virDomainDefFree(virDomainDefPtr def)
{
    unsigned int i;

    if (!def)
        return;

    /* hostdevs must be freed before nets (or any future "intelligent
     * hostdevs") because the pointer to the hostdev is really
     * pointing into the middle of the higher level device's object,
     * so the original object must still be available during the call
     * to virDomainHostdevDefFree().
     */
    for (i = 0 ; i < def->nhostdevs ; i++)
        virDomainHostdevDefFree(def->hostdevs[i]);
    VIR_FREE(def->hostdevs);

    for (i = 0 ; i < def->nleases ; i++)
        virDomainLeaseDefFree(def->leases[i]);
    VIR_FREE(def->leases);

    for (i = 0 ; i < def->ngraphics ; i++)
        virDomainGraphicsDefFree(def->graphics[i]);
    VIR_FREE(def->graphics);

    for (i = 0 ; i < def->ninputs ; i++)
        virDomainInputDefFree(def->inputs[i]);
    VIR_FREE(def->inputs);

    for (i = 0 ; i < def->ndisks ; i++)
        virDomainDiskDefFree(def->disks[i]);
    VIR_FREE(def->disks);

    for (i = 0 ; i < def->ncontrollers ; i++)
        virDomainControllerDefFree(def->controllers[i]);
    VIR_FREE(def->controllers);

    for (i = 0 ; i < def->nfss ; i++)
        virDomainFSDefFree(def->fss[i]);
    VIR_FREE(def->fss);

    for (i = 0 ; i < def->nnets ; i++)
        virDomainNetDefFree(def->nets[i]);
    VIR_FREE(def->nets);

    for (i = 0 ; i < def->nsmartcards ; i++)
        virDomainSmartcardDefFree(def->smartcards[i]);
    VIR_FREE(def->smartcards);

    for (i = 0 ; i < def->nserials ; i++)
        virDomainChrDefFree(def->serials[i]);
    VIR_FREE(def->serials);

    for (i = 0 ; i < def->nparallels ; i++)
        virDomainChrDefFree(def->parallels[i]);
    VIR_FREE(def->parallels);

    for (i = 0 ; i < def->nchannels ; i++)
        virDomainChrDefFree(def->channels[i]);
    VIR_FREE(def->channels);

    for (i = 0 ; i < def->nconsoles ; i++)
        virDomainChrDefFree(def->consoles[i]);
    VIR_FREE(def->consoles);

    for (i = 0 ; i < def->nsounds ; i++)
        virDomainSoundDefFree(def->sounds[i]);
    VIR_FREE(def->sounds);

    for (i = 0 ; i < def->nvideos ; i++)
        virDomainVideoDefFree(def->videos[i]);
    VIR_FREE(def->videos);

    for (i = 0 ; i < def->nhubs ; i++)
        virDomainHubDefFree(def->hubs[i]);
    VIR_FREE(def->hubs);

    for (i = 0 ; i < def->nredirdevs ; i++)
        virDomainRedirdevDefFree(def->redirdevs[i]);
    VIR_FREE(def->redirdevs);

    virDomainRNGDefFree(def->rng);

    VIR_FREE(def->os.type);
    VIR_FREE(def->os.machine);
    VIR_FREE(def->os.init);
    for (i = 0 ; def->os.initargv && def->os.initargv[i] ; i++)
        VIR_FREE(def->os.initargv[i]);
    VIR_FREE(def->os.initargv);
    VIR_FREE(def->os.kernel);
    VIR_FREE(def->os.initrd);
    VIR_FREE(def->os.cmdline);
    VIR_FREE(def->os.dtb);
    VIR_FREE(def->os.root);
    VIR_FREE(def->os.loader);
    VIR_FREE(def->os.bootloader);
    VIR_FREE(def->os.bootloaderArgs);

    virDomainClockDefClear(&def->clock);

    VIR_FREE(def->name);
    virBitmapFree(def->cpumask);
    VIR_FREE(def->emulator);
    VIR_FREE(def->description);
    VIR_FREE(def->title);

    virBlkioDeviceWeightArrayClear(def->blkio.devices,
                                   def->blkio.ndevices);
    VIR_FREE(def->blkio.devices);

    virDomainWatchdogDefFree(def->watchdog);

    virDomainMemballoonDefFree(def->memballoon);

    for (i = 0; i < def->nseclabels; i++)
        virSecurityLabelDefFree(def->seclabels[i]);
    VIR_FREE(def->seclabels);

    virCPUDefFree(def->cpu);

    virDomainVcpuPinDefArrayFree(def->cputune.vcpupin, def->cputune.nvcpupin);

    virDomainVcpuPinDefFree(def->cputune.emulatorpin);

    virBitmapFree(def->numatune.memory.nodemask);

    virSysinfoDefFree(def->sysinfo);

    virDomainRedirFilterDefFree(def->redirfilter);

    if (def->namespaceData && def->ns.free)
        (def->ns.free)(def->namespaceData);

    xmlFreeNode(def->metadata);

    VIR_FREE(def);
}

static void virDomainObjDispose(void *obj)
{
    virDomainObjPtr dom = obj;

    VIR_DEBUG("obj=%p", dom);
    virDomainDefFree(dom->def);
    virDomainDefFree(dom->newDef);

    if (dom->privateDataFreeFunc)
        (dom->privateDataFreeFunc)(dom->privateData);

    virDomainSnapshotObjListFree(dom->snapshots);
}

virDomainObjPtr
virDomainObjNew(virDomainXMLOptionPtr xmlopt)
{
    virDomainObjPtr domain;

    if (virDomainObjInitialize() < 0)
        return NULL;

    if (!(domain = virObjectLockableNew(virDomainObjClass)))
        return NULL;

    if (xmlopt->privateData.alloc) {
        if (!(domain->privateData = (xmlopt->privateData.alloc)())) {
            virReportOOMError();
            goto error;
        }
        domain->privateDataFreeFunc = xmlopt->privateData.free;
    }

    if (!(domain->snapshots = virDomainSnapshotObjListNew()))
        goto error;

    virObjectLock(domain);
    virDomainObjSetState(domain, VIR_DOMAIN_SHUTOFF,
                                 VIR_DOMAIN_SHUTOFF_UNKNOWN);

    VIR_DEBUG("obj=%p", domain);
    return domain;

error:
    virObjectUnref(domain);
    return NULL;
}

void virDomainObjAssignDef(virDomainObjPtr domain,
                           const virDomainDefPtr def,
                           bool live,
                           virDomainDefPtr *oldDef)
{
    if (oldDef)
        *oldDef = NULL;
    if (virDomainObjIsActive(domain)) {
        if (oldDef)
            *oldDef = domain->newDef;
        else
            virDomainDefFree(domain->newDef);
        domain->newDef = def;
    } else {
        if (live) {
            if (domain->def) {
                /* save current configuration to be restored on domain shutdown */
                if (!domain->newDef)
                    domain->newDef = domain->def;
                else
                    virDomainDefFree(domain->def);
            }
            domain->def = def;
        } else {
            if (oldDef)
                *oldDef = domain->def;
            else
                virDomainDefFree(domain->def);
            domain->def = def;
        }
    }
}



/*
 *
 * If flags & VIR_DOMAIN_OBJ_LIST_ADD_CHECK_LIVE then
 * this will refuse updating an existing def if the
 * current def is Live
 *
 * If flags & VIR_DOMAIN_OBJ_LIST_ADD_LIVE then
 * the @def being added is assumed to represent a
 * live config, not a future inactive config
 *
 */
static virDomainObjPtr
virDomainObjListAddLocked(virDomainObjListPtr doms,
                          const virDomainDefPtr def,
                          virDomainXMLOptionPtr xmlopt,
                          unsigned int flags,
                          virDomainDefPtr *oldDef)
{
    virDomainObjPtr vm;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    if (oldDef)
        *oldDef = false;

    virUUIDFormat(def->uuid, uuidstr);

    /* See if a VM with matching UUID already exists */
    if ((vm = virHashLookup(doms->objs, uuidstr))) {
        virObjectLock(vm);
        /* UUID matches, but if names don't match, refuse it */
        if (STRNEQ(vm->def->name, def->name)) {
            virUUIDFormat(vm->def->uuid, uuidstr);
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("domain '%s' is already defined with uuid %s"),
                           vm->def->name, uuidstr);
            goto error;
        }

        if (flags & VIR_DOMAIN_OBJ_LIST_ADD_CHECK_LIVE) {
            /* UUID & name match, but if VM is already active, refuse it */
            if (virDomainObjIsActive(vm)) {
                virReportError(VIR_ERR_OPERATION_INVALID,
                               _("domain is already active as '%s'"),
                               vm->def->name);
                goto error;
            }
        }

        virDomainObjAssignDef(vm,
                              def,
                              !!(flags & VIR_DOMAIN_OBJ_LIST_ADD_LIVE),
                              oldDef);
    } else {
        /* UUID does not match, but if a name matches, refuse it */
        if ((vm = virHashSearch(doms->objs, virDomainObjListSearchName, def->name))) {
            virObjectLock(vm);
            virUUIDFormat(vm->def->uuid, uuidstr);
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("domain '%s' already exists with uuid %s"),
                           def->name, uuidstr);
            goto error;
        }

        if (!(vm = virDomainObjNew(xmlopt)))
            goto cleanup;
        vm->def = def;

        virUUIDFormat(def->uuid, uuidstr);
        if (virHashAddEntry(doms->objs, uuidstr, vm) < 0) {
            virObjectUnref(vm);
            return NULL;
        }
    }
cleanup:
    return vm;

error:
    virObjectUnlock(vm);
    vm = NULL;
    goto cleanup;
}


virDomainObjPtr virDomainObjListAdd(virDomainObjListPtr doms,
                                    const virDomainDefPtr def,
                                    virDomainXMLOptionPtr xmlopt,
                                    unsigned int flags,
                                    virDomainDefPtr *oldDef)
{
    virDomainObjPtr ret;

    virObjectLock(doms);
    ret = virDomainObjListAddLocked(doms, def, xmlopt, flags, oldDef);
    virObjectUnlock(doms);
    return ret;
}

/*
 * Mark the running VM config as transient. Ensures transient hotplug
 * operations do not persist past shutdown.
 *
 * @param caps pointer to capabilities info
 * @param domain domain object pointer
 * @param live if true, run this operation even for an inactive domain.
 *   this allows freely updated domain->def with runtime defaults before
 *   starting the VM, which will be discarded on VM shutdown. Any cleanup
 *   paths need to be sure to handle newDef if the domain is never started.
 * @return 0 on success, -1 on failure
 */
int
virDomainObjSetDefTransient(virCapsPtr caps,
                            virDomainXMLOptionPtr xmlopt,
                            virDomainObjPtr domain,
                            bool live)
{
    int ret = -1;

    if (!virDomainObjIsActive(domain) && !live)
        return 0;

    if (!domain->persistent)
        return 0;

    if (domain->newDef)
        return 0;

    if (!(domain->newDef = virDomainDefCopy(domain->def, caps, xmlopt, false)))
        goto out;

    ret = 0;
out:
    return ret;
}

/*
 * Return the persistent domain configuration. If domain is transient,
 * return the running config.
 *
 * @param caps pointer to capabilities info
 * @param domain domain object pointer
 * @return NULL on error, virDOmainDefPtr on success
 */
virDomainDefPtr
virDomainObjGetPersistentDef(virCapsPtr caps,
                             virDomainXMLOptionPtr xmlopt,
                             virDomainObjPtr domain)
{
    if (virDomainObjSetDefTransient(caps, xmlopt, domain, false) < 0)
        return NULL;

    if (domain->newDef)
        return domain->newDef;
    else
        return domain->def;
}

/*
 * Helper method for --current, --live, and --config options, and check
 * whether domain is active or can get persistent domain configuration.
 *
 * Return 0 if success, also change the flags and get the persistent
 * domain configuration if needed. Return -1 on error.
 */
int
virDomainLiveConfigHelperMethod(virCapsPtr caps,
                                virDomainXMLOptionPtr xmlopt,
                                virDomainObjPtr dom,
                                unsigned int *flags,
                                virDomainDefPtr *persistentDef)
{
    bool isActive;
    int ret = -1;

    isActive = virDomainObjIsActive(dom);

    if ((*flags & (VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG)) ==
        VIR_DOMAIN_AFFECT_CURRENT) {
        if (isActive)
            *flags |= VIR_DOMAIN_AFFECT_LIVE;
        else
            *flags |= VIR_DOMAIN_AFFECT_CONFIG;
    }

    if (!isActive && (*flags & VIR_DOMAIN_AFFECT_LIVE)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("domain is not running"));
        goto cleanup;
    }

    if (*flags & VIR_DOMAIN_AFFECT_CONFIG) {
        if (!dom->persistent) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("cannot change persistent config of a "
                             "transient domain"));
            goto cleanup;
        }
        if (!(*persistentDef = virDomainObjGetPersistentDef(caps, xmlopt, dom))) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Get persistent config failed"));
            goto cleanup;
        }
    }

    ret = 0;

cleanup:
    return ret;
}

/*
 * The caller must hold a lock on the driver owning 'doms',
 * and must also have locked 'dom', to ensure no one else
 * is either waiting for 'dom' or still using it
 */
void virDomainObjListRemove(virDomainObjListPtr doms,
                            virDomainObjPtr dom)
{
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    virUUIDFormat(dom->def->uuid, uuidstr);
    virObjectRef(dom);
    virObjectUnlock(dom);

    virObjectLock(doms);
    virObjectLock(dom);
    virHashRemoveEntry(doms->objs, uuidstr);
    virObjectUnlock(dom);
    virObjectUnref(dom);
    virObjectUnlock(doms);
}

static int
virDomainDeviceCCWAddressIsValid(virDomainDeviceCCWAddressPtr addr)
{
    return addr->cssid <= VIR_DOMAIN_DEVICE_CCW_MAX_CSSID &&
        addr->ssid <= VIR_DOMAIN_DEVICE_CCW_MAX_SSID &&
        addr->devno <= VIR_DOMAIN_DEVICE_CCW_MAX_DEVNO;
}

int virDomainDeviceAddressIsValid(virDomainDeviceInfoPtr info,
                                  int type)
{
    if (info->type != type)
        return 0;

    switch (info->type) {
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI:
        return virDevicePCIAddressIsValid(&info->addr.pci);

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE:
        return 1;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390:
        return 1;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW:
        return virDomainDeviceCCWAddressIsValid(&info->addr.ccw);

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB:
        return 1;
    }

    return 0;
}

static bool
virDomainDeviceInfoIsSet(virDomainDeviceInfoPtr info, unsigned int flags)
{
    if (info->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)
        return true;
    if (info->alias && !(flags & VIR_DOMAIN_XML_INACTIVE))
        return true;
    if (info->mastertype != VIR_DOMAIN_CONTROLLER_MASTER_NONE)
        return true;
    if ((info->rombar != VIR_DOMAIN_PCI_ROMBAR_DEFAULT) ||
        info->romfile)
        return true;
    if (info->bootIndex)
        return true;
    return false;
}

int
virDomainDeviceInfoCopy(virDomainDeviceInfoPtr dst,
                        virDomainDeviceInfoPtr src)
{
    /* Assume that dst is already cleared */

    /* first a shallow copy of *everything* */
    *dst = *src;

    /* then redo the two fields that are pointers */
    dst->alias = NULL;
    dst->romfile = NULL;

    if (src->alias && !(dst->alias = strdup(src->alias))) {
        virReportOOMError();
        return -1;
    }
    if (src->romfile && !(dst->romfile = strdup(src->romfile))) {
        virReportOOMError();
        return -1;
    }
    return 0;
}

void virDomainDeviceInfoClear(virDomainDeviceInfoPtr info)
{
    VIR_FREE(info->alias);
    if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB) {
        VIR_FREE(info->addr.usb.port);
    }
    memset(&info->addr, 0, sizeof(info->addr));
    info->type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE;
    VIR_FREE(info->romfile);
}


static int virDomainDeviceInfoClearAlias(virDomainDefPtr def ATTRIBUTE_UNUSED,
                                         virDomainDeviceDefPtr device ATTRIBUTE_UNUSED,
                                         virDomainDeviceInfoPtr info,
                                         void *opaque ATTRIBUTE_UNUSED)
{
    VIR_FREE(info->alias);
    return 0;
}

static int virDomainDeviceInfoClearPCIAddress(virDomainDefPtr def ATTRIBUTE_UNUSED,
                                              virDomainDeviceDefPtr device ATTRIBUTE_UNUSED,
                                              virDomainDeviceInfoPtr info,
                                              void *opaque ATTRIBUTE_UNUSED)
{
    if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        memset(&info->addr, 0, sizeof(info->addr));
        info->type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE;
    }
    return 0;
}

static int
virDomainDeviceInfoClearCCWAddress(virDomainDefPtr def ATTRIBUTE_UNUSED,
                                   virDomainDeviceDefPtr device ATTRIBUTE_UNUSED,
                                   virDomainDeviceInfoPtr info,
                                   void *opaque ATTRIBUTE_UNUSED)
{
    if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW) {
        memset(&info->addr, 0, sizeof(info->addr));
        info->type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE;
    }
    return 0;
}

static int
virDomainDeviceInfoIterateInternal(virDomainDefPtr def,
                                   virDomainDeviceInfoCallback cb,
                                   bool all,
                                   void *opaque)
{
    int i;
    virDomainDeviceDef device;

    device.type = VIR_DOMAIN_DEVICE_DISK;
    for (i = 0; i < def->ndisks ; i++) {
        device.data.disk = def->disks[i];
        if (cb(def, &device, &def->disks[i]->info, opaque) < 0)
            return -1;
    }
    device.type = VIR_DOMAIN_DEVICE_NET;
    for (i = 0; i < def->nnets ; i++) {
        device.data.net = def->nets[i];
        if (cb(def, &device, &def->nets[i]->info, opaque) < 0)
            return -1;
    }
    device.type = VIR_DOMAIN_DEVICE_SOUND;
    for (i = 0; i < def->nsounds ; i++) {
        device.data.sound = def->sounds[i];
        if (cb(def, &device, &def->sounds[i]->info, opaque) < 0)
            return -1;
    }
    device.type = VIR_DOMAIN_DEVICE_HOSTDEV;
    for (i = 0; i < def->nhostdevs ; i++) {
        device.data.hostdev = def->hostdevs[i];
        if (cb(def, &device, def->hostdevs[i]->info, opaque) < 0)
            return -1;
    }
    device.type = VIR_DOMAIN_DEVICE_VIDEO;
    for (i = 0; i < def->nvideos ; i++) {
        device.data.video = def->videos[i];
        if (cb(def, &device, &def->videos[i]->info, opaque) < 0)
            return -1;
    }
    device.type = VIR_DOMAIN_DEVICE_CONTROLLER;
    for (i = 0; i < def->ncontrollers ; i++) {
        device.data.controller = def->controllers[i];
        if (cb(def, &device, &def->controllers[i]->info, opaque) < 0)
            return -1;
    }
    device.type = VIR_DOMAIN_DEVICE_SMARTCARD;
    for (i = 0; i < def->nsmartcards ; i++) {
        device.data.smartcard = def->smartcards[i];
        if (cb(def, &device, &def->smartcards[i]->info, opaque) < 0)
            return -1;
    }
    device.type = VIR_DOMAIN_DEVICE_CHR;
    for (i = 0; i < def->nserials ; i++) {
        device.data.chr = def->serials[i];
        if (cb(def, &device, &def->serials[i]->info, opaque) < 0)
            return -1;
    }
    for (i = 0; i < def->nparallels ; i++) {
        device.data.chr = def->parallels[i];
        if (cb(def, &device, &def->parallels[i]->info, opaque) < 0)
            return -1;
    }
    for (i = 0; i < def->nchannels ; i++) {
        device.data.chr = def->channels[i];
        if (cb(def, &device, &def->channels[i]->info, opaque) < 0)
            return -1;
    }
    for (i = 0; i < def->nconsoles ; i++) {
        if (!all &&
            i == 0 &&
            (def->consoles[i]->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL ||
             def->consoles[i]->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_NONE) &&
             STREQ_NULLABLE(def->os.type, "hvm"))
            continue;
        device.data.chr = def->consoles[i];
        if (cb(def, &device, &def->consoles[i]->info, opaque) < 0)
            return -1;
    }
    device.type = VIR_DOMAIN_DEVICE_INPUT;
    for (i = 0; i < def->ninputs ; i++) {
        device.data.input = def->inputs[i];
        if (cb(def, &device, &def->inputs[i]->info, opaque) < 0)
            return -1;
    }
    device.type = VIR_DOMAIN_DEVICE_FS;
    for (i = 0; i < def->nfss ; i++) {
        device.data.fs = def->fss[i];
        if (cb(def, &device, &def->fss[i]->info, opaque) < 0)
            return -1;
    }
    if (def->watchdog) {
        device.type = VIR_DOMAIN_DEVICE_WATCHDOG;
        device.data.watchdog = def->watchdog;
        if (cb(def, &device, &def->watchdog->info, opaque) < 0)
            return -1;
    }
    if (def->memballoon) {
        device.type = VIR_DOMAIN_DEVICE_MEMBALLOON;
        device.data.memballoon = def->memballoon;
        if (cb(def, &device, &def->memballoon->info, opaque) < 0)
            return -1;
    }
    if (def->rng) {
        device.type = VIR_DOMAIN_DEVICE_RNG;
        device.data.rng = def->rng;
        if (cb(def, &device, &def->rng->info, opaque) < 0)
            return -1;
    }
    device.type = VIR_DOMAIN_DEVICE_HUB;
    for (i = 0; i < def->nhubs ; i++) {
        device.data.hub = def->hubs[i];
        if (cb(def, &device, &def->hubs[i]->info, opaque) < 0)
            return -1;
    }

    /* This switch statement is here to trigger compiler warning when adding
     * a new device type. When you are adding a new field to the swtich you
     * also have to add a iteration statement above. Otherwise the switch
     * statement has no real function here and should be optimized out by the
     * compiler. */
    i = VIR_DOMAIN_DEVICE_LAST;
    switch ((virDomainDeviceType) i) {
    case VIR_DOMAIN_DEVICE_DISK:
    case VIR_DOMAIN_DEVICE_LEASE:
    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_NET:
    case VIR_DOMAIN_DEVICE_INPUT:
    case VIR_DOMAIN_DEVICE_SOUND:
    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_HOSTDEV:
    case VIR_DOMAIN_DEVICE_WATCHDOG:
    case VIR_DOMAIN_DEVICE_CONTROLLER:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_REDIRDEV:
    case VIR_DOMAIN_DEVICE_NONE:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_CHR:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_LAST:
    case VIR_DOMAIN_DEVICE_RNG:
        break;
    }

    return 0;
}


int
virDomainDeviceInfoIterate(virDomainDefPtr def,
                           virDomainDeviceInfoCallback cb,
                           void *opaque)
{
    return virDomainDeviceInfoIterateInternal(def, cb, false, opaque);
}


static int
virDomainDefPostParseInternal(virDomainDefPtr def,
                              virCapsPtr caps ATTRIBUTE_UNUSED)
{
    int i;

    /* verify init path for container based domains */
    if (STREQ(def->os.type, "exe") && !def->os.init) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("init binary must be specified"));
        return -1;
    }

    /*
     * Some really crazy backcompat stuff for consoles
     *
     * Historically the first (and only) '<console>' element in an HVM guest
     * was treated as being an alias for a <serial> device.
     *
     * So if we see that this console device should be a serial device, then we
     * move the config over to def->serials[0] (or discard it if that already
     * exists). However, given console can already be filled with aliased data
     * of def->serials[0]. Keep it then.
     *
     * We then fill def->consoles[0] with a stub just so we get sequencing
     * correct for consoles > 0
     */
    if (def->nconsoles > 0 && STREQ(def->os.type, "hvm") &&
        (def->consoles[0]->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL ||
         def->consoles[0]->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_NONE)) {
        /* First verify that only the first console is of type serial */
        for (i = 1; i < def->nconsoles; i++) {
            virDomainChrDefPtr cons = def->consoles[i];

            if (cons->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("Only the first console can be a serial port"));
                return -1;
            }
        }

        /* If there isn't a corresponding serial port:
         *  - create one and set, the console to be an alias for it
         *
         * If there is a corresponding serial port:
         * - Check if the source definition is equal:
         *    - if yes: leave it as-is
         *    - if no: change the console to be alias of the serial port
         */

        /* create the serial port definition from the console definition */
        if (def->nserials == 0) {
            if (VIR_APPEND_ELEMENT(def->serials, def->nserials,
                                   def->consoles[0]) < 0)
                goto no_memory;

            /* modify it to be a serial port */
            def->serials[0]->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL;
            def->serials[0]->targetType = VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_ISA;
            def->serials[0]->target.port = 0;
        } else {
            /* if the console source doesn't match */
            if (!virDomainChrSourceDefIsEqual(&def->serials[0]->source,
                                              &def->consoles[0]->source)) {
                virDomainChrDefFree(def->consoles[0]);
                def->consoles[0] = NULL;
            }
        }

        if (!def->consoles[0]) {
            /* allocate a new console type for the stolen one */
            if (VIR_ALLOC(def->consoles[0]) < 0)
                goto no_memory;

            /* Create an console alias for the serial port */
            def->consoles[0]->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE;
            def->consoles[0]->targetType = VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL;
        }
    }

    return 0;

no_memory:
    virReportOOMError();
    return -1;
}


static int
virDomainDeviceDefPostParseInternal(virDomainDeviceDefPtr dev,
                                    virDomainDefPtr def ATTRIBUTE_UNUSED,
                                    virCapsPtr caps ATTRIBUTE_UNUSED)
{
    if (dev->type == VIR_DOMAIN_DEVICE_CHR &&
        dev->data.chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE &&
        dev->data.chr->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_NONE)
        dev->data.chr->targetType = VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL;

    return 0;
}


static int
virDomainDeviceDefPostParse(virDomainDeviceDefPtr dev,
                            virDomainDefPtr def,
                            virCapsPtr caps,
                            virDomainXMLOptionPtr xmlopt)
{
    int ret;

    if (xmlopt && xmlopt->config.devicesPostParseCallback) {
        ret = xmlopt->config.devicesPostParseCallback(dev, def, caps,
                                                      xmlopt->config.priv);
        if (ret < 0)
            return ret;
    }

    if ((ret = virDomainDeviceDefPostParseInternal(dev, def, caps)) < 0)
        return ret;

    return 0;
}


struct virDomainDefPostParseDeviceIteratorData {
    virDomainDefPtr def;
    virCapsPtr caps;
    virDomainXMLOptionPtr xmlopt;
};


static int
virDomainDefPostParseDeviceIterator(virDomainDefPtr def ATTRIBUTE_UNUSED,
                                    virDomainDeviceDefPtr dev,
                                    virDomainDeviceInfoPtr info ATTRIBUTE_UNUSED,
                                    void *opaque)
{
    struct virDomainDefPostParseDeviceIteratorData *data = opaque;
    return virDomainDeviceDefPostParse(dev, data->def, data->caps, data->xmlopt);
}


static int
virDomainDefPostParse(virDomainDefPtr def,
                      virCapsPtr caps,
                      virDomainXMLOptionPtr xmlopt)
{
    int ret;
    struct virDomainDefPostParseDeviceIteratorData data = {
        .def = def,
        .caps = caps,
        .xmlopt = xmlopt,
    };

    /* call the domain config callback */
    if (xmlopt && xmlopt->config.domainPostParseCallback) {
        ret = xmlopt->config.domainPostParseCallback(def, caps,
                                                     xmlopt->config.priv);
        if (ret < 0)
            return ret;
    }

    /* iterate the devices */
    if ((ret = virDomainDeviceInfoIterateInternal(def,
                                                  virDomainDefPostParseDeviceIterator,
                                                  true,
                                                  &data)) < 0)
        return ret;


    if ((ret = virDomainDefPostParseInternal(def, caps)) < 0)
        return ret;

    return 0;
}


void virDomainDefClearPCIAddresses(virDomainDefPtr def)
{
    virDomainDeviceInfoIterate(def, virDomainDeviceInfoClearPCIAddress, NULL);
}

void virDomainDefClearCCWAddresses(virDomainDefPtr def)
{
    virDomainDeviceInfoIterate(def, virDomainDeviceInfoClearCCWAddress, NULL);
}

void virDomainDefClearDeviceAliases(virDomainDefPtr def)
{
    virDomainDeviceInfoIterate(def, virDomainDeviceInfoClearAlias, NULL);
}


/* Generate a string representation of a device address
 * @info address Device address to stringify
 */
static int ATTRIBUTE_NONNULL(2)
virDomainDeviceInfoFormat(virBufferPtr buf,
                          virDomainDeviceInfoPtr info,
                          unsigned int flags)
{
    if ((flags & VIR_DOMAIN_XML_INTERNAL_ALLOW_BOOT) && info->bootIndex)
        virBufferAsprintf(buf, "      <boot order='%d'/>\n", info->bootIndex);

    if (info->alias &&
        !(flags & VIR_DOMAIN_XML_INACTIVE)) {
        virBufferAsprintf(buf, "      <alias name='%s'/>\n", info->alias);
    }

    if (info->mastertype == VIR_DOMAIN_CONTROLLER_MASTER_USB) {
        virBufferAsprintf(buf, "      <master startport='%d'/>\n",
                          info->master.usb.startport);
    }

    if ((flags & VIR_DOMAIN_XML_INTERNAL_ALLOW_ROM) &&
        (info->rombar || info->romfile)) {

        virBufferAddLit(buf, "      <rom");
        if (info->rombar) {

            const char *rombar = virDomainPciRombarModeTypeToString(info->rombar);

            if (!rombar) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("unexpected rom bar value %d"),
                               info->rombar);
                return -1;
            }
            virBufferAsprintf(buf, " bar='%s'", rombar);
        }
        if (info->romfile)
            virBufferAsprintf(buf, " file='%s'", info->romfile);
        virBufferAddLit(buf, "/>\n");
    }

    if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE ||
        info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390)
        return 0;

    /* We'll be in domain/devices/[device type]/ so 3 level indent */
    virBufferAsprintf(buf, "      <address type='%s'",
                      virDomainDeviceAddressTypeToString(info->type));

    switch (info->type) {
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI:
        virBufferAsprintf(buf, " domain='0x%.4x' bus='0x%.2x' slot='0x%.2x' function='0x%.1x'",
                          info->addr.pci.domain,
                          info->addr.pci.bus,
                          info->addr.pci.slot,
                          info->addr.pci.function);
        if (info->addr.pci.multi) {
           virBufferAsprintf(buf, " multifunction='%s'",
                             virDeviceAddressPciMultiTypeToString(info->addr.pci.multi));
        }
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE:
        virBufferAsprintf(buf, " controller='%d' bus='%d' target='%d' unit='%d'",
                          info->addr.drive.controller,
                          info->addr.drive.bus,
                          info->addr.drive.target,
                          info->addr.drive.unit);
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL:
        virBufferAsprintf(buf, " controller='%d' bus='%d' port='%d'",
                          info->addr.vioserial.controller,
                          info->addr.vioserial.bus,
                          info->addr.vioserial.port);
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID:
        virBufferAsprintf(buf, " controller='%d' slot='%d'",
                          info->addr.ccid.controller,
                          info->addr.ccid.slot);
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB:
        virBufferAsprintf(buf, " bus='%d' port='%s'",
                          info->addr.usb.bus,
                          info->addr.usb.port);
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO:
        if (info->addr.spaprvio.has_reg)
            virBufferAsprintf(buf, " reg='0x%llx'", info->addr.spaprvio.reg);
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW:
        virBufferAsprintf(buf, " cssid='0x%x' ssid='0x%x' devno='0x%04x'",
                          info->addr.ccw.cssid,
                          info->addr.ccw.ssid,
                          info->addr.ccw.devno);
        break;

    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown address type '%d'"), info->type);
        return -1;
    }

    virBufferAddLit(buf, "/>\n");

    return 0;
}

static int
virDomainDeviceDriveAddressParseXML(xmlNodePtr node,
                                    virDomainDeviceDriveAddressPtr addr)
{
    char *bus, *unit, *controller, *target;
    int ret = -1;

    memset(addr, 0, sizeof(*addr));

    controller = virXMLPropString(node, "controller");
    bus = virXMLPropString(node, "bus");
    target = virXMLPropString(node, "target");
    unit = virXMLPropString(node, "unit");

    if (controller &&
        virStrToLong_ui(controller, NULL, 10, &addr->controller) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot parse <address> 'controller' attribute"));
        goto cleanup;
    }

    if (bus &&
        virStrToLong_ui(bus, NULL, 10, &addr->bus) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot parse <address> 'bus' attribute"));
        goto cleanup;
    }

    if (target &&
        virStrToLong_ui(target, NULL, 10, &addr->target) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot parse <address> 'target' attribute"));
        goto cleanup;
    }

    if (unit &&
        virStrToLong_ui(unit, NULL, 10, &addr->unit) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot parse <address> 'unit' attribute"));
        goto cleanup;
    }

    ret = 0;

cleanup:
    VIR_FREE(controller);
    VIR_FREE(bus);
    VIR_FREE(target);
    VIR_FREE(unit);
    return ret;
}


static int
virDomainDeviceVirtioSerialAddressParseXML(
    xmlNodePtr node,
    virDomainDeviceVirtioSerialAddressPtr addr
)
{
    char *controller, *bus, *port;
    int ret = -1;

    memset(addr, 0, sizeof(*addr));

    controller = virXMLPropString(node, "controller");
    bus = virXMLPropString(node, "bus");
    port = virXMLPropString(node, "port");

    if (controller &&
        virStrToLong_ui(controller, NULL, 10, &addr->controller) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot parse <address> 'controller' attribute"));
        goto cleanup;
    }

    if (bus &&
        virStrToLong_ui(bus, NULL, 10, &addr->bus) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot parse <address> 'bus' attribute"));
        goto cleanup;
    }

    if (port &&
        virStrToLong_ui(port, NULL, 10, &addr->port) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot parse <address> 'port' attribute"));
        goto cleanup;
    }

    ret = 0;

cleanup:
    VIR_FREE(controller);
    VIR_FREE(bus);
    VIR_FREE(port);
    return ret;
}

static int
virDomainDeviceCCWAddressParseXML(xmlNodePtr node,
                                  virDomainDeviceCCWAddressPtr addr)
{
    int   ret = -1;
    char *cssid;
    char *ssid;
    char *devno;

    memset(addr, 0, sizeof(*addr));

    cssid = virXMLPropString(node, "cssid");
    ssid = virXMLPropString(node, "ssid");
    devno = virXMLPropString(node, "devno");

    if (cssid && ssid && devno) {
        if (cssid &&
            virStrToLong_ui(cssid, NULL, 0, &addr->cssid) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Cannot parse <address> 'cssid' attribute"));
            goto cleanup;
        }
        if (ssid &&
            virStrToLong_ui(ssid, NULL, 0, &addr->ssid) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Cannot parse <address> 'ssid' attribute"));
            goto cleanup;
        }
        if (devno &&
            virStrToLong_ui(devno, NULL, 0, &addr->devno) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Cannot parse <address> 'devno' attribute"));
            goto cleanup;
        }
        if (!virDomainDeviceCCWAddressIsValid(addr)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Invalid specification for virtio ccw"
                             " address: cssid='%s' ssid='%s' devno='%s'"),
                           cssid, ssid, devno);
            goto cleanup;
        }
        addr->assigned = true;
    } else if (cssid || ssid || devno) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Invalid partial specification for virtio ccw"
                         " address"));
        goto cleanup;
    }

    ret = 0;

cleanup:
    VIR_FREE(cssid);
    VIR_FREE(ssid);
    VIR_FREE(devno);
    return ret;
}

static int
virDomainDeviceCcidAddressParseXML(xmlNodePtr node,
                                   virDomainDeviceCcidAddressPtr addr)
{
    char *controller, *slot;
    int ret = -1;

    memset(addr, 0, sizeof(*addr));

    controller = virXMLPropString(node, "controller");
    slot = virXMLPropString(node, "slot");

    if (controller &&
        virStrToLong_ui(controller, NULL, 10, &addr->controller) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot parse <address> 'controller' attribute"));
        goto cleanup;
    }

    if (slot &&
        virStrToLong_ui(slot, NULL, 10, &addr->slot) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot parse <address> 'slot' attribute"));
        goto cleanup;
    }

    ret = 0;

cleanup:
    VIR_FREE(controller);
    VIR_FREE(slot);
    return ret;
}

static int
virDomainDeviceUSBAddressParseXML(xmlNodePtr node,
                                  virDomainDeviceUSBAddressPtr addr)
{
    char *port, *bus, *tmp;
    unsigned int p;
    int ret = -1;

    memset(addr, 0, sizeof(*addr));

    port = virXMLPropString(node, "port");
    bus = virXMLPropString(node, "bus");

    if (port &&
        ((virStrToLong_ui(port, &tmp, 10, &p) < 0 || (*tmp != '\0' && *tmp != '.')) ||
         (*tmp == '.' && (virStrToLong_ui(tmp + 1, &tmp, 10, &p) < 0 || (*tmp != '\0' && *tmp != '.'))) ||
         (*tmp == '.' && (virStrToLong_ui(tmp + 1, &tmp, 10, &p) < 0 || (*tmp != '\0' && *tmp != '.'))) ||
         (*tmp == '.' && (virStrToLong_ui(tmp + 1, &tmp, 10, &p) < 0 || (*tmp != '\0'))))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot parse <address> 'port' attribute"));
        goto cleanup;
    }

    addr->port = port;
    port = NULL;

    if (bus &&
        virStrToLong_ui(bus, NULL, 10, &addr->bus) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot parse <address> 'bus' attribute"));
        goto cleanup;
    }

    ret = 0;

cleanup:
    VIR_FREE(bus);
    VIR_FREE(port);
    return ret;
}

static int
virDomainDeviceSpaprVioAddressParseXML(xmlNodePtr node,
                                      virDomainDeviceSpaprVioAddressPtr addr)
{
    char *reg;
    int ret;

    memset(addr, 0, sizeof(*addr));

    reg = virXMLPropString(node, "reg");
    if (reg) {
        if (virStrToLong_ull(reg, NULL, 16, &addr->reg) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Cannot parse <address> 'reg' attribute"));
            ret = -1;
            goto cleanup;
        }

        addr->has_reg = true;
    }

    ret = 0;
cleanup:
    VIR_FREE(reg);
    return ret;
}

static int
virDomainDeviceUSBMasterParseXML(xmlNodePtr node,
                                 virDomainDeviceUSBMasterPtr master)
{
    char *startport;
    int ret = -1;

    memset(master, 0, sizeof(*master));

    startport = virXMLPropString(node, "startport");

    if (startport &&
        virStrToLong_ui(startport, NULL, 10, &master->startport) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot parse <master> 'startport' attribute"));
        goto cleanup;
    }

    ret = 0;

cleanup:
    VIR_FREE(startport);
    return ret;
}

static int
virDomainDeviceBootParseXML(xmlNodePtr node,
                            int *bootIndex,
                            virBitmapPtr bootMap)
{
    char *order;
    int boot;
    int ret = -1;

    order = virXMLPropString(node, "order");
    if (!order) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing boot order attribute"));
        goto cleanup;
    } else if (virStrToLong_i(order, NULL, 10, &boot) < 0 ||
               boot <= 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("incorrect boot order '%s', expecting positive integer"),
                       order);
        goto cleanup;
    }

    if (bootMap) {
        bool set;
        if (virBitmapGetBit(bootMap, boot - 1, &set) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("boot orders have to be contiguous and starting from 1"));
            goto cleanup;
        } else if (set) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("boot order %d used for more than one device"), boot);
            goto cleanup;
        }
        ignore_value(virBitmapSetBit(bootMap, boot - 1));
    }

    *bootIndex = boot;
    ret = 0;

cleanup:
    VIR_FREE(order);
    return ret;
}

/* Parse the XML definition for a device address
 * @param node XML nodeset to parse for device address definition
 */
static int
virDomainDeviceInfoParseXML(xmlNodePtr node,
                            virBitmapPtr bootMap,
                            virDomainDeviceInfoPtr info,
                            unsigned int flags)
{
    xmlNodePtr cur;
    xmlNodePtr address = NULL;
    xmlNodePtr master = NULL;
    xmlNodePtr alias = NULL;
    xmlNodePtr boot = NULL;
    xmlNodePtr rom = NULL;
    char *type = NULL;
    int ret = -1;

    virDomainDeviceInfoClear(info);

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (alias == NULL &&
                !(flags & VIR_DOMAIN_XML_INACTIVE) &&
                xmlStrEqual(cur->name, BAD_CAST "alias")) {
                alias = cur;
            } else if (address == NULL &&
                       xmlStrEqual(cur->name, BAD_CAST "address")) {
                address = cur;
            } else if (master == NULL &&
                       xmlStrEqual(cur->name, BAD_CAST "master")) {
                master = cur;
            } else if (boot == NULL &&
                       (flags & VIR_DOMAIN_XML_INTERNAL_ALLOW_BOOT) &&
                       xmlStrEqual(cur->name, BAD_CAST "boot")) {
                boot = cur;
            } else if (rom == NULL &&
                       (flags & VIR_DOMAIN_XML_INTERNAL_ALLOW_ROM) &&
                       xmlStrEqual(cur->name, BAD_CAST "rom")) {
                rom = cur;
            }
        }
        cur = cur->next;
    }

    if (alias)
        info->alias = virXMLPropString(alias, "name");

    if (master) {
        info->mastertype = VIR_DOMAIN_CONTROLLER_MASTER_USB;
        if (virDomainDeviceUSBMasterParseXML(master, &info->master.usb) < 0)
            goto cleanup;
    }

    if (boot) {
        if (virDomainDeviceBootParseXML(boot, &info->bootIndex, bootMap))
            goto cleanup;
    }

    if (rom) {
        char *rombar = virXMLPropString(rom, "bar");
        if (rombar &&
            ((info->rombar = virDomainPciRombarModeTypeFromString(rombar)) <= 0)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unknown rom bar value '%s'"), rombar);
            VIR_FREE(rombar);
            goto cleanup;
        }
        VIR_FREE(rombar);
        info->romfile = virXMLPropString(rom, "file");
    }

    if (!address)
        return 0;

    type = virXMLPropString(address, "type");

    if (type) {
        if ((info->type = virDomainDeviceAddressTypeFromString(type)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown address type '%s'"), type);
            goto cleanup;
        }
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("No type specified for device address"));
        goto cleanup;
    }

    switch (info->type) {
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI:
        if (virDevicePCIAddressParseXML(address, &info->addr.pci) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE:
        if (virDomainDeviceDriveAddressParseXML(address, &info->addr.drive) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL:
        if (virDomainDeviceVirtioSerialAddressParseXML
                (address, &info->addr.vioserial) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID:
        if (virDomainDeviceCcidAddressParseXML(address, &info->addr.ccid) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB:
        if (virDomainDeviceUSBAddressParseXML(address, &info->addr.usb) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO:
        if (virDomainDeviceSpaprVioAddressParseXML(address, &info->addr.spaprvio) < 0)
            goto cleanup;
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW:
        if (virDomainDeviceCCWAddressParseXML
                (address, &info->addr.ccw) < 0)
            goto cleanup;
        break;

    default:
        /* Should not happen */
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Unknown device address type"));
        goto cleanup;
    }

    ret = 0;

cleanup:
    if (ret == -1)
        VIR_FREE(info->alias);
    VIR_FREE(type);
    return ret;
}

static int
virDomainParseLegacyDeviceAddress(char *devaddr,
                                  virDevicePCIAddressPtr pci)
{
    char *tmp;

    /* expected format: <domain>:<bus>:<slot> */
    if (/* domain */
        virStrToLong_ui(devaddr, &tmp, 16, &pci->domain) < 0 || *tmp != ':' ||
        /* bus */
        virStrToLong_ui(tmp + 1, &tmp, 16, &pci->bus) < 0 || *tmp != ':' ||
        /* slot */
        virStrToLong_ui(tmp + 1, NULL, 16, &pci->slot) < 0)
        return -1;

    return 0;
}

static int
virDomainHostdevSubsysUsbDefParseXML(const xmlNodePtr node,
                                     virDomainHostdevDefPtr def)
{

    int ret = -1;
    int got_product, got_vendor;
    xmlNodePtr cur;
    char *startupPolicy = NULL;
    char *autoAddress;

    if ((startupPolicy = virXMLPropString(node, "startupPolicy"))) {
        def->startupPolicy =
            virDomainStartupPolicyTypeFromString(startupPolicy);
        if (def->startupPolicy <= 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("Unknown startup policy '%s'"),
                           startupPolicy);
            VIR_FREE(startupPolicy);
            goto out;
        }
        VIR_FREE(startupPolicy);
    }

    if ((autoAddress = virXMLPropString(node, "autoAddress"))) {
        if (STREQ(autoAddress, "yes"))
            def->source.subsys.u.usb.autoAddress = true;
        VIR_FREE(autoAddress);
    }

    /* Product can validly be 0, so we need some extra help to determine
     * if it is uninitialized*/
    got_product = 0;
    got_vendor = 0;

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (xmlStrEqual(cur->name, BAD_CAST "vendor")) {
                char *vendor = virXMLPropString(cur, "id");

                if (vendor) {
                    got_vendor = 1;
                    if (virStrToLong_ui(vendor, NULL, 0,
                                    &def->source.subsys.u.usb.vendor) < 0) {
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                       _("cannot parse vendor id %s"), vendor);
                        VIR_FREE(vendor);
                        goto out;
                    }
                    VIR_FREE(vendor);
                } else {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   "%s", _("usb vendor needs id"));
                    goto out;
                }
            } else if (xmlStrEqual(cur->name, BAD_CAST "product")) {
                char* product = virXMLPropString(cur, "id");

                if (product) {
                    got_product = 1;
                    if (virStrToLong_ui(product, NULL, 0,
                                        &def->source.subsys.u.usb.product) < 0) {
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                       _("cannot parse product %s"),
                                       product);
                        VIR_FREE(product);
                        goto out;
                    }
                    VIR_FREE(product);
                } else {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   "%s", _("usb product needs id"));
                    goto out;
                }
            } else if (xmlStrEqual(cur->name, BAD_CAST "address")) {
                char *bus, *device;

                bus = virXMLPropString(cur, "bus");
                if (bus) {
                    if (virStrToLong_ui(bus, NULL, 0,
                                        &def->source.subsys.u.usb.bus) < 0) {
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                       _("cannot parse bus %s"), bus);
                        VIR_FREE(bus);
                        goto out;
                    }
                    VIR_FREE(bus);
                } else {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   "%s", _("usb address needs bus id"));
                    goto out;
                }

                device = virXMLPropString(cur, "device");
                if (device) {
                    if (virStrToLong_ui(device, NULL, 0,
                                        &def->source.subsys.u.usb.device) < 0)  {
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                       _("cannot parse device %s"),
                                       device);
                        VIR_FREE(device);
                        goto out;
                    }
                    VIR_FREE(device);
                } else {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                   _("usb address needs device id"));
                    goto out;
                }
            } else {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("unknown usb source type '%s'"),
                               cur->name);
                goto out;
            }
        }
        cur = cur->next;
    }

    if (got_vendor && def->source.subsys.u.usb.vendor == 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("vendor cannot be 0."));
        goto out;
    }

    if (!got_vendor && got_product) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing vendor"));
        goto out;
    }
    if (got_vendor && !got_product) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing product"));
        goto out;
    }

    ret = 0;
out:
    return ret;
}

/* The internal XML for host PCI device's original states:
 *
 * <origstates>
 *   <unbind/>
 *   <removeslot/>
 *   <reprobe/>
 * </origstates>
 */
static int
virDomainHostdevSubsysPciOrigStatesDefParseXML(const xmlNodePtr node,
                                               virDomainHostdevOrigStatesPtr def)
{
    xmlNodePtr cur;
    cur = node->children;

    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (xmlStrEqual(cur->name, BAD_CAST "unbind")) {
                def->states.pci.unbind_from_stub = true;
            } else if (xmlStrEqual(cur->name, BAD_CAST "removeslot")) {
                def->states.pci.remove_slot = true;
            } else if (xmlStrEqual(cur->name, BAD_CAST "reprobe")) {
                def->states.pci.reprobe = true;
            } else {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("unsupported element '%s' of 'origstates'"),
                               cur->name);
                return -1;
            }
        }
        cur = cur->next;
    }

    return 0;
}

static int
virDomainHostdevSubsysPciDefParseXML(const xmlNodePtr node,
                                     virDomainHostdevDefPtr def,
                                     unsigned int flags)
{
    int ret = -1;
    xmlNodePtr cur;

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (xmlStrEqual(cur->name, BAD_CAST "address")) {
                virDevicePCIAddressPtr addr =
                    &def->source.subsys.u.pci;

                if (virDevicePCIAddressParseXML(cur, addr) < 0)
                    goto out;
            } else if ((flags & VIR_DOMAIN_XML_INTERNAL_STATUS) &&
                       xmlStrEqual(cur->name, BAD_CAST "state")) {
                /* Legacy back-compat. Don't add any more attributes here */
                char *devaddr = virXMLPropString(cur, "devaddr");
                if (devaddr &&
                    virDomainParseLegacyDeviceAddress(devaddr,
                                                      &def->info->addr.pci) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("Unable to parse devaddr parameter '%s'"),
                                   devaddr);
                    VIR_FREE(devaddr);
                    goto out;
                }
                def->info->type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
            } else if ((flags & VIR_DOMAIN_XML_INTERNAL_PCI_ORIG_STATES) &&
                       xmlStrEqual(cur->name, BAD_CAST "origstates")) {
                virDomainHostdevOrigStatesPtr states = &def->origstates;
                if (virDomainHostdevSubsysPciOrigStatesDefParseXML(cur, states) < 0)
                    goto out;
            } else {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("unknown pci source type '%s'"),
                               cur->name);
                goto out;
            }
        }
        cur = cur->next;
    }

    ret = 0;
out:
    return ret;
}

static int
virDomainHostdevDefParseXMLSubsys(xmlNodePtr node,
                                  xmlXPathContextPtr ctxt,
                                  const char *type,
                                  virDomainHostdevDefPtr def,
                                  unsigned int flags)
{
    xmlNodePtr sourcenode;
    char *managed = NULL;
    int ret = -1;

    /* @managed can be read from the xml document - it is always an
     * attribute of the toplevel element, no matter what type of
     * element that might be (pure hostdev, or higher level device
     * (e.g. <interface>) with type='hostdev')
     */
    if ((managed = virXMLPropString(node, "managed"))!= NULL) {
        if (STREQ(managed, "yes"))
            def->managed = true;
    }

    /* @type is passed in from the caller rather than read from the
     * xml document, because it is specified in different places for
     * different kinds of defs - it is an attribute of
     * <source>/<address> for an intelligent hostdev (<interface>),
     * but an attribute of the toplevel element for a standard
     * <hostdev>.  (the functions we're going to call expect address
     * type to already be known).
     */
    if (type) {
        if ((def->source.subsys.type
             = virDomainHostdevSubsysTypeFromString(type)) < 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("unknown host device source address type '%s'"),
                           type);
            goto error;
        }
    } else {
        virReportError(VIR_ERR_XML_ERROR,
                       "%s", _("missing source address type"));
        goto error;
    }

    if (!(sourcenode = virXPathNode("./source", ctxt))) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("Missing <source> element in hostdev device"));
        goto error;
    }

    if (def->source.subsys.type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB &&
        virXPathBoolean("boolean(./source/@startupPolicy)", ctxt)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Setting startupPolicy is only allowed for USB"
                         " devices"));
        goto error;
    }

    switch (def->source.subsys.type) {
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI:
        if (virDomainHostdevSubsysPciDefParseXML(sourcenode, def, flags) < 0)
            goto error;
        break;
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB:
        if (virDomainHostdevSubsysUsbDefParseXML(sourcenode, def) < 0)
            goto error;
        break;
    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("address type='%s' not supported in hostdev interfaces"),
                       virDomainHostdevSubsysTypeToString(def->source.subsys.type));
        goto error;
    }
    ret = 0;
error:
    VIR_FREE(managed);
    return ret;
}

static int
virDomainHostdevDefParseXMLCaps(xmlNodePtr node ATTRIBUTE_UNUSED,
                                xmlXPathContextPtr ctxt,
                                const char *type,
                                virDomainHostdevDefPtr def)
{
    xmlNodePtr sourcenode;
    int ret = -1;

    /* @type is passed in from the caller rather than read from the
     * xml document, because it is specified in different places for
     * different kinds of defs - it is an attribute of
     * <source>/<address> for an intelligent hostdev (<interface>),
     * but an attribute of the toplevel element for a standard
     * <hostdev>.  (the functions we're going to call expect address
     * type to already be known).
     */
    if (type) {
        if ((def->source.caps.type
             = virDomainHostdevCapsTypeFromString(type)) < 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("unknown host device source address type '%s'"),
                           type);
            goto error;
        }
    } else {
        virReportError(VIR_ERR_XML_ERROR,
                       "%s", _("missing source address type"));
        goto error;
    }

    if (!(sourcenode = virXPathNode("./source", ctxt))) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("Missing <source> element in hostdev device"));
        goto error;
    }

    switch (def->source.caps.type) {
    case VIR_DOMAIN_HOSTDEV_CAPS_TYPE_STORAGE:
        if (!(def->source.caps.u.storage.block =
              virXPathString("string(./source/block[1])", ctxt))) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("Missing <block> element in hostdev storage device"));
            goto error;
        }
        break;
    case VIR_DOMAIN_HOSTDEV_CAPS_TYPE_MISC:
        if (!(def->source.caps.u.misc.chardev =
              virXPathString("string(./source/char[1])", ctxt))) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("Missing <char> element in hostdev character device"));
            goto error;
        }
        break;
    case VIR_DOMAIN_HOSTDEV_CAPS_TYPE_NET:
        if (!(def->source.caps.u.net.iface =
              virXPathString("string(./source/interface[1])", ctxt))) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("Missing <interface> element in hostdev net device"));
            goto error;
        }
        break;
    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("address type='%s' not supported in hostdev interfaces"),
                       virDomainHostdevCapsTypeToString(def->source.caps.type));
        goto error;
    }
    ret = 0;
error:
    return ret;
}

int
virDomainDeviceFindControllerModel(virDomainDefPtr def,
                                   virDomainDeviceInfoPtr info,
                                   int controllerType)
{
    int model = -1;
    int i;

    for (i = 0; i < def->ncontrollers; i++) {
        if (def->controllers[i]->type == controllerType &&
            def->controllers[i]->idx == info->addr.drive.controller)
            model = def->controllers[i]->model;
    }

    return model;
}

virDomainDiskDefPtr
virDomainDiskFindByBusAndDst(virDomainDefPtr def,
                             int bus,
                             char *dst)
{
    int i;

    if (!dst)
        return NULL;

    for (i = 0 ; i < def->ndisks ; i++) {
        if (def->disks[i]->bus == bus &&
            STREQ(def->disks[i]->dst, dst)) {
            return def->disks[i];
        }
    }

    return NULL;
}

int
virDomainDiskDefAssignAddress(virDomainXMLOptionPtr xmlopt,
                              virDomainDiskDefPtr def)
{
    int idx = virDiskNameToIndex(def->dst);
    if (idx < 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("Unknown disk name '%s' and no address specified"),
                       def->dst);
        return -1;
    }

    switch (def->bus) {
    case VIR_DOMAIN_DISK_BUS_SCSI:
        def->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE;

        if (xmlopt->config.hasWideScsiBus) {
            /* For a wide SCSI bus we define the default mapping to be
             * 16 units per bus, 1 bus per controller, many controllers.
             * Unit 7 is the SCSI controller itself. Therefore unit 7
             * cannot be assigned to disks and is skipped.
             */
            def->info.addr.drive.controller = idx / 15;
            def->info.addr.drive.bus = 0;
            def->info.addr.drive.unit = idx % 15;

            /* Skip the SCSI controller at unit 7 */
            if (def->info.addr.drive.unit >= 7) {
                ++def->info.addr.drive.unit;
            }
        } else {
            /* For a narrow SCSI bus we define the default mapping to be
             * 7 units per bus, 1 bus per controller, many controllers */
            def->info.addr.drive.controller = idx / 7;
            def->info.addr.drive.bus = 0;
            def->info.addr.drive.unit = idx % 7;
        }

        break;

    case VIR_DOMAIN_DISK_BUS_IDE:
        /* For IDE we define the default mapping to be 2 units
         * per bus, 2 bus per controller, many controllers */
        def->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE;
        def->info.addr.drive.controller = idx / 4;
        def->info.addr.drive.bus = (idx % 4) / 2;
        def->info.addr.drive.unit = (idx % 2);
        break;

    case VIR_DOMAIN_DISK_BUS_SATA:
        /* For SATA we define the default mapping to be 6 units
         * per bus, 1 bus per controller, many controllers */
        def->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE;
        def->info.addr.drive.controller = idx / 6;
        def->info.addr.drive.bus = 0;
        def->info.addr.drive.unit = idx % 6;
        break;

    case VIR_DOMAIN_DISK_BUS_FDC:
        /* For FDC we define the default mapping to be 2 units
         * per bus, 1 bus per controller, many controllers */
        def->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE;
        def->info.addr.drive.controller = idx / 2;
        def->info.addr.drive.bus = 0;
        def->info.addr.drive.unit = idx % 2;
        break;

    default:
        /* Other disk bus's aren't controller based */
        break;
    }

    return 0;
}

static virSecurityLabelDefPtr
virSecurityLabelDefParseXML(xmlXPathContextPtr ctxt,
                            unsigned int flags)
{
    char *p;
    virSecurityLabelDefPtr def = NULL;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        goto error;
    }

    p = virXPathStringLimit("string(./@type)",
                            VIR_SECURITY_LABEL_BUFLEN-1, ctxt);
    if (p == NULL) {
        def->type = VIR_DOMAIN_SECLABEL_DYNAMIC;
    } else {
        def->type = virDomainSeclabelTypeFromString(p);
        VIR_FREE(p);
        if (def->type <= 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           "%s", _("invalid security type"));
            goto error;
        }
    }

    p = virXPathStringLimit("string(./@relabel)",
                            VIR_SECURITY_LABEL_BUFLEN-1, ctxt);
    if (p != NULL) {
        if (STREQ(p, "yes")) {
            def->norelabel = false;
        } else if (STREQ(p, "no")) {
            def->norelabel = true;
        } else {
            virReportError(VIR_ERR_XML_ERROR,
                           _("invalid security relabel value %s"), p);
            VIR_FREE(p);
            goto error;
        }
        VIR_FREE(p);
        if (def->type == VIR_DOMAIN_SECLABEL_DYNAMIC &&
            def->norelabel) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("dynamic label type must use resource relabeling"));
            goto error;
        }
        if (def->type == VIR_DOMAIN_SECLABEL_NONE &&
            !def->norelabel) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("resource relabeling is not compatible with 'none' label type"));
            goto error;
        }
    } else {
        if (def->type == VIR_DOMAIN_SECLABEL_STATIC ||
            def->type == VIR_DOMAIN_SECLABEL_NONE)
            def->norelabel = true;
        else
            def->norelabel = false;
    }

    /* Only parse label, if using static labels, or
     * if the 'live' VM XML is requested
     */
    if (def->type == VIR_DOMAIN_SECLABEL_STATIC ||
        (!(flags & VIR_DOMAIN_XML_INACTIVE) &&
         def->type != VIR_DOMAIN_SECLABEL_NONE)) {
        p = virXPathStringLimit("string(./label[1])",
                                VIR_SECURITY_LABEL_BUFLEN-1, ctxt);
        if (p == NULL) {
            virReportError(VIR_ERR_XML_ERROR,
                           "%s", _("security label is missing"));
            goto error;
        }

        def->label = p;
    }

    /* Only parse imagelabel, if requested live XML with relabeling */
    if (!def->norelabel &&
        (!(flags & VIR_DOMAIN_XML_INACTIVE) &&
         def->type != VIR_DOMAIN_SECLABEL_NONE)) {
        p = virXPathStringLimit("string(./imagelabel[1])",
                                VIR_SECURITY_LABEL_BUFLEN-1, ctxt);
        if (p == NULL) {
            virReportError(VIR_ERR_XML_ERROR,
                           "%s", _("security imagelabel is missing"));
            goto error;
        }
        def->imagelabel = p;
    }

    /* Only parse baselabel for dynamic label type */
    if (def->type == VIR_DOMAIN_SECLABEL_DYNAMIC) {
        p = virXPathStringLimit("string(./baselabel[1])",
                                VIR_SECURITY_LABEL_BUFLEN-1, ctxt);
        def->baselabel = p;
    }

    /* Always parse model */
    p = virXPathStringLimit("string(./@model)",
                            VIR_SECURITY_MODEL_BUFLEN-1, ctxt);
    def->model = p;

    return def;

error:
    virSecurityLabelDefFree(def);
    return NULL;
}

static int
virSecurityLabelDefsParseXML(virDomainDefPtr def,
                            xmlXPathContextPtr ctxt,
                            virCapsPtr caps,
                            unsigned int flags)
{
    int i = 0, n;
    xmlNodePtr *list = NULL, saved_node;
    virCapsHostPtr host = &caps->host;

    /* Check args and save context */
    if (def == NULL || ctxt == NULL)
        return 0;
    saved_node = ctxt->node;

    /* Allocate a security labels based on XML */
    if ((n = virXPathNodeSet("./seclabel", ctxt, &list)) < 0)
        goto error;
    if (n == 0)
        return 0;

    if (VIR_ALLOC_N(def->seclabels, n) < 0) {
        virReportOOMError();
        goto error;
    }

    /* Parse each "seclabel" tag */
    for (i = 0; i < n; i++) {
        ctxt->node = list[i];
        def->seclabels[i] = virSecurityLabelDefParseXML(ctxt, flags);
        if (def->seclabels[i] == NULL)
            goto error;
    }
    def->nseclabels = n;
    ctxt->node = saved_node;
    VIR_FREE(list);

    /* libvirt versions prior to 0.10.0 support just a single seclabel element
     * in guest's XML and model attribute can be suppressed if type is none or
     * type is dynamic, baselabel is not defined and INACTIVE flag is set.
     *
     * To avoid compatibility issues, for this specific case the first model
     * defined in host's capabilities is used as model for the seclabel.
     */
    if (def->nseclabels == 1 &&
        !def->seclabels[0]->model &&
        host->nsecModels > 0) {
        if (def->seclabels[0]->type == VIR_DOMAIN_SECLABEL_NONE ||
            (def->seclabels[0]->type == VIR_DOMAIN_SECLABEL_DYNAMIC &&
             !def->seclabels[0]->baselabel &&
             (flags & VIR_DOMAIN_XML_INACTIVE))) {
            /* Copy model from host. */
            VIR_DEBUG("Found seclabel without a model, using '%s'",
                      host->secModels[0].model);
            def->seclabels[0]->model = strdup(host->secModels[0].model);
            if (!def->seclabels[0]->model) {
                virReportOOMError();
                goto error;
            }
        } else {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("missing security model in domain seclabel"));
            goto error;
        }
    }

    /* Checking missing model information */
    if (def->nseclabels > 1) {
        for (; n; n--) {
            if (def->seclabels[n - 1]->model == NULL) {
                virReportError(VIR_ERR_XML_ERROR, "%s",
                               _("missing security model "
                                 "when using multiple labels"));
                goto error;
            }
        }
    }

    return 0;

error:
    ctxt->node = saved_node;
    for (; i > 0; i--) {
        virSecurityLabelDefFree(def->seclabels[i - 1]);
    }
    VIR_FREE(def->seclabels);
    def->nseclabels = 0;
    VIR_FREE(list);
    return -1;
}

/* Parse the <seclabel> from a disk or character device. */
static int
virSecurityDeviceLabelDefParseXML(virSecurityDeviceLabelDefPtr **seclabels_rtn,
                                  size_t *nseclabels_rtn,
                                  virSecurityLabelDefPtr *vmSeclabels,
                                  int nvmSeclabels, xmlXPathContextPtr ctxt)
{
    virSecurityDeviceLabelDefPtr *seclabels;
    size_t nseclabels = 0;
    int n, i, j;
    xmlNodePtr *list = NULL;
    virSecurityLabelDefPtr vmDef = NULL;
    char *model, *relabel, *label;

    if ((n = virXPathNodeSet("./seclabel", ctxt, &list)) < 0)
        goto error;
    if (n == 0)
        return 0;

    if (VIR_ALLOC_N(seclabels, n) < 0) {
        virReportOOMError();
        goto error;
    }
    nseclabels = n;
    for (i = 0; i < n; i++) {
        if (VIR_ALLOC(seclabels[i]) < 0) {
            virReportOOMError();
            goto error;
        }
    }

    for (i = 0; i < n; i++) {
        /* get model associated to this override */
        model = virXMLPropString(list[i], "model");
        if (model) {
            /* find the security label that it's being overridden */
            for (j = 0; j < nvmSeclabels; j++) {
                if (STREQ(vmSeclabels[j]->model, model)) {
                    vmDef = vmSeclabels[j];
                    break;
                }
            }
            seclabels[i]->model = model;
        }

        /* Can't use overrides if top-level doesn't allow relabeling.  */
        if (vmDef && vmDef->norelabel) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("label overrides require relabeling to be "
                             "enabled at the domain level"));
            goto error;
        }

        relabel = virXMLPropString(list[i], "relabel");
        if (relabel != NULL) {
            if (STREQ(relabel, "yes")) {
                seclabels[i]->norelabel = false;
            } else if (STREQ(relabel, "no")) {
                seclabels[i]->norelabel = true;
            } else {
                virReportError(VIR_ERR_XML_ERROR,
                               _("invalid security relabel value %s"),
                               relabel);
                VIR_FREE(relabel);
                goto error;
            }
            VIR_FREE(relabel);
        } else {
            seclabels[i]->norelabel = false;
        }

        ctxt->node = list[i];
        label = virXPathStringLimit("string(./label)",
                                    VIR_SECURITY_LABEL_BUFLEN-1, ctxt);
        seclabels[i]->label = label;

        if (label && seclabels[i]->norelabel) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("Cannot specify a label if relabelling is "
                             "turned off. model=%s"),
                             NULLSTR(seclabels[i]->model));
            goto error;
        }
    }
    VIR_FREE(list);

    *nseclabels_rtn = nseclabels;
    *seclabels_rtn = seclabels;

    return 0;

error:
    for (i = 0; i < nseclabels; i++) {
        virSecurityDeviceLabelDefFree(seclabels[i]);
    }
    VIR_FREE(seclabels);
    VIR_FREE(list);
    return -1;
}


/* Parse the XML definition for a lease
 */
static virDomainLeaseDefPtr
virDomainLeaseDefParseXML(xmlNodePtr node)
{
    virDomainLeaseDefPtr def;
    xmlNodePtr cur;
    char *lockspace = NULL;
    char *key = NULL;
    char *path = NULL;
    char *offset = NULL;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (!key && xmlStrEqual(cur->name, BAD_CAST "key")) {
                key = (char *)xmlNodeGetContent(cur);
            } else if (!lockspace &&
                       xmlStrEqual(cur->name, BAD_CAST "lockspace")) {
                lockspace = (char *)xmlNodeGetContent(cur);
            } else if (!path &&
                       xmlStrEqual(cur->name, BAD_CAST "target")) {
                path = virXMLPropString(cur, "path");
                offset = virXMLPropString(cur, "offset");
            }
        }
        cur = cur->next;
    }

    if (!key) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("Missing 'key' element for lease"));
        goto error;
    }
    if (!path) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("Missing 'target' element for lease"));
        goto error;
    }

    if (offset &&
        virStrToLong_ull(offset, NULL, 10, &def->offset) < 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("Malformed lease target offset %s"), offset);
        goto error;
    }

    def->key = key;
    def->lockspace = lockspace;
    def->path = path;
    path = key = lockspace = NULL;

cleanup:
    VIR_FREE(lockspace);
    VIR_FREE(key);
    VIR_FREE(path);
    VIR_FREE(offset);

    return def;

 error:
    virDomainLeaseDefFree(def);
    def = NULL;
    goto cleanup;
}

static int
virDomainDiskSourcePoolDefParse(xmlNodePtr node,
                                virDomainDiskDefPtr def)
{
    char *pool = NULL;
    char *volume = NULL;
    int ret = -1;

    pool = virXMLPropString(node, "pool");
    volume = virXMLPropString(node, "volume");

    /* CD-ROM and Floppy allows no source */
    if (!pool && !volume)
        return 0;

    if (!pool || !volume) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("'pool' and 'volume' must be specified together "
                         "for 'pool' type source"));
        goto cleanup;
    }

    if (VIR_ALLOC(def->srcpool) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    def->srcpool->pool = pool;
    pool = NULL;
    def->srcpool->volume = volume;
    volume = NULL;

    ret = 0;

cleanup:
    VIR_FREE(pool);
    VIR_FREE(volume);
    return ret;
}

#define VENDOR_LEN  8
#define PRODUCT_LEN 16

/* Parse the XML definition for a disk
 * @param node XML nodeset to parse for disk definition
 */
static virDomainDiskDefPtr
virDomainDiskDefParseXML(virDomainXMLOptionPtr xmlopt,
                         xmlNodePtr node,
                         xmlXPathContextPtr ctxt,
                         virBitmapPtr bootMap,
                         virSecurityLabelDefPtr* vmSeclabels,
                         int nvmSeclabels,
                         unsigned int flags)
{
    virDomainDiskDefPtr def;
    xmlNodePtr sourceNode = NULL;
    xmlNodePtr cur, child;
    xmlNodePtr save_ctxt = ctxt->node;
    char *type = NULL;
    char *device = NULL;
    char *snapshot = NULL;
    char *rawio = NULL;
    char *sgio = NULL;
    char *driverName = NULL;
    char *driverType = NULL;
    char *source = NULL;
    char *target = NULL;
    char *protocol = NULL;
    char *protocol_transport = NULL;
    char *trans = NULL;
    virDomainDiskHostDefPtr hosts = NULL;
    int nhosts = 0;
    char *bus = NULL;
    char *cachetag = NULL;
    char *error_policy = NULL;
    char *rerror_policy = NULL;
    char *iotag = NULL;
    char *ioeventfd = NULL;
    char *event_idx = NULL;
    char *copy_on_read = NULL;
    char *mirror = NULL;
    char *mirrorFormat = NULL;
    bool mirroring = false;
    char *devaddr = NULL;
    virStorageEncryptionPtr encryption = NULL;
    char *serial = NULL;
    char *startupPolicy = NULL;
    char *authUsername = NULL;
    char *authUsage = NULL;
    char *authUUID = NULL;
    char *usageType = NULL;
    char *tray = NULL;
    char *logical_block_size = NULL;
    char *physical_block_size = NULL;
    char *wwn = NULL;
    char *vendor = NULL;
    char *product = NULL;
    int expected_secret_usage = -1;
    int auth_secret_usage = -1;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    def->geometry.cylinders = 0;
    def->geometry.heads = 0;
    def->geometry.sectors = 0;
    def->geometry.trans = VIR_DOMAIN_DISK_TRANS_DEFAULT;

    def->blockio.logical_block_size = 0;
    def->blockio.physical_block_size = 0;

    ctxt->node = node;

    type = virXMLPropString(node, "type");
    if (type) {
        if ((def->type = virDomainDiskTypeFromString(type)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown disk type '%s'"), type);
            goto error;
        }
    } else {
        def->type = VIR_DOMAIN_DISK_TYPE_FILE;
    }

    snapshot = virXMLPropString(node, "snapshot");

    rawio = virXMLPropString(node, "rawio");
    sgio = virXMLPropString(node, "sgio");

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (!source && !hosts && !def->srcpool &&
                xmlStrEqual(cur->name, BAD_CAST "source")) {
                sourceNode = cur;

                switch (def->type) {
                case VIR_DOMAIN_DISK_TYPE_FILE:
                    source = virXMLPropString(cur, "file");
                    startupPolicy = virXMLPropString(cur, "startupPolicy");
                    break;
                case VIR_DOMAIN_DISK_TYPE_BLOCK:
                    source = virXMLPropString(cur, "dev");
                    break;
                case VIR_DOMAIN_DISK_TYPE_DIR:
                    source = virXMLPropString(cur, "dir");
                    break;
                case VIR_DOMAIN_DISK_TYPE_NETWORK:
                    protocol = virXMLPropString(cur, "protocol");
                    if (protocol == NULL) {
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                       "%s", _("missing protocol type"));
                        goto error;
                    }
                    def->protocol = virDomainDiskProtocolTypeFromString(protocol);
                    if (def->protocol < 0) {
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                       _("unknown protocol type '%s'"),
                                       protocol);
                        goto error;
                    }
                    if (def->protocol == VIR_DOMAIN_DISK_PROTOCOL_ISCSI) {
                        expected_secret_usage = VIR_SECRET_USAGE_TYPE_ISCSI;
                    } else if (def->protocol == VIR_DOMAIN_DISK_PROTOCOL_RBD) {
                        expected_secret_usage = VIR_SECRET_USAGE_TYPE_CEPH;
                    }
                    if (!(source = virXMLPropString(cur, "name")) &&
                        def->protocol != VIR_DOMAIN_DISK_PROTOCOL_NBD) {
                        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                       _("missing name for disk source"));
                        goto error;
                    }
                    child = cur->children;
                    while (child != NULL) {
                        if (child->type == XML_ELEMENT_NODE &&
                            xmlStrEqual(child->name, BAD_CAST "host")) {
                            if (VIR_REALLOC_N(hosts, nhosts + 1) < 0) {
                                virReportOOMError();
                                goto error;
                            }
                            hosts[nhosts].name = NULL;
                            hosts[nhosts].port = NULL;
                            hosts[nhosts].transport = VIR_DOMAIN_DISK_PROTO_TRANS_TCP;
                            hosts[nhosts].socket = NULL;
                            nhosts++;

                            /* transport can be tcp (default), unix or rdma.  */
                            protocol_transport = virXMLPropString(child, "transport");
                            if (protocol_transport != NULL) {
                                hosts[nhosts - 1].transport = virDomainDiskProtocolTransportTypeFromString(protocol_transport);
                                if (hosts[nhosts - 1].transport < 0) {
                                    virReportError(VIR_ERR_XML_ERROR,
                                                   _("unknown protocol transport type '%s'"),
                                                   protocol_transport);
                                    goto error;
                                }
                            }
                            hosts[nhosts - 1].socket = virXMLPropString(child, "socket");
                            if (hosts[nhosts - 1].transport == VIR_DOMAIN_DISK_PROTO_TRANS_UNIX &&
                                hosts[nhosts - 1].socket == NULL) {
                                virReportError(VIR_ERR_XML_ERROR,
                                               "%s", _("missing socket for unix transport"));
                                goto error;
                            }
                            if (hosts[nhosts - 1].transport != VIR_DOMAIN_DISK_PROTO_TRANS_UNIX &&
                                hosts[nhosts - 1].socket != NULL) {
                                virReportError(VIR_ERR_XML_ERROR,
                                               _("transport %s does not support socket attribute"),
                                               protocol_transport);
                                goto error;
                            }
                            VIR_FREE(protocol_transport);
                            if (hosts[nhosts - 1].transport != VIR_DOMAIN_DISK_PROTO_TRANS_UNIX) {
                                hosts[nhosts - 1].name = virXMLPropString(child, "name");
                                if (!hosts[nhosts - 1].name) {
                                    virReportError(VIR_ERR_XML_ERROR,
                                                   "%s", _("missing name for host"));
                                    goto error;
                                }
                                hosts[nhosts - 1].port = virXMLPropString(child, "port");
                            }
                        }
                        child = child->next;
                    }
                    break;
                case VIR_DOMAIN_DISK_TYPE_VOLUME:
                    if (virDomainDiskSourcePoolDefParse(cur, def) < 0)
                        goto error;
                    startupPolicy = virXMLPropString(cur, "startupPolicy");
                    break;
                default:
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("unexpected disk type %s"),
                                   virDomainDiskTypeToString(def->type));
                    goto error;
                }

                /* People sometimes pass a bogus '' source path
                   when they mean to omit the source element
                   completely (e.g. CDROM without media). This is
                   just a little compatibility check to help
                   those broken apps */
                if (source && STREQ(source, ""))
                    VIR_FREE(source);
            } else if (!target &&
                       xmlStrEqual(cur->name, BAD_CAST "target")) {
                target = virXMLPropString(cur, "dev");
                bus = virXMLPropString(cur, "bus");
                tray = virXMLPropString(cur, "tray");

                /* HACK: Work around for compat with Xen
                 * driver in previous libvirt releases */
                if (target &&
                    STRPREFIX(target, "ioemu:"))
                    memmove(target, target+6, strlen(target)-5);
            } else if (xmlStrEqual(cur->name, BAD_CAST "geometry")) {
                if (virXPathUInt("string(./geometry/@cyls)",
                                 ctxt, &def->geometry.cylinders) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                   _("invalid geometry settings (cyls)"));
                    goto error;
                }
                if (virXPathUInt("string(./geometry/@heads)",
                                 ctxt, &def->geometry.heads) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                   _("invalid geometry settings (heads)"));
                    goto error;
                }
                if (virXPathUInt("string(./geometry/@secs)",
                                 ctxt, &def->geometry.sectors) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                   _("invalid geometry settings (secs)"));
                    goto error;
                }
                trans = virXMLPropString(cur, "trans");
                if (trans) {
                    def->geometry.trans = virDomainDiskGeometryTransTypeFromString(trans);
                    if (def->geometry.trans <= 0) {
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                       _("invalid translation value '%s'"),
                                       trans);
                        goto error;
                    }
                }
            } else if (xmlStrEqual(cur->name, BAD_CAST "blockio")) {
                logical_block_size =
                    virXMLPropString(cur, "logical_block_size");
                if (logical_block_size &&
                    virStrToLong_ui(logical_block_size, NULL, 0,
                                    &def->blockio.logical_block_size) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("invalid logical block size '%s'"),
                                   logical_block_size);
                    goto error;
                }
                physical_block_size =
                    virXMLPropString(cur, "physical_block_size");
                if (physical_block_size &&
                    virStrToLong_ui(physical_block_size, NULL, 0,
                                    &def->blockio.physical_block_size) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("invalid physical block size '%s'"),
                                   physical_block_size);
                    goto error;
                }
            } else if (!driverName &&
                       xmlStrEqual(cur->name, BAD_CAST "driver")) {
                driverName = virXMLPropString(cur, "name");
                driverType = virXMLPropString(cur, "type");
                if (STREQ_NULLABLE(driverType, "aio")) {
                    /* In-place conversion to "raw", for Xen back-compat */
                    driverType[0] = 'r';
                    driverType[1] = 'a';
                    driverType[2] = 'w';
                }
                cachetag = virXMLPropString(cur, "cache");
                error_policy = virXMLPropString(cur, "error_policy");
                rerror_policy = virXMLPropString(cur, "rerror_policy");
                iotag = virXMLPropString(cur, "io");
                ioeventfd = virXMLPropString(cur, "ioeventfd");
                event_idx = virXMLPropString(cur, "event_idx");
                copy_on_read = virXMLPropString(cur, "copy_on_read");
            } else if (!mirror && xmlStrEqual(cur->name, BAD_CAST "mirror") &&
                       !(flags & VIR_DOMAIN_XML_INACTIVE)) {
                char *ready;
                mirror = virXMLPropString(cur, "file");
                if (!mirror) {
                    virReportError(VIR_ERR_XML_ERROR, "%s",
                                   _("mirror requires file name"));
                    goto error;
                }
                mirrorFormat = virXMLPropString(cur, "format");
                ready = virXMLPropString(cur, "ready");
                if (ready) {
                    mirroring = true;
                    VIR_FREE(ready);
                }
            } else if (xmlStrEqual(cur->name, BAD_CAST "auth")) {
                authUsername = virXMLPropString(cur, "username");
                if (authUsername == NULL) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                   _("missing username for auth"));
                    goto error;
                }

                def->auth.secretType = VIR_DOMAIN_DISK_SECRET_TYPE_NONE;
                child = cur->children;
                while (child != NULL) {
                    if (child->type == XML_ELEMENT_NODE &&
                        xmlStrEqual(child->name, BAD_CAST "secret")) {
                        usageType = virXMLPropString(child, "type");
                        if (usageType == NULL) {
                            virReportError(VIR_ERR_XML_ERROR, "%s",
                                           _("missing type for secret"));
                            goto error;
                        }
                        auth_secret_usage =
                            virSecretUsageTypeTypeFromString(usageType);
                        if (auth_secret_usage < 0) {
                            virReportError(VIR_ERR_XML_ERROR,
                                           _("invalid secret type %s"),
                                           usageType);
                            goto error;
                        }

                        authUUID = virXMLPropString(child, "uuid");
                        authUsage = virXMLPropString(child, "usage");

                        if (authUUID != NULL && authUsage != NULL) {
                            virReportError(VIR_ERR_XML_ERROR, "%s",
                                           _("only one of uuid and usage can be specified"));
                            goto error;
                        }
                        if (authUUID != NULL) {
                            def->auth.secretType = VIR_DOMAIN_DISK_SECRET_TYPE_UUID;
                            if (virUUIDParse(authUUID,
                                             def->auth.secret.uuid) < 0) {
                                virReportError(VIR_ERR_XML_ERROR,
                                               _("malformed uuid %s"),
                                               authUUID);
                                goto error;
                            }
                        } else if (authUsage != NULL) {
                            def->auth.secretType = VIR_DOMAIN_DISK_SECRET_TYPE_USAGE;
                            def->auth.secret.usage = authUsage;
                            authUsage = NULL;
                        }
                    }
                    child = child->next;
                }
            } else if (xmlStrEqual(cur->name, BAD_CAST "iotune")) {
                if (virXPathULongLong("string(./iotune/total_bytes_sec)",
                                      ctxt,
                                      &def->blkdeviotune.total_bytes_sec) < 0) {
                    def->blkdeviotune.total_bytes_sec = 0;
                }

                if (virXPathULongLong("string(./iotune/read_bytes_sec)",
                                      ctxt,
                                      &def->blkdeviotune.read_bytes_sec) < 0) {
                    def->blkdeviotune.read_bytes_sec = 0;
                }

                if (virXPathULongLong("string(./iotune/write_bytes_sec)",
                                      ctxt,
                                      &def->blkdeviotune.write_bytes_sec) < 0) {
                    def->blkdeviotune.write_bytes_sec = 0;
                }

                if (virXPathULongLong("string(./iotune/total_iops_sec)",
                                      ctxt,
                                      &def->blkdeviotune.total_iops_sec) < 0) {
                    def->blkdeviotune.total_iops_sec = 0;
                }

                if (virXPathULongLong("string(./iotune/read_iops_sec)",
                                      ctxt,
                                      &def->blkdeviotune.read_iops_sec) < 0) {
                    def->blkdeviotune.read_iops_sec = 0;
                }

                if (virXPathULongLong("string(./iotune/write_iops_sec)",
                                      ctxt,
                                      &def->blkdeviotune.write_iops_sec) < 0) {
                    def->blkdeviotune.write_iops_sec = 0;
                }

                if ((def->blkdeviotune.total_bytes_sec &&
                     def->blkdeviotune.read_bytes_sec) ||
                    (def->blkdeviotune.total_bytes_sec &&
                     def->blkdeviotune.write_bytes_sec)) {
                    virReportError(VIR_ERR_XML_ERROR, "%s",
                                   _("total and read/write bytes_sec "
                                     "cannot be set at the same time"));
                    goto error;
                }

                if ((def->blkdeviotune.total_iops_sec &&
                     def->blkdeviotune.read_iops_sec) ||
                    (def->blkdeviotune.total_iops_sec &&
                     def->blkdeviotune.write_iops_sec)) {
                    virReportError(VIR_ERR_XML_ERROR, "%s",
                                   _("total and read/write iops_sec "
                                     "cannot be set at the same time"));
                    goto error;
                }
            } else if (xmlStrEqual(cur->name, BAD_CAST "readonly")) {
                def->readonly = true;
            } else if (xmlStrEqual(cur->name, BAD_CAST "shareable")) {
                def->shared = true;
            } else if (xmlStrEqual(cur->name, BAD_CAST "transient")) {
                def->transient = true;
            } else if ((flags & VIR_DOMAIN_XML_INTERNAL_STATUS) &&
                       xmlStrEqual(cur->name, BAD_CAST "state")) {
                /* Legacy back-compat. Don't add any more attributes here */
                devaddr = virXMLPropString(cur, "devaddr");
            } else if (encryption == NULL &&
                       xmlStrEqual(cur->name, BAD_CAST "encryption")) {
                encryption = virStorageEncryptionParseNode(node->doc,
                                                           cur);
                if (encryption == NULL)
                    goto error;
            } else if (!serial &&
                       xmlStrEqual(cur->name, BAD_CAST "serial")) {
                serial = (char *)xmlNodeGetContent(cur);
            } else if (!wwn &&
                       xmlStrEqual(cur->name, BAD_CAST "wwn")) {
                wwn = (char *)xmlNodeGetContent(cur);

                if (!virValidateWWN(wwn))
                    goto error;
            } else if (!vendor &&
                       xmlStrEqual(cur->name, BAD_CAST "vendor")) {
                vendor = (char *)xmlNodeGetContent(cur);

                if (strlen(vendor) > VENDOR_LEN) {
                    virReportError(VIR_ERR_XML_ERROR, "%s",
                                   _("disk vendor is more than 8 characters"));
                    goto error;
                }

                if (!virStrIsPrint(vendor)) {
                    virReportError(VIR_ERR_XML_ERROR, "%s",
                                   _("disk vendor is not printable string"));
                    goto error;
                }
            } else if (!product &&
                       xmlStrEqual(cur->name, BAD_CAST "product")) {
                product = (char *)xmlNodeGetContent(cur);

                if (strlen(vendor) > PRODUCT_LEN) {
                    virReportError(VIR_ERR_XML_ERROR, "%s",
                                   _("disk product is more than 16 characters"));
                    goto error;
                }

                if (!virStrIsPrint(product)) {
                    virReportError(VIR_ERR_XML_ERROR, "%s",
                                   _("disk product is not printable string"));
                    goto error;
                }
            } else if (xmlStrEqual(cur->name, BAD_CAST "boot")) {
                /* boot is parsed as part of virDomainDeviceInfoParseXML */
            }
        }
        cur = cur->next;
    }

    if (auth_secret_usage != -1 && auth_secret_usage != expected_secret_usage) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("invalid secret type '%s'"),
                       virSecretUsageTypeTypeToString(auth_secret_usage));
        goto error;
    }

    device = virXMLPropString(node, "device");
    if (device) {
        if ((def->device = virDomainDiskDeviceTypeFromString(device)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown disk device '%s'"), device);
            goto error;
        }
    } else {
        def->device = VIR_DOMAIN_DISK_DEVICE_DISK;
    }

    /* Only CDROM and Floppy devices are allowed missing source path
     * to indicate no media present */
    if (source == NULL && hosts == NULL && !def->srcpool &&
        def->device != VIR_DOMAIN_DISK_DEVICE_CDROM &&
        def->device != VIR_DOMAIN_DISK_DEVICE_FLOPPY) {
        virReportError(VIR_ERR_NO_SOURCE,
                       target ? "%s" : NULL, target);
        goto error;
    }

    /* If source is present, check for an optional seclabel override.  */
    if (sourceNode) {
        xmlNodePtr saved_node = ctxt->node;
        ctxt->node = sourceNode;
        if (virSecurityDeviceLabelDefParseXML(&def->seclabels,
                                              &def->nseclabels,
                                              vmSeclabels,
                                              nvmSeclabels,
                                              ctxt) < 0)
            goto error;
        ctxt->node = saved_node;
    }

    if (target == NULL) {
        if (def->srcpool) {
            char *tmp;
            if (virAsprintf(&tmp, "pool = '%s', volume = '%s'",
                def->srcpool->pool, def->srcpool->volume) < 0) {
                virReportOOMError();
                goto error;
            }

            virReportError(VIR_ERR_NO_TARGET, "%s", tmp);
            VIR_FREE(tmp);
        } else {
            virReportError(VIR_ERR_NO_TARGET, source ? "%s" : NULL, source);
        }
        goto error;
    }

    if (def->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY &&
        !STRPREFIX(target, "fd")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Invalid floppy device name: %s"), target);
        goto error;
    }

    /* Force CDROM to be listed as read only */
    if (def->device == VIR_DOMAIN_DISK_DEVICE_CDROM)
        def->readonly = true;

    if ((def->device == VIR_DOMAIN_DISK_DEVICE_DISK ||
         def->device == VIR_DOMAIN_DISK_DEVICE_LUN) &&
        !STRPREFIX((const char *)target, "hd") &&
        !STRPREFIX((const char *)target, "sd") &&
        !STRPREFIX((const char *)target, "vd") &&
        !STRPREFIX((const char *)target, "xvd") &&
        !STRPREFIX((const char *)target, "ubd")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Invalid harddisk device name: %s"), target);
        goto error;
    }

    if (snapshot) {
        def->snapshot = virDomainSnapshotLocationTypeFromString(snapshot);
        if (def->snapshot <= 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown disk snapshot setting '%s'"),
                           snapshot);
            goto error;
        }
    } else if (def->readonly) {
        def->snapshot = VIR_DOMAIN_SNAPSHOT_LOCATION_NONE;
    }

    if ((rawio || sgio) &&
        (def->device != VIR_DOMAIN_DISK_DEVICE_LUN)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("rawio or sgio can be used only with "
                         "device='lun'"));
        goto error;
    }

    if (rawio) {
        def->rawio_specified = true;
        if (STREQ(rawio, "yes")) {
            def->rawio = 1;
        } else if (STREQ(rawio, "no")) {
            def->rawio = 0;
        } else {
            virReportError(VIR_ERR_XML_ERROR,
                           _("unknown disk rawio setting '%s'"),
                           rawio);
            goto error;
        }
    }

    if (sgio) {
        if ((def->sgio = virDomainDiskSGIOTypeFromString(sgio)) <= 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("unknown disk sgio mode '%s'"), sgio);
            goto error;
        }
    }

    if (bus) {
        if ((def->bus = virDomainDiskBusTypeFromString(bus)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown disk bus type '%s'"), bus);
            goto error;
        }
    } else {
        if (def->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY) {
            def->bus = VIR_DOMAIN_DISK_BUS_FDC;
        } else {
            if (STRPREFIX(target, "hd"))
                def->bus = VIR_DOMAIN_DISK_BUS_IDE;
            else if (STRPREFIX(target, "sd"))
                def->bus = VIR_DOMAIN_DISK_BUS_SCSI;
            else if (STRPREFIX(target, "vd"))
                def->bus = VIR_DOMAIN_DISK_BUS_VIRTIO;
            else if (STRPREFIX(target, "xvd"))
                def->bus = VIR_DOMAIN_DISK_BUS_XEN;
            else if (STRPREFIX(target, "ubd"))
                def->bus = VIR_DOMAIN_DISK_BUS_UML;
            else
                def->bus = VIR_DOMAIN_DISK_BUS_IDE;
        }
    }

    if (tray) {
        if ((def->tray_status = virDomainDiskTrayTypeFromString(tray)) < 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("unknown disk tray status '%s'"), tray);
            goto error;
        }

        if (def->device != VIR_DOMAIN_DISK_DEVICE_FLOPPY &&
            def->device != VIR_DOMAIN_DISK_DEVICE_CDROM) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("tray is only valid for cdrom and floppy"));
            goto error;
        }
    } else {
        if (def->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY ||
            def->device == VIR_DOMAIN_DISK_DEVICE_CDROM)
            def->tray_status = VIR_DOMAIN_DISK_TRAY_CLOSED;
    }

    if (def->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY &&
        def->bus != VIR_DOMAIN_DISK_BUS_FDC) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Invalid bus type '%s' for floppy disk"), bus);
        goto error;
    }
    if (def->device != VIR_DOMAIN_DISK_DEVICE_FLOPPY &&
        def->bus == VIR_DOMAIN_DISK_BUS_FDC) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Invalid bus type '%s' for disk"), bus);
        goto error;
    }

    if (cachetag &&
        (def->cachemode = virDomainDiskCacheTypeFromString(cachetag)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown disk cache mode '%s'"), cachetag);
        goto error;
    }

    if (error_policy &&
        (def->error_policy = virDomainDiskErrorPolicyTypeFromString(error_policy)) <= 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown disk error policy '%s'"), error_policy);
        goto error;
    }

    if (rerror_policy &&
        (((def->rerror_policy
           = virDomainDiskErrorPolicyTypeFromString(rerror_policy)) <= 0) ||
         (def->rerror_policy == VIR_DOMAIN_DISK_ERROR_POLICY_ENOSPACE))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown disk read error policy '%s'"),
                       rerror_policy);
        goto error;
    }

    if (iotag) {
        if ((def->iomode = virDomainDiskIoTypeFromString(iotag)) < 0 ||
            def->iomode == VIR_DOMAIN_DISK_IO_DEFAULT) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown disk io mode '%s'"), iotag);
            goto error;
        }
    }

    if (ioeventfd) {
        if (def->bus != VIR_DOMAIN_DISK_BUS_VIRTIO) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("disk ioeventfd mode supported "
                             "only for virtio bus"));
            goto error;
        }

        int i;
        if ((i = virDomainIoEventFdTypeFromString(ioeventfd)) <= 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unknown disk ioeventfd mode '%s'"),
                           ioeventfd);
            goto error;
        }
        def->ioeventfd=i;
    }

    if (event_idx) {
        if (def->bus != VIR_DOMAIN_DISK_BUS_VIRTIO) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("disk event_idx mode supported "
                             "only for virtio bus"));
            goto error;
        }

        int idx;
        if ((idx = virDomainVirtioEventIdxTypeFromString(event_idx)) <= 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unknown disk event_idx mode '%s'"),
                           event_idx);
            goto error;
        }
        def->event_idx = idx;
    }

    if (copy_on_read) {
        int cor;
        if ((cor = virDomainDiskCopyOnReadTypeFromString(copy_on_read)) <= 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unknown disk copy_on_read mode '%s'"),
                           copy_on_read);
            goto error;
        }
        def->copy_on_read = cor;
    }

    if (devaddr) {
        if (virDomainParseLegacyDeviceAddress(devaddr,
                                              &def->info.addr.pci) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unable to parse devaddr parameter '%s'"),
                           devaddr);
            goto error;
        }
        def->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
    } else {
        if (virDomainDeviceInfoParseXML(node, bootMap, &def->info,
                                        flags | VIR_DOMAIN_XML_INTERNAL_ALLOW_BOOT) < 0)
            goto error;
    }

    if (startupPolicy) {
        int i;

        if ((i = virDomainStartupPolicyTypeFromString(startupPolicy)) <= 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unknown startupPolicy value '%s'"),
                           startupPolicy);
            goto error;
        }

        if (def->device != VIR_DOMAIN_DISK_DEVICE_CDROM &&
            def->device != VIR_DOMAIN_DISK_DEVICE_FLOPPY) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("Setting disk %s is allowed only for "
                             "cdrom or floppy"),
                           startupPolicy);
            goto error;
        }
        def->startupPolicy = i;
    }

    def->src = source;
    source = NULL;
    def->dst = target;
    target = NULL;
    def->hosts = hosts;
    hosts = NULL;
    def->nhosts = nhosts;
    nhosts = 0;
    def->auth.username = authUsername;
    authUsername = NULL;
    def->driverName = driverName;
    driverName = NULL;
    def->mirror = mirror;
    mirror = NULL;
    def->mirroring = mirroring;
    def->encryption = encryption;
    encryption = NULL;
    def->serial = serial;
    serial = NULL;
    def->wwn = wwn;
    wwn = NULL;
    def->vendor = vendor;
    vendor = NULL;
    def->product = product;
    product = NULL;

    if (driverType) {
        def->format = virStorageFileFormatTypeFromString(driverType);
        if (def->format <= 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unknown driver format value '%s'"),
                           driverType);
            goto error;
        }
    }

    if (mirrorFormat) {
        def->mirrorFormat = virStorageFileFormatTypeFromString(mirrorFormat);
        if (def->mirrorFormat <= 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unknown mirror format value '%s'"),
                           driverType);
            goto error;
        }
    }

    if (def->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE
        && virDomainDiskDefAssignAddress(xmlopt, def) < 0)
        goto error;

cleanup:
    VIR_FREE(bus);
    VIR_FREE(type);
    VIR_FREE(snapshot);
    VIR_FREE(rawio);
    VIR_FREE(sgio);
    VIR_FREE(target);
    VIR_FREE(source);
    VIR_FREE(tray);
    VIR_FREE(trans);
    while (nhosts > 0) {
        virDomainDiskHostDefFree(&hosts[nhosts - 1]);
        nhosts--;
    }
    VIR_FREE(hosts);
    VIR_FREE(protocol);
    VIR_FREE(protocol_transport);
    VIR_FREE(device);
    VIR_FREE(authUsername);
    VIR_FREE(usageType);
    VIR_FREE(authUUID);
    VIR_FREE(authUsage);
    VIR_FREE(driverType);
    VIR_FREE(driverName);
    VIR_FREE(mirror);
    VIR_FREE(mirrorFormat);
    VIR_FREE(cachetag);
    VIR_FREE(error_policy);
    VIR_FREE(rerror_policy);
    VIR_FREE(iotag);
    VIR_FREE(ioeventfd);
    VIR_FREE(event_idx);
    VIR_FREE(copy_on_read);
    VIR_FREE(devaddr);
    VIR_FREE(serial);
    virStorageEncryptionFree(encryption);
    VIR_FREE(startupPolicy);
    VIR_FREE(logical_block_size);
    VIR_FREE(physical_block_size);
    VIR_FREE(wwn);
    VIR_FREE(vendor);
    VIR_FREE(product);

    ctxt->node = save_ctxt;
    return def;

error:
    virDomainDiskDefFree(def);
    def = NULL;
    goto cleanup;
}


static int
virDomainControllerModelTypeFromString(const virDomainControllerDefPtr def,
                                       const char *model)
{
    if (def->type == VIR_DOMAIN_CONTROLLER_TYPE_SCSI)
        return virDomainControllerModelSCSITypeFromString(model);
    else if (def->type == VIR_DOMAIN_CONTROLLER_TYPE_USB)
        return virDomainControllerModelUSBTypeFromString(model);

    return -1;
}

/* Parse the XML definition for a controller
 * @param node XML nodeset to parse for controller definition
 */
static virDomainControllerDefPtr
virDomainControllerDefParseXML(xmlNodePtr node,
                               unsigned int flags)
{
    virDomainControllerDefPtr def;
    char *type = NULL;
    char *idx = NULL;
    char *model = NULL;
    char *num_queues = NULL;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    type = virXMLPropString(node, "type");
    if (type) {
        if ((def->type = virDomainControllerTypeFromString(type)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown controller type '%s'"), type);
            goto error;
        }
    }

    idx = virXMLPropString(node, "index");
    if (idx) {
        if (virStrToLong_i(idx, NULL, 10, &def->idx) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Cannot parse controller index %s"), idx);
            goto error;
        }
    }

    model = virXMLPropString(node, "model");
    if (model) {
        if ((def->model = virDomainControllerModelTypeFromString(def, model)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown model type '%s'"), model);
            goto error;
        }
    } else {
        def->model = -1;
    }

    if ((num_queues = virXMLPropString(node, "num_queues"))) {
        if (virStrToLong_ui(num_queues, NULL, 10, &def->num_queues) < 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("Malformed 'num_queues' value '%s'"), num_queues);
            goto error;
        }
    }

    if (virDomainDeviceInfoParseXML(node, NULL, &def->info, flags) < 0)
        goto error;

    switch (def->type) {
    case VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL: {
        char *ports = virXMLPropString(node, "ports");
        if (ports) {
            int r = virStrToLong_i(ports, NULL, 10,
                                   &def->opts.vioserial.ports);
            if (r != 0 || def->opts.vioserial.ports < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Invalid ports: %s"), ports);
                VIR_FREE(ports);
                goto error;
            }
        } else {
            def->opts.vioserial.ports = -1;
        }
        VIR_FREE(ports);

        char *vectors = virXMLPropString(node, "vectors");
        if (vectors) {
            int r = virStrToLong_i(vectors, NULL, 10,
                                   &def->opts.vioserial.vectors);
            if (r != 0 || def->opts.vioserial.vectors < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Invalid vectors: %s"), vectors);
                VIR_FREE(vectors);
                goto error;
            }
        } else {
            def->opts.vioserial.vectors = -1;
        }
        VIR_FREE(vectors);
        break;
    }
    case VIR_DOMAIN_CONTROLLER_TYPE_USB: {
        /* If the XML has a uhci1, uhci2, uhci3 controller and no
         * master port was given, we should set a sensible one */
        int masterPort = -1;
        switch (def->model) {
        case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI1:
            masterPort = 0;
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI2:
            masterPort = 2;
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI3:
            masterPort = 4;
            break;
        }
        if (masterPort != -1 &&
            def->info.mastertype == VIR_DOMAIN_CONTROLLER_MASTER_NONE) {
            def->info.mastertype = VIR_DOMAIN_CONTROLLER_MASTER_USB;
            def->info.master.usb.startport = masterPort;
        }
        break;
    }

    default:
        break;
    }

    if (def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
        def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO &&
        def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW &&
        def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390 &&
        def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Controllers must use the 'pci' address type"));
        goto error;
    }

cleanup:
    VIR_FREE(type);
    VIR_FREE(idx);
    VIR_FREE(model);
    VIR_FREE(num_queues);

    return def;

 error:
    virDomainControllerDefFree(def);
    def = NULL;
    goto cleanup;
}


void
virDomainNetGenerateMAC(virDomainXMLOptionPtr xmlopt,
                        virMacAddrPtr mac)
{
    virMacAddrGenerate(xmlopt->config.macPrefix, mac);
}


/* Parse a value located at XPATH within CTXT, and store the
 * result into val.  If REQUIRED, then the value must exist;
 * otherwise, the value is optional.  The value is in bytes.
 * Return 0 on success, -1 on failure after issuing error. */
static int
virDomainParseScaledValue(const char *xpath,
                          xmlXPathContextPtr ctxt,
                          unsigned long long *val,
                          unsigned long long scale,
                          unsigned long long max,
                          bool required)
{
    char *xpath_full = NULL;
    char *unit = NULL;
    int ret = -1;
    unsigned long long bytes;

    *val = 0;
    if (virAsprintf(&xpath_full, "string(%s)", xpath) < 0) {
        virReportOOMError();
        goto cleanup;
    }
    ret = virXPathULongLong(xpath_full, ctxt, &bytes);
    if (ret < 0) {
        if (ret == -2)
            virReportError(VIR_ERR_XML_ERROR,
                           _("could not parse element %s"),
                           xpath);
        else if (required)
            virReportError(VIR_ERR_XML_ERROR,
                           _("missing element %s"),
                           xpath);
        else
            ret = 0;
        goto cleanup;
    }
    VIR_FREE(xpath_full);

    if (virAsprintf(&xpath_full, "string(%s/@unit)", xpath) < 0) {
        virReportOOMError();
        goto cleanup;
    }
    unit = virXPathString(xpath_full, ctxt);

    if (virScaleInteger(&bytes, unit, scale, max) < 0)
        goto cleanup;

    *val = bytes;
    ret = 0;
cleanup:
    VIR_FREE(xpath_full);
    VIR_FREE(unit);
    return ret;
}


/* Parse the XML definition for a disk
 * @param node XML nodeset to parse for disk definition
 */
static virDomainFSDefPtr
virDomainFSDefParseXML(xmlNodePtr node,
                       xmlXPathContextPtr ctxt,
                       unsigned int flags) {
    virDomainFSDefPtr def;
    xmlNodePtr cur, save_node = ctxt->node;
    char *type = NULL;
    char *fsdriver = NULL;
    char *source = NULL;
    char *target = NULL;
    char *accessmode = NULL;
    char *wrpolicy = NULL;
    char *usage = NULL;
    char *unit = NULL;

    ctxt->node = node;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    type = virXMLPropString(node, "type");
    if (type) {
        if ((def->type = virDomainFSTypeFromString(type)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown filesystem type '%s'"), type);
            goto error;
        }
    } else {
        def->type = VIR_DOMAIN_FS_TYPE_MOUNT;
    }

    accessmode = virXMLPropString(node, "accessmode");
    if (accessmode) {
        if ((def->accessmode = virDomainFSAccessModeTypeFromString(accessmode)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown accessmode '%s'"), accessmode);
            goto error;
        }
    } else {
        def->accessmode = VIR_DOMAIN_FS_ACCESSMODE_PASSTHROUGH;
    }

    if (virDomainParseScaledValue("./space_hard_limit[1]", ctxt,
                                  &def->space_hard_limit, 1,
                                  ULLONG_MAX, false) < 0)
        goto error;

    if (virDomainParseScaledValue("./space_soft_limit[1]", ctxt,
                                  &def->space_soft_limit, 1,
                                  ULLONG_MAX, false) < 0)
        goto error;

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (!source &&
                xmlStrEqual(cur->name, BAD_CAST "source")) {

                if (def->type == VIR_DOMAIN_FS_TYPE_MOUNT ||
                    def->type == VIR_DOMAIN_FS_TYPE_BIND)
                    source = virXMLPropString(cur, "dir");
                else if (def->type == VIR_DOMAIN_FS_TYPE_FILE)
                    source = virXMLPropString(cur, "file");
                else if (def->type == VIR_DOMAIN_FS_TYPE_BLOCK)
                    source = virXMLPropString(cur, "dev");
                else if (def->type == VIR_DOMAIN_FS_TYPE_TEMPLATE)
                    source = virXMLPropString(cur, "name");
                else if (def->type == VIR_DOMAIN_FS_TYPE_RAM) {
                    usage = virXMLPropString(cur, "usage");
                    unit = virXMLPropString(cur, "unit");
                }
            } else if (!target &&
                       xmlStrEqual(cur->name, BAD_CAST "target")) {
                target = virXMLPropString(cur, "dir");
            } else if (xmlStrEqual(cur->name, BAD_CAST "readonly")) {
                def->readonly = 1;
            } else if (!fsdriver && xmlStrEqual(cur->name, BAD_CAST "driver")) {
                fsdriver = virXMLPropString(cur, "type");
                wrpolicy = virXMLPropString(cur, "wrpolicy");
            }
        }
        cur = cur->next;
    }

    if (fsdriver) {
        if ((def->fsdriver = virDomainFSDriverTypeTypeFromString(fsdriver)) <= 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown fs driver type '%s'"), fsdriver);
            goto error;
        }
    }

    if (wrpolicy) {
        if ((def->wrpolicy = virDomainFSWrpolicyTypeFromString(wrpolicy)) <= 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unknown filesystem write policy '%s'"), wrpolicy);
            goto error;
        }
    } else {
        def->wrpolicy = VIR_DOMAIN_FS_WRPOLICY_DEFAULT;
    }

    if (source == NULL &&
        def->type != VIR_DOMAIN_FS_TYPE_RAM) {
        virReportError(VIR_ERR_NO_SOURCE,
                       target ? "%s" : NULL, target);
        goto error;
    }

    if (target == NULL) {
        virReportError(VIR_ERR_NO_TARGET,
                       source ? "%s" : NULL, source);
        goto error;
    }

    if (def->type == VIR_DOMAIN_FS_TYPE_RAM) {
        if (!usage) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("missing 'usage' attribute for RAM filesystem"));
            goto error;
        }
        if (virStrToLong_ull(usage, NULL, 10, &def->usage) < 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("cannot parse usage '%s' for RAM filesystem"),
                           usage);
            goto error;
        }
        if (unit &&
            virScaleInteger(&def->usage, unit,
                            1024, ULLONG_MAX) < 0)
            goto error;
    }

    def->src = source;
    source = NULL;
    def->dst = target;
    target = NULL;

    if (virDomainDeviceInfoParseXML(node, NULL, &def->info, flags) < 0)
        goto error;

cleanup:
    ctxt->node = save_node;
    VIR_FREE(type);
    VIR_FREE(fsdriver);
    VIR_FREE(target);
    VIR_FREE(source);
    VIR_FREE(accessmode);
    VIR_FREE(wrpolicy);
    VIR_FREE(usage);
    VIR_FREE(unit);

    return def;

 error:
    virDomainFSDefFree(def);
    def = NULL;
    goto cleanup;
}

static int
virDomainActualNetDefParseXML(xmlNodePtr node,
                              xmlXPathContextPtr ctxt,
                              virDomainNetDefPtr parent,
                              virDomainActualNetDefPtr *def,
                              unsigned int flags)
{
    virDomainActualNetDefPtr actual = NULL;
    int ret = -1;
    xmlNodePtr save_ctxt = ctxt->node;
    xmlNodePtr bandwidth_node = NULL;
    xmlNodePtr vlanNode;
    xmlNodePtr virtPortNode;
    char *type = NULL;
    char *mode = NULL;
    char *addrtype = NULL;

    if (VIR_ALLOC(actual) < 0) {
        virReportOOMError();
        return -1;
    }

    ctxt->node = node;

    type = virXMLPropString(node, "type");
    if (!type) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("missing type attribute in interface's <actual> element"));
        goto error;
    }
    if ((actual->type = virDomainNetTypeFromString(type)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown type '%s' in interface's <actual> element"), type);
        goto error;
    }
    if (actual->type != VIR_DOMAIN_NET_TYPE_BRIDGE &&
        actual->type != VIR_DOMAIN_NET_TYPE_DIRECT &&
        actual->type != VIR_DOMAIN_NET_TYPE_HOSTDEV &&
        actual->type != VIR_DOMAIN_NET_TYPE_NETWORK) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unsupported type '%s' in interface's <actual> element"),
                       type);
        goto error;
    }

    virtPortNode = virXPathNode("./virtualport", ctxt);
    if (virtPortNode) {
        if (actual->type == VIR_DOMAIN_NET_TYPE_BRIDGE ||
            actual->type == VIR_DOMAIN_NET_TYPE_DIRECT ||
            actual->type == VIR_DOMAIN_NET_TYPE_HOSTDEV) {
            /* the virtualport in <actual> should always already
             * have an instanceid/interfaceid if its required,
             * so don't let the parser generate one */
            if (!(actual->virtPortProfile
                  = virNetDevVPortProfileParse(virtPortNode,
                                               VIR_VPORT_XML_REQUIRE_ALL_ATTRIBUTES |
                                               VIR_VPORT_XML_REQUIRE_TYPE))) {
                goto error;
            }
        } else {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("<virtualport> element unsupported for type='%s'"
                             " in interface's <actual> element"), type);
            goto error;
        }
    }

    if (actual->type == VIR_DOMAIN_NET_TYPE_DIRECT) {
        actual->data.direct.linkdev = virXPathString("string(./source[1]/@dev)", ctxt);

        mode = virXPathString("string(./source[1]/@mode)", ctxt);
        if (mode) {
            int m;
            if ((m = virNetDevMacVLanModeTypeFromString(mode)) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Unknown mode '%s' in interface <actual> element"),
                               mode);
                goto error;
            }
            actual->data.direct.mode = m;
        }
    } else if (actual->type == VIR_DOMAIN_NET_TYPE_HOSTDEV) {
        virDomainHostdevDefPtr hostdev = &actual->data.hostdev.def;

        hostdev->parent.type = VIR_DOMAIN_DEVICE_NET;
        hostdev->parent.data.net = parent;
        hostdev->info = &parent->info;
        /* The helper function expects type to already be found and
         * passed in as a string, since it is in a different place in
         * NetDef vs HostdevDef.
         */
        addrtype = virXPathString("string(./source/address/@type)", ctxt);
        /* if not explicitly stated, source/vendor implies usb device */
        if (!addrtype && virXPathNode("./source/vendor", ctxt) &&
            (addrtype = strdup("usb")) == NULL) {
            virReportOOMError();
            goto error;
        }
        hostdev->mode = VIR_DOMAIN_HOSTDEV_MODE_SUBSYS;
        if (virDomainHostdevDefParseXMLSubsys(node, ctxt, addrtype,
                                              hostdev, flags) < 0) {
            goto error;
        }
    } else if (actual->type == VIR_DOMAIN_NET_TYPE_NETWORK) {
        char *class_id = virXPathString("string(./class/@id)", ctxt);
        if (class_id &&
            virStrToLong_ui(class_id, NULL, 10, &actual->class_id) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unable to parse class id '%s'"),
                           class_id);
            VIR_FREE(class_id);
            goto error;
        }
        VIR_FREE(class_id);
    }

    bandwidth_node = virXPathNode("./bandwidth", ctxt);
    if (bandwidth_node &&
        !(actual->bandwidth = virNetDevBandwidthParse(bandwidth_node,
                                                      actual->type)))
        goto error;

    vlanNode = virXPathNode("./vlan", ctxt);
    if (vlanNode && virNetDevVlanParse(vlanNode, ctxt, &actual->vlan) < 0)
       goto error;

    *def = actual;
    actual = NULL;
    ret = 0;
error:
    VIR_FREE(type);
    VIR_FREE(mode);
    VIR_FREE(addrtype);
    virDomainActualNetDefFree(actual);

    ctxt->node = save_ctxt;
    return ret;
}

#define NET_MODEL_CHARS \
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ091234567890_-"

/* Parse the XML definition for a network interface
 * @param node XML nodeset to parse for net definition
 * @return 0 on success, -1 on failure
 */
static virDomainNetDefPtr
virDomainNetDefParseXML(virDomainXMLOptionPtr xmlopt,
                        xmlNodePtr node,
                        xmlXPathContextPtr ctxt,
                        virBitmapPtr bootMap,
                        unsigned int flags)
{
    virDomainNetDefPtr def;
    virDomainHostdevDefPtr hostdev;
    xmlNodePtr cur;
    char *macaddr = NULL;
    char *type = NULL;
    char *network = NULL;
    char *portgroup = NULL;
    char *bridge = NULL;
    char *dev = NULL;
    char *ifname = NULL;
    char *script = NULL;
    char *address = NULL;
    char *port = NULL;
    char *model = NULL;
    char *backend = NULL;
    char *txmode = NULL;
    char *ioeventfd = NULL;
    char *event_idx = NULL;
    char *filter = NULL;
    char *internal = NULL;
    char *devaddr = NULL;
    char *mode = NULL;
    char *linkstate = NULL;
    char *addrtype = NULL;
    virNWFilterHashTablePtr filterparams = NULL;
    virDomainActualNetDefPtr actual = NULL;
    xmlNodePtr oldnode = ctxt->node;
    int ret;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    ctxt->node = node;

    type = virXMLPropString(node, "type");
    if (type != NULL) {
        if ((int)(def->type = virDomainNetTypeFromString(type)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown interface type '%s'"), type);
            goto error;
        }
    } else {
        def->type = VIR_DOMAIN_NET_TYPE_USER;
    }

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (!macaddr && xmlStrEqual(cur->name, BAD_CAST "mac")) {
                macaddr = virXMLPropString(cur, "address");
            } else if (!network &&
                       def->type == VIR_DOMAIN_NET_TYPE_NETWORK &&
                       xmlStrEqual(cur->name, BAD_CAST "source")) {
                network = virXMLPropString(cur, "network");
                portgroup = virXMLPropString(cur, "portgroup");
            } else if (!internal &&
                       def->type == VIR_DOMAIN_NET_TYPE_INTERNAL &&
                       xmlStrEqual(cur->name, BAD_CAST "source")) {
                internal = virXMLPropString(cur, "name");
            } else if (!bridge &&
                       def->type == VIR_DOMAIN_NET_TYPE_BRIDGE &&
                       xmlStrEqual(cur->name, BAD_CAST "source")) {
                bridge = virXMLPropString(cur, "bridge");
            } else if (!dev &&
                       (def->type == VIR_DOMAIN_NET_TYPE_ETHERNET ||
                        def->type == VIR_DOMAIN_NET_TYPE_DIRECT) &&
                       xmlStrEqual(cur->name, BAD_CAST "source")) {
                dev  = virXMLPropString(cur, "dev");
                mode = virXMLPropString(cur, "mode");
            } else if (!def->virtPortProfile
                       && xmlStrEqual(cur->name, BAD_CAST "virtualport")) {
                if (def->type == VIR_DOMAIN_NET_TYPE_NETWORK) {
                    if (!(def->virtPortProfile
                          = virNetDevVPortProfileParse(cur,
                                                       VIR_VPORT_XML_GENERATE_MISSING_DEFAULTS))) {
                        goto error;
                    }
                } else if (def->type == VIR_DOMAIN_NET_TYPE_BRIDGE ||
                           def->type == VIR_DOMAIN_NET_TYPE_DIRECT ||
                           def->type == VIR_DOMAIN_NET_TYPE_HOSTDEV) {
                    if (!(def->virtPortProfile
                          = virNetDevVPortProfileParse(cur,
                                                       VIR_VPORT_XML_GENERATE_MISSING_DEFAULTS|
                                                       VIR_VPORT_XML_REQUIRE_ALL_ATTRIBUTES|
                                                       VIR_VPORT_XML_REQUIRE_TYPE))) {
                        goto error;
                    }
                } else {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                   _("<virtualport> element unsupported for"
                                     " <interface type='%s'>"), type);
                    goto error;
                }
            } else if (!address &&
                       (def->type == VIR_DOMAIN_NET_TYPE_SERVER ||
                        def->type == VIR_DOMAIN_NET_TYPE_CLIENT ||
                        def->type == VIR_DOMAIN_NET_TYPE_MCAST) &&
                       xmlStrEqual(cur->name, BAD_CAST "source")) {
                address = virXMLPropString(cur, "address");
                port = virXMLPropString(cur, "port");
            } else if (!address &&
                       (def->type == VIR_DOMAIN_NET_TYPE_ETHERNET ||
                        def->type == VIR_DOMAIN_NET_TYPE_BRIDGE) &&
                       xmlStrEqual(cur->name, BAD_CAST "ip")) {
                address = virXMLPropString(cur, "address");
            } else if (!ifname &&
                       xmlStrEqual(cur->name, BAD_CAST "target")) {
                ifname = virXMLPropString(cur, "dev");
                if (ifname &&
                    (flags & VIR_DOMAIN_XML_INACTIVE) &&
                    STRPREFIX(ifname, VIR_NET_GENERATED_PREFIX)) {
                    /* An auto-generated target name, blank it out */
                    VIR_FREE(ifname);
                }
            } else if (!linkstate &&
                       xmlStrEqual(cur->name, BAD_CAST "link")) {
                linkstate = virXMLPropString(cur, "state");
            } else if (!script &&
                       xmlStrEqual(cur->name, BAD_CAST "script")) {
                script = virXMLPropString(cur, "path");
            } else if (xmlStrEqual(cur->name, BAD_CAST "model")) {
                model = virXMLPropString(cur, "type");
            } else if (xmlStrEqual(cur->name, BAD_CAST "driver")) {
                backend = virXMLPropString(cur, "name");
                txmode = virXMLPropString(cur, "txmode");
                ioeventfd = virXMLPropString(cur, "ioeventfd");
                event_idx = virXMLPropString(cur, "event_idx");
            } else if (xmlStrEqual(cur->name, BAD_CAST "filterref")) {
                if (filter) {
                    virReportError(VIR_ERR_XML_ERROR, "%s",
                                   _("Invalid specification of multiple <filterref>s "
                                     "in a single <interface>"));
                    goto error;
                }
                filter = virXMLPropString(cur, "filter");
                virNWFilterHashTableFree(filterparams);
                filterparams = virNWFilterParseParamAttributes(cur);
            } else if ((flags & VIR_DOMAIN_XML_INTERNAL_STATUS) &&
                       xmlStrEqual(cur->name, BAD_CAST "state")) {
                /* Legacy back-compat. Don't add any more attributes here */
                devaddr = virXMLPropString(cur, "devaddr");
            } else if (xmlStrEqual(cur->name, BAD_CAST "boot")) {
                /* boot is parsed as part of virDomainDeviceInfoParseXML */
            } else if (!actual &&
                       (flags & VIR_DOMAIN_XML_INTERNAL_ACTUAL_NET) &&
                       def->type == VIR_DOMAIN_NET_TYPE_NETWORK &&
                       xmlStrEqual(cur->name, BAD_CAST "actual")) {
                if (virDomainActualNetDefParseXML(cur, ctxt, def,
                                                  &actual, flags) < 0) {
                    goto error;
                }
            } else if (xmlStrEqual(cur->name, BAD_CAST "bandwidth")) {
                if (!(def->bandwidth = virNetDevBandwidthParse(cur,
                                                               def->type)))
                    goto error;
            } else if (xmlStrEqual(cur->name, BAD_CAST "vlan")) {
                if (virNetDevVlanParse(cur, ctxt, &def->vlan) < 0)
                    goto error;
            }
        }
        cur = cur->next;
    }

    if (macaddr) {
        if (virMacAddrParse((const char *)macaddr, &def->mac) < 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("unable to parse mac address '%s'"),
                           (const char *)macaddr);
            goto error;
        }
        if (virMacAddrIsMulticast(&def->mac)) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("expected unicast mac address, found multicast '%s'"),
                           (const char *)macaddr);
            goto error;
        }
    } else {
        virDomainNetGenerateMAC(xmlopt, &def->mac);
    }

    if (devaddr) {
        if (virDomainParseLegacyDeviceAddress(devaddr,
                                              &def->info.addr.pci) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unable to parse devaddr parameter '%s'"),
                           devaddr);
            goto error;
        }
        def->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
    } else {
        if (virDomainDeviceInfoParseXML(node, bootMap, &def->info,
                                        flags | VIR_DOMAIN_XML_INTERNAL_ALLOW_BOOT
                                        | VIR_DOMAIN_XML_INTERNAL_ALLOW_ROM) < 0)
            goto error;
    }

    /* XXX what about ISA/USB based NIC models - once we support
     * them we should make sure address type is correct */
    if (def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
        def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO &&
        def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW &&
        def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390 &&
        def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Network interfaces must use 'pci' address type"));
        goto error;
    }

    switch (def->type) {
    case VIR_DOMAIN_NET_TYPE_NETWORK:
        if (network == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("No <source> 'network' attribute "
                             "specified with <interface type='network'/>"));
            goto error;
        }
        def->data.network.name = network;
        network = NULL;
        def->data.network.portgroup = portgroup;
        portgroup = NULL;
        def->data.network.actual = actual;
        actual = NULL;
        break;

    case VIR_DOMAIN_NET_TYPE_ETHERNET:
        if (dev != NULL) {
            def->data.ethernet.dev = dev;
            dev = NULL;
        }
        if (address != NULL) {
            def->data.ethernet.ipaddr = address;
            address = NULL;
        }
        break;

    case VIR_DOMAIN_NET_TYPE_BRIDGE:
        if (bridge == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("No <source> 'bridge' attribute "
                             "specified with <interface type='bridge'/>"));
            goto error;
        }
        def->data.bridge.brname = bridge;
        bridge = NULL;
        if (address != NULL) {
            def->data.bridge.ipaddr = address;
            address = NULL;
        }
        break;

    case VIR_DOMAIN_NET_TYPE_CLIENT:
    case VIR_DOMAIN_NET_TYPE_SERVER:
    case VIR_DOMAIN_NET_TYPE_MCAST:
        if (port == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("No <source> 'port' attribute "
                             "specified with socket interface"));
            goto error;
        }
        if (virStrToLong_i(port, NULL, 10, &def->data.socket.port) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Cannot parse <source> 'port' attribute "
                             "with socket interface"));
            goto error;
        }

        if (address == NULL) {
            if (def->type == VIR_DOMAIN_NET_TYPE_CLIENT ||
                def->type == VIR_DOMAIN_NET_TYPE_MCAST) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("No <source> 'address' attribute "
                                 "specified with socket interface"));
                goto error;
            }
        } else {
            def->data.socket.address = address;
            address = NULL;
        }
        break;

    case VIR_DOMAIN_NET_TYPE_INTERNAL:
        if (internal == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("No <source> 'name' attribute specified "
                             "with <interface type='internal'/>"));
            goto error;
        }
        def->data.internal.name = internal;
        internal = NULL;
        break;

    case VIR_DOMAIN_NET_TYPE_DIRECT:
        if (dev == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("No <source> 'dev' attribute specified "
                             "with <interface type='direct'/>"));
            goto error;
        }

        if (mode != NULL) {
            int m;
            if ((m = virNetDevMacVLanModeTypeFromString(mode)) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Unknown mode has been specified"));
                goto error;
            }
            def->data.direct.mode = m;
        } else {
            def->data.direct.mode = VIR_NETDEV_MACVLAN_MODE_VEPA;
        }

        def->data.direct.linkdev = dev;
        dev = NULL;

        if (flags & VIR_DOMAIN_XML_INACTIVE)
            VIR_FREE(ifname);

        break;

    case VIR_DOMAIN_NET_TYPE_HOSTDEV:
        hostdev = &def->data.hostdev.def;
        hostdev->parent.type = VIR_DOMAIN_DEVICE_NET;
        hostdev->parent.data.net = def;
        hostdev->info = &def->info;
        /* The helper function expects type to already be found and
         * passed in as a string, since it is in a different place in
         * NetDef vs HostdevDef.
         */
        addrtype = virXPathString("string(./source/address/@type)", ctxt);
        /* if not explicitly stated, source/vendor implies usb device */
        if (!addrtype && virXPathNode("./source/vendor", ctxt) &&
            ((addrtype = strdup("usb")) == NULL)) {
            virReportOOMError();
            goto error;
        }
        hostdev->mode = VIR_DOMAIN_HOSTDEV_MODE_SUBSYS;
        if (virDomainHostdevDefParseXMLSubsys(node, ctxt, addrtype,
                                              hostdev, flags) < 0) {
            goto error;
        }
        break;

    case VIR_DOMAIN_NET_TYPE_USER:
    case VIR_DOMAIN_NET_TYPE_LAST:
        break;
    }

    if (script != NULL) {
        def->script = script;
        script = NULL;
    }
    if (ifname != NULL) {
        def->ifname = ifname;
        ifname = NULL;
    }

    /* NIC model (see -net nic,model=?).  We only check that it looks
     * reasonable, not that it is a supported NIC type.  FWIW kvm
     * supports these types as of April 2008:
     * i82551 i82557b i82559er ne2k_pci pcnet rtl8139 e1000 virtio
     * QEMU PPC64 supports spapr-vlan
     */
    if (model != NULL) {
        if (strspn(model, NET_MODEL_CHARS) < strlen(model)) {
            virReportError(VIR_ERR_INVALID_ARG, "%s",
                           _("Model name contains invalid characters"));
            goto error;
        }
        def->model = model;
        model = NULL;
    }

    if (def->model && STREQ(def->model, "virtio")) {
        if (backend != NULL) {
            int name;
            if ((name = virDomainNetBackendTypeFromString(backend)) < 0 ||
                name == VIR_DOMAIN_NET_BACKEND_TYPE_DEFAULT) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Unknown interface <driver name='%s'> "
                                 "has been specified"),
                               backend);
                goto error;
            }
            def->driver.virtio.name = name;
        }
        if (txmode != NULL) {
            int m;
            if ((m = virDomainNetVirtioTxModeTypeFromString(txmode)) < 0 ||
                m == VIR_DOMAIN_NET_VIRTIO_TX_MODE_DEFAULT) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Unknown interface <driver txmode='%s'> "
                                 "has been specified"),
                               txmode);
                goto error;
            }
            def->driver.virtio.txmode = m;
        }
        if (ioeventfd) {
            int i;
            if ((i = virDomainIoEventFdTypeFromString(ioeventfd)) <= 0) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("unknown interface ioeventfd mode '%s'"),
                               ioeventfd);
                goto error;
            }
            def->driver.virtio.ioeventfd = i;
        }
        if (event_idx) {
            int idx;
            if ((idx = virDomainVirtioEventIdxTypeFromString(event_idx)) <= 0) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("unknown interface event_idx mode '%s'"),
                               event_idx);
                goto error;
            }
            def->driver.virtio.event_idx = idx;
        }
    }

    def->linkstate = VIR_DOMAIN_NET_INTERFACE_LINK_STATE_DEFAULT;
    if (linkstate != NULL) {
        if ((def->linkstate = virDomainNetInterfaceLinkStateTypeFromString(linkstate)) <= 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unknown interface link state '%s'"),
                           linkstate);
            goto error;
        }
    }

    if (filter != NULL) {
        switch (def->type) {
        case VIR_DOMAIN_NET_TYPE_ETHERNET:
        case VIR_DOMAIN_NET_TYPE_NETWORK:
        case VIR_DOMAIN_NET_TYPE_BRIDGE:
            def->filter = filter;
            filter = NULL;
            def->filterparams = filterparams;
            filterparams = NULL;
        break;
        default:
        break;
        }
    }

    ret = virXPathULong("string(./tune/sndbuf)", ctxt, &def->tune.sndbuf);
    if (ret >= 0) {
        def->tune.sndbuf_specified = true;
    } else if (ret == -2) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("sndbuf must be a positive integer"));
        goto error;
    }

cleanup:
    ctxt->node = oldnode;
    VIR_FREE(macaddr);
    VIR_FREE(network);
    VIR_FREE(portgroup);
    VIR_FREE(address);
    VIR_FREE(port);
    VIR_FREE(ifname);
    VIR_FREE(dev);
    virDomainActualNetDefFree(actual);
    VIR_FREE(script);
    VIR_FREE(bridge);
    VIR_FREE(model);
    VIR_FREE(backend);
    VIR_FREE(txmode);
    VIR_FREE(ioeventfd);
    VIR_FREE(event_idx);
    VIR_FREE(filter);
    VIR_FREE(type);
    VIR_FREE(internal);
    VIR_FREE(devaddr);
    VIR_FREE(mode);
    VIR_FREE(linkstate);
    VIR_FREE(addrtype);
    virNWFilterHashTableFree(filterparams);

    return def;

error:
    virDomainNetDefFree(def);
    def = NULL;
    goto cleanup;
}

static int
virDomainChrDefaultTargetType(int devtype) {
    switch ((enum virDomainChrDeviceType) devtype) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
        virReportError(VIR_ERR_XML_ERROR,
                       _("target type must be specified for %s device"),
                       virDomainChrDeviceTypeToString(devtype));
        return -1;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE:
        return VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_NONE;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL:
        return VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_ISA;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_PARALLEL:
    case VIR_DOMAIN_CHR_DEVICE_TYPE_LAST:
        /* No target type yet*/
        break;
    }

    return 0;
}

static int
virDomainChrTargetTypeFromString(virDomainChrDefPtr def,
                                 int devtype,
                                 const char *targetType)
{
    int ret = -1;

    if (!targetType)
        return virDomainChrDefaultTargetType(devtype);

    switch ((enum virDomainChrDeviceType) devtype) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
        ret = virDomainChrChannelTargetTypeFromString(targetType);
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE:
        ret = virDomainChrConsoleTargetTypeFromString(targetType);
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL:
        ret = virDomainChrSerialTargetTypeFromString(targetType);
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_PARALLEL:
    case VIR_DOMAIN_CHR_DEVICE_TYPE_LAST:
        /* No target type yet*/
        ret = 0;
        break;
    }

    def->targetTypeAttr = true;

    return ret;
}

static int
virDomainChrDefParseTargetXML(virDomainChrDefPtr def,
                              xmlNodePtr cur)
{
    int ret = -1;
    unsigned int port;
    const char *targetType = virXMLPropString(cur, "type");
    const char *addrStr = NULL;
    const char *portStr = NULL;

    if ((def->targetType =
         virDomainChrTargetTypeFromString(def, def->deviceType,
                                          targetType)) < 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("unknown target type '%s' specified for character device"),
                       targetType);
        goto error;
    }

    switch (def->deviceType) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
        switch (def->targetType) {
        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_GUESTFWD:
            addrStr = virXMLPropString(cur, "address");
            portStr = virXMLPropString(cur, "port");

            if (VIR_ALLOC(def->target.addr) < 0) {
                virReportOOMError();
                goto error;
            }

            if (addrStr == NULL) {
                virReportError(VIR_ERR_XML_ERROR, "%s",
                               _("guestfwd channel does not "
                                 "define a target address"));
                goto error;
            }

            if (virSocketAddrParse(def->target.addr, addrStr, AF_UNSPEC) < 0)
                goto error;

            if (def->target.addr->data.stor.ss_family != AF_INET) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               "%s", _("guestfwd channel only supports "
                                       "IPv4 addresses"));
                goto error;
            }

            if (portStr == NULL) {
                virReportError(VIR_ERR_XML_ERROR, "%s",
                               _("guestfwd channel does "
                                 "not define a target port"));
                goto error;
            }

            if (virStrToLong_ui(portStr, NULL, 10, &port) < 0) {
                virReportError(VIR_ERR_XML_ERROR,
                               _("Invalid port number: %s"),
                               portStr);
                goto error;
            }

            virSocketAddrSetPort(def->target.addr, port);
            break;

        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO:
            def->target.name = virXMLPropString(cur, "name");
            break;
        }
        break;

    default:
        portStr = virXMLPropString(cur, "port");
        if (portStr == NULL) {
            /* Set to negative value to indicate we should set it later */
            def->target.port = -1;
            break;
        }

        if (virStrToLong_ui(portStr, NULL, 10, &port) < 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("Invalid port number: %s"),
                           portStr);
            goto error;
        }
        def->target.port = port;
        break;
    }


    ret = 0;
error:
    VIR_FREE(targetType);
    VIR_FREE(addrStr);
    VIR_FREE(portStr);

    return ret;
}

/* Parse the source half of the XML definition for a character device,
 * where node is the first element of node->children of the parent
 * element.  def->type must already be valid.  Return -1 on failure,
 * otherwise the number of ignored children (this intentionally skips
 * <target>, which is used by <serial> but not <smartcard>). */
static int
virDomainChrSourceDefParseXML(virDomainChrSourceDefPtr def,
                              xmlNodePtr cur, unsigned int flags,
                              virDomainChrDefPtr chr_def,
                              xmlXPathContextPtr ctxt,
                              virSecurityLabelDefPtr* vmSeclabels,
                              int nvmSeclabels)
{
    char *bindHost = NULL;
    char *bindService = NULL;
    char *connectHost = NULL;
    char *connectService = NULL;
    char *path = NULL;
    char *mode = NULL;
    char *protocol = NULL;
    int remaining = 0;

    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (xmlStrEqual(cur->name, BAD_CAST "source")) {
                if (mode == NULL)
                    mode = virXMLPropString(cur, "mode");

                switch (def->type) {
                case VIR_DOMAIN_CHR_TYPE_PTY:
                case VIR_DOMAIN_CHR_TYPE_DEV:
                case VIR_DOMAIN_CHR_TYPE_FILE:
                case VIR_DOMAIN_CHR_TYPE_PIPE:
                case VIR_DOMAIN_CHR_TYPE_UNIX:
                    /* PTY path is only parsed from live xml.  */
                    if (path == NULL &&
                        (def->type != VIR_DOMAIN_CHR_TYPE_PTY ||
                         !(flags & VIR_DOMAIN_XML_INACTIVE)))
                        path = virXMLPropString(cur, "path");

                    break;

                case VIR_DOMAIN_CHR_TYPE_UDP:
                case VIR_DOMAIN_CHR_TYPE_TCP:
                    if (mode == NULL ||
                        STREQ((const char *)mode, "connect")) {

                        if (connectHost == NULL)
                            connectHost = virXMLPropString(cur, "host");
                        if (connectService == NULL)
                            connectService = virXMLPropString(cur, "service");
                    } else if (STREQ((const char *)mode, "bind")) {
                        if (bindHost == NULL)
                            bindHost = virXMLPropString(cur, "host");
                        if (bindService == NULL)
                            bindService = virXMLPropString(cur, "service");
                    } else {
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                       _("Unknown source mode '%s'"),
                                       mode);
                        goto error;
                    }

                    if (def->type == VIR_DOMAIN_CHR_TYPE_UDP)
                        VIR_FREE(mode);
                }

                /* Check for an optional seclabel override in <source/>. */
                if (chr_def) {
                    xmlNodePtr saved_node = ctxt->node;
                    ctxt->node = cur;
                    if (virSecurityDeviceLabelDefParseXML(&chr_def->seclabels,
                                                          &chr_def->nseclabels,
                                                          vmSeclabels,
                                                          nvmSeclabels,
                                                          ctxt) < 0) {
                        ctxt->node = saved_node;
                        goto error;
                    }
                    ctxt->node = saved_node;
                }
            } else if (xmlStrEqual(cur->name, BAD_CAST "protocol")) {
                if (protocol == NULL)
                    protocol = virXMLPropString(cur, "type");
            } else {
                remaining++;
            }
        }
        cur = cur->next;
    }

    switch (def->type) {
    case VIR_DOMAIN_CHR_TYPE_NULL:
        /* Nada */
        break;

    case VIR_DOMAIN_CHR_TYPE_VC:
        break;

    case VIR_DOMAIN_CHR_TYPE_PTY:
    case VIR_DOMAIN_CHR_TYPE_DEV:
    case VIR_DOMAIN_CHR_TYPE_FILE:
    case VIR_DOMAIN_CHR_TYPE_PIPE:
        if (path == NULL &&
            def->type != VIR_DOMAIN_CHR_TYPE_PTY) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Missing source path attribute for char device"));
            goto error;
        }

        def->data.file.path = path;
        path = NULL;
        break;

    case VIR_DOMAIN_CHR_TYPE_STDIO:
    case VIR_DOMAIN_CHR_TYPE_SPICEVMC:
        /* Nada */
        break;

    case VIR_DOMAIN_CHR_TYPE_TCP:
        if (mode == NULL ||
            STREQ(mode, "connect")) {
            if (connectHost == NULL) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Missing source host attribute for char device"));
                goto error;
            }
            if (connectService == NULL) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Missing source service attribute for char device"));
                goto error;
            }

            def->data.tcp.host = connectHost;
            connectHost = NULL;
            def->data.tcp.service = connectService;
            connectService = NULL;
            def->data.tcp.listen = false;
        } else {
            if (bindHost == NULL) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Missing source host attribute for char device"));
                goto error;
            }
            if (bindService == NULL) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Missing source service attribute for char device"));
                goto error;
            }

            def->data.tcp.host = bindHost;
            bindHost = NULL;
            def->data.tcp.service = bindService;
            bindService = NULL;
            def->data.tcp.listen = true;
        }

        if (protocol == NULL)
            def->data.tcp.protocol = VIR_DOMAIN_CHR_TCP_PROTOCOL_RAW;
        else if ((def->data.tcp.protocol =
                  virDomainChrTcpProtocolTypeFromString(protocol)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown protocol '%s'"), protocol);
            goto error;
        }

        break;

    case VIR_DOMAIN_CHR_TYPE_UDP:
        if (connectService == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Missing source service attribute for char device"));
            goto error;
        }

        def->data.udp.connectHost = connectHost;
        connectHost = NULL;
        def->data.udp.connectService = connectService;
        connectService = NULL;

        def->data.udp.bindHost = bindHost;
        bindHost = NULL;
        def->data.udp.bindService = bindService;
        bindService = NULL;
        break;

    case VIR_DOMAIN_CHR_TYPE_UNIX:
        if (path == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Missing source path attribute for char device"));
            goto error;
        }

        def->data.nix.listen = mode != NULL && STRNEQ(mode, "connect");

        def->data.nix.path = path;
        path = NULL;
        break;
    }

cleanup:
    VIR_FREE(mode);
    VIR_FREE(protocol);
    VIR_FREE(bindHost);
    VIR_FREE(bindService);
    VIR_FREE(connectHost);
    VIR_FREE(connectService);
    VIR_FREE(path);

    return remaining;

error:
    virDomainChrSourceDefClear(def);
    remaining = -1;
    goto cleanup;
}

/* Create a new character device definition and set
 * default port.
 */
virDomainChrDefPtr
virDomainChrDefNew(void) {
    virDomainChrDefPtr def = NULL;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    def->target.port = -1;
    return def;
}

/* Parse the XML definition for a character device
 * @param node XML nodeset to parse for net definition
 *
 * The XML we're dealing with looks like
 *
 * <serial type="pty">
 *   <source path="/dev/pts/3"/>
 *   <target port="1"/>
 * </serial>
 *
 * <serial type="dev">
 *   <source path="/dev/ttyS0"/>
 *   <target port="1"/>
 * </serial>
 *
 * <serial type="tcp">
 *   <source mode="connect" host="0.0.0.0" service="2445"/>
 *   <target port="1"/>
 * </serial>
 *
 * <serial type="tcp">
 *   <source mode="bind" host="0.0.0.0" service="2445"/>
 *   <target port="1"/>
 *   <protocol type='raw'/>
 * </serial>
 *
 * <serial type="udp">
 *   <source mode="bind" host="0.0.0.0" service="2445"/>
 *   <source mode="connect" host="0.0.0.0" service="2445"/>
 *   <target port="1"/>
 * </serial>
 *
 * <serial type="unix">
 *   <source mode="bind" path="/tmp/foo"/>
 *   <target port="1"/>
 * </serial>
 *
 */
static virDomainChrDefPtr
virDomainChrDefParseXML(xmlXPathContextPtr ctxt,
                        xmlNodePtr node,
                        virSecurityLabelDefPtr* vmSeclabels,
                        int nvmSeclabels,
                        unsigned int flags)
{
    xmlNodePtr cur;
    char *type = NULL;
    const char *nodeName;
    virDomainChrDefPtr def;
    int remaining;
    bool seenTarget = false;

    if (!(def = virDomainChrDefNew()))
        return NULL;

    type = virXMLPropString(node, "type");
    if (type == NULL) {
        def->source.type = VIR_DOMAIN_CHR_TYPE_PTY;
    } else if ((def->source.type = virDomainChrTypeFromString(type)) < 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("unknown type presented to host for character device: %s"),
                       type);
        goto error;
    }

    nodeName = (const char *) node->name;
    if ((def->deviceType = virDomainChrDeviceTypeFromString(nodeName)) < 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("unknown character device type: %s"),
                       nodeName);
    }

    cur = node->children;
    remaining = virDomainChrSourceDefParseXML(&def->source, cur, flags,
                                              def, ctxt,
                                              vmSeclabels, nvmSeclabels);
    if (remaining < 0)
        goto error;
    if (remaining) {
        while (cur != NULL) {
            if (cur->type == XML_ELEMENT_NODE) {
                if (xmlStrEqual(cur->name, BAD_CAST "target")) {
                    seenTarget = true;
                    if (virDomainChrDefParseTargetXML(def, cur) < 0) {
                        goto error;
                    }
                }
            }
            cur = cur->next;
        }
    }

    if (!seenTarget &&
        ((def->targetType = virDomainChrDefaultTargetType(def->deviceType)) < 0))
        goto cleanup;

    if (def->source.type == VIR_DOMAIN_CHR_TYPE_SPICEVMC) {
        if (def->targetType != VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("spicevmc device type only supports "
                             "virtio"));
            goto error;
        } else {
            def->source.data.spicevmc = VIR_DOMAIN_CHR_SPICEVMC_VDAGENT;
        }
    }

    if (virDomainDeviceInfoParseXML(node, NULL, &def->info, flags) < 0)
        goto error;

    if (def->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL &&
        def->targetType == VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_USB &&
        def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
        def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("usb-serial requires address of usb type"));
        goto error;
    }

cleanup:
    VIR_FREE(type);

    return def;

error:
    virDomainChrDefFree(def);
    def = NULL;
    goto cleanup;
}

static virDomainSmartcardDefPtr
virDomainSmartcardDefParseXML(xmlNodePtr node,
                              unsigned int flags)
{
    xmlNodePtr cur;
    char *mode = NULL;
    char *type = NULL;
    virDomainSmartcardDefPtr def;
    int i;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    mode = virXMLPropString(node, "mode");
    if (mode == NULL) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("missing smartcard device mode"));
        goto error;
    }
    if ((def->type = virDomainSmartcardTypeFromString(mode)) < 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("unknown smartcard device mode: %s"),
                       mode);
        goto error;
    }

    switch (def->type) {
    case VIR_DOMAIN_SMARTCARD_TYPE_HOST:
        break;

    case VIR_DOMAIN_SMARTCARD_TYPE_HOST_CERTIFICATES:
        i = 0;
        cur = node->children;
        while (cur) {
            if (cur->type == XML_ELEMENT_NODE &&
                xmlStrEqual(cur->name, BAD_CAST "certificate")) {
                if (i == 3) {
                    virReportError(VIR_ERR_XML_ERROR, "%s",
                                   _("host-certificates mode needs "
                                     "exactly three certificates"));
                    goto error;
                }
                def->data.cert.file[i] = (char *)xmlNodeGetContent(cur);
                if (!def->data.cert.file[i]) {
                    virReportOOMError();
                    goto error;
                }
                i++;
            } else if (cur->type == XML_ELEMENT_NODE &&
                       xmlStrEqual(cur->name, BAD_CAST "database") &&
                       !def->data.cert.database) {
                def->data.cert.database = (char *)xmlNodeGetContent(cur);
                if (!def->data.cert.database) {
                    virReportOOMError();
                    goto error;
                }
                if (*def->data.cert.database != '/') {
                    virReportError(VIR_ERR_XML_ERROR,
                                   _("expecting absolute path: %s"),
                                   def->data.cert.database);
                    goto error;
                }
            }
            cur = cur->next;
        }
        if (i < 3) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("host-certificates mode needs "
                             "exactly three certificates"));
            goto error;
        }
        break;

    case VIR_DOMAIN_SMARTCARD_TYPE_PASSTHROUGH:
        type = virXMLPropString(node, "type");
        if (type == NULL) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("passthrough mode requires a character "
                             "device type attribute"));
            goto error;
        }
        if ((def->data.passthru.type = virDomainChrTypeFromString(type)) < 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("unknown type presented to host for "
                             "character device: %s"), type);
            goto error;
        }

        cur = node->children;
        if (virDomainChrSourceDefParseXML(&def->data.passthru, cur, flags,
                                          NULL, NULL, NULL, 0) < 0)
            goto error;

        if (def->data.passthru.type == VIR_DOMAIN_CHR_TYPE_SPICEVMC) {
            def->data.passthru.data.spicevmc
                = VIR_DOMAIN_CHR_SPICEVMC_SMARTCARD;
        }

        break;

    default:
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("unknown smartcard mode"));
        goto error;
    }

    if (virDomainDeviceInfoParseXML(node, NULL, &def->info, flags) < 0)
        goto error;
    if (def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
        def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Controllers must use the 'ccid' address type"));
        goto error;
    }

cleanup:
    VIR_FREE(mode);
    VIR_FREE(type);

    return def;

error:
    virDomainSmartcardDefFree(def);
    def = NULL;
    goto cleanup;
}

/* Parse the XML definition for an input device */
static virDomainInputDefPtr
virDomainInputDefParseXML(const char *ostype,
                          xmlNodePtr node,
                          unsigned int flags)
{
    virDomainInputDefPtr def;
    char *type = NULL;
    char *bus = NULL;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    type = virXMLPropString(node, "type");
    bus = virXMLPropString(node, "bus");

    if (!type) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing input device type"));
        goto error;
    }

    if ((def->type = virDomainInputTypeFromString(type)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown input device type '%s'"), type);
        goto error;
    }

    if (bus) {
        if ((def->bus = virDomainInputBusTypeFromString(bus)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown input bus type '%s'"), bus);
            goto error;
        }

        if (STREQ(ostype, "hvm")) {
            if (def->bus == VIR_DOMAIN_INPUT_BUS_PS2 && /* Only allow mouse for ps2 */
                def->type != VIR_DOMAIN_INPUT_TYPE_MOUSE) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("ps2 bus does not support %s input device"),
                               type);
                goto error;
            }
            if (def->bus == VIR_DOMAIN_INPUT_BUS_XEN) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("unsupported input bus %s"),
                               bus);
                goto error;
            }
        } else {
            if (def->bus != VIR_DOMAIN_INPUT_BUS_XEN) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("unsupported input bus %s"),
                               bus);
            }
            if (def->type != VIR_DOMAIN_INPUT_TYPE_MOUSE) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("xen bus does not support %s input device"),
                               type);
                goto error;
            }
        }
    } else {
        if (STREQ(ostype, "hvm")) {
            if (def->type == VIR_DOMAIN_INPUT_TYPE_MOUSE)
                def->bus = VIR_DOMAIN_INPUT_BUS_PS2;
            else
                def->bus = VIR_DOMAIN_INPUT_BUS_USB;
        } else {
            def->bus = VIR_DOMAIN_INPUT_BUS_XEN;
        }
    }

    if (virDomainDeviceInfoParseXML(node, NULL, &def->info, flags) < 0)
        goto error;

    if (def->bus == VIR_DOMAIN_INPUT_BUS_USB &&
        def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
        def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("Invalid address for a USB device"));
        goto error;
    }

cleanup:
    VIR_FREE(type);
    VIR_FREE(bus);

    return def;

error:
    virDomainInputDefFree(def);
    def = NULL;
    goto cleanup;
}


/* Parse the XML definition for a hub device */
static virDomainHubDefPtr
virDomainHubDefParseXML(xmlNodePtr node, unsigned int flags)
{
    virDomainHubDefPtr def;
    char *type = NULL;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    type = virXMLPropString(node, "type");

    if (!type) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing hub device type"));
        goto error;
    }

    if ((def->type = virDomainHubTypeFromString(type)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown hub device type '%s'"), type);
        goto error;
    }

    if (virDomainDeviceInfoParseXML(node, NULL, &def->info, flags) < 0)
        goto error;

cleanup:
    VIR_FREE(type);

    return def;

error:
    virDomainHubDefFree(def);
    def = NULL;
    goto cleanup;
}


/* Parse the XML definition for a clock timer */
static virDomainTimerDefPtr
virDomainTimerDefParseXML(const xmlNodePtr node,
                          xmlXPathContextPtr ctxt)
{
    char *name = NULL;
    char *present = NULL;
    char *tickpolicy = NULL;
    char *track = NULL;
    char *mode = NULL;

    virDomainTimerDefPtr def;
    xmlNodePtr oldnode = ctxt->node;
    xmlNodePtr catchup;
    int ret;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    ctxt->node = node;

    name = virXMLPropString(node, "name");
    if (name == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing timer name"));
        goto error;
    }
    if ((def->name = virDomainTimerNameTypeFromString(name)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown timer name '%s'"), name);
        goto error;
    }

    def->present = -1; /* unspecified */
    if ((present = virXMLPropString(node, "present")) != NULL) {
        if (STREQ(present, "yes")) {
            def->present = 1;
        } else if (STREQ(present, "no")) {
            def->present = 0;
        } else {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown timer present value '%s'"), present);
            goto error;
        }
    }

    def->tickpolicy = -1;
    tickpolicy = virXMLPropString(node, "tickpolicy");
    if (tickpolicy != NULL) {
        if ((def->tickpolicy = virDomainTimerTickpolicyTypeFromString(tickpolicy)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown timer tickpolicy '%s'"), tickpolicy);
            goto error;
        }
    }

    def->track = -1;
    track = virXMLPropString(node, "track");
    if (track != NULL) {
        if ((def->track = virDomainTimerTrackTypeFromString(track)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown timer track '%s'"), track);
            goto error;
        }
    }

    ret = virXPathULong("string(./frequency)", ctxt, &def->frequency);
    if (ret == -1) {
        def->frequency = 0;
    } else if (ret < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("invalid timer frequency"));
        goto error;
    }

    def->mode = -1;
    mode = virXMLPropString(node, "mode");
    if (mode != NULL) {
        if ((def->mode = virDomainTimerModeTypeFromString(mode)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown timer mode '%s'"), mode);
            goto error;
        }
    }

    catchup = virXPathNode("./catchup", ctxt);
    if (catchup != NULL) {
        ret = virXPathULong("string(./catchup/@threshold)", ctxt,
                            &def->catchup.threshold);
        if (ret == -1) {
            def->catchup.threshold = 0;
        } else if (ret < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("invalid catchup threshold"));
            goto error;
        }

        ret = virXPathULong("string(./catchup/@slew)", ctxt, &def->catchup.slew);
        if (ret == -1) {
            def->catchup.slew = 0;
        } else if (ret < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("invalid catchup slew"));
            goto error;
        }

        ret = virXPathULong("string(./catchup/@limit)", ctxt, &def->catchup.limit);
        if (ret == -1) {
            def->catchup.limit = 0;
        } else if (ret < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("invalid catchup limit"));
            goto error;
        }
    }

cleanup:
    VIR_FREE(name);
    VIR_FREE(present);
    VIR_FREE(tickpolicy);
    VIR_FREE(track);
    VIR_FREE(mode);
    ctxt->node = oldnode;

    return def;

error:
    VIR_FREE(def);
    goto cleanup;
}


static int
virDomainGraphicsAuthDefParseXML(xmlNodePtr node,
                                 virDomainGraphicsAuthDefPtr def,
                                 int type)
{
    char *validTo = NULL;
    char *connected = virXMLPropString(node, "connected");

    def->passwd = virXMLPropString(node, "passwd");

    if (!def->passwd)
        return 0;

    validTo = virXMLPropString(node, "passwdValidTo");
    if (validTo) {
        char *tmp;
        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        /* Expect: YYYY-MM-DDTHH:MM:SS (%d-%d-%dT%d:%d:%d)  eg 2010-11-28T14:29:01 */
        if (/* year */
            virStrToLong_i(validTo, &tmp, 10, &tm.tm_year) < 0 || *tmp != '-' ||
            /* month */
            virStrToLong_i(tmp+1, &tmp, 10, &tm.tm_mon) < 0 || *tmp != '-' ||
            /* day */
            virStrToLong_i(tmp+1, &tmp, 10, &tm.tm_mday) < 0 || *tmp != 'T' ||
            /* hour */
            virStrToLong_i(tmp+1, &tmp, 10, &tm.tm_hour) < 0 || *tmp != ':' ||
            /* minute */
            virStrToLong_i(tmp+1, &tmp, 10, &tm.tm_min) < 0 || *tmp != ':' ||
            /* second */
            virStrToLong_i(tmp+1, &tmp, 10, &tm.tm_sec) < 0 || *tmp != '\0') {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot parse password validity time '%s', expect YYYY-MM-DDTHH:MM:SS"),
                           validTo);
            VIR_FREE(validTo);
            VIR_FREE(def->passwd);
            return -1;
        }
        VIR_FREE(validTo);

        tm.tm_year -= 1900; /* Human epoch starts at 0 BC, not 1900BC */
        tm.tm_mon--; /* Humans start months at 1, computers at 0 */

        def->validTo = timegm(&tm);
        def->expires = true;
    }

    if (connected) {
        int action = virDomainGraphicsAuthConnectedTypeFromString(connected);
        if (action <= 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unknown connected value %s"),
                           connected);
            VIR_FREE(connected);
            return -1;
        }
        VIR_FREE(connected);

        /* VNC supports connected='keep' only */
        if (type == VIR_DOMAIN_GRAPHICS_TYPE_VNC &&
            action != VIR_DOMAIN_GRAPHICS_AUTH_CONNECTED_KEEP) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("VNC supports connected='keep' only"));
            return -1;
        }

        def->connected = action;
    }

    return 0;
}

static int
virDomainGraphicsListenDefParseXML(virDomainGraphicsListenDefPtr def,
                                   xmlNodePtr node,
                                   unsigned int flags)
{
    int ret = -1;
    char *type     = virXMLPropString(node, "type");
    char *address  = virXMLPropString(node, "address");
    char *network  = virXMLPropString(node, "network");

    if (!type) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("graphics listen type must be specified"));
        goto error;
    }

    if ((def->type = virDomainGraphicsListenTypeFromString(type)) < 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("unknown graphics listen type '%s'"), type);
        goto error;
    }

    /* address is recognized if either type='address', or if
     * type='network' and we're looking at live XML (i.e. *not*
     * inactive). It is otherwise ignored. */
    if (address && address[0] &&
        (def->type == VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS ||
         (def->type == VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK &&
          !(flags & VIR_DOMAIN_XML_INACTIVE)))) {
        def->address = address;
        address = NULL;
    }

    if (network && network[0]) {
        if (def->type != VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK) {
            /* network='xxx' never makes sense with anything except
             * type='address' */
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("network attribute not allowed when listen type is not network"));
            goto error;
        }
        def->network = network;
        network = NULL;
    }

    ret = 0;
error:
    if (ret < 0)
        virDomainGraphicsListenDefClear(def);
    VIR_FREE(type);
    VIR_FREE(address);
    VIR_FREE(network);
    return ret;
}


/* Parse the XML definition for a graphics device */
static virDomainGraphicsDefPtr
virDomainGraphicsDefParseXML(xmlNodePtr node,
                             xmlXPathContextPtr ctxt,
                             unsigned int flags)
{
    virDomainGraphicsDefPtr def;
    char *type = NULL;
    int nListens;
    xmlNodePtr *listenNodes = NULL;
    char *listenAddr = NULL;
    xmlNodePtr save = ctxt->node;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    ctxt->node = node;

    type = virXMLPropString(node, "type");

    if (!type) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing graphics device type"));
        goto error;
    }

    if ((def->type = virDomainGraphicsTypeFromString(type)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown graphics device type '%s'"), type);
        goto error;
    }

    if (def->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC ||
        def->type == VIR_DOMAIN_GRAPHICS_TYPE_RDP ||
        def->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE) {

        /* parse the <listen> subelements for graphics types that support it */
        nListens = virXPathNodeSet("./listen", ctxt, &listenNodes);
        if (nListens < 0)
            goto error;

        if (nListens > 0) {
            int ii;

            if (VIR_ALLOC_N(def->listens, nListens) < 0) {
                virReportOOMError();
                goto error;
            }

            for (ii = 0; ii < nListens; ii++) {
                int ret = virDomainGraphicsListenDefParseXML(&def->listens[ii],
                                                             listenNodes[ii],
                                                             flags);
                if (ret < 0)
                    goto error;
                def->nListens++;
            }
            VIR_FREE(listenNodes);
        }

        /* listen attribute of <graphics> is also supported by these,
         * but must match the 'address' attribute of the first listen
         * that is type='address' (if present) */
        listenAddr = virXMLPropString(node, "listen");
        if (listenAddr && !listenAddr[0])
            VIR_FREE(listenAddr);

        if (listenAddr) {
            if (def->nListens == 0) {
                /* There were no <listen> elements, so we can just
                 * directly set listenAddr as listens[0]->address */
                if (virDomainGraphicsListenSetAddress(def, 0, listenAddr,
                                                      -1, true) < 0)
                    goto error;
            } else {
                /* There is at least 1 listen element, so we look for
                 * the first listen of type='address', and make sure
                 * its address matches the listen attribute from
                 * graphics. */
                bool matched = false;
                const char *found = NULL;
                int ii;

                for (ii = 0; ii < nListens; ii++) {
                    if (virDomainGraphicsListenGetType(def, ii)
                        == VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS) {
                        found = virDomainGraphicsListenGetAddress(def, ii);
                        if (STREQ_NULLABLE(found, listenAddr)) {
                            matched = true;
                        }
                        break;
                    }
                }
                if (!matched) {
                    virReportError(VIR_ERR_XML_ERROR,
                                   _("graphics listen attribute %s must match address "
                                     "attribute of first listen element (found %s)"),
                                   listenAddr, found ? found : "none");
                    goto error;
                }
            }
        }
    }

    if (def->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC) {
        char *port = virXMLPropString(node, "port");
        char *autoport;

        if (port) {
            if (virStrToLong_i(port, NULL, 10, &def->data.vnc.port) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot parse vnc port %s"), port);
                VIR_FREE(port);
                goto error;
            }
            VIR_FREE(port);
            /* Legacy compat syntax, used -1 for auto-port */
            if (def->data.vnc.port == -1) {
                if (flags & VIR_DOMAIN_XML_INACTIVE)
                    def->data.vnc.port = 0;
                def->data.vnc.autoport = 1;
            }
        } else {
            def->data.vnc.port = 0;
            def->data.vnc.autoport = 1;
        }

        if ((autoport = virXMLPropString(node, "autoport")) != NULL) {
            if (STREQ(autoport, "yes")) {
                if (flags & VIR_DOMAIN_XML_INACTIVE)
                    def->data.vnc.port = 0;
                def->data.vnc.autoport = 1;
            }
            VIR_FREE(autoport);
        }

        def->data.vnc.socket = virXMLPropString(node, "socket");
        def->data.vnc.keymap = virXMLPropString(node, "keymap");

        if (virDomainGraphicsAuthDefParseXML(node, &def->data.vnc.auth,
                                             def->type) < 0)
            goto error;
    } else if (def->type == VIR_DOMAIN_GRAPHICS_TYPE_SDL) {
        char *fullscreen = virXMLPropString(node, "fullscreen");

        if (fullscreen != NULL) {
            if (STREQ(fullscreen, "yes")) {
                def->data.sdl.fullscreen = 1;
            } else if (STREQ(fullscreen, "no")) {
                def->data.sdl.fullscreen = 0;
            } else {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("unknown fullscreen value '%s'"), fullscreen);
                VIR_FREE(fullscreen);
                goto error;
            }
            VIR_FREE(fullscreen);
        } else {
            def->data.sdl.fullscreen = 0;
        }
        def->data.sdl.xauth = virXMLPropString(node, "xauth");
        def->data.sdl.display = virXMLPropString(node, "display");
    } else if (def->type == VIR_DOMAIN_GRAPHICS_TYPE_RDP) {
        char *port = virXMLPropString(node, "port");
        char *autoport;
        char *replaceUser;
        char *multiUser;

        if (port) {
            if (virStrToLong_i(port, NULL, 10, &def->data.rdp.port) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot parse rdp port %s"), port);
                VIR_FREE(port);
                goto error;
            }
            /* Legacy compat syntax, used -1 for auto-port */
            if (def->data.rdp.port == -1)
                def->data.rdp.autoport = 1;

            VIR_FREE(port);
        } else {
            def->data.rdp.port = 0;
            def->data.rdp.autoport = 1;
        }

        if ((autoport = virXMLPropString(node, "autoport")) != NULL) {
            if (STREQ(autoport, "yes"))
                def->data.rdp.autoport = 1;

            VIR_FREE(autoport);
        }

        if (def->data.rdp.autoport && (flags & VIR_DOMAIN_XML_INACTIVE))
            def->data.rdp.port = 0;

        if ((replaceUser = virXMLPropString(node, "replaceUser")) != NULL) {
            if (STREQ(replaceUser, "yes")) {
                def->data.rdp.replaceUser = 1;
            }
            VIR_FREE(replaceUser);
        }

        if ((multiUser = virXMLPropString(node, "multiUser")) != NULL) {
            if (STREQ(multiUser, "yes")) {
                def->data.rdp.multiUser = 1;
            }
            VIR_FREE(multiUser);
        }

    } else if (def->type == VIR_DOMAIN_GRAPHICS_TYPE_DESKTOP) {
        char *fullscreen = virXMLPropString(node, "fullscreen");

        if (fullscreen != NULL) {
            if (STREQ(fullscreen, "yes")) {
                def->data.desktop.fullscreen = 1;
            } else if (STREQ(fullscreen, "no")) {
                def->data.desktop.fullscreen = 0;
            } else {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("unknown fullscreen value '%s'"), fullscreen);
                VIR_FREE(fullscreen);
                goto error;
            }
            VIR_FREE(fullscreen);
        } else {
            def->data.desktop.fullscreen = 0;
        }

        def->data.desktop.display = virXMLPropString(node, "display");
    } else if (def->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE) {
        xmlNodePtr cur;
        char *port = virXMLPropString(node, "port");
        char *tlsPort;
        char *autoport;
        char *defaultMode;
        int defaultModeVal;

        if (port) {
            if (virStrToLong_i(port, NULL, 10, &def->data.spice.port) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot parse spice port %s"), port);
                VIR_FREE(port);
                goto error;
            }
            VIR_FREE(port);
        } else {
            def->data.spice.port = 0;
        }

        tlsPort = virXMLPropString(node, "tlsPort");
        if (tlsPort) {
            if (virStrToLong_i(tlsPort, NULL, 10, &def->data.spice.tlsPort) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot parse spice tlsPort %s"), tlsPort);
                VIR_FREE(tlsPort);
                goto error;
            }
            VIR_FREE(tlsPort);
        } else {
            def->data.spice.tlsPort = 0;
        }

        if ((autoport = virXMLPropString(node, "autoport")) != NULL) {
            if (STREQ(autoport, "yes"))
                def->data.spice.autoport = 1;
            VIR_FREE(autoport);
        }

        def->data.spice.defaultMode = VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_ANY;

        if ((defaultMode = virXMLPropString(node, "defaultMode")) != NULL) {
            if ((defaultModeVal = virDomainGraphicsSpiceChannelModeTypeFromString(defaultMode)) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("unknown default spice channel mode %s"),
                               defaultMode);
                VIR_FREE(defaultMode);
                goto error;
            }
            def->data.spice.defaultMode = defaultModeVal;
            VIR_FREE(defaultMode);
        }

        if (def->data.spice.port == -1 && def->data.spice.tlsPort == -1) {
            /* Legacy compat syntax, used -1 for auto-port */
            def->data.spice.autoport = 1;
        }

        if (def->data.spice.autoport && (flags & VIR_DOMAIN_XML_INACTIVE)) {
            def->data.spice.port = 0;
            def->data.spice.tlsPort = 0;
        }

        def->data.spice.keymap = virXMLPropString(node, "keymap");

        if (virDomainGraphicsAuthDefParseXML(node, &def->data.spice.auth,
                                             def->type) < 0)
            goto error;

        cur = node->children;
        while (cur != NULL) {
            if (cur->type == XML_ELEMENT_NODE) {
                if (xmlStrEqual(cur->name, BAD_CAST "channel")) {
                    const char *name, *mode;
                    int nameval, modeval;
                    name = virXMLPropString(cur, "name");
                    mode = virXMLPropString(cur, "mode");

                    if (!name || !mode) {
                        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                       _("spice channel missing name/mode"));
                        VIR_FREE(name);
                        VIR_FREE(mode);
                        goto error;
                    }

                    if ((nameval = virDomainGraphicsSpiceChannelNameTypeFromString(name)) < 0) {
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                       _("unknown spice channel name %s"),
                                       name);
                        VIR_FREE(name);
                        VIR_FREE(mode);
                        goto error;
                    }
                    if ((modeval = virDomainGraphicsSpiceChannelModeTypeFromString(mode)) < 0) {
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                       _("unknown spice channel mode %s"),
                                       mode);
                        VIR_FREE(name);
                        VIR_FREE(mode);
                        goto error;
                    }
                    VIR_FREE(name);
                    VIR_FREE(mode);

                    def->data.spice.channels[nameval] = modeval;
                } else if (xmlStrEqual(cur->name, BAD_CAST "image")) {
                    const char *compression = virXMLPropString(cur, "compression");
                    int compressionVal;

                    if (!compression) {
                        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                       _("spice image missing compression"));
                        goto error;
                    }

                    if ((compressionVal =
                         virDomainGraphicsSpiceImageCompressionTypeFromString(compression)) <= 0) {
                        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                       _("unknown spice image compression %s"),
                                       compression);
                        VIR_FREE(compression);
                        goto error;
                    }
                    VIR_FREE(compression);

                    def->data.spice.image = compressionVal;
                } else if (xmlStrEqual(cur->name, BAD_CAST "jpeg")) {
                    const char *compression = virXMLPropString(cur, "compression");
                    int compressionVal;

                    if (!compression) {
                        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                       _("spice jpeg missing compression"));
                        goto error;
                    }

                    if ((compressionVal =
                         virDomainGraphicsSpiceJpegCompressionTypeFromString(compression)) <= 0) {
                        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                       _("unknown spice jpeg compression %s"),
                                       compression);
                        VIR_FREE(compression);
                        goto error;
                    }
                    VIR_FREE(compression);

                    def->data.spice.jpeg = compressionVal;
                } else if (xmlStrEqual(cur->name, BAD_CAST "zlib")) {
                    const char *compression = virXMLPropString(cur, "compression");
                    int compressionVal;

                    if (!compression) {
                        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                       _("spice zlib missing compression"));
                        goto error;
                    }

                    if ((compressionVal =
                         virDomainGraphicsSpiceZlibCompressionTypeFromString(compression)) <= 0) {
                        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                       _("unknown spice zlib compression %s"),
                                       compression);
                        VIR_FREE(compression);
                        goto error;
                    }
                    VIR_FREE(compression);

                    def->data.spice.zlib = compressionVal;
                } else if (xmlStrEqual(cur->name, BAD_CAST "playback")) {
                    const char *compression = virXMLPropString(cur, "compression");
                    int compressionVal;

                    if (!compression) {
                        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                       _("spice playback missing compression"));
                        goto error;
                    }

                    if ((compressionVal =
                         virDomainGraphicsSpicePlaybackCompressionTypeFromString(compression)) <= 0) {
                        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                       _("unknown spice playback compression"));
                        VIR_FREE(compression);
                        goto error;

                    }
                    VIR_FREE(compression);

                    def->data.spice.playback = compressionVal;
                } else if (xmlStrEqual(cur->name, BAD_CAST "streaming")) {
                    const char *mode = virXMLPropString(cur, "mode");
                    int modeVal;

                    if (!mode) {
                        virReportError(VIR_ERR_XML_ERROR, "%s",
                                       _("spice streaming missing mode"));
                        goto error;
                    }
                    if ((modeVal =
                         virDomainGraphicsSpiceStreamingModeTypeFromString(mode)) <= 0) {
                        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                       _("unknown spice streaming mode"));
                        VIR_FREE(mode);
                        goto error;

                    }
                    VIR_FREE(mode);

                    def->data.spice.streaming = modeVal;
                } else if (xmlStrEqual(cur->name, BAD_CAST "clipboard")) {
                    const char *copypaste = virXMLPropString(cur, "copypaste");
                    int copypasteVal;

                    if (!copypaste) {
                        virReportError(VIR_ERR_XML_ERROR, "%s",
                                       _("spice clipboard missing copypaste"));
                        goto error;
                    }

                    if ((copypasteVal =
                         virDomainGraphicsSpiceClipboardCopypasteTypeFromString(copypaste)) <= 0) {
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                       _("unknown copypaste value '%s'"), copypaste);
                        VIR_FREE(copypaste);
                        goto error;
                    }
                    VIR_FREE(copypaste);

                    def->data.spice.copypaste = copypasteVal;
                } else if (xmlStrEqual(cur->name, BAD_CAST "mouse")) {
                    const char *mode = virXMLPropString(cur, "mode");
                    int modeVal;

                    if (!mode) {
                        virReportError(VIR_ERR_XML_ERROR, "%s",
                                       _("spice mouse missing mode"));
                        goto error;
                    }

                    if ((modeVal = virDomainGraphicsSpiceMouseModeTypeFromString(mode)) <= 0) {
                        virReportError(VIR_ERR_XML_ERROR,
                                       _("unknown mouse mode value '%s'"),
                                       mode);
                        VIR_FREE(mode);
                        goto error;
                    }
                    VIR_FREE(mode);

                    def->data.spice.mousemode = modeVal;
                }
            }
            cur = cur->next;
        }
    }

cleanup:
    VIR_FREE(type);
    VIR_FREE(listenNodes);
    VIR_FREE(listenAddr);

    ctxt->node = save;
    return def;

error:
    virDomainGraphicsDefFree(def);
    def = NULL;
    goto cleanup;
}


static virDomainSoundCodecDefPtr
virDomainSoundCodecDefParseXML(const xmlNodePtr node)
{
    char *type;
    virDomainSoundCodecDefPtr def;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    type = virXMLPropString(node, "type");
    if ((def->type = virDomainSoundCodecTypeFromString(type)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown codec type '%s'"), type);
        goto error;
    }

cleanup:
    VIR_FREE(type);

    return def;

error:
    virDomainSoundCodecDefFree(def);
    def = NULL;
    goto cleanup;
}


static virDomainSoundDefPtr
virDomainSoundDefParseXML(const xmlNodePtr node,
                          xmlXPathContextPtr ctxt,
                          unsigned int flags)
{
    char *model;
    virDomainSoundDefPtr def;
    xmlNodePtr save = ctxt->node;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    ctxt->node = node;

    model = virXMLPropString(node, "model");
    if ((def->model = virDomainSoundModelTypeFromString(model)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown sound model '%s'"), model);
        goto error;
    }

    if (def->model == VIR_DOMAIN_SOUND_MODEL_ICH6) {
        int ncodecs;
        xmlNodePtr *codecNodes = NULL;

        /* parse the <codec> subelements for sound models that support it */
        ncodecs = virXPathNodeSet("./codec", ctxt, &codecNodes);
        if (ncodecs < 0)
            goto error;

        if (ncodecs > 0) {
            int ii;

            if (VIR_ALLOC_N(def->codecs, ncodecs) < 0) {
                virReportOOMError();
                VIR_FREE(codecNodes);
                goto error;
            }

            for (ii = 0; ii < ncodecs; ii++) {
                virDomainSoundCodecDefPtr codec = virDomainSoundCodecDefParseXML(codecNodes[ii]);
                if (codec == NULL)
                    goto error;

                codec->cad = def->ncodecs; /* that will do for now */
                def->codecs[def->ncodecs++] = codec;
            }
            VIR_FREE(codecNodes);
        }
    }

    if (virDomainDeviceInfoParseXML(node, NULL, &def->info, flags) < 0)
        goto error;

cleanup:
    VIR_FREE(model);

    ctxt->node = save;
    return def;

error:
    virDomainSoundDefFree(def);
    def = NULL;
    goto cleanup;
}


static virDomainWatchdogDefPtr
virDomainWatchdogDefParseXML(const xmlNodePtr node,
                             unsigned int flags)
{

    char *model = NULL;
    char *action = NULL;
    virDomainWatchdogDefPtr def;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    model = virXMLPropString(node, "model");
    if (model == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("watchdog must contain model name"));
        goto error;
    }
    def->model = virDomainWatchdogModelTypeFromString(model);
    if (def->model < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown watchdog model '%s'"), model);
        goto error;
    }

    action = virXMLPropString(node, "action");
    if (action == NULL)
        def->action = VIR_DOMAIN_WATCHDOG_ACTION_RESET;
    else {
        def->action = virDomainWatchdogActionTypeFromString(action);
        if (def->action < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown watchdog action '%s'"), action);
            goto error;
        }
    }

    if (virDomainDeviceInfoParseXML(node, NULL, &def->info, flags) < 0)
        goto error;

cleanup:
    VIR_FREE(action);
    VIR_FREE(model);

    return def;

error:
    virDomainWatchdogDefFree(def);
    def = NULL;
    goto cleanup;
}


static virDomainRNGDefPtr
virDomainRNGDefParseXML(const xmlNodePtr node,
                        xmlXPathContextPtr ctxt,
                        unsigned int flags)
{
    const char *model = NULL;
    const char *backend = NULL;
    const char *type = NULL;
    virDomainRNGDefPtr def;
    xmlNodePtr save = ctxt->node;
    xmlNodePtr *backends = NULL;
    int nbackends;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    if (!(model = virXMLPropString(node, "model"))) {
        virReportError(VIR_ERR_XML_ERROR, "%s", _("missing RNG device model"));
        goto error;
    }

    if ((def->model = virDomainRNGModelTypeFromString(model)) < 0) {
        virReportError(VIR_ERR_XML_ERROR, _("unknown RNG model '%s'"), model);
        goto error;
    }

    ctxt->node = node;

    if (virXPathUInt("string(./rate/@bytes)", ctxt, &def->rate) < -1) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("invalid RNG rate bytes value"));
        goto error;
    }

    if (def->rate > 0 &&
        virXPathUInt("string(./rate/@period)", ctxt, &def->period) < -1) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("invalid RNG rate period value"));
        goto error;
    }

    if ((nbackends = virXPathNodeSet("./backend", ctxt, &backends)) < 0)
        goto error;

    if (nbackends != 1) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("only one RNG backend is supported"));
        goto error;
    }

    if (!(backend = virXMLPropString(backends[0], "model"))) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("missing RNG device backend model"));
        goto error;
    }

    if ((def->backend = virDomainRNGBackendTypeFromString(backend)) < 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("unknown RNG backend model '%s'"), backend);
        goto error;
    }

    switch ((enum virDomainRNGBackend) def->backend) {
    case VIR_DOMAIN_RNG_BACKEND_RANDOM:
        def->source.file = virXPathString("string(./backend)", ctxt);
        if (def->source.file &&
            STRNEQ(def->source.file, "/dev/random") &&
            STRNEQ(def->source.file, "/dev/hwrng")) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("file '%s' is not a supported random source"),
                           def->source.file);
            goto error;
        }
        break;

    case VIR_DOMAIN_RNG_BACKEND_EGD:
        if (!(type = virXMLPropString(backends[0], "type"))) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("missing EGD backend type"));
            goto error;
        }

        if (VIR_ALLOC(def->source.chardev) < 0) {
            virReportOOMError();
            goto error;
        }

        def->source.chardev->type = virDomainChrTypeFromString(type);
        if (def->source.chardev->type < 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("unknown backend type '%s' for egd"),
                           type);
            goto error;
        }

        if (virDomainChrSourceDefParseXML(def->source.chardev,
                                          backends[0]->children, flags,
                                          NULL, ctxt, NULL, 0) < 0)
            goto error;
        break;

    case VIR_DOMAIN_RNG_BACKEND_LAST:
        break;
    }

    if (virDomainDeviceInfoParseXML(node, NULL, &def->info, flags) < 0)
        goto error;

cleanup:
    VIR_FREE(model);
    VIR_FREE(backend);
    VIR_FREE(type);
    VIR_FREE(backends);
    ctxt->node = save;
    return def;

error:
    virDomainRNGDefFree(def);
    def = NULL;
    goto cleanup;
}


static virDomainMemballoonDefPtr
virDomainMemballoonDefParseXML(const xmlNodePtr node,
                               unsigned int flags)
{
    char *model;
    virDomainMemballoonDefPtr def;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    model = virXMLPropString(node, "model");
    if (model == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("balloon memory must contain model name"));
        goto error;
    }
    if ((def->model = virDomainMemballoonModelTypeFromString(model)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown memory balloon model '%s'"), model);
        goto error;
    }

    if (virDomainDeviceInfoParseXML(node, NULL, &def->info, flags) < 0)
        goto error;

cleanup:
    VIR_FREE(model);

    return def;

error:
    virDomainMemballoonDefFree(def);
    def = NULL;
    goto cleanup;
}

static virSysinfoDefPtr
virSysinfoParseXML(const xmlNodePtr node,
                  xmlXPathContextPtr ctxt)
{
    virSysinfoDefPtr def;
    char *type;

    if (!xmlStrEqual(node->name, BAD_CAST "sysinfo")) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("XML does not contain expected 'sysinfo' element"));
        return NULL;
    }

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    type = virXMLPropString(node, "type");
    if (type == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("sysinfo must contain a type attribute"));
        goto error;
    }
    if ((def->type = virSysinfoTypeFromString(type)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown sysinfo type '%s'"), type);
        goto error;
    }


    /* Extract BIOS related metadata */
    def->bios_vendor =
        virXPathString("string(bios/entry[@name='vendor'])", ctxt);
    def->bios_version =
        virXPathString("string(bios/entry[@name='version'])", ctxt);
    def->bios_date =
        virXPathString("string(bios/entry[@name='date'])", ctxt);
    def->bios_release =
        virXPathString("string(bios/entry[@name='release'])", ctxt);

    /* Extract system related metadata */
    def->system_manufacturer =
        virXPathString("string(system/entry[@name='manufacturer'])", ctxt);
    def->system_product =
        virXPathString("string(system/entry[@name='product'])", ctxt);
    def->system_version =
        virXPathString("string(system/entry[@name='version'])", ctxt);
    def->system_serial =
        virXPathString("string(system/entry[@name='serial'])", ctxt);
    def->system_uuid =
        virXPathString("string(system/entry[@name='uuid'])", ctxt);
    def->system_sku =
        virXPathString("string(system/entry[@name='sku'])", ctxt);
    def->system_family =
        virXPathString("string(system/entry[@name='family'])", ctxt);

cleanup:
    VIR_FREE(type);
    return def;

error:
    virSysinfoDefFree(def);
    def = NULL;
    goto cleanup;
}

int
virDomainVideoDefaultRAM(virDomainDefPtr def,
                         int type)
{
    switch (type) {
        /* Weird, QEMU defaults to 9 MB ??! */
    case VIR_DOMAIN_VIDEO_TYPE_VGA:
    case VIR_DOMAIN_VIDEO_TYPE_CIRRUS:
    case VIR_DOMAIN_VIDEO_TYPE_VMVGA:
        if (def->virtType == VIR_DOMAIN_VIRT_VBOX)
            return 8 * 1024;
        else if (def->virtType == VIR_DOMAIN_VIRT_VMWARE)
            return 4 * 1024;
        else
            return 9 * 1024;
        break;

    case VIR_DOMAIN_VIDEO_TYPE_XEN:
        /* Original Xen PVFB hardcoded to 4 MB */
        return 4 * 1024;

    case VIR_DOMAIN_VIDEO_TYPE_QXL:
        /* QEMU use 64M as the minimal video video memory for qxl device */
        return 64 * 1024;

    default:
        return 0;
    }
}


int
virDomainVideoDefaultType(virDomainDefPtr def)
{
    switch (def->virtType) {
    case VIR_DOMAIN_VIRT_TEST:
    case VIR_DOMAIN_VIRT_QEMU:
    case VIR_DOMAIN_VIRT_KQEMU:
    case VIR_DOMAIN_VIRT_KVM:
    case VIR_DOMAIN_VIRT_XEN:
        if (def->os.type &&
            (STREQ(def->os.type, "xen") ||
             STREQ(def->os.type, "linux")))
            return VIR_DOMAIN_VIDEO_TYPE_XEN;
        else if (def->os.arch == VIR_ARCH_PPC64)
            return VIR_DOMAIN_VIDEO_TYPE_VGA;
        else
            return VIR_DOMAIN_VIDEO_TYPE_CIRRUS;

    case VIR_DOMAIN_VIRT_VBOX:
        return VIR_DOMAIN_VIDEO_TYPE_VBOX;

    case VIR_DOMAIN_VIRT_VMWARE:
        return VIR_DOMAIN_VIDEO_TYPE_VMVGA;

    default:
        return -1;
    }
}

static virDomainVideoAccelDefPtr
virDomainVideoAccelDefParseXML(const xmlNodePtr node) {
    xmlNodePtr cur;
    virDomainVideoAccelDefPtr def;
    char *support3d = NULL;
    char *support2d = NULL;

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (!support3d && !support2d &&
                xmlStrEqual(cur->name, BAD_CAST "acceleration")) {
                support3d = virXMLPropString(cur, "accel3d");
                support2d = virXMLPropString(cur, "accel2d");
            }
        }
        cur = cur->next;
    }

    if (!support3d && !support2d)
        return NULL;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    if (support3d) {
        if (STREQ(support3d, "yes"))
            def->support3d = 1;
        else
            def->support3d = 0;
        VIR_FREE(support3d);
    }

    if (support2d) {
        if (STREQ(support2d, "yes"))
            def->support2d = 1;
        else
            def->support2d = 0;
        VIR_FREE(support2d);
    }

    return def;
}

static virDomainVideoDefPtr
virDomainVideoDefParseXML(const xmlNodePtr node,
                          virDomainDefPtr dom,
                          unsigned int flags)
{
    virDomainVideoDefPtr def;
    xmlNodePtr cur;
    char *type = NULL;
    char *heads = NULL;
    char *vram = NULL;
    char *ram = NULL;
    char *primary = NULL;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (!type && !vram && !ram && !heads &&
                xmlStrEqual(cur->name, BAD_CAST "model")) {
                type = virXMLPropString(cur, "type");
                ram = virXMLPropString(cur, "ram");
                vram = virXMLPropString(cur, "vram");
                heads = virXMLPropString(cur, "heads");

                if ((primary = virXMLPropString(cur, "primary")) != NULL) {
                    if (STREQ(primary, "yes"))
                        def->primary = 1;
                    VIR_FREE(primary);
                }

                def->accel = virDomainVideoAccelDefParseXML(cur);
            }
        }
        cur = cur->next;
    }

    if (type) {
        if ((def->type = virDomainVideoTypeFromString(type)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown video model '%s'"), type);
            goto error;
        }
    } else {
        if ((def->type = virDomainVideoDefaultType(dom)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("missing video model and cannot determine default"));
            goto error;
        }
    }

    if (ram) {
        if (def->type != VIR_DOMAIN_VIDEO_TYPE_QXL) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("ram attribute only supported for type of qxl"));
            goto error;
        }
        if (virStrToLong_ui(ram, NULL, 10, &def->ram) < 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("cannot parse video ram '%s'"), ram);
            goto error;
        }
    } else if (def->type == VIR_DOMAIN_VIDEO_TYPE_QXL) {
        def->ram = virDomainVideoDefaultRAM(dom, def->type);
    }

    if (vram) {
        if (virStrToLong_ui(vram, NULL, 10, &def->vram) < 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("cannot parse video ram '%s'"), vram);
            goto error;
        }
    } else {
        def->vram = virDomainVideoDefaultRAM(dom, def->type);
    }

    if (heads) {
        if (virStrToLong_ui(heads, NULL, 10, &def->heads) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot parse video heads '%s'"), heads);
            goto error;
        }
    } else {
        def->heads = 1;
    }

    if (virDomainDeviceInfoParseXML(node, NULL, &def->info, flags) < 0)
        goto error;

    VIR_FREE(type);
    VIR_FREE(ram);
    VIR_FREE(vram);
    VIR_FREE(heads);

    return def;

error:
    virDomainVideoDefFree(def);
    VIR_FREE(type);
    VIR_FREE(ram);
    VIR_FREE(vram);
    VIR_FREE(heads);
    return NULL;
}

static virDomainHostdevDefPtr
virDomainHostdevDefParseXML(const xmlNodePtr node,
                            xmlXPathContextPtr ctxt,
                            virBitmapPtr bootMap,
                            unsigned int flags)
{
    virDomainHostdevDefPtr def;
    xmlNodePtr save = ctxt->node;
    char *mode = virXMLPropString(node, "mode");
    char *type = virXMLPropString(node, "type");

    ctxt->node = node;

    if (!(def = virDomainHostdevDefAlloc()))
        goto error;

    if (mode) {
        if ((def->mode = virDomainHostdevModeTypeFromString(mode)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown hostdev mode '%s'"), mode);
            goto error;
        }
    } else {
        def->mode = VIR_DOMAIN_HOSTDEV_MODE_SUBSYS;
    }

    switch (def->mode) {
    case VIR_DOMAIN_HOSTDEV_MODE_SUBSYS:
        /* parse managed/mode/type, and the <source> element */
        if (virDomainHostdevDefParseXMLSubsys(node, ctxt, type, def, flags) < 0)
            goto error;
        break;
    case VIR_DOMAIN_HOSTDEV_MODE_CAPABILITIES:
        /* parse managed/mode/type, and the <source> element */
        if (virDomainHostdevDefParseXMLCaps(node, ctxt, type, def) < 0)
            goto error;
        break;
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unexpected hostdev mode %d"), def->mode);
        goto error;
    }

    if (def->info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
        if (virDomainDeviceInfoParseXML(node, bootMap, def->info,
                                        flags  | VIR_DOMAIN_XML_INTERNAL_ALLOW_BOOT
                                        | VIR_DOMAIN_XML_INTERNAL_ALLOW_ROM) < 0)
            goto error;
    }

    if (def->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS) {
        switch (def->source.subsys.type) {
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI:
            if (def->info->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
                def->info->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("PCI host devices must use 'pci' address type"));
                goto error;
            }
            break;
        }
    }

cleanup:
    VIR_FREE(type);
    VIR_FREE(mode);
    ctxt->node = save;
    return def;

error:
    virDomainHostdevDefFree(def);
    def = NULL;
    goto cleanup;
}


static virDomainRedirdevDefPtr
virDomainRedirdevDefParseXML(const xmlNodePtr node,
                             virBitmapPtr bootMap,
                             unsigned int flags)
{
    xmlNodePtr cur;
    virDomainRedirdevDefPtr def;
    char *bus, *type = NULL;
    int remaining;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    bus = virXMLPropString(node, "bus");
    if (bus) {
        if ((def->bus = virDomainRedirdevBusTypeFromString(bus)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown redirdev bus '%s'"), bus);
            goto error;
        }
    } else {
        def->bus = VIR_DOMAIN_REDIRDEV_BUS_USB;
    }

    type = virXMLPropString(node, "type");
    if (type) {
        if ((def->source.chr.type = virDomainChrTypeFromString(type)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown redirdev character device type '%s'"), type);
            goto error;
        }
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing type in redirdev"));
        goto error;
    }

    cur = node->children;
    /* boot gets parsed in virDomainDeviceInfoParseXML
     * source gets parsed in virDomainChrSourceDefParseXML
     * we don't know any of the elements that might remain */
    remaining = virDomainChrSourceDefParseXML(&def->source.chr, cur, flags,
                                              NULL, NULL, NULL, 0);
    if (remaining < 0)
        goto error;

    if (def->source.chr.type == VIR_DOMAIN_CHR_TYPE_SPICEVMC) {
        def->source.chr.data.spicevmc = VIR_DOMAIN_CHR_SPICEVMC_USBREDIR;
    }

    if (virDomainDeviceInfoParseXML(node, bootMap, &def->info,
                                    flags | VIR_DOMAIN_XML_INTERNAL_ALLOW_BOOT) < 0)
        goto error;

    if (def->bus == VIR_DOMAIN_REDIRDEV_BUS_USB &&
        def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
        def->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("Invalid address for a USB device"));
        goto error;
    }


cleanup:
    VIR_FREE(bus);
    VIR_FREE(type);
    return def;

error:
    virDomainRedirdevDefFree(def);
    def = NULL;
    goto cleanup;
}

/*
 * This is the helper function to convert USB version from a
 * format of JJ.MN to a format of 0xJJMN where JJ is the major
 * version number, M is the minor version number and N is the
 * sub minor version number.
 * e.g. USB 2.0 is reported as 0x0200,
 *      USB 1.1 as 0x0110 and USB 1.0 as 0x0100.
 */
static int
virDomainRedirFilterUsbVersionHelper(const char *version,
                                     virDomainRedirFilterUsbDevDefPtr def)
{
    char *version_copy = NULL;
    char *temp = NULL;
    int ret = -1;
    size_t len;
    size_t fraction_len;
    unsigned int major;
    unsigned int minor;
    unsigned int hex;

    if (!(version_copy = strdup(version))) {
        virReportOOMError();
        return -1;
    }

    len = strlen(version_copy);
    /*
     * The valid format of version is like 01.10, 1.10, 1.1, etc.
     */
    if (len > 5 ||
        !(temp = strchr(version_copy, '.')) ||
        temp - version_copy < 1 ||
        temp - version_copy > 2 ||
        !(fraction_len = strlen(temp + 1)) ||
        fraction_len > 2) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Incorrect USB version format %s"), version);
        goto cleanup;
    }

    *temp = '\0';
    temp++;

    if ((virStrToLong_ui(version_copy, NULL, 0, &major)) < 0 ||
        (virStrToLong_ui(temp, NULL, 0, &minor)) < 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("Cannot parse USB version %s"), version);
        goto cleanup;
    }

    hex = (major / 10) << 12 | (major % 10) << 8;
    if (fraction_len == 1)
        hex |= (minor % 10) << 4;
    else
        hex |= (minor / 10) << 4 | (minor % 10) << 0;

    def->version = hex;
    ret = 0;

cleanup:
    VIR_FREE(version_copy);
    return ret;
}

static virDomainRedirFilterUsbDevDefPtr
virDomainRedirFilterUsbDevDefParseXML(const xmlNodePtr node)
{
    char *class;
    char *vendor = NULL, *product = NULL;
    char *version = NULL, *allow = NULL;
    virDomainRedirFilterUsbDevDefPtr def;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    class = virXMLPropString(node, "class");
    if (class) {
        if ((virStrToLong_i(class, NULL, 0, &def->usbClass)) < 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("Cannot parse USB Class code %s"), class);
            goto error;
        }

        if (def->usbClass != -1 && def->usbClass &~ 0xFF) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Invalid USB Class code %s"), class);
            goto error;
        }
    } else {
        def->usbClass = -1;
    }

    vendor = virXMLPropString(node, "vendor");
    if (vendor) {
        if ((virStrToLong_i(vendor, NULL, 0, &def->vendor)) < 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("Cannot parse USB vendor ID %s"), vendor);
            goto error;
        }
    } else {
        def->vendor = -1;
    }

    product = virXMLPropString(node, "product");
    if (product) {
        if ((virStrToLong_i(product, NULL, 0, &def->product)) < 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("Cannot parse USB product ID %s"), product);
            goto error;
        }
    } else {
        def->product = -1;
    }

    version = virXMLPropString(node, "version");
    if (version) {
        if (STREQ(version, "-1"))
            def->version = -1;
        else if ((virDomainRedirFilterUsbVersionHelper(version, def)) < 0)
            goto error;
    } else {
        def->version = -1;
    }

    allow = virXMLPropString(node, "allow");
    if (allow) {
        if (STREQ(allow, "yes"))
            def->allow = 1;
        else if (STREQ(allow, "no"))
            def->allow = 0;
        else {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Invalid allow value, either 'yes' or 'no'"));
            goto error;
        }
    } else {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("Missing allow attribute for USB redirection filter"));
        goto error;
    }

cleanup:
    VIR_FREE(class);
    VIR_FREE(vendor);
    VIR_FREE(product);
    VIR_FREE(version);
    VIR_FREE(allow);
    return def;

error:
    VIR_FREE(def);
    def = NULL;
    goto cleanup;
}

static virDomainRedirFilterDefPtr
virDomainRedirFilterDefParseXML(const xmlNodePtr node,
                                xmlXPathContextPtr ctxt)
{
    int n;
    size_t i;
    xmlNodePtr *nodes = NULL;
    xmlNodePtr save = ctxt->node;
    virDomainRedirFilterDefPtr def = NULL;

    if (VIR_ALLOC(def) < 0)
        goto no_memory;

    ctxt->node = node;
    if ((n = virXPathNodeSet("./usbdev", ctxt, &nodes)) < 0) {
        goto error;
    }

    if (n && VIR_ALLOC_N(def->usbdevs, n) < 0)
        goto no_memory;

    for (i = 0; i < n; i++) {
        virDomainRedirFilterUsbDevDefPtr usbdev =
            virDomainRedirFilterUsbDevDefParseXML(nodes[i]);

        if (!usbdev)
            goto error;
        def->usbdevs[def->nusbdevs++] = usbdev;
    }
    VIR_FREE(nodes);

    ctxt->node = save;
    return def;

no_memory:
    virReportOOMError();

error:
    VIR_FREE(nodes);
    virDomainRedirFilterDefFree(def);
    return NULL;
}

static int
virDomainEventActionParseXML(xmlXPathContextPtr ctxt,
                             const char *name,
                             const char *xpath,
                             int *val,
                             int defaultVal,
                             virEventActionFromStringFunc convFunc)
{
    char *tmp = virXPathString(xpath, ctxt);
    if (tmp == NULL) {
        *val = defaultVal;
    } else {
        *val = convFunc(tmp);
        if (*val < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown %s action: %s"), name, tmp);
            VIR_FREE(tmp);
            return -1;
        }
        VIR_FREE(tmp);
    }
    return 0;
}

static int
virDomainPMStateParseXML(xmlXPathContextPtr ctxt,
                         const char *xpath,
                         int *val)
{
    int ret = -1;
    char *tmp = virXPathString(xpath, ctxt);
    if (tmp) {
        *val = virDomainPMStateTypeFromString(tmp);
        if (*val < 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unknown PM state value %s"), tmp);
            goto cleanup;
        }
    }

    ret = 0;
 cleanup:
    VIR_FREE(tmp);
    return ret;
}

virDomainDeviceDefPtr
virDomainDeviceDefParse(const char *xmlStr,
                        virDomainDefPtr def,
                        virCapsPtr caps,
                        virDomainXMLOptionPtr xmlopt,
                        unsigned int flags)
{
    xmlDocPtr xml;
    xmlNodePtr node;
    xmlXPathContextPtr ctxt = NULL;
    virDomainDeviceDefPtr dev = NULL;

    if (!(xml = virXMLParseStringCtxt(xmlStr, _("(device_definition)"), &ctxt)))
        goto error;

    node = ctxt->node;

    if (VIR_ALLOC(dev) < 0) {
        virReportOOMError();
        goto error;
    }

    if (xmlStrEqual(node->name, BAD_CAST "disk")) {
        dev->type = VIR_DOMAIN_DEVICE_DISK;
        if (!(dev->data.disk = virDomainDiskDefParseXML(xmlopt, node, ctxt,
                                                        NULL, def->seclabels,
                                                        def->nseclabels,
                                                        flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "lease")) {
        dev->type = VIR_DOMAIN_DEVICE_LEASE;
        if (!(dev->data.lease = virDomainLeaseDefParseXML(node)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "filesystem")) {
        dev->type = VIR_DOMAIN_DEVICE_FS;
        if (!(dev->data.fs = virDomainFSDefParseXML(node, ctxt, flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "interface")) {
        dev->type = VIR_DOMAIN_DEVICE_NET;
        if (!(dev->data.net = virDomainNetDefParseXML(xmlopt, node, ctxt,
                                                      NULL, flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "input")) {
        dev->type = VIR_DOMAIN_DEVICE_INPUT;
        if (!(dev->data.input = virDomainInputDefParseXML(def->os.type,
                                                          node, flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "sound")) {
        dev->type = VIR_DOMAIN_DEVICE_SOUND;
        if (!(dev->data.sound = virDomainSoundDefParseXML(node, ctxt, flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "watchdog")) {
        dev->type = VIR_DOMAIN_DEVICE_WATCHDOG;
        if (!(dev->data.watchdog = virDomainWatchdogDefParseXML(node, flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "video")) {
        dev->type = VIR_DOMAIN_DEVICE_VIDEO;
        if (!(dev->data.video = virDomainVideoDefParseXML(node, def, flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "hostdev")) {
        dev->type = VIR_DOMAIN_DEVICE_HOSTDEV;
        if (!(dev->data.hostdev = virDomainHostdevDefParseXML(node, ctxt, NULL,
                                                              flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "controller")) {
        dev->type = VIR_DOMAIN_DEVICE_CONTROLLER;
        if (!(dev->data.controller = virDomainControllerDefParseXML(node, flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "graphics")) {
        dev->type = VIR_DOMAIN_DEVICE_GRAPHICS;
        if (!(dev->data.graphics = virDomainGraphicsDefParseXML(node, ctxt, flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "hub")) {
        dev->type = VIR_DOMAIN_DEVICE_HUB;
        if (!(dev->data.hub = virDomainHubDefParseXML(node, flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "redirdev")) {
        dev->type = VIR_DOMAIN_DEVICE_REDIRDEV;
        if (!(dev->data.redirdev = virDomainRedirdevDefParseXML(node, NULL, flags)))
            goto error;
    } else if (xmlStrEqual(node->name, BAD_CAST "rng")) {
        dev->type = VIR_DOMAIN_DEVICE_RNG;
        if (!(dev->data.rng = virDomainRNGDefParseXML(node, ctxt, flags)))
            goto error;
    } else {
        virReportError(VIR_ERR_XML_ERROR, "%s", _("unknown device type"));
        goto error;
    }

    /* callback to fill driver specific device aspects */
    if (virDomainDeviceDefPostParse(dev, def,  caps, xmlopt) < 0)
        goto error;

cleanup:
    xmlFreeDoc(xml);
    xmlXPathFreeContext(ctxt);
    return dev;

error:
    VIR_FREE(dev);
    goto cleanup;
}


static const char *
virDomainChrTargetTypeToString(int deviceType,
                               int targetType)
{
    const char *type = NULL;

    switch (deviceType) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
        type = virDomainChrChannelTargetTypeToString(targetType);
        break;
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE:
        type = virDomainChrConsoleTargetTypeToString(targetType);
        break;
    case VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL:
        type = virDomainChrSerialTargetTypeToString(targetType);
        break;
    default:
        break;
    }

    return type;
}

int
virDomainHostdevInsert(virDomainDefPtr def, virDomainHostdevDefPtr hostdev)
{
    if (VIR_REALLOC_N(def->hostdevs, def->nhostdevs + 1) < 0)
        return -1;
    def->hostdevs[def->nhostdevs++]  = hostdev;
    return 0;
}

virDomainHostdevDefPtr
virDomainHostdevRemove(virDomainDefPtr def, size_t i)
{
    virDomainHostdevDefPtr hostdev = def->hostdevs[i];

    if (def->nhostdevs > 1) {
        memmove(def->hostdevs + i,
                def->hostdevs + i + 1,
                sizeof(*def->hostdevs) *
                (def->nhostdevs - (i + 1)));
        def->nhostdevs--;
        if (VIR_REALLOC_N(def->hostdevs, def->nhostdevs) < 0) {
            /* ignore, harmless */
        }
    } else {
        VIR_FREE(def->hostdevs);
        def->nhostdevs = 0;
    }
    return hostdev;
}


static int
virDomainHostdevMatchSubsysUSB(virDomainHostdevDefPtr a,
                               virDomainHostdevDefPtr b)
{
    if (a->source.subsys.u.usb.bus && a->source.subsys.u.usb.device) {
        /* specified by bus location on host */
        if (a->source.subsys.u.usb.bus == b->source.subsys.u.usb.bus &&
            a->source.subsys.u.usb.device == b->source.subsys.u.usb.device)
            return 1;
    } else {
        /* specified by product & vendor id */
        if (a->source.subsys.u.usb.product == b->source.subsys.u.usb.product &&
            a->source.subsys.u.usb.vendor == b->source.subsys.u.usb.vendor)
            return 1;
    }
    return 0;
}

static int
virDomainHostdevMatchSubsysPCI(virDomainHostdevDefPtr a,
                               virDomainHostdevDefPtr b)
{
    if (a->source.subsys.u.pci.domain == b->source.subsys.u.pci.domain &&
        a->source.subsys.u.pci.bus == b->source.subsys.u.pci.bus &&
        a->source.subsys.u.pci.slot == b->source.subsys.u.pci.slot &&
        a->source.subsys.u.pci.function == b->source.subsys.u.pci.function)
        return 1;
    return 0;
}


static int
virDomainHostdevMatchSubsys(virDomainHostdevDefPtr a,
                            virDomainHostdevDefPtr b)
{
    if (a->source.subsys.type != b->source.subsys.type)
        return 0;

    switch (a->source.subsys.type) {
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI:
        return virDomainHostdevMatchSubsysPCI(a, b);
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB:
        return virDomainHostdevMatchSubsysUSB(a, b);
    }
    return 0;
}


static int
virDomainHostdevMatchCapsStorage(virDomainHostdevDefPtr a,
                                 virDomainHostdevDefPtr b)
{
    return STREQ_NULLABLE(a->source.caps.u.storage.block,
                          b->source.caps.u.storage.block);
}


static int
virDomainHostdevMatchCapsMisc(virDomainHostdevDefPtr a,
                              virDomainHostdevDefPtr b)
{
    return STREQ_NULLABLE(a->source.caps.u.misc.chardev,
                          b->source.caps.u.misc.chardev);
}

static int
virDomainHostdevMatchCapsNet(virDomainHostdevDefPtr a,
                              virDomainHostdevDefPtr b)
{
    return STREQ_NULLABLE(a->source.caps.u.net.iface,
                          b->source.caps.u.net.iface);
}


static int
virDomainHostdevMatchCaps(virDomainHostdevDefPtr a,
                          virDomainHostdevDefPtr b)
{
    if (a->source.caps.type != b->source.caps.type)
        return 0;

    switch (a->source.caps.type) {
    case VIR_DOMAIN_HOSTDEV_CAPS_TYPE_STORAGE:
        return virDomainHostdevMatchCapsStorage(a, b);
    case VIR_DOMAIN_HOSTDEV_CAPS_TYPE_MISC:
        return virDomainHostdevMatchCapsMisc(a, b);
    case VIR_DOMAIN_HOSTDEV_CAPS_TYPE_NET:
        return virDomainHostdevMatchCapsNet(a, b);
    }
    return 0;
}


static int
virDomainHostdevMatch(virDomainHostdevDefPtr a,
                      virDomainHostdevDefPtr b)
{
    if (a->mode != b->mode)
        return 0;

    switch (a->mode) {
    case VIR_DOMAIN_HOSTDEV_MODE_SUBSYS:
        return virDomainHostdevMatchSubsys(a, b);
    case VIR_DOMAIN_HOSTDEV_MODE_CAPABILITIES:
        return virDomainHostdevMatchCaps(a, b);
    }
    return 0;
}

/* Find an entry in hostdevs that matches the source spec in
 * @match. return pointer to the entry in @found (if found is
 * non-NULL). Returns index (within hostdevs) of matched entry, or -1
 * if no match was found.
 */
int
virDomainHostdevFind(virDomainDefPtr def,
                     virDomainHostdevDefPtr match,
                     virDomainHostdevDefPtr *found)
{
    virDomainHostdevDefPtr local_found;
    int i;

    if (!found)
        found = &local_found;
    *found = NULL;

    for (i = 0 ; i < def->nhostdevs ; i++) {
        if (virDomainHostdevMatch(match, def->hostdevs[i])) {
            *found = def->hostdevs[i];
            break;
        }
    }
    return *found ? i : -1;
}

int
virDomainDiskIndexByName(virDomainDefPtr def, const char *name,
                         bool allow_ambiguous)
{
    virDomainDiskDefPtr vdisk;
    int i;
    int candidate = -1;

    /* We prefer the <target dev='name'/> name (it's shorter, required
     * for all disks, and should be unambiguous), but also support
     * <source file='name'/> (if unambiguous).  Assume dst if there is
     * no leading slash, source name otherwise.  */
    for (i = 0; i < def->ndisks; i++) {
        vdisk = def->disks[i];
        if (*name != '/') {
            if (STREQ(vdisk->dst, name))
                return i;
        } else if (vdisk->src &&
                   STREQ(vdisk->src, name)) {
            if (allow_ambiguous)
                return i;
            if (candidate >= 0)
                return -1;
            candidate = i;
        }
    }
    return candidate;
}

/* Return the path to a disk image if a string identifies at least one
 * disk belonging to the domain (both device strings 'vda' and paths
 * '/path/to/file' are converted into '/path/to/file').  */
const char *
virDomainDiskPathByName(virDomainDefPtr def, const char *name)
{
    int i = virDomainDiskIndexByName(def, name, true);

    return i < 0 ? NULL : def->disks[i]->src;
}

int virDomainDiskInsert(virDomainDefPtr def,
                        virDomainDiskDefPtr disk)
{

    if (VIR_REALLOC_N(def->disks, def->ndisks+1) < 0)
        return -1;

    virDomainDiskInsertPreAlloced(def, disk);

    return 0;
}

void virDomainDiskInsertPreAlloced(virDomainDefPtr def,
                                   virDomainDiskDefPtr disk)
{
    int i;
    /* Tenatively plan to insert disk at the end. */
    int insertAt = -1;

    /* Then work backwards looking for disks on
     * the same bus. If we find a disk with a drive
     * index greater than the new one, insert at
     * that position
     */
    for (i = (def->ndisks - 1) ; i >= 0 ; i--) {
        /* If bus matches and current disk is after
         * new disk, then new disk should go here */
        if (def->disks[i]->bus == disk->bus &&
            (virDiskNameToIndex(def->disks[i]->dst) >
             virDiskNameToIndex(disk->dst))) {
            insertAt = i;
        } else if (def->disks[i]->bus == disk->bus &&
                   insertAt == -1) {
            /* Last disk with match bus is before the
             * new disk, then put new disk just after
             */
            insertAt = i + 1;
        }
    }

    /* No disks with this bus yet, so put at end of list */
    if (insertAt == -1)
        insertAt = def->ndisks;

    if (insertAt < def->ndisks)
        memmove(def->disks + insertAt + 1,
                def->disks + insertAt,
                (sizeof(def->disks[0]) * (def->ndisks-insertAt)));

    def->disks[insertAt] = disk;
    def->ndisks++;
}


virDomainDiskDefPtr
virDomainDiskRemove(virDomainDefPtr def, size_t i)
{
    virDomainDiskDefPtr disk = def->disks[i];

    if (def->ndisks > 1) {
        memmove(def->disks + i,
                def->disks + i + 1,
                sizeof(*def->disks) *
                (def->ndisks - (i + 1)));
        def->ndisks--;
        if (VIR_REALLOC_N(def->disks, def->ndisks) < 0) {
            /* ignore, harmless */
        }
    } else {
        VIR_FREE(def->disks);
        def->ndisks = 0;
    }
    return disk;
}

virDomainDiskDefPtr
virDomainDiskRemoveByName(virDomainDefPtr def, const char *name)
{
    int i = virDomainDiskIndexByName(def, name, false);
    if (i < 0)
        return NULL;
    return virDomainDiskRemove(def, i);
}

/* Return true if VM has at least one disk involved in a current block
 * copy job (that is, with a <mirror> element in the disk xml).  */
bool
virDomainHasDiskMirror(virDomainObjPtr vm)
{
    int i;
    for (i = 0; i < vm->def->ndisks; i++)
        if (vm->def->disks[i]->mirror)
            return true;
    return false;
}

int virDomainNetInsert(virDomainDefPtr def, virDomainNetDefPtr net)
{
    if (VIR_REALLOC_N(def->nets, def->nnets + 1) < 0)
        return -1;
    def->nets[def->nnets]  = net;
    def->nnets++;
    if (net->type == VIR_DOMAIN_NET_TYPE_HOSTDEV) {
        /* hostdev net devices must also exist in the hostdevs array */
        return virDomainHostdevInsert(def, &net->data.hostdev.def);
    }
    return 0;
}

/* virDomainNetFindIdx: search according to mac address and guest side
 *                      PCI address (if specified)
 *
 * Return: index of match if unique match found
 *         -1 if not found
 *         -2 if multiple matches
 */
int
virDomainNetFindIdx(virDomainDefPtr def, virDomainNetDefPtr net)
{
    int ii, matchidx = -1;
    bool PCIAddrSpecified = virDomainDeviceAddressIsValid(&net->info,
                                                          VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI);

    for (ii = 0 ; ii < def->nnets ; ii++) {
        if (virMacAddrCmp(&def->nets[ii]->mac, &net->mac))
            continue;

        if ((matchidx >= 0) && !PCIAddrSpecified) {
            /* there were multiple matches on mac address, and no
             * qualifying guest-side PCI address was given, so we must
             * fail (NB: a USB address isn't adequate, since it may
             * specify only vendor and product ID, and there may be
             * multiples of those.
             */
            matchidx = -2; /* indicates "multiple matches" to caller */
            break;
        }
        if (PCIAddrSpecified) {
            if (virDevicePCIAddressEqual(&def->nets[ii]->info.addr.pci,
                                         &net->info.addr.pci)) {
                /* exit early if the pci address was specified and
                 * it matches, as this guarantees no duplicates.
                 */
                matchidx = ii;
                break;
            }
        } else {
            /* no PCI address given, so there may be multiple matches */
            matchidx = ii;
        }
    }
    return matchidx;
}

virDomainNetDefPtr
virDomainNetRemove(virDomainDefPtr def, size_t i)
{
    virDomainNetDefPtr net = def->nets[i];

    if (net->type == VIR_DOMAIN_NET_TYPE_HOSTDEV) {
        /* hostdev net devices are normally also be in the hostdevs
         * array, but might have already been removed by the time we
         * get here.
         */
        virDomainHostdevDefPtr hostdev = &net->data.hostdev.def;
        size_t h;

        for (h = 0; h < def->nhostdevs; h++) {
            if (def->hostdevs[h] == hostdev) {
                virDomainHostdevRemove(def, h);
                break;
            }
        }
    }
    if (def->nnets > 1) {
        memmove(def->nets + i,
                def->nets + i + 1,
                sizeof(*def->nets) * (def->nnets - (i + 1)));
        def->nnets--;
        if (VIR_REALLOC_N(def->nets, def->nnets) < 0) {
            /* ignore harmless */
        }
    } else {
        VIR_FREE(def->nets);
        def->nnets = 0;
    }
    return net;
}

int virDomainControllerInsert(virDomainDefPtr def,
                              virDomainControllerDefPtr controller)
{

    if (VIR_REALLOC_N(def->controllers, def->ncontrollers+1) < 0)
        return -1;

    virDomainControllerInsertPreAlloced(def, controller);

    return 0;
}

void virDomainControllerInsertPreAlloced(virDomainDefPtr def,
                                         virDomainControllerDefPtr controller)
{
    int i;
    /* Tenatively plan to insert controller at the end. */
    int insertAt = -1;

    /* Then work backwards looking for controllers of
     * the same type. If we find a controller with a
     * index greater than the new one, insert at
     * that position
     */
    for (i = (def->ncontrollers - 1) ; i >= 0 ; i--) {
        /* If bus matches and current controller is after
         * new controller, then new controller should go here */
        if (def->controllers[i]->type == controller->type &&
            def->controllers[i]->idx > controller->idx) {
            insertAt = i;
        } else if (def->controllers[i]->type == controller->type &&
                   insertAt == -1) {
            /* Last controller with match bus is before the
             * new controller, then put new controller just after
             */
            insertAt = i + 1;
        }
    }

    /* No controllers with this bus yet, so put at end of list */
    if (insertAt == -1)
        insertAt = def->ncontrollers;

    if (insertAt < def->ncontrollers)
        memmove(def->controllers + insertAt + 1,
                def->controllers + insertAt,
                (sizeof(def->controllers[0]) * (def->ncontrollers-insertAt)));

    def->controllers[insertAt] = controller;
    def->ncontrollers++;
}

int
virDomainControllerFind(virDomainDefPtr def,
                        int type, int idx)
{
    int i;

    for (i = 0 ; i < def->ncontrollers ; i++) {
        if ((def->controllers[i]->type == type) &&
            (def->controllers[i]->idx == idx)) {
            return i;
        }
    }

    return -1;
}

virDomainControllerDefPtr
virDomainControllerRemove(virDomainDefPtr def, size_t i)
{
    virDomainControllerDefPtr controller = def->controllers[i];

    if (def->ncontrollers > 1) {
        memmove(def->controllers + i,
                def->controllers + i + 1,
                sizeof(*def->controllers) *
                (def->ncontrollers - (i + 1)));
        def->ncontrollers--;
        if (VIR_REALLOC_N(def->controllers, def->ncontrollers) < 0) {
            /* ignore, harmless */
        }
    } else {
        VIR_FREE(def->controllers);
        def->ncontrollers = 0;
    }

    return controller;
}

int virDomainLeaseIndex(virDomainDefPtr def,
                        virDomainLeaseDefPtr lease)
{
    virDomainLeaseDefPtr vlease;
    int i;

    for (i = 0; i < def->nleases; i++) {
        vlease = def->leases[i];
        /* Either both must have lockspaces present which  match.. */
        if (vlease->lockspace && lease->lockspace &&
            STRNEQ(vlease->lockspace, lease->lockspace))
            continue;
        /* ...or neither must have a lockspace present */
        if (vlease->lockspace || lease->lockspace)
            continue;
        if (STREQ(vlease->key, lease->key))
            return i;
    }
    return -1;
}


int virDomainLeaseInsertPreAlloc(virDomainDefPtr def)
{
    if (VIR_EXPAND_N(def->leases, def->nleases, 1) < 0) {
        virReportOOMError();
        return -1;
    }
    return 0;
}

int virDomainLeaseInsert(virDomainDefPtr def,
                         virDomainLeaseDefPtr lease)
{
    if (virDomainLeaseInsertPreAlloc(def) < 0)
        return -1;

    virDomainLeaseInsertPreAlloced(def, lease);
    return 0;
}


void virDomainLeaseInsertPreAlloced(virDomainDefPtr def,
                                    virDomainLeaseDefPtr lease)
{
    if (lease == NULL)
        VIR_SHRINK_N(def->leases, def->nleases, 1);
    else
        def->leases[def->nleases-1] = lease;
}


virDomainLeaseDefPtr
virDomainLeaseRemoveAt(virDomainDefPtr def, size_t i)
{

    virDomainLeaseDefPtr lease = def->leases[i];

    if (def->nleases > 1) {
        memmove(def->leases + i,
                def->leases + i + 1,
                sizeof(*def->leases) *
                (def->nleases - (i + 1)));
        VIR_SHRINK_N(def->leases, def->nleases, 1);
    } else {
        VIR_FREE(def->leases);
        def->nleases = 0;
    }
    return lease;
}


virDomainLeaseDefPtr
virDomainLeaseRemove(virDomainDefPtr def,
                     virDomainLeaseDefPtr lease)
{
    int i = virDomainLeaseIndex(def, lease);
    if (i < 0)
        return NULL;
    return virDomainLeaseRemoveAt(def, i);
}


char *
virDomainDefGetDefaultEmulator(virDomainDefPtr def,
                               virCapsPtr caps)
{
    const char *type;
    const char *emulator;
    char *retemu;

    type = virDomainVirtTypeToString(def->virtType);
    if (!type) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("unknown virt type"));
        return NULL;
    }

    emulator = virCapabilitiesDefaultGuestEmulator(caps,
                                                   def->os.type,
                                                   def->os.arch,
                                                   type);

    if (!emulator) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("no emulator for domain %s os type %s "
                         "on architecture %s"),
                       type, def->os.type, virArchToString(def->os.arch));
        return NULL;
    }

    if (!(retemu = strdup(emulator)))
        virReportOOMError();

    return retemu;
}

static int
virDomainDefParseBootXML(xmlXPathContextPtr ctxt,
                         virDomainDefPtr def,
                         unsigned long *bootCount)
{
    xmlNodePtr *nodes = NULL;
    int i, n;
    char *tmp = NULL;
    int ret = -1;
    unsigned long deviceBoot, serialPorts;

    if (virXPathULong("count(./devices/disk[boot]"
                      "|./devices/interface[boot]"
                      "|./devices/hostdev[boot]"
                      "|./devices/redirdev[boot])", ctxt, &deviceBoot) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("cannot count boot devices"));
        goto cleanup;
    }

    /* analysis of the boot devices */
    if ((n = virXPathNodeSet("./os/boot", ctxt, &nodes)) < 0) {
        goto cleanup;
    }

    if (n > 0 && deviceBoot) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("per-device boot elements cannot be used"
                         " together with os/boot elements"));
        goto cleanup;
    }

    for (i = 0 ; i < n && i < VIR_DOMAIN_BOOT_LAST ; i++) {
        int val;
        char *dev = virXMLPropString(nodes[i], "dev");
        if (!dev) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("missing boot device"));
            goto cleanup;
        }
        if ((val = virDomainBootTypeFromString(dev)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown boot device '%s'"),
                           dev);
            VIR_FREE(dev);
            goto cleanup;
        }
        VIR_FREE(dev);
        def->os.bootDevs[def->os.nBootDevs++] = val;
    }
    if (def->os.nBootDevs == 0 && !deviceBoot) {
        def->os.nBootDevs = 1;
        def->os.bootDevs[0] = VIR_DOMAIN_BOOT_DISK;
    }

    tmp = virXPathString("string(./os/bootmenu[1]/@enable)", ctxt);
    if (tmp) {
        def->os.bootmenu = virDomainBootMenuTypeFromString(tmp);
        if (def->os.bootmenu <= 0) {
            /* In order not to break misconfigured machines, this
             * should not emit an error, but rather set the bootmenu
             * to disabled */
            VIR_WARN("disabling bootmenu due to unknown option '%s'",
                     tmp);
            def->os.bootmenu = VIR_DOMAIN_BOOT_MENU_DISABLED;
        }
        VIR_FREE(tmp);
    }

    tmp = virXPathString("string(./os/bios[1]/@useserial)", ctxt);
    if (tmp) {
        if (STREQ(tmp, "yes")) {
            if (virXPathULong("count(./devices/serial)",
                              ctxt, &serialPorts) < 0) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("need at least one serial port "
                                 "for useserial"));
                goto cleanup;
            }
            def->os.bios.useserial = VIR_DOMAIN_BIOS_USESERIAL_YES;
        } else {
            def->os.bios.useserial = VIR_DOMAIN_BIOS_USESERIAL_NO;
        }
        VIR_FREE(tmp);
    }

    tmp = virXPathString("string(./os/bios[1]/@rebootTimeout)", ctxt);
    if (tmp) {
        /* that was really just for the check if it is there */

        if (virStrToLong_i(tmp, NULL, 0, &def->os.bios.rt_delay) < 0 ||
            def->os.bios.rt_delay < -1 || def->os.bios.rt_delay > 65535) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("invalid value for rebootTimeout, "
                             "must be in range [-1,65535]"));
            goto cleanup;
        }
        def->os.bios.rt_set = true;
    }

    *bootCount = deviceBoot;
    ret = 0;

cleanup:
    VIR_FREE(tmp);
    VIR_FREE(nodes);
    return ret;
}

/* Parse the XML definition for a vcpupin or emulatorpin.
 *
 * vcpupin has the form of
 *
 *   <vcpupin vcpu='0' cpuset='0'/>
 *
 * and emulatorpin has the form of
 *
 *   <emulatorpin cpuset='0'/>
 *
 * A vcpuid of -1 is valid and only valid for emulatorpin. So callers
 * have to check the returned cpuid for validity.
 */
static virDomainVcpuPinDefPtr
virDomainVcpuPinDefParseXML(const xmlNodePtr node,
                            xmlXPathContextPtr ctxt,
                            int maxvcpus,
                            int emulator)
{
    virDomainVcpuPinDefPtr def;
    xmlNodePtr oldnode = ctxt->node;
    int vcpuid = -1;
    char *tmp = NULL;
    int ret;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    ctxt->node = node;

    if (emulator == 0) {
        ret = virXPathInt("string(./@vcpu)", ctxt, &vcpuid);
        if ((ret == -2) || (vcpuid < -1)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("vcpu id must be an unsigned integer or -1"));
            goto error;
        } else if (vcpuid == -1) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("vcpu id value -1 is not allowed for vcpupin"));
            goto error;
        }
    }

    if (vcpuid >= maxvcpus) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("vcpu id must be less than maxvcpus"));
        goto error;
    }

    def->vcpuid = vcpuid;

    tmp = virXMLPropString(node, "cpuset");

    if (tmp) {
        char *set = tmp;
        int cpumasklen = VIR_DOMAIN_CPUMASK_LEN;

        if (virBitmapParse(set, 0, &def->cpumask,
                           cpumasklen) < 0)
           goto error;
        VIR_FREE(tmp);
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing cpuset for vcpupin"));
        goto error;
    }

cleanup:
    ctxt->node = oldnode;
    return def;

error:
    VIR_FREE(def);
    goto cleanup;
}

/*
 * Return the vcpupin related with the vcpu id on SUCCESS, or
 * NULL on failure.
 */
virDomainVcpuPinDefPtr
virDomainLookupVcpuPin(virDomainDefPtr def,
                       int vcpuid)
{
    int i;

    if (!def->cputune.vcpupin)
        return NULL;

    for (i = 0; i < def->cputune.nvcpupin; i++) {
        if (def->cputune.vcpupin[i]->vcpuid == vcpuid)
            return def->cputune.vcpupin[i];
    }

    return NULL;
}

static int virDomainDefMaybeAddController(virDomainDefPtr def,
                                          int type,
                                          int idx)
{
    int found = 0;
    int i;
    virDomainControllerDefPtr cont;

    for (i = 0 ; (i < def->ncontrollers) && !found; i++) {
        if (def->controllers[i]->type == type &&
            def->controllers[i]->idx == idx)
            found = 1;
    }

    if (found)
        return 0;

    if (VIR_ALLOC(cont) < 0) {
        virReportOOMError();
        return -1;
    }

    cont->type = type;
    cont->idx = idx;
    cont->model = -1;

    if (cont->type == VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL) {
        cont->opts.vioserial.ports = -1;
        cont->opts.vioserial.vectors = -1;
    }


    if (VIR_REALLOC_N(def->controllers, def->ncontrollers+1) < 0) {
        VIR_FREE(cont);
        virReportOOMError();
        return -1;
    }
    def->controllers[def->ncontrollers] = cont;
    def->ncontrollers++;

    return 0;
}


/* Parse a memory element located at XPATH within CTXT, and store the
 * result into MEM.  If REQUIRED, then the value must exist;
 * otherwise, the value is optional.  The value is in blocks of 1024.
 * Return 0 on success, -1 on failure after issuing error.  */
static int
virDomainParseMemory(const char *xpath, xmlXPathContextPtr ctxt,
                     unsigned long long *mem, bool required)
{
    int ret = -1;
    unsigned long long bytes, max;

    /* On 32-bit machines, our bound is 0xffffffff * KiB. On 64-bit
     * machines, our bound is off_t (2^63).  */
    if (sizeof(unsigned long) < sizeof(long long))
        max = 1024ull * ULONG_MAX;
    else
        max = LLONG_MAX;

    ret = virDomainParseScaledValue(xpath, ctxt, &bytes, 1024, max, required);
    if (ret < 0)
        goto cleanup;

    /* Yes, we really do use kibibytes for our internal sizing.  */
    *mem = VIR_DIV_UP(bytes, 1024);
    ret = 0;
cleanup:
    return ret;
}


static virDomainDefPtr
virDomainDefParseXML(xmlDocPtr xml,
                     xmlNodePtr root,
                     xmlXPathContextPtr ctxt,
                     virCapsPtr caps,
                     virDomainXMLOptionPtr xmlopt,
                     unsigned int expectedVirtTypes,
                     unsigned int flags)
{
    xmlNodePtr *nodes = NULL, node = NULL;
    char *tmp = NULL;
    int i, n;
    long id = -1;
    virDomainDefPtr def;
    unsigned long count;
    bool uuid_generated = false;
    virBitmapPtr bootMap = NULL;
    unsigned long bootMapSize = 0;
    xmlNodePtr cur;
    bool usb_none = false;
    bool usb_other = false;
    bool primaryVideo = false;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError();
        return NULL;
    }

    if (!(flags & VIR_DOMAIN_XML_INACTIVE))
        if (virXPathLong("string(./@id)", ctxt, &id) < 0)
            id = -1;
    def->id = (int)id;

    /* Find out what type of virtualization to use */
    if (!(tmp = virXPathString("string(./@type)", ctxt))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing domain type attribute"));
        goto error;
    }

    if ((def->virtType = virDomainVirtTypeFromString(tmp)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("invalid domain type %s"), tmp);
        goto error;
    }
    VIR_FREE(tmp);

    if ((expectedVirtTypes & (1 << def->virtType)) == 0) {
        if (count_one_bits(expectedVirtTypes) == 1) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unexpected domain type %s, expecting %s"),
                           virDomainVirtTypeToString(def->virtType),
                           virDomainVirtTypeToString(ffs(expectedVirtTypes) - 1));
        } else {
            virBuffer buffer = VIR_BUFFER_INITIALIZER;
            char *string;

            for (i = 0; i < VIR_DOMAIN_VIRT_LAST; ++i) {
                if ((expectedVirtTypes & (1 << i)) != 0) {
                    if (virBufferUse(&buffer) > 0)
                        virBufferAddLit(&buffer, ", ");

                    virBufferAdd(&buffer, virDomainVirtTypeToString(i), -1);
                }
            }

            if (virBufferError(&buffer)) {
                virReportOOMError();
                virBufferFreeAndReset(&buffer);
                goto error;
            }

            string = virBufferContentAndReset(&buffer);

            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unexpected domain type %s, "
                             "expecting one of these: %s"),
                           virDomainVirtTypeToString(def->virtType),
                           string);

            VIR_FREE(string);
        }

        goto error;
    }

    /* Extract domain name */
    if (!(def->name = virXPathString("string(./name[1])", ctxt))) {
        virReportError(VIR_ERR_NO_NAME, NULL);
        goto error;
    }

    /* Extract domain uuid. If both uuid and sysinfo/system/entry/uuid
     * exist, they must match; and if only the latter exists, it can
     * also serve as the uuid. */
    tmp = virXPathString("string(./uuid[1])", ctxt);
    if (!tmp) {
        if (virUUIDGenerate(def->uuid)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("Failed to generate UUID"));
            goto error;
        }
        uuid_generated = true;
    } else {
        if (virUUIDParse(tmp, def->uuid) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("malformed uuid element"));
            goto error;
        }
        VIR_FREE(tmp);
    }

    /* Extract short description of domain (title) */
    def->title = virXPathString("string(./title[1])", ctxt);
    if (def->title && strchr(def->title, '\n')) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("Domain title can't contain newlines"));
        goto error;
    }

    /* Extract documentation if present */
    def->description = virXPathString("string(./description[1])", ctxt);

    /* analysis of security label, done early even though we format it
     * late, so devices can refer to this for defaults */
    if (virSecurityLabelDefsParseXML(def, ctxt, caps, flags) == -1)
        goto error;

    /* Extract domain memory */
    if (virDomainParseMemory("./memory[1]", ctxt,
                             &def->mem.max_balloon, true) < 0)
        goto error;

    if (virDomainParseMemory("./currentMemory[1]", ctxt,
                             &def->mem.cur_balloon, false) < 0)
        goto error;

    /* and info about it */
    if ((tmp = virXPathString("string(./memory[1]/@dumpCore)", ctxt)) &&
        (def->mem.dump_core = virDomainMemDumpTypeFromString(tmp)) <= 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("Invalid memory core dump attribute value '%s'"), tmp);
        goto error;
    }
    VIR_FREE(tmp);

    if (def->mem.cur_balloon > def->mem.max_balloon) {
        /* Older libvirt could get into this situation due to
         * rounding; if the discrepancy is less than 4MiB, we silently
         * round down, otherwise we flag the issue.  */
        if (VIR_DIV_UP(def->mem.cur_balloon, 4096) >
            VIR_DIV_UP(def->mem.max_balloon, 4096)) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("current memory '%lluk' exceeds "
                             "maximum '%lluk'"),
                           def->mem.cur_balloon, def->mem.max_balloon);
            goto error;
        } else {
            VIR_DEBUG("Truncating current %lluk to maximum %lluk",
                      def->mem.cur_balloon, def->mem.max_balloon);
            def->mem.cur_balloon = def->mem.max_balloon;
        }
    } else if (def->mem.cur_balloon == 0) {
        def->mem.cur_balloon = def->mem.max_balloon;
    }

    if ((node = virXPathNode("./memoryBacking/hugepages", ctxt)))
        def->mem.hugepage_backed = true;

    /* Extract blkio cgroup tunables */
    if (virXPathUInt("string(./blkiotune/weight)", ctxt,
                     &def->blkio.weight) < 0)
        def->blkio.weight = 0;

    if ((n = virXPathNodeSet("./blkiotune/device", ctxt, &nodes)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("cannot extract blkiotune nodes"));
        goto error;
    }
    if (n && VIR_ALLOC_N(def->blkio.devices, n) < 0)
        goto no_memory;

    for (i = 0; i < n; i++) {
        int j;
        if (virDomainBlkioDeviceWeightParseXML(nodes[i],
                                               &def->blkio.devices[i]) < 0)
            goto error;
        def->blkio.ndevices++;
        for (j = 0; j < i; j++) {
            if (STREQ(def->blkio.devices[j].path,
                      def->blkio.devices[i].path)) {
                virReportError(VIR_ERR_XML_ERROR,
                               _("duplicate device weight path '%s'"),
                               def->blkio.devices[i].path);
                goto error;
            }
        }
    }
    VIR_FREE(nodes);

    /* Extract other memory tunables */
    if (virDomainParseMemory("./memtune/hard_limit[1]", ctxt,
                             &def->mem.hard_limit, false) < 0)
        goto error;

    if (virDomainParseMemory("./memtune/soft_limit[1]", ctxt,
                             &def->mem.soft_limit, false) < 0)
        goto error;

    if (virDomainParseMemory("./memtune/min_guarantee[1]", ctxt,
                             &def->mem.min_guarantee, false) < 0)
        goto error;

    if (virDomainParseMemory("./memtune/swap_hard_limit[1]", ctxt,
                             &def->mem.swap_hard_limit, false) < 0)
        goto error;

    n = virXPathULong("string(./vcpu[1])", ctxt, &count);
    if (n == -2) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("maximum vcpus must be an integer"));
        goto error;
    } else if (n < 0) {
        def->maxvcpus = 1;
    } else {
        def->maxvcpus = count;
        if (count == 0 || (unsigned short) count != count) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("invalid maximum number of vCPUs '%lu'"), count);
            goto error;
        }
    }

    n = virXPathULong("string(./vcpu[1]/@current)", ctxt, &count);
    if (n == -2) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("current vcpus must be an integer"));
        goto error;
    } else if (n < 0) {
        def->vcpus = def->maxvcpus;
    } else {
        def->vcpus = count;
        if (count == 0 || (unsigned short) count != count) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("invalid current number of vCPUs '%lu'"), count);
            goto error;
        }

        if (def->maxvcpus < count) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("maxvcpus must not be less than current vcpus "
                             "(%d < %lu)"),
                           def->maxvcpus, count);
            goto error;
        }
    }

    tmp = virXPathString("string(./vcpu[1]/@placement)", ctxt);
    if (tmp) {
        if ((def->placement_mode =
             virDomainCpuPlacementModeTypeFromString(tmp)) < 0) {
             virReportError(VIR_ERR_XML_ERROR,
                            _("Unsupported CPU placement mode '%s'"),
                            tmp);
             goto error;
        }
        VIR_FREE(tmp);
    } else {
        def->placement_mode = VIR_DOMAIN_CPU_PLACEMENT_MODE_STATIC;
    }

    if (def->placement_mode != VIR_DOMAIN_CPU_PLACEMENT_MODE_AUTO) {
        tmp = virXPathString("string(./vcpu[1]/@cpuset)", ctxt);
        if (tmp) {
            if (virBitmapParse(tmp, 0, &def->cpumask,
                               VIR_DOMAIN_CPUMASK_LEN) < 0) {
                virReportError(VIR_ERR_XML_ERROR,
                               "%s", _("topology cpuset syntax error"));
                goto error;
            }
            VIR_FREE(tmp);
        }
    }

    /* Extract cpu tunables. */
    if (virXPathULong("string(./cputune/shares[1])", ctxt,
                      &def->cputune.shares) < -1) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("can't parse cputune shares value"));
        goto error;
    }

    if (virXPathULongLong("string(./cputune/period[1])", ctxt,
                          &def->cputune.period) < -1) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("can't parse cputune period value"));
        goto error;
    }

    if (def->cputune.period > 0 &&
        (def->cputune.period < 1000 || def->cputune.period > 1000000)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Value of cputune period must be in range "
                         "[1000, 1000000]"));
        goto error;
    }

    if (virXPathLongLong("string(./cputune/quota[1])", ctxt,
                         &def->cputune.quota) < -1) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("can't parse cputune quota value"));
        goto error;
    }

    if (def->cputune.quota > 0 &&
        (def->cputune.quota < 1000 ||
         def->cputune.quota > 18446744073709551)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Value of cputune quota must be in range "
                         "[1000, 18446744073709551]"));
        goto error;
    }

    if (virXPathULongLong("string(./cputune/emulator_period[1])", ctxt,
                          &def->cputune.emulator_period) < -1) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("can't parse cputune emulator period value"));
        goto error;
    }

    if (def->cputune.emulator_period > 0 &&
        (def->cputune.emulator_period < 1000 ||
         def->cputune.emulator_period > 1000000)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Value of cputune emulator_period must be in range "
                         "[1000, 1000000]"));
        goto error;
    }

    if (virXPathLongLong("string(./cputune/emulator_quota[1])", ctxt,
                         &def->cputune.emulator_quota) < -1) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("can't parse cputune emulator quota value"));
        goto error;
    }

    if (def->cputune.emulator_quota > 0 &&
        (def->cputune.emulator_quota < 1000 ||
         def->cputune.emulator_quota > 18446744073709551)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Value of cputune emulator_quota must be in range "
                         "[1000, 18446744073709551]"));
        goto error;
    }

    if ((n = virXPathNodeSet("./cputune/vcpupin", ctxt, &nodes)) < 0)
        goto error;

    if (n && VIR_ALLOC_N(def->cputune.vcpupin, n) < 0)
        goto no_memory;

    if (n > def->maxvcpus) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("vcpupin nodes must be less than maxvcpus"));
        goto error;
    }

    for (i = 0 ; i < n ; i++) {
        virDomainVcpuPinDefPtr vcpupin = NULL;
        vcpupin = virDomainVcpuPinDefParseXML(nodes[i], ctxt, def->maxvcpus, 0);

        if (!vcpupin)
            goto error;

        if (virDomainVcpuPinIsDuplicate(def->cputune.vcpupin,
                                        def->cputune.nvcpupin,
                                        vcpupin->vcpuid)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("duplicate vcpupin for same vcpu"));
            VIR_FREE(vcpupin);
            goto error;
        }

        if (vcpupin->vcpuid >= def->vcpus)
            /* To avoid the regression when daemon loading
             * domain confs, we can't simply error out if
             * <vcpupin> nodes greater than current vcpus,
             * ignoring them instead.
             */
            VIR_WARN("Ignore vcpupin for not onlined vcpus");
        else
            def->cputune.vcpupin[def->cputune.nvcpupin++] = vcpupin;
    }
    VIR_FREE(nodes);

    /* Initialize the pinning policy for vcpus which doesn't has
     * the policy specified explicitly as def->cpuset.
     */
    if (def->cpumask) {
        if (VIR_REALLOC_N(def->cputune.vcpupin, def->vcpus) < 0) {
            virReportOOMError();
            goto error;
        }

        for (i = 0; i < def->vcpus; i++) {
            if (virDomainVcpuPinIsDuplicate(def->cputune.vcpupin,
                                            def->cputune.nvcpupin,
                                            i))
                continue;

            virDomainVcpuPinDefPtr vcpupin = NULL;

            if (VIR_ALLOC(vcpupin) < 0) {
                virReportOOMError();
                goto error;
            }

            vcpupin->cpumask = virBitmapNew(VIR_DOMAIN_CPUMASK_LEN);
            virBitmapCopy(vcpupin->cpumask, def->cpumask);
            vcpupin->vcpuid = i;
            def->cputune.vcpupin[def->cputune.nvcpupin++] = vcpupin;
        }
    }

    if ((n = virXPathNodeSet("./cputune/emulatorpin", ctxt, &nodes)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("cannot extract emulatorpin nodes"));
        goto error;
    }

    /* Ignore emulatorpin if <vcpu> placement is "auto", they
     * conflicts with each other, and <vcpu> placement can't be
     * simply ignored, as <numatune>'s placement defaults to it.
     */
    if (n) {
        if (def->placement_mode != VIR_DOMAIN_CPU_PLACEMENT_MODE_AUTO) {
            if (n > 1) {
                virReportError(VIR_ERR_XML_ERROR, "%s",
                               _("only one emulatorpin is supported"));
                VIR_FREE(nodes);
                goto error;
            }

            def->cputune.emulatorpin = virDomainVcpuPinDefParseXML(nodes[0], ctxt,
                                                                   def->maxvcpus, 1);

            if (!def->cputune.emulatorpin)
                goto error;
        } else {
            VIR_WARN("Ignore emulatorpin for <vcpu> placement is 'auto'");
        }
    }
    VIR_FREE(nodes);

    /* Extract numatune if exists. */
    if ((n = virXPathNodeSet("./numatune", ctxt, &nodes)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("cannot extract numatune nodes"));
        goto error;
    }

    if (n > 1) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("only one numatune is supported"));
        VIR_FREE(nodes);
        goto error;
    }

    if (n) {
        cur = nodes[0]->children;
        while (cur != NULL) {
            if (cur->type == XML_ELEMENT_NODE) {
                if (xmlStrEqual(cur->name, BAD_CAST "memory")) {
                    char *mode = NULL;
                    char *placement = NULL;
                    char *nodeset = NULL;

                    mode = virXMLPropString(cur, "mode");
                    if (mode) {
                        if ((def->numatune.memory.mode =
                             virDomainNumatuneMemModeTypeFromString(mode)) < 0) {
                            virReportError(VIR_ERR_XML_ERROR,
                                           _("Unsupported NUMA memory "
                                             "tuning mode '%s'"),
                                           mode);
                            VIR_FREE(mode);
                            goto error;
                        }
                        VIR_FREE(mode);
                    } else {
                        def->numatune.memory.mode = VIR_DOMAIN_NUMATUNE_MEM_STRICT;
                    }

                    nodeset = virXMLPropString(cur, "nodeset");
                    if (nodeset) {
                        if (virBitmapParse(nodeset,
                                           0,
                                           &def->numatune.memory.nodemask,
                                           VIR_DOMAIN_CPUMASK_LEN) < 0) {
                            VIR_FREE(nodeset);
                            goto error;
                        }
                        VIR_FREE(nodeset);
                    }

                    placement = virXMLPropString(cur, "placement");
                    int placement_mode = 0;
                    if (placement) {
                        if ((placement_mode =
                             virNumaTuneMemPlacementModeTypeFromString(placement)) < 0) {
                            virReportError(VIR_ERR_XML_ERROR,
                                           _("Unsupported memory placement "
                                             "mode '%s'"), placement);
                            VIR_FREE(placement);
                            goto error;
                        }
                        VIR_FREE(placement);
                    } else if (def->numatune.memory.nodemask) {
                        /* Defaults to "static" if nodeset is specified. */
                        placement_mode = VIR_NUMA_TUNE_MEM_PLACEMENT_MODE_STATIC;
                    } else {
                        /* Defaults to "placement" of <vcpu> if nodeset is
                         * not specified.
                         */
                        if (def->placement_mode == VIR_DOMAIN_CPU_PLACEMENT_MODE_STATIC)
                            placement_mode = VIR_NUMA_TUNE_MEM_PLACEMENT_MODE_STATIC;
                        else
                            placement_mode = VIR_NUMA_TUNE_MEM_PLACEMENT_MODE_AUTO;
                    }

                    if (placement_mode == VIR_NUMA_TUNE_MEM_PLACEMENT_MODE_STATIC &&
                        !def->numatune.memory.nodemask) {
                        virReportError(VIR_ERR_XML_ERROR, "%s",
                                       _("nodeset for NUMA memory tuning must be set "
                                         "if 'placement' is 'static'"));
                        goto error;
                    }

                    /* Ignore 'nodeset' if 'placement' is 'auto' finally */
                    if (placement_mode == VIR_NUMA_TUNE_MEM_PLACEMENT_MODE_AUTO)
                        virBitmapFree(def->numatune.memory.nodemask);

                    /* Copy 'placement' of <numatune> to <vcpu> if its 'placement'
                     * is not specified and 'placement' of <numatune> is specified.
                     */
                    if (placement_mode == VIR_NUMA_TUNE_MEM_PLACEMENT_MODE_AUTO &&
                        !def->cpumask)
                        def->placement_mode = VIR_DOMAIN_CPU_PLACEMENT_MODE_AUTO;

                    def->numatune.memory.placement_mode = placement_mode;
                } else {
                    virReportError(VIR_ERR_XML_ERROR,
                                   _("unsupported XML element %s"),
                                   (const char *)cur->name);
                    goto error;
                }
            }
            cur = cur->next;
        }
    } else {
        /* Defaults NUMA memory placement mode to 'auto' if no <numatune>
         * and 'placement' of <vcpu> is 'auto'.
         */
        if (def->placement_mode == VIR_DOMAIN_CPU_PLACEMENT_MODE_AUTO) {
            def->numatune.memory.placement_mode = VIR_NUMA_TUNE_MEM_PLACEMENT_MODE_AUTO;
            def->numatune.memory.mode = VIR_DOMAIN_NUMATUNE_MEM_STRICT;
        }
    }
    VIR_FREE(nodes);

    if ((n = virXPathNodeSet("./features/*", ctxt, &nodes)) < 0)
        goto error;

    for (i = 0 ; i < n ; i++) {
        int val = virDomainFeatureTypeFromString((const char *)nodes[i]->name);
        if (val < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unexpected feature %s"),
                           nodes[i]->name);
            goto error;
        }
        def->features |= (1 << val);
        if (val == VIR_DOMAIN_FEATURE_APIC) {
            tmp = virXPathString("string(./features/apic/@eoi)", ctxt);
            if (tmp) {
                int eoi;
                if ((eoi = virDomainFeatureStateTypeFromString(tmp)) <= 0) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                   _("unknown value for attribute eoi: %s"),
                                   tmp);
                    goto error;
                }
                def->apic_eoi = eoi;
                VIR_FREE(tmp);
            }
        }
    }
    VIR_FREE(nodes);

    if (def->features & (1 << VIR_DOMAIN_FEATURE_HYPERV)) {
        int feature;
        int value;
        node = ctxt->node;
        if ((n = virXPathNodeSet("./features/hyperv/*", ctxt, &nodes)) < 0)
            goto error;

        for (i = 0; i < n; i++) {
            feature = virDomainHypervTypeFromString((const char *)nodes[i]->name);
            if (feature < 0) {
                virReportError(VIR_ERR_XML_ERROR,
                               _("unsupported HyperV Enlightenment feature: %s"),
                               nodes[i]->name);
                goto error;
            }

            ctxt->node = nodes[i];

            switch ((enum virDomainHyperv) feature) {
                case VIR_DOMAIN_HYPERV_RELAXED:
                    if (!(tmp = virXPathString("string(./@state)", ctxt))) {
                        virReportError(VIR_ERR_XML_ERROR,
                                       _("missing 'state' attribute for "
                                         "HyperV Enlightenment feature '%s'"),
                                       nodes[i]->name);
                        goto error;
                    }

                    if ((value = virDomainFeatureStateTypeFromString(tmp)) < 0) {
                        virReportError(VIR_ERR_XML_ERROR,
                                       _("invalid value of state argument "
                                         "for HyperV Enlightenment feature '%s'"),
                                       nodes[i]->name);
                        goto error;
                    }

                    VIR_FREE(tmp);
                    def->hyperv_features[feature] = value;
                    break;

                case VIR_DOMAIN_HYPERV_LAST:
                    break;
            }
        }
        VIR_FREE(nodes);
        ctxt->node = node;
    }

    if (virDomainEventActionParseXML(ctxt, "on_reboot",
                                     "string(./on_reboot[1])",
                                     &def->onReboot,
                                     VIR_DOMAIN_LIFECYCLE_RESTART,
                                     virDomainLifecycleTypeFromString) < 0)
        goto error;

    if (virDomainEventActionParseXML(ctxt, "on_poweroff",
                                     "string(./on_poweroff[1])",
                                     &def->onPoweroff,
                                     VIR_DOMAIN_LIFECYCLE_DESTROY,
                                     virDomainLifecycleTypeFromString) < 0)
        goto error;

    if (virDomainEventActionParseXML(ctxt, "on_crash",
                                     "string(./on_crash[1])",
                                     &def->onCrash,
                                     VIR_DOMAIN_LIFECYCLE_CRASH_DESTROY,
                                     virDomainLifecycleCrashTypeFromString) < 0)
        goto error;

    if (virDomainEventActionParseXML(ctxt, "on_lockfailure",
                                     "string(./on_lockfailure[1])",
                                     &def->onLockFailure,
                                     VIR_DOMAIN_LOCK_FAILURE_DEFAULT,
                                     virDomainLockFailureTypeFromString) < 0)
        goto error;

    if (virDomainPMStateParseXML(ctxt,
                                 "string(./pm/suspend-to-mem/@enabled)",
                                 &def->pm.s3) < 0)
        goto error;

    if (virDomainPMStateParseXML(ctxt,
                                 "string(./pm/suspend-to-disk/@enabled)",
                                 &def->pm.s4) < 0)
        goto error;

    if ((tmp = virXPathString("string(./clock/@offset)", ctxt)) &&
        (def->clock.offset = virDomainClockOffsetTypeFromString(tmp)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown clock offset '%s'"), tmp);
        goto error;
    }
    VIR_FREE(tmp);

    switch (def->clock.offset) {
    case VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME:
    case VIR_DOMAIN_CLOCK_OFFSET_UTC:
        tmp = virXPathString("string(./clock/@adjustment)", ctxt);
        if (tmp) {
            if (STREQ(tmp, "reset")) {
                def->clock.data.utc_reset = true;
            } else {
                if (virStrToLong_ll(tmp, NULL, 10,
                                    &def->clock.data.variable.adjustment) < 0) {
                    virReportError(VIR_ERR_XML_ERROR,
                                   _("unknown clock adjustment '%s'"),
                                   tmp);
                    goto error;
                }
                switch (def->clock.offset) {
                case VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME:
                    def->clock.data.variable.basis = VIR_DOMAIN_CLOCK_BASIS_LOCALTIME;
                    break;
                case VIR_DOMAIN_CLOCK_OFFSET_UTC:
                    def->clock.data.variable.basis = VIR_DOMAIN_CLOCK_BASIS_UTC;
                    break;
                }
                def->clock.offset = VIR_DOMAIN_CLOCK_OFFSET_VARIABLE;
            }
            VIR_FREE(tmp);
        } else {
            def->clock.data.utc_reset = false;
        }
        break;

    case VIR_DOMAIN_CLOCK_OFFSET_VARIABLE:
        if (virXPathLongLong("number(./clock/@adjustment)", ctxt,
                             &def->clock.data.variable.adjustment) < 0)
            def->clock.data.variable.adjustment = 0;
        tmp = virXPathString("string(./clock/@basis)", ctxt);
        if (tmp) {
            if ((def->clock.data.variable.basis = virDomainClockBasisTypeFromString(tmp)) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("unknown clock basis '%s'"), tmp);
                goto error;
            }
            VIR_FREE(tmp);
        } else {
            def->clock.data.variable.basis = VIR_DOMAIN_CLOCK_BASIS_UTC;
        }
        break;

    case VIR_DOMAIN_CLOCK_OFFSET_TIMEZONE:
        def->clock.data.timezone = virXPathString("string(./clock/@timezone)", ctxt);
        if (!def->clock.data.timezone) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("missing 'timezone' attribute for clock with offset='timezone'"));
            goto error;
        }
        break;
    }

    if ((n = virXPathNodeSet("./clock/timer", ctxt, &nodes)) < 0)
        goto error;

    if (n && VIR_ALLOC_N(def->clock.timers, n) < 0)
        goto no_memory;

    for (i = 0 ; i < n ; i++) {
        virDomainTimerDefPtr timer = virDomainTimerDefParseXML(nodes[i],
                                                               ctxt);
        if (!timer)
            goto error;

        def->clock.timers[def->clock.ntimers++] = timer;
    }
    VIR_FREE(nodes);

    def->os.bootloader = virXPathString("string(./bootloader)", ctxt);
    def->os.bootloaderArgs = virXPathString("string(./bootloader_args)", ctxt);

    def->os.type = virXPathString("string(./os/type[1])", ctxt);
    if (!def->os.type) {
        if (def->os.bootloader) {
            def->os.type = strdup("xen");
            if (!def->os.type) {
                goto no_memory;
            }
        } else {
            virReportError(VIR_ERR_OS_TYPE,
                           "%s", _("no OS type"));
            goto error;
        }
    }
    /*
     * HACK: For xen driver we previously used bogus 'linux' as the
     * os type for paravirt, whereas capabilities declare it to
     * be 'xen'. So we accept the former and convert
     */
    if (STREQ(def->os.type, "linux") &&
        def->virtType == VIR_DOMAIN_VIRT_XEN) {
        VIR_FREE(def->os.type);
        if (!(def->os.type = strdup("xen"))) {
            goto no_memory;
        }
    }

    if (!virCapabilitiesSupportsGuestOSType(caps, def->os.type)) {
        virReportError(VIR_ERR_OS_TYPE,
                       "%s", def->os.type);
        goto error;
    }

    tmp = virXPathString("string(./os/type[1]/@arch)", ctxt);
    if (tmp) {
        def->os.arch = virArchFromString(tmp);
        if (!def->os.arch) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Unknown architecture %s"),
                           tmp);
            goto error;
        }
        VIR_FREE(tmp);

        if (!virCapabilitiesSupportsGuestArch(caps, def->os.arch)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("No guest options available for arch '%s'"),
                           virArchToString(def->os.arch));
            goto error;
        }

        if (!virCapabilitiesSupportsGuestOSTypeArch(caps,
                                                    def->os.type,
                                                    def->os.arch)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("No os type '%s' available for arch '%s'"),
                           def->os.type, virArchToString(def->os.arch));
            goto error;
        }
    } else {
        def->os.arch =
            virCapabilitiesDefaultGuestArch(caps,
                                            def->os.type,
                                            virDomainVirtTypeToString(def->virtType));
        if (!def->os.arch) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("no supported architecture for os type '%s'"),
                           def->os.type);
            goto error;
        }
    }

    def->os.machine = virXPathString("string(./os/type[1]/@machine)", ctxt);
    if (!def->os.machine) {
        const char *defaultMachine = virCapabilitiesDefaultGuestMachine(caps,
                                                                        def->os.type,
                                                                        def->os.arch,
                                                                        virDomainVirtTypeToString(def->virtType));
        if (defaultMachine != NULL) {
            if (!(def->os.machine = strdup(defaultMachine))) {
                goto no_memory;
            }
        }
    }

    /*
     * Booting options for different OS types....
     *
     *   - A bootloader (and optional kernel+initrd)  (xen)
     *   - A kernel + initrd                          (xen)
     *   - A boot device (and optional kernel+initrd) (hvm)
     *   - An init script                             (exe)
     */

    if (STREQ(def->os.type, "exe")) {
        def->os.init = virXPathString("string(./os/init[1])", ctxt);
        def->os.cmdline = virXPathString("string(./os/cmdline[1])", ctxt);

        if ((n = virXPathNodeSet("./os/initarg", ctxt, &nodes)) < 0) {
            goto error;
        }

        if (VIR_ALLOC_N(def->os.initargv, n+1) < 0)
            goto no_memory;
        for (i = 0 ; i < n ; i++) {
            if (!nodes[i]->children ||
                !nodes[i]->children->content) {
                virReportError(VIR_ERR_XML_ERROR, "%s",
                               _("No data supplied for <initarg> element"));
                goto error;
            }
            if (!(def->os.initargv[i] = strdup((const char*)nodes[i]->children->content)))
                goto no_memory;
        }
        def->os.initargv[n] = NULL;
        VIR_FREE(nodes);
    }

    if (STREQ(def->os.type, "xen") ||
        STREQ(def->os.type, "hvm") ||
        STREQ(def->os.type, "uml")) {
        def->os.kernel = virXPathString("string(./os/kernel[1])", ctxt);
        def->os.initrd = virXPathString("string(./os/initrd[1])", ctxt);
        def->os.cmdline = virXPathString("string(./os/cmdline[1])", ctxt);
        def->os.dtb = virXPathString("string(./os/dtb[1])", ctxt);
        def->os.root = virXPathString("string(./os/root[1])", ctxt);
        def->os.loader = virXPathString("string(./os/loader[1])", ctxt);
    }

    if (STREQ(def->os.type, "hvm")) {
        if (virDomainDefParseBootXML(ctxt, def, &bootMapSize) < 0)
            goto error;
        if (bootMapSize && !(bootMap = virBitmapNew(bootMapSize)))
            goto no_memory;
    }

    def->emulator = virXPathString("string(./devices/emulator[1])", ctxt);

    /* analysis of the disk devices */
    if ((n = virXPathNodeSet("./devices/disk", ctxt, &nodes)) < 0)
        goto error;

    if (n && VIR_ALLOC_N(def->disks, n) < 0)
        goto no_memory;

    for (i = 0 ; i < n ; i++) {
        virDomainDiskDefPtr disk = virDomainDiskDefParseXML(xmlopt,
                                                            nodes[i],
                                                            ctxt,
                                                            bootMap,
                                                            def->seclabels,
                                                            def->nseclabels,
                                                            flags);
        if (!disk)
            goto error;

        virDomainDiskInsertPreAlloced(def, disk);
    }
    VIR_FREE(nodes);

    /* analysis of the controller devices */
    if ((n = virXPathNodeSet("./devices/controller", ctxt, &nodes)) < 0)
        goto error;

    if (n && VIR_ALLOC_N(def->controllers, n) < 0)
        goto no_memory;

    for (i = 0 ; i < n ; i++) {
        virDomainControllerDefPtr controller = virDomainControllerDefParseXML(nodes[i],
                                                                              flags);
        if (!controller)
            goto error;

        /* sanitize handling of "none" usb controller */
        if (controller->type == VIR_DOMAIN_CONTROLLER_TYPE_USB) {
            if (controller->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_NONE) {
                if (usb_other || usb_none) {
                    virDomainControllerDefFree(controller);
                    virReportError(VIR_ERR_XML_DETAIL, "%s",
                                   _("Can't add another USB controller: "
                                     "USB is disabled for this domain"));
                    goto error;
                }
                usb_none = true;
            } else {
                if (usb_none) {
                    virDomainControllerDefFree(controller);
                    virReportError(VIR_ERR_XML_DETAIL, "%s",
                                   _("Can't add another USB controller: "
                                     "USB is disabled for this domain"));
                    goto error;
                }
                usb_other = true;
            }
        }

        virDomainControllerInsertPreAlloced(def, controller);
    }
    VIR_FREE(nodes);

    if (def->virtType == VIR_DOMAIN_VIRT_QEMU ||
        def->virtType == VIR_DOMAIN_VIRT_KQEMU ||
        def->virtType == VIR_DOMAIN_VIRT_KVM)
        if (virDomainDefMaybeAddController(def, VIR_DOMAIN_CONTROLLER_TYPE_USB, 0) < 0)
            goto error;

    /* analysis of the resource leases */
    if ((n = virXPathNodeSet("./devices/lease", ctxt, &nodes)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("cannot extract device leases"));
        goto error;
    }
    if (n && VIR_ALLOC_N(def->leases, n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        virDomainLeaseDefPtr lease = virDomainLeaseDefParseXML(nodes[i]);
        if (!lease)
            goto error;

        def->leases[def->nleases++] = lease;
    }
    VIR_FREE(nodes);

    /* analysis of the filesystems */
    if ((n = virXPathNodeSet("./devices/filesystem", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->fss, n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        virDomainFSDefPtr fs = virDomainFSDefParseXML(nodes[i], ctxt,
                                                      flags);
        if (!fs)
            goto error;

        def->fss[def->nfss++] = fs;
    }
    VIR_FREE(nodes);

    /* analysis of the network devices */
    if ((n = virXPathNodeSet("./devices/interface", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->nets, n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        virDomainNetDefPtr net = virDomainNetDefParseXML(xmlopt,
                                                         nodes[i],
                                                         ctxt,
                                                         bootMap,
                                                         flags);
        if (!net)
            goto error;

        def->nets[def->nnets++] = net;

        /* <interface type='hostdev'> must also be in the hostdevs array */
        if (net->type == VIR_DOMAIN_NET_TYPE_HOSTDEV &&
            virDomainHostdevInsert(def, &net->data.hostdev.def) < 0) {
            goto no_memory;
        }
    }
    VIR_FREE(nodes);


    /* analysis of the smartcard devices */
    if ((n = virXPathNodeSet("./devices/smartcard", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->smartcards, n) < 0)
        goto no_memory;

    for (i = 0 ; i < n ; i++) {
        virDomainSmartcardDefPtr card = virDomainSmartcardDefParseXML(nodes[i],
                                                                      flags);
        if (!card)
            goto error;

        def->smartcards[def->nsmartcards++] = card;
    }
    VIR_FREE(nodes);


    /* analysis of the character devices */
    if ((n = virXPathNodeSet("./devices/parallel", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->parallels, n) < 0)
        goto no_memory;

    for (i = 0 ; i < n ; i++) {
        virDomainChrDefPtr chr = virDomainChrDefParseXML(ctxt,
                                                         nodes[i],
                                                         def->seclabels,
                                                         def->nseclabels,
                                                         flags);
        if (!chr)
            goto error;

        if (chr->target.port == -1) {
            int maxport = -1;
            int j;
            for (j = 0 ; j < i ; j++) {
                if (def->parallels[j]->target.port > maxport)
                    maxport = def->parallels[j]->target.port;
            }
            chr->target.port = maxport + 1;
        }
        def->parallels[def->nparallels++] = chr;
    }
    VIR_FREE(nodes);

    if ((n = virXPathNodeSet("./devices/serial", ctxt, &nodes)) < 0)
        goto error;

    if (n && VIR_ALLOC_N(def->serials, n) < 0)
        goto no_memory;

    for (i = 0 ; i < n ; i++) {
        virDomainChrDefPtr chr = virDomainChrDefParseXML(ctxt,
                                                         nodes[i],
                                                         def->seclabels,
                                                         def->nseclabels,
                                                         flags);
        if (!chr)
            goto error;

        if (chr->target.port == -1) {
            int maxport = -1;
            int j;
            for (j = 0 ; j < i ; j++) {
                if (def->serials[j]->target.port > maxport)
                    maxport = def->serials[j]->target.port;
            }
            chr->target.port = maxport + 1;
        }
        def->serials[def->nserials++] = chr;
    }
    VIR_FREE(nodes);

    if ((n = virXPathNodeSet("./devices/console", ctxt, &nodes)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("cannot extract console devices"));
        goto error;
    }
    if (n && VIR_ALLOC_N(def->consoles, n) < 0)
        goto no_memory;

    for (i = 0 ; i < n ; i++) {
        virDomainChrDefPtr chr = virDomainChrDefParseXML(ctxt,
                                                         nodes[i],
                                                         def->seclabels,
                                                         def->nseclabels,
                                                         flags);
        if (!chr)
            goto error;

        chr->target.port = i;
        def->consoles[def->nconsoles++] = chr;
    }
    VIR_FREE(nodes);

    if ((n = virXPathNodeSet("./devices/channel", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->channels, n) < 0)
        goto no_memory;

    for (i = 0 ; i < n ; i++) {
        virDomainChrDefPtr chr = virDomainChrDefParseXML(ctxt,
                                                         nodes[i],
                                                         def->seclabels,
                                                         def->nseclabels,
                                                         flags);
        if (!chr)
            goto error;

        def->channels[def->nchannels++] = chr;

        if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL &&
            chr->targetType == VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO &&
            chr->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)
            chr->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL;

        if (chr->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL &&
            chr->info.addr.vioserial.port == 0) {
            int maxport = 0;
            int j;
            for (j = 0 ; j < i ; j++) {
                virDomainChrDefPtr thischr = def->channels[j];
                if (thischr->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL &&
                    thischr->info.addr.vioserial.controller == chr->info.addr.vioserial.controller &&
                    thischr->info.addr.vioserial.bus == chr->info.addr.vioserial.bus &&
                    (int)thischr->info.addr.vioserial.port > maxport)
                    maxport = thischr->info.addr.vioserial.port;
            }
            chr->info.addr.vioserial.port = maxport + 1;
        }
    }
    VIR_FREE(nodes);


    /* analysis of the input devices */
    if ((n = virXPathNodeSet("./devices/input", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->inputs, n) < 0)
        goto no_memory;

    for (i = 0 ; i < n ; i++) {
        virDomainInputDefPtr input = virDomainInputDefParseXML(def->os.type,
                                                               nodes[i],
                                                               flags);
        if (!input)
            goto error;

        /* Check if USB bus is required */
        if (input->bus == VIR_DOMAIN_INPUT_BUS_USB && usb_none) {
            virDomainInputDefFree(input);
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Can't add USB input device. "
                             "USB bus is disabled"));
            goto error;
        }

        /* With QEMU / KVM / Xen graphics, mouse + PS/2 is implicit
         * with graphics, so don't store it.
         * XXX will this be true for other virt types ? */
        if ((STREQ(def->os.type, "hvm") &&
             input->bus == VIR_DOMAIN_INPUT_BUS_PS2 &&
             input->type == VIR_DOMAIN_INPUT_TYPE_MOUSE) ||
            (STRNEQ(def->os.type, "hvm") &&
             input->bus == VIR_DOMAIN_INPUT_BUS_XEN &&
             input->type == VIR_DOMAIN_INPUT_TYPE_MOUSE)) {
            virDomainInputDefFree(input);
            continue;
        }

        def->inputs[def->ninputs++] = input;
    }
    VIR_FREE(nodes);

    /* analysis of the graphics devices */
    if ((n = virXPathNodeSet("./devices/graphics", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->graphics, n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        virDomainGraphicsDefPtr graphics = virDomainGraphicsDefParseXML(nodes[i],
                                                                        ctxt,
                                                                        flags);
        if (!graphics)
            goto error;

        def->graphics[def->ngraphics++] = graphics;
    }
    VIR_FREE(nodes);

    /* If graphics are enabled, there's an implicit PS2 mouse */
    if (def->ngraphics > 0) {
        virDomainInputDefPtr input;

        if (VIR_ALLOC(input) < 0) {
            goto no_memory;
        }
        if (STREQ(def->os.type, "hvm")) {
            input->type = VIR_DOMAIN_INPUT_TYPE_MOUSE;
            input->bus = VIR_DOMAIN_INPUT_BUS_PS2;
        } else {
            input->type = VIR_DOMAIN_INPUT_TYPE_MOUSE;
            input->bus = VIR_DOMAIN_INPUT_BUS_XEN;
        }

        if (VIR_REALLOC_N(def->inputs, def->ninputs + 1) < 0) {
            virDomainInputDefFree(input);
            goto no_memory;
        }
        def->inputs[def->ninputs] = input;
        def->ninputs++;
    }


    /* analysis of the sound devices */
    if ((n = virXPathNodeSet("./devices/sound", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->sounds, n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        virDomainSoundDefPtr sound = virDomainSoundDefParseXML(nodes[i],
                                                               ctxt,
                                                               flags);
        if (!sound)
            goto error;

        def->sounds[def->nsounds++] = sound;
    }
    VIR_FREE(nodes);

    /* analysis of the video devices */
    if ((n = virXPathNodeSet("./devices/video", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->videos, n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        size_t ii = def->nvideos;
        virDomainVideoDefPtr video = virDomainVideoDefParseXML(nodes[i],
                                                               def,
                                                               flags);
        if (!video)
            goto error;

        if (video->primary) {
            if (primaryVideo) {
                virDomainVideoDefFree(video);
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("Only one primary video device is supported"));
                goto error;
            }

            ii = 0;
            primaryVideo = true;
        }
        if (VIR_INSERT_ELEMENT_INPLACE(def->videos,
                                       ii,
                                       def->nvideos,
                                       video) < 0) {
            virDomainVideoDefFree(video);
            goto error;
        }
    }
    VIR_FREE(nodes);

    /* For backwards compatibility, if no <video> tag is set but there
     * is a <graphics> tag, then we add a single video tag */
    if (def->ngraphics && !def->nvideos) {
        virDomainVideoDefPtr video;
        if (VIR_ALLOC(video) < 0)
            goto no_memory;
        video->type = virDomainVideoDefaultType(def);
        if (video->type < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("cannot determine default video type"));
            VIR_FREE(video);
            goto error;
        }
        video->vram = virDomainVideoDefaultRAM(def, video->type);
        video->heads = 1;
        if (VIR_ALLOC_N(def->videos, 1) < 0) {
            virDomainVideoDefFree(video);
            goto no_memory;
        }
        def->videos[def->nvideos++] = video;
    }

    /* analysis of the host devices */
    if ((n = virXPathNodeSet("./devices/hostdev", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_REALLOC_N(def->hostdevs, def->nhostdevs + n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        virDomainHostdevDefPtr hostdev;

        hostdev = virDomainHostdevDefParseXML(nodes[i], ctxt, bootMap, flags);
        if (!hostdev)
            goto error;

        if (hostdev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB &&
            usb_none) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Can't add host USB device: "
                             "USB is disabled in this host"));
            goto error;
        }

        def->hostdevs[def->nhostdevs++] = hostdev;
    }
    VIR_FREE(nodes);

    /* analysis of the watchdog devices */
    def->watchdog = NULL;
    if ((n = virXPathNodeSet("./devices/watchdog", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n > 1) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("only a single watchdog device is supported"));
        goto error;
    }
    if (n > 0) {
        virDomainWatchdogDefPtr watchdog =
            virDomainWatchdogDefParseXML(nodes[0], flags);
        if (!watchdog)
            goto error;

        def->watchdog = watchdog;
        VIR_FREE(nodes);
    }

    /* analysis of the memballoon devices */
    def->memballoon = NULL;
    if ((n = virXPathNodeSet("./devices/memballoon", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n > 1) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("only a single memory balloon device is supported"));
        goto error;
    }
    if (n > 0) {
        virDomainMemballoonDefPtr memballoon =
            virDomainMemballoonDefParseXML(nodes[0], flags);
        if (!memballoon)
            goto error;

        def->memballoon = memballoon;
        VIR_FREE(nodes);
    } else {
        if (def->virtType == VIR_DOMAIN_VIRT_XEN ||
            def->virtType == VIR_DOMAIN_VIRT_QEMU ||
            def->virtType == VIR_DOMAIN_VIRT_KQEMU ||
            def->virtType == VIR_DOMAIN_VIRT_KVM) {
            virDomainMemballoonDefPtr memballoon;
            if (VIR_ALLOC(memballoon) < 0)
                goto no_memory;
            memballoon->model = def->virtType == VIR_DOMAIN_VIRT_XEN ?
                VIR_DOMAIN_MEMBALLOON_MODEL_XEN :
                VIR_DOMAIN_MEMBALLOON_MODEL_VIRTIO;
            def->memballoon = memballoon;
        }
    }

    /* Parse the RNG device */
    if ((n = virXPathNodeSet("./devices/rng", ctxt, &nodes)) < 0)
        goto error;

    if (n > 1) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("only a single RNG device is supported"));
        goto error;
    }

    if (n > 0) {
        if (!(def->rng = virDomainRNGDefParseXML(nodes[0], ctxt, flags)))
            goto error;
        VIR_FREE(nodes);
    }

    /* analysis of the hub devices */
    if ((n = virXPathNodeSet("./devices/hub", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->hubs, n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        virDomainHubDefPtr hub = virDomainHubDefParseXML(nodes[i], flags);
        if (!hub)
            goto error;

        if (hub->type == VIR_DOMAIN_HUB_TYPE_USB && usb_none) {
            virDomainHubDefFree(hub);
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Can't add USB hub: "
                             "USB is disabled for this domain"));
            goto error;
        }

        def->hubs[def->nhubs++] = hub;
    }
    VIR_FREE(nodes);

    /* analysis of the redirected devices */
    if ((n = virXPathNodeSet("./devices/redirdev", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n && VIR_ALLOC_N(def->redirdevs, n) < 0)
        goto no_memory;
    for (i = 0 ; i < n ; i++) {
        virDomainRedirdevDefPtr redirdev = virDomainRedirdevDefParseXML(nodes[i],
                                                                        bootMap,
                                                                        flags);
        if (!redirdev)
            goto error;

        if (redirdev->bus == VIR_DOMAIN_REDIRDEV_BUS_USB && usb_none) {
             virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                            _("Can't add redirected USB device: "
                              "USB is disabled for this domain"));
            goto error;
        }

        def->redirdevs[def->nredirdevs++] = redirdev;
    }
    VIR_FREE(nodes);

    /* analysis of the redirection filter rules */
    if ((n = virXPathNodeSet("./devices/redirfilter", ctxt, &nodes)) < 0) {
        goto error;
    }
    if (n > 1) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("only one set of redirection filter rule is supported"));
        goto error;
    }

    if (n) {
        virDomainRedirFilterDefPtr redirfilter =
            virDomainRedirFilterDefParseXML(nodes[0], ctxt);
        if (!redirfilter)
            goto error;

        def->redirfilter = redirfilter;
    }
    VIR_FREE(nodes);

    /* analysis of cpu handling */
    if ((node = virXPathNode("./cpu[1]", ctxt)) != NULL) {
        xmlNodePtr oldnode = ctxt->node;
        ctxt->node = node;
        def->cpu = virCPUDefParseXML(node, ctxt, VIR_CPU_TYPE_GUEST);
        ctxt->node = oldnode;

        if (def->cpu == NULL)
            goto error;

        if (def->cpu->sockets &&
            def->maxvcpus >
            def->cpu->sockets * def->cpu->cores * def->cpu->threads) {
            virReportError(VIR_ERR_XML_DETAIL, "%s",
                           _("Maximum CPUs greater than topology limit"));
            goto error;
        }

        if (def->cpu->cells_cpus > def->maxvcpus) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Number of CPUs in <numa> exceeds the"
                             " <vcpu> count"));
            goto error;
        }
    }

    if ((node = virXPathNode("./sysinfo[1]", ctxt)) != NULL) {
        xmlNodePtr oldnode = ctxt->node;
        ctxt->node = node;
        def->sysinfo = virSysinfoParseXML(node, ctxt);
        ctxt->node = oldnode;

        if (def->sysinfo == NULL)
            goto error;
        if (def->sysinfo->system_uuid != NULL) {
            unsigned char uuidbuf[VIR_UUID_BUFLEN];
            if (virUUIDParse(def->sysinfo->system_uuid, uuidbuf) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               "%s", _("malformed uuid element"));
                goto error;
            }
            if (uuid_generated)
                memcpy(def->uuid, uuidbuf, VIR_UUID_BUFLEN);
            else if (memcmp(def->uuid, uuidbuf, VIR_UUID_BUFLEN) != 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("UUID mismatch between <uuid> and "
                                 "<sysinfo>"));
                goto error;
            }
        }
    }

    if ((tmp = virXPathString("string(./os/smbios/@mode)", ctxt))) {
        int mode;

        if ((mode = virDomainSmbiosModeTypeFromString(tmp)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown smbios mode '%s'"), tmp);
            goto error;
        }
        def->os.smbios_mode = mode;
        VIR_FREE(tmp);
    }

    /* Extract custom metadata */
    if ((node = virXPathNode("./metadata[1]", ctxt)) != NULL)
        def->metadata = xmlCopyNode(node, 1);

    /* we have to make a copy of all of the callback pointers here since
     * we won't have the virCaps structure available during free
     */
    def->ns = xmlopt->ns;

    if (def->ns.parse &&
        (def->ns.parse)(xml, root, ctxt, &def->namespaceData) < 0)
        goto error;

    /* Auto-add any implied controllers which aren't present */
    if (virDomainDefAddImplicitControllers(def) < 0)
        goto error;

    /* callback to fill driver specific domain aspects */
    if (virDomainDefPostParse(def, caps, xmlopt) < 0)
        goto error;

    virBitmapFree(bootMap);

    return def;

no_memory:
    virReportOOMError();
error:
    VIR_FREE(tmp);
    VIR_FREE(nodes);
    virBitmapFree(bootMap);
    virDomainDefFree(def);
    return NULL;
}


static virDomainObjPtr
virDomainObjParseXML(xmlDocPtr xml,
                     xmlXPathContextPtr ctxt,
                     virCapsPtr caps,
                     virDomainXMLOptionPtr xmlopt,
                     unsigned int expectedVirtTypes,
                     unsigned int flags)
{
    char *tmp = NULL;
    long val;
    xmlNodePtr config;
    xmlNodePtr oldnode;
    virDomainObjPtr obj;
    xmlNodePtr *nodes = NULL;
    int i, n;
    int state;
    int reason = 0;

    if (!(obj = virDomainObjNew(xmlopt)))
        return NULL;

    if (!(config = virXPathNode("./domain", ctxt))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("no domain config"));
        goto error;
    }

    oldnode = ctxt->node;
    ctxt->node = config;
    obj->def = virDomainDefParseXML(xml, config, ctxt, caps, xmlopt,
                                    expectedVirtTypes, flags);
    ctxt->node = oldnode;
    if (!obj->def)
        goto error;

    if (!(tmp = virXPathString("string(./@state)", ctxt))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing domain state"));
        goto error;
    }
    if ((state = virDomainStateTypeFromString(tmp)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("invalid domain state '%s'"), tmp);
        VIR_FREE(tmp);
        goto error;
    }
    VIR_FREE(tmp);

    if ((tmp = virXPathString("string(./@reason)", ctxt))) {
        if ((reason = virDomainStateReasonFromString(state, tmp)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("invalid domain state reason '%s'"), tmp);
            VIR_FREE(tmp);
            goto error;
        }
        VIR_FREE(tmp);
    }

    virDomainObjSetState(obj, state, reason);

    if (virXPathLong("string(./@pid)", ctxt, &val) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("invalid pid"));
        goto error;
    }
    obj->pid = (pid_t)val;

    if ((n = virXPathNodeSet("./taint", ctxt, &nodes)) < 0) {
        goto error;
    }
    for (i = 0 ; i < n ; i++) {
        char *str = virXMLPropString(nodes[i], "flag");
        if (str) {
            int flag = virDomainTaintTypeFromString(str);
            if (flag < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Unknown taint flag %s"), str);
                VIR_FREE(str);
                goto error;
            }
            VIR_FREE(str);
            virDomainObjTaint(obj, flag);
        }
    }
    VIR_FREE(nodes);

    if (xmlopt->privateData.parse &&
        ((xmlopt->privateData.parse)(ctxt, obj->privateData)) < 0)
        goto error;

    return obj;

error:
    virObjectUnref(obj);
    VIR_FREE(nodes);
    return NULL;
}


static virDomainDefPtr
virDomainDefParse(const char *xmlStr,
                  const char *filename,
                  virCapsPtr caps,
                  virDomainXMLOptionPtr xmlopt,
                  unsigned int expectedVirtTypes,
                  unsigned int flags)
{
    xmlDocPtr xml;
    virDomainDefPtr def = NULL;
    int keepBlanksDefault = xmlKeepBlanksDefault(0);

    if ((xml = virXMLParse(filename, xmlStr, _("(domain_definition)")))) {
        def = virDomainDefParseNode(xml, xmlDocGetRootElement(xml), caps,
                                    xmlopt, expectedVirtTypes, flags);
        xmlFreeDoc(xml);
    }

    xmlKeepBlanksDefault(keepBlanksDefault);
    return def;
}

virDomainDefPtr
virDomainDefParseString(const char *xmlStr,
                        virCapsPtr caps,
                        virDomainXMLOptionPtr xmlopt,
                        unsigned int expectedVirtTypes,
                        unsigned int flags)
{
    return virDomainDefParse(xmlStr, NULL, caps, xmlopt,
                             expectedVirtTypes, flags);
}

virDomainDefPtr
virDomainDefParseFile(const char *filename,
                      virCapsPtr caps,
                      virDomainXMLOptionPtr xmlopt,
                      unsigned int expectedVirtTypes,
                      unsigned int flags)
{
    return virDomainDefParse(NULL, filename, caps, xmlopt,
                             expectedVirtTypes, flags);
}


virDomainDefPtr
virDomainDefParseNode(xmlDocPtr xml,
                      xmlNodePtr root,
                      virCapsPtr caps,
                      virDomainXMLOptionPtr xmlopt,
                      unsigned int expectedVirtTypes,
                      unsigned int flags)
{
    xmlXPathContextPtr ctxt = NULL;
    virDomainDefPtr def = NULL;

    if (!xmlStrEqual(root->name, BAD_CAST "domain")) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("unexpected root element <%s>, "
                         "expecting <domain>"),
                       root->name);
        goto cleanup;
    }

    ctxt = xmlXPathNewContext(xml);
    if (ctxt == NULL) {
        virReportOOMError();
        goto cleanup;
    }

    ctxt->node = root;
    def = virDomainDefParseXML(xml, root, ctxt, caps, xmlopt,
                               expectedVirtTypes, flags);

cleanup:
    xmlXPathFreeContext(ctxt);
    return def;
}


static virDomainObjPtr
virDomainObjParseNode(xmlDocPtr xml,
                      xmlNodePtr root,
                      virCapsPtr caps,
                      virDomainXMLOptionPtr xmlopt,
                      unsigned int expectedVirtTypes,
                      unsigned int flags)
{
    xmlXPathContextPtr ctxt = NULL;
    virDomainObjPtr obj = NULL;

    if (!xmlStrEqual(root->name, BAD_CAST "domstatus")) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("unexpected root element <%s>, "
                         "expecting <domstatus>"),
                       root->name);
        goto cleanup;
    }

    if (!(ctxt = xmlXPathNewContext(xml))) {
        virReportOOMError();
        goto cleanup;
    }

    ctxt->node = root;
    obj = virDomainObjParseXML(xml, ctxt, caps, xmlopt, expectedVirtTypes, flags);

cleanup:
    xmlXPathFreeContext(ctxt);
    return obj;
}


static virDomainObjPtr
virDomainObjParseFile(const char *filename,
                      virCapsPtr caps,
                      virDomainXMLOptionPtr xmlopt,
                      unsigned int expectedVirtTypes,
                      unsigned int flags)
{
    xmlDocPtr xml;
    virDomainObjPtr obj = NULL;
    int keepBlanksDefault = xmlKeepBlanksDefault(0);

    if ((xml = virXMLParseFile(filename))) {
        obj = virDomainObjParseNode(xml, xmlDocGetRootElement(xml),
                                    caps, xmlopt,
                                    expectedVirtTypes, flags);
        xmlFreeDoc(xml);
    }

    xmlKeepBlanksDefault(keepBlanksDefault);
    return obj;
}


static bool
virDomainTimerDefCheckABIStability(virDomainTimerDefPtr src,
                                   virDomainTimerDefPtr dst)
{
    if (src->name != dst->name) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target timer %s does not match source %s"),
                       virDomainTimerNameTypeToString(dst->name),
                       virDomainTimerNameTypeToString(src->name));
        return false;
    }

    if (src->present != dst->present) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target timer presence %d does not match source %d"),
                       dst->present, src->present);
        return false;
    }

    if (src->name == VIR_DOMAIN_TIMER_NAME_TSC) {
        if (src->frequency != dst->frequency) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Target TSC frequency %lu does not match source %lu"),
                           dst->frequency, src->frequency);
            return false;
        }

        if (src->mode != dst->mode) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Target TSC mode %s does not match source %s"),
                           virDomainTimerModeTypeToString(dst->mode),
                           virDomainTimerModeTypeToString(src->mode));
            return false;
        }
    }

    return true;
}


static bool
virDomainDeviceInfoCheckABIStability(virDomainDeviceInfoPtr src,
                                     virDomainDeviceInfoPtr dst)
{
    if (src->type != dst->type) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target device address type %s does not match source %s"),
                       virDomainDeviceAddressTypeToString(dst->type),
                       virDomainDeviceAddressTypeToString(src->type));
        return false;
    }

    switch (src->type) {
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI:
        if (src->addr.pci.domain != dst->addr.pci.domain ||
            src->addr.pci.bus != dst->addr.pci.bus ||
            src->addr.pci.slot != dst->addr.pci.slot ||
            src->addr.pci.function != dst->addr.pci.function) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Target device PCI address %04x:%02x:%02x.%02x "
                             "does not match source %04x:%02x:%02x.%02x"),
                           dst->addr.pci.domain, dst->addr.pci.bus,
                           dst->addr.pci.slot, dst->addr.pci.function,
                           src->addr.pci.domain, src->addr.pci.bus,
                           src->addr.pci.slot, src->addr.pci.function);
            return false;
        }
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE:
        if (src->addr.drive.controller != dst->addr.drive.controller ||
            src->addr.drive.bus != dst->addr.drive.bus ||
            src->addr.drive.unit != dst->addr.drive.unit) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Target device drive address %d:%d:%d "
                             "does not match source %d:%d:%d"),
                           dst->addr.drive.controller, dst->addr.drive.bus,
                           dst->addr.drive.unit,
                           src->addr.drive.controller, src->addr.drive.bus,
                           src->addr.drive.unit);
            return false;
        }
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL:
        if (src->addr.vioserial.controller != dst->addr.vioserial.controller ||
            src->addr.vioserial.bus != dst->addr.vioserial.bus ||
            src->addr.vioserial.port != dst->addr.vioserial.port) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Target device virtio serial address %d:%d:%d "
                             "does not match source %d:%d:%d"),
                           dst->addr.vioserial.controller, dst->addr.vioserial.bus,
                           dst->addr.vioserial.port,
                           src->addr.vioserial.controller, src->addr.vioserial.bus,
                           src->addr.vioserial.port);
            return false;
        }
        break;

    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID:
        if (src->addr.ccid.controller != dst->addr.ccid.controller ||
            src->addr.ccid.slot != dst->addr.ccid.slot) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Target device ccid address %d:%d "
                             "does not match source %d:%d"),
                           dst->addr.ccid.controller,
                           dst->addr.ccid.slot,
                           src->addr.ccid.controller,
                           src->addr.ccid.slot);
            return false;
        }
        break;
    }

    return true;
}


static bool
virDomainDiskDefCheckABIStability(virDomainDiskDefPtr src,
                                  virDomainDiskDefPtr dst)
{
    if (src->device != dst->device) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target disk device %s does not match source %s"),
                       virDomainDiskDeviceTypeToString(dst->device),
                       virDomainDiskDeviceTypeToString(src->device));
        return false;
    }

    if (src->bus != dst->bus) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target disk bus %s does not match source %s"),
                       virDomainDiskBusTypeToString(dst->bus),
                       virDomainDiskBusTypeToString(src->bus));
        return false;
    }

    if (STRNEQ(src->dst, dst->dst)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target disk %s does not match source %s"),
                       dst->dst, src->dst);
        return false;
    }

    if (STRNEQ_NULLABLE(src->serial, dst->serial)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target disk serial %s does not match source %s"),
                       NULLSTR(dst->serial), NULLSTR(src->serial));
        return false;
    }

    if (src->readonly != dst->readonly || src->shared != dst->shared) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Target disk access mode does not match source"));
        return false;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        return false;

    return true;
}


static bool
virDomainControllerDefCheckABIStability(virDomainControllerDefPtr src,
                                        virDomainControllerDefPtr dst)
{
    if (src->type != dst->type) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target controller type %s does not match source %s"),
                       virDomainControllerTypeToString(dst->type),
                       virDomainControllerTypeToString(src->type));
        return false;
    }

    if (src->idx != dst->idx) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target controller index %d does not match source %d"),
                       dst->idx, src->idx);
        return false;
    }

    if (src->model != dst->model) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target controller model %d does not match source %d"),
                       dst->model, src->model);
        return false;
    }

    if (src->type == VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL) {
        if (src->opts.vioserial.ports != dst->opts.vioserial.ports) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Target controller ports %d does not match source %d"),
                           dst->opts.vioserial.ports, src->opts.vioserial.ports);
            return false;
        }

        if (src->opts.vioserial.vectors != dst->opts.vioserial.vectors) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Target controller vectors %d does not match source %d"),
                           dst->opts.vioserial.vectors, src->opts.vioserial.vectors);
            return false;
        }
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        return false;

    return true;
}


static bool
virDomainFsDefCheckABIStability(virDomainFSDefPtr src,
                                virDomainFSDefPtr dst)
{
    if (STRNEQ(src->dst, dst->dst)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target filesystem guest target %s does not match source %s"),
                       dst->dst, src->dst);
        return false;
    }

    if (src->readonly != dst->readonly) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Target filesystem access mode does not match source"));
        return false;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        return false;

    return true;
}


static bool
virDomainNetDefCheckABIStability(virDomainNetDefPtr src,
                                 virDomainNetDefPtr dst)
{
    char srcmac[VIR_MAC_STRING_BUFLEN];
    char dstmac[VIR_MAC_STRING_BUFLEN];

    if (virMacAddrCmp(&src->mac, &dst->mac) != 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target network card mac %s"
                         " does not match source %s"),
                       virMacAddrFormat(&dst->mac, dstmac),
                       virMacAddrFormat(&src->mac, srcmac));
        return false;
    }

    if (STRNEQ_NULLABLE(src->model, dst->model)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target network card model %s does not match source %s"),
                       NULLSTR(dst->model), NULLSTR(src->model));
        return false;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        return false;

    return true;
}


static bool
virDomainInputDefCheckABIStability(virDomainInputDefPtr src,
                                   virDomainInputDefPtr dst)
{
    if (src->type != dst->type) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target input device type %s does not match source %s"),
                       virDomainInputTypeToString(dst->type),
                       virDomainInputTypeToString(src->type));
        return false;
    }

    if (src->bus != dst->bus) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target input device bus %s does not match source %s"),
                       virDomainInputBusTypeToString(dst->bus),
                       virDomainInputBusTypeToString(src->bus));
        return false;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        return false;

    return true;
}


static bool
virDomainSoundDefCheckABIStability(virDomainSoundDefPtr src,
                                   virDomainSoundDefPtr dst)
{
    if (src->model != dst->model) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target sound card model %s does not match source %s"),
                       virDomainSoundModelTypeToString(dst->model),
                       virDomainSoundModelTypeToString(src->model));
        return false;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        return false;

    return true;
}


static bool
virDomainVideoDefCheckABIStability(virDomainVideoDefPtr src,
                                   virDomainVideoDefPtr dst)
{
    if (src->type != dst->type) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target video card model %s does not match source %s"),
                       virDomainVideoTypeToString(dst->type),
                       virDomainVideoTypeToString(src->type));
        return false;
    }

    if (src->vram != dst->vram) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target video card vram %u does not match source %u"),
                       dst->vram, src->vram);
        return false;
    }

    if (src->heads != dst->heads) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target video card heads %u does not match source %u"),
                       dst->heads, src->heads);
        return false;
    }

    if ((src->accel && !dst->accel) ||
        (!src->accel && dst->accel)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Target video card acceleration does not match source"));
        return false;
    }

    if (src->accel) {
        if (src->accel->support2d != dst->accel->support2d) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Target video card 2d accel %u does not match source %u"),
                           dst->accel->support2d, src->accel->support2d);
            return false;
        }

        if (src->accel->support3d != dst->accel->support3d) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Target video card 3d accel %u does not match source %u"),
                           dst->accel->support3d, src->accel->support3d);
            return false;
        }
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        return false;

    return true;
}


static bool
virDomainHostdevDefCheckABIStability(virDomainHostdevDefPtr src,
                                     virDomainHostdevDefPtr dst)
{
    if (src->mode != dst->mode) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target host device mode %s does not match source %s"),
                       virDomainHostdevModeTypeToString(dst->mode),
                       virDomainHostdevModeTypeToString(src->mode));
        return false;
    }

    if (src->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
        src->source.subsys.type != dst->source.subsys.type) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target host device subsystem %s does not match source %s"),
                       virDomainHostdevSubsysTypeToString(dst->source.subsys.type),
                       virDomainHostdevSubsysTypeToString(src->source.subsys.type));
        return false;
    }

    if (!virDomainDeviceInfoCheckABIStability(src->info, dst->info))
        return false;

    return true;
}


static bool
virDomainSmartcardDefCheckABIStability(virDomainSmartcardDefPtr src,
                                       virDomainSmartcardDefPtr dst)
{
    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        return false;

    return true;
}


static bool
virDomainSerialDefCheckABIStability(virDomainChrDefPtr src,
                                    virDomainChrDefPtr dst)
{
    if (src->target.port != dst->target.port) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target serial port %d does not match source %d"),
                       dst->target.port, src->target.port);
        return false;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        return false;

    return true;
}


static bool
virDomainParallelDefCheckABIStability(virDomainChrDefPtr src,
                                      virDomainChrDefPtr dst)
{
    if (src->target.port != dst->target.port) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target serial port %d does not match source %d"),
                       dst->target.port, src->target.port);
        return false;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        return false;

    return true;
}


static bool
virDomainChannelDefCheckABIStability(virDomainChrDefPtr src,
                                     virDomainChrDefPtr dst)
{
    if (src->targetType != dst->targetType) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target channel type %s does not match source %s"),
                       virDomainChrChannelTargetTypeToString(dst->targetType),
                       virDomainChrChannelTargetTypeToString(src->targetType));
        return false;
    }

    switch (src->targetType) {
    case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO:
        if (STRNEQ_NULLABLE(src->target.name, dst->target.name)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Target channel name %s does not match source %s"),
                           NULLSTR(dst->target.name), NULLSTR(src->target.name));
            return false;
        }
        if (src->source.type != dst->source.type &&
            (src->source.type == VIR_DOMAIN_CHR_TYPE_SPICEVMC ||
             dst->source.type == VIR_DOMAIN_CHR_TYPE_SPICEVMC) &&
            !src->target.name) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Changing device type to/from spicevmc would"
                             " change default target channel name"));
            return false;
        }
        break;
    case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_GUESTFWD:
        if (memcmp(src->target.addr, dst->target.addr,
                   sizeof(*src->target.addr)) != 0) {
            char *saddr = virSocketAddrFormatFull(src->target.addr, true, ":");
            char *daddr = virSocketAddrFormatFull(dst->target.addr, true, ":");
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Target channel addr %s does not match source %s"),
                           NULLSTR(daddr), NULLSTR(saddr));
            VIR_FREE(saddr);
            VIR_FREE(daddr);
            return false;
        }
        break;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        return false;

    return true;
}


static bool
virDomainConsoleDefCheckABIStability(virDomainChrDefPtr src,
                                     virDomainChrDefPtr dst)
{
    if (src->targetType != dst->targetType) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target console type %s does not match source %s"),
                       virDomainChrConsoleTargetTypeToString(dst->targetType),
                       virDomainChrConsoleTargetTypeToString(src->targetType));
        return false;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        return false;

    return true;
}


static bool
virDomainWatchdogDefCheckABIStability(virDomainWatchdogDefPtr src,
                                      virDomainWatchdogDefPtr dst)
{
    if (src->model != dst->model) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target watchdog model %s does not match source %s"),
                       virDomainWatchdogModelTypeToString(dst->model),
                       virDomainWatchdogModelTypeToString(src->model));
        return false;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        return false;

    return true;
}


static bool
virDomainMemballoonDefCheckABIStability(virDomainMemballoonDefPtr src,
                                        virDomainMemballoonDefPtr dst)
{
    if (src->model != dst->model) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target balloon model %s does not match source %s"),
                       virDomainMemballoonModelTypeToString(dst->model),
                       virDomainMemballoonModelTypeToString(src->model));
        return false;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        return false;

    return true;
}


static bool
virDomainRNGDefCheckABIStability(virDomainRNGDefPtr src,
                                 virDomainRNGDefPtr dst)
{
    if (!src && !dst)
        return true;

    if (!src || !dst) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain RNG device count '%d' "
                         "does not match source count '%d'"),
                       src ? 1 : 0, dst ? 1 : 0);
        return false;
    }

    if (src->model != dst->model) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target RNG model '%s' does not match source '%s'"),
                       virDomainRNGModelTypeToString(dst->model),
                       virDomainRNGModelTypeToString(src->model));
        return false;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        return false;

    return true;
}


static bool
virDomainHubDefCheckABIStability(virDomainHubDefPtr src,
                                 virDomainHubDefPtr dst)
{
    if (src->type != dst->type) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target hub device type %s does not match source %s"),
                       virDomainHubTypeToString(dst->type),
                       virDomainHubTypeToString(src->type));
        return false;
    }

    if (!virDomainDeviceInfoCheckABIStability(&src->info, &dst->info))
        return false;

    return true;
}

static bool
virDomainRedirFilterDefCheckABIStability(virDomainRedirFilterDefPtr src,
                                         virDomainRedirFilterDefPtr dst)
{
    int i;

    if (src->nusbdevs != dst->nusbdevs) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target USB redirection filter rule "
                         "count %zu does not match source %zu"),
                         dst->nusbdevs, src->nusbdevs);
        return false;
    }

    for (i = 0; i < src->nusbdevs; i++) {
        virDomainRedirFilterUsbDevDefPtr srcUsbDev = src->usbdevs[i];
        virDomainRedirFilterUsbDevDefPtr dstUsbDev = dst->usbdevs[i];
        if (srcUsbDev->usbClass != dstUsbDev->usbClass) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("Target USB Class code does not match source"));
            return false;
        }

        if (srcUsbDev->vendor != dstUsbDev->vendor) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("Target USB vendor ID does not match source"));
            return false;
        }

        if (srcUsbDev->product != dstUsbDev->product) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("Target USB product ID does not match source"));
            return false;
        }

        if (srcUsbDev->version != dstUsbDev->version) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("Target USB version does not match source"));
            return false;
        }

        if (srcUsbDev->allow != dstUsbDev->allow) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Target USB allow '%s' does not match source '%s'"),
                             dstUsbDev->allow ? "yes" : "no",
                             srcUsbDev->allow ? "yes" : "no");
            return false;
        }
    }

    return true;
}

/* This compares two configurations and looks for any differences
 * which will affect the guest ABI. This is primarily to allow
 * validation of custom XML config passed in during migration
 */
bool
virDomainDefCheckABIStability(virDomainDefPtr src,
                              virDomainDefPtr dst)
{
    int i;

    if (src->virtType != dst->virtType) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain virt type %s does not match source %s"),
                       virDomainVirtTypeToString(dst->virtType),
                       virDomainVirtTypeToString(src->virtType));
        return false;
    }

    if (memcmp(src->uuid, dst->uuid, VIR_UUID_BUFLEN) != 0) {
        char uuidsrc[VIR_UUID_STRING_BUFLEN];
        char uuiddst[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(src->uuid, uuidsrc);
        virUUIDFormat(dst->uuid, uuiddst);
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain uuid %s does not match source %s"),
                       uuiddst, uuidsrc);
        return false;
    }

    if (src->mem.max_balloon != dst->mem.max_balloon) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain max memory %lld does not match source %lld"),
                       dst->mem.max_balloon, src->mem.max_balloon);
        return false;
    }
    if (src->mem.cur_balloon != dst->mem.cur_balloon) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain current memory %lld does not match source %lld"),
                       dst->mem.cur_balloon, src->mem.cur_balloon);
        return false;
    }
    if (src->mem.hugepage_backed != dst->mem.hugepage_backed) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain huge page backing %d does not match source %d"),
                       dst->mem.hugepage_backed,
                       src->mem.hugepage_backed);
        return false;
    }

    if (src->vcpus != dst->vcpus) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain vpu count %d does not match source %d"),
                       dst->vcpus, src->vcpus);
        return false;
    }
    if (src->maxvcpus != dst->maxvcpus) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain vpu max %d does not match source %d"),
                       dst->maxvcpus, src->maxvcpus);
        return false;
    }

    if (STRNEQ(src->os.type, dst->os.type)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain OS type %s does not match source %s"),
                       dst->os.type, src->os.type);
        return false;
    }
    if (src->os.arch != dst->os.arch){
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain architecture %s does not match source %s"),
                       virArchToString(dst->os.arch),
                       virArchToString(src->os.arch));
        return false;
    }
    if (STRNEQ(src->os.machine, dst->os.machine)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain OS type %s does not match source %s"),
                       dst->os.machine, src->os.machine);
        return false;
    }

    if (src->os.smbios_mode != dst->os.smbios_mode) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain SMBIOS mode %s does not match source %s"),
                       virDomainSmbiosModeTypeToString(dst->os.smbios_mode),
                       virDomainSmbiosModeTypeToString(src->os.smbios_mode));
        return false;
    }

    if (src->features != dst->features) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain features %d does not match source %d"),
                       dst->features, src->features);
        return false;
    }

    if (src->clock.ntimers != dst->clock.ntimers) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Target domain timers do not match source"));
        return false;
    }

    for (i = 0 ; i < src->clock.ntimers ; i++) {
        if (!virDomainTimerDefCheckABIStability(src->clock.timers[i],
                                                dst->clock.timers[i]))
            return false;
    }

    if (!virCPUDefIsEqual(src->cpu, dst->cpu))
        return false;

    if (!virSysinfoIsEqual(src->sysinfo, dst->sysinfo))
        return false;

    if (src->ndisks != dst->ndisks) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain disk count %zu does not match source %zu"),
                       dst->ndisks, src->ndisks);
        return false;
    }

    for (i = 0 ; i < src->ndisks ; i++)
        if (!virDomainDiskDefCheckABIStability(src->disks[i], dst->disks[i]))
            return false;

    if (src->ncontrollers != dst->ncontrollers) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain disk controller count %zu "
                         "does not match source %zu"),
                       dst->ncontrollers, src->ncontrollers);
        return false;
    }

    for (i = 0 ; i < src->ncontrollers ; i++)
        if (!virDomainControllerDefCheckABIStability(src->controllers[i],
                                                     dst->controllers[i]))
            return false;

    if (src->nfss != dst->nfss) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain filesystem count %zu "
                         "does not match source %zu"),
                       dst->nfss, src->nfss);
        return false;
    }

    for (i = 0 ; i < src->nfss ; i++)
        if (!virDomainFsDefCheckABIStability(src->fss[i], dst->fss[i]))
            return false;

    if (src->nnets != dst->nnets) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain net card count %zu "
                         "does not match source %zu"),
                       dst->nnets, src->nnets);
        return false;
    }

    for (i = 0 ; i < src->nnets ; i++)
        if (!virDomainNetDefCheckABIStability(src->nets[i], dst->nets[i]))
            return false;

    if (src->ninputs != dst->ninputs) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain input device count %zu "
                         "does not match source %zu"),
                       dst->ninputs, src->ninputs);
        return false;
    }

    for (i = 0 ; i < src->ninputs ; i++)
        if (!virDomainInputDefCheckABIStability(src->inputs[i], dst->inputs[i]))
            return false;

    if (src->nsounds != dst->nsounds) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain sound card count %zu "
                         "does not match source %zu"),
                       dst->nsounds, src->nsounds);
        return false;
    }

    for (i = 0 ; i < src->nsounds ; i++)
        if (!virDomainSoundDefCheckABIStability(src->sounds[i], dst->sounds[i]))
            return false;

    if (src->nvideos != dst->nvideos) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain video card count %zu "
                         "does not match source %zu"),
                       dst->nvideos, src->nvideos);
        return false;
    }

    for (i = 0 ; i < src->nvideos ; i++)
        if (!virDomainVideoDefCheckABIStability(src->videos[i], dst->videos[i]))
            return false;

    if (src->nhostdevs != dst->nhostdevs) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain host device count %zu "
                         "does not match source %zu"),
                       dst->nhostdevs, src->nhostdevs);
        return false;
    }

    for (i = 0 ; i < src->nhostdevs ; i++)
        if (!virDomainHostdevDefCheckABIStability(src->hostdevs[i],
                                                  dst->hostdevs[i]))
            return false;

    if (src->nsmartcards != dst->nsmartcards) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain smartcard count %zu "
                         "does not match source %zu"),
                       dst->nsmartcards, src->nsmartcards);
        return false;
    }

    for (i = 0 ; i < src->nsmartcards ; i++)
        if (!virDomainSmartcardDefCheckABIStability(src->smartcards[i],
                                                    dst->smartcards[i]))
            return false;

    if (src->nserials != dst->nserials) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain serial port count %zu "
                         "does not match source %zu"),
                       dst->nserials, src->nserials);
        return false;
    }

    for (i = 0 ; i < src->nserials ; i++)
        if (!virDomainSerialDefCheckABIStability(src->serials[i],
                                                 dst->serials[i]))
            return false;

    if (src->nparallels != dst->nparallels) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain parallel port count %zu "
                         "does not match source %zu"),
                       dst->nparallels, src->nparallels);
        return false;
    }

    for (i = 0 ; i < src->nparallels ; i++)
        if (!virDomainParallelDefCheckABIStability(src->parallels[i],
                                                   dst->parallels[i]))
            return false;

    if (src->nchannels != dst->nchannels) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain channel count %zu "
                         "does not match source %zu"),
                       dst->nchannels, src->nchannels);
        return false;
    }

    for (i = 0 ; i < src->nchannels ; i++)
        if (!virDomainChannelDefCheckABIStability(src->channels[i],
                                                  dst->channels[i]))
            return false;

    if (src->nconsoles != dst->nconsoles) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain console count %zu "
                         "does not match source %zu"),
                       dst->nconsoles, src->nconsoles);
        return false;
    }

    for (i = 0 ; i < src->nconsoles ; i++)
        if (!virDomainConsoleDefCheckABIStability(src->consoles[i],
                                                  dst->consoles[i]))
            return false;

    if (src->nhubs != dst->nhubs) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain hub device count %zu "
                         "does not match source %zu"),
                       dst->nhubs, src->nhubs);
        return false;
    }

    for (i = 0 ; i < src->nhubs ; i++)
        if (!virDomainHubDefCheckABIStability(src->hubs[i], dst->hubs[i]))
            return false;

    if ((!src->redirfilter && dst->redirfilter) ||
        (src->redirfilter && !dst->redirfilter)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain USB redirection filter count %d "
                         "does not match source %d"),
                       dst->redirfilter ? 1 : 0, src->redirfilter ? 1 : 0);
        return false;
    }

    if (src->redirfilter &&
        !virDomainRedirFilterDefCheckABIStability(src->redirfilter,
                                                  dst->redirfilter))
        return false;

    if ((!src->watchdog && dst->watchdog) ||
        (src->watchdog && !dst->watchdog)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain watchdog count %d "
                         "does not match source %d"),
                       dst->watchdog ? 1 : 0, src->watchdog ? 1 : 0);
        return false;
    }

    if (src->watchdog &&
        !virDomainWatchdogDefCheckABIStability(src->watchdog, dst->watchdog))
        return false;

    if ((!src->memballoon && dst->memballoon) ||
        (src->memballoon && !dst->memballoon)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target domain memory balloon count %d "
                         "does not match source %d"),
                       dst->memballoon ? 1 : 0, src->memballoon ? 1 : 0);
        return false;
    }

    if (src->memballoon &&
        !virDomainMemballoonDefCheckABIStability(src->memballoon,
                                                 dst->memballoon))
        return false;

    if (!virDomainRNGDefCheckABIStability(src->rng, dst->rng))
        return false;

    return true;
}


static int
virDomainDefAddDiskControllersForType(virDomainDefPtr def,
                                      int controllerType,
                                      int diskBus)
{
    int i;
    int maxController = -1;

    for (i = 0 ; i < def->ndisks ; i++) {
        if (def->disks[i]->bus != diskBus)
            continue;

        if (def->disks[i]->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE)
            continue;

        if ((int)def->disks[i]->info.addr.drive.controller > maxController)
            maxController = def->disks[i]->info.addr.drive.controller;
    }

    for (i = 0 ; i <= maxController ; i++) {
        if (virDomainDefMaybeAddController(def, controllerType, i) < 0)
            return -1;
    }

    return 0;
}


static int
virDomainDefMaybeAddVirtioSerialController(virDomainDefPtr def)
{
    /* Look for any virtio serial or virtio console devs */
    int i;

    for (i = 0 ; i < def->nchannels ; i++) {
        virDomainChrDefPtr channel = def->channels[i];

        if (channel->targetType == VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO) {
            int idx = 0;
            if (channel->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL)
                idx = channel->info.addr.vioserial.controller;

            if (virDomainDefMaybeAddController(def,
                VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL, idx) < 0)
                return -1;
        }
    }

    for (i = 0 ; i < def->nconsoles ; i++) {
        virDomainChrDefPtr console = def->consoles[i];

        if (console->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_VIRTIO) {
            int idx = 0;
            if (console->info.type ==
                VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL)
                idx = console->info.addr.vioserial.controller;

            if (virDomainDefMaybeAddController(def,
                VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL, idx) < 0)
                return -1;
        }
    }

    return 0;
}


static int
virDomainDefMaybeAddSmartcardController(virDomainDefPtr def)
{
    /* Look for any smartcard devs */
    int i;

    for (i = 0 ; i < def->nsmartcards ; i++) {
        virDomainSmartcardDefPtr smartcard = def->smartcards[i];
        int idx = 0;

        if (smartcard->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID) {
            idx = smartcard->info.addr.ccid.controller;
        } else if (smartcard->info.type
                   == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
            int j;
            int max = -1;

            for (j = 0; j < def->nsmartcards; j++) {
                virDomainDeviceInfoPtr info = &def->smartcards[j]->info;
                if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID &&
                    info->addr.ccid.controller == 0 &&
                    (int) info->addr.ccid.slot > max)
                    max = info->addr.ccid.slot;
            }
            smartcard->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID;
            smartcard->info.addr.ccid.controller = 0;
            smartcard->info.addr.ccid.slot = max + 1;
        }

        if (virDomainDefMaybeAddController(def,
                                           VIR_DOMAIN_CONTROLLER_TYPE_CCID,
                                           idx) < 0)
            return -1;
    }

    return 0;
}


/*
 * Based on the declared <address/> info for any devices,
 * add necessary drive controllers which are not already present
 * in the XML. This is for compat with existing apps which will
 * not know/care about <controller> info in the XML
 */
int
virDomainDefAddImplicitControllers(virDomainDefPtr def)
{
    if (virDomainDefAddDiskControllersForType(def,
                                              VIR_DOMAIN_CONTROLLER_TYPE_SCSI,
                                              VIR_DOMAIN_DISK_BUS_SCSI) < 0)
        return -1;

    if (virDomainDefAddDiskControllersForType(def,
                                              VIR_DOMAIN_CONTROLLER_TYPE_FDC,
                                              VIR_DOMAIN_DISK_BUS_FDC) < 0)
        return -1;

    if (virDomainDefAddDiskControllersForType(def,
                                              VIR_DOMAIN_CONTROLLER_TYPE_IDE,
                                              VIR_DOMAIN_DISK_BUS_IDE) < 0)
        return -1;

    if (virDomainDefAddDiskControllersForType(def,
                                              VIR_DOMAIN_CONTROLLER_TYPE_SATA,
                                              VIR_DOMAIN_DISK_BUS_SATA) < 0)
        return -1;

    if (virDomainDefMaybeAddVirtioSerialController(def) < 0)
        return -1;

    if (virDomainDefMaybeAddSmartcardController(def) < 0)
        return -1;

    return 0;
}

/* Check if vcpupin with same vcpuid already exists.
 * Return 1 if exists, 0 if not. */
int
virDomainVcpuPinIsDuplicate(virDomainVcpuPinDefPtr *def,
                            int nvcpupin,
                            int vcpu)
{
    int i;

    if (!def || !nvcpupin)
        return 0;

    for (i = 0; i < nvcpupin; i++) {
        if (def[i]->vcpuid == vcpu)
            return 1;
    }

    return 0;
}

virDomainVcpuPinDefPtr
virDomainVcpuPinFindByVcpu(virDomainVcpuPinDefPtr *def,
                           int nvcpupin,
                           int vcpu)
{
    int i;

    if (!def || !nvcpupin)
        return NULL;

    for (i = 0; i < nvcpupin; i++) {
        if (def[i]->vcpuid == vcpu)
            return def[i];
    }

    return NULL;
}

int
virDomainVcpuPinAdd(virDomainVcpuPinDefPtr **vcpupin_list,
                    int *nvcpupin,
                    unsigned char *cpumap,
                    int maplen,
                    int vcpu)
{
    virDomainVcpuPinDefPtr vcpupin = NULL;

    if (!vcpupin_list)
        return -1;

    vcpupin = virDomainVcpuPinFindByVcpu(*vcpupin_list,
                                         *nvcpupin,
                                         vcpu);
    if (vcpupin) {
        vcpupin->vcpuid = vcpu;
        virBitmapFree(vcpupin->cpumask);
        vcpupin->cpumask = virBitmapNewData(cpumap, maplen);
        if (!vcpupin->cpumask) {
            virReportOOMError();
            return -1;
        }

        return 0;
    }

    /* No existing vcpupin matches vcpu, adding a new one */

    if (VIR_ALLOC(vcpupin) < 0)
        goto no_memory;

    vcpupin->vcpuid = vcpu;
    vcpupin->cpumask = virBitmapNewData(cpumap, maplen);
    if (!vcpupin->cpumask)
        goto no_memory;

    if (VIR_REALLOC_N(*vcpupin_list, *nvcpupin + 1) < 0)
        goto no_memory;

    (*vcpupin_list)[(*nvcpupin)++] = vcpupin;

    return 0;

no_memory:
    virReportOOMError();
    virDomainVcpuPinDefFree(vcpupin);
    return -1;
}

int
virDomainVcpuPinDel(virDomainDefPtr def, int vcpu)
{
    int n;
    bool deleted = false;
    virDomainVcpuPinDefPtr *vcpupin_list = def->cputune.vcpupin;

    /* No vcpupin exists yet */
    if (!def->cputune.nvcpupin) {
        return 0;
    }

    for (n = 0; n < def->cputune.nvcpupin; n++) {
        if (vcpupin_list[n]->vcpuid == vcpu) {
            VIR_FREE(vcpupin_list[n]->cpumask);
            VIR_FREE(vcpupin_list[n]);
            memmove(&vcpupin_list[n],
                    &vcpupin_list[n+1],
                    (def->cputune.nvcpupin - n - 1) * sizeof(virDomainVcpuPinDef *));
            deleted = true;
            break;
        }
    }

    if (!deleted)
        return 0;

    if (--def->cputune.nvcpupin == 0) {
        VIR_FREE(def->cputune.vcpupin);
    } else {
        if (VIR_REALLOC_N(def->cputune.vcpupin, def->cputune.nvcpupin) < 0) {
            virReportOOMError();
            return -1;
        }
    }

    return 0;
}

int
virDomainEmulatorPinAdd(virDomainDefPtr def,
                        unsigned char *cpumap,
                        int maplen)
{
    virDomainVcpuPinDefPtr emulatorpin = NULL;

    if (!def->cputune.emulatorpin) {
        /* No emulatorpin exists yet. */
        if (VIR_ALLOC(emulatorpin) < 0) {
            virReportOOMError();
            return -1;
        }

        emulatorpin->vcpuid = -1;
        emulatorpin->cpumask = virBitmapNewData(cpumap, maplen);
        if (!emulatorpin->cpumask)
            return -1;

        def->cputune.emulatorpin = emulatorpin;
    } else {
        /* Since there is only 1 emulatorpin for each vm,
         * juest replace the old one.
         */
        virBitmapFree(def->cputune.emulatorpin->cpumask);
        def->cputune.emulatorpin->cpumask = virBitmapNewData(cpumap, maplen);
        if (!def->cputune.emulatorpin->cpumask)
            return -1;
    }

    return 0;
}

int
virDomainEmulatorPinDel(virDomainDefPtr def)
{
    if (!def->cputune.emulatorpin) {
        return 0;
    }

    virDomainVcpuPinDefFree(def->cputune.emulatorpin);
    def->cputune.emulatorpin = NULL;

    return 0;
}

static int
virDomainEventActionDefFormat(virBufferPtr buf,
                              int type,
                              const char *name,
                              virEventActionToStringFunc convFunc)
{
    const char *typeStr = convFunc(type);
    if (!typeStr) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected %s action: %d"), name, type);
        return -1;
    }

    virBufferAsprintf(buf, "  <%s>%s</%s>\n", name, typeStr, name);

    return 0;
}


static void
virSecurityLabelDefFormat(virBufferPtr buf, virSecurityLabelDefPtr def)
{
    const char *sectype = virDomainSeclabelTypeToString(def->type);

    if (!sectype)
        return;

    if (def->type == VIR_DOMAIN_SECLABEL_DEFAULT)
        return;

    /* To avoid backward compatibility issues, suppress DAC labels that are
     * automatically generated.
     */
    if (STREQ_NULLABLE(def->model, "dac") && def->implicit)
        return;

    virBufferAsprintf(buf, "<seclabel type='%s'",
                      sectype);

    if (def->model && STRNEQ(def->model, "none"))
        virBufferEscapeString(buf, " model='%s'", def->model);

    if (def->type == VIR_DOMAIN_SECLABEL_NONE) {
        virBufferAddLit(buf, "/>\n");
        return;
    }

    virBufferAsprintf(buf, " relabel='%s'",
                      def->norelabel ? "no" : "yes");

    if (def->label || def->imagelabel || def->baselabel) {
        virBufferAddLit(buf, ">\n");

        virBufferEscapeString(buf, "  <label>%s</label>\n",
                              def->label);
        if (!def->norelabel)
            virBufferEscapeString(buf, "  <imagelabel>%s</imagelabel>\n",
                                  def->imagelabel);
        if (def->type == VIR_DOMAIN_SECLABEL_DYNAMIC)
            virBufferEscapeString(buf, "  <baselabel>%s</baselabel>\n",
                                  def->baselabel);
        virBufferAddLit(buf, "</seclabel>\n");
    } else {
        virBufferAddLit(buf, "/>\n");
    }
}


static void
virSecurityDeviceLabelDefFormat(virBufferPtr buf,
                                virSecurityDeviceLabelDefPtr def)
{
    virBufferAsprintf(buf, "<seclabel");

    if (def->model)
        virBufferAsprintf(buf, " model='%s'", def->model);

    virBufferAsprintf(buf, " relabel='%s'", def->norelabel ? "no" : "yes");

    if (def->label) {
        virBufferAddLit(buf, ">\n");
        virBufferEscapeString(buf, "  <label>%s</label>\n",
                              def->label);
        virBufferAddLit(buf, "</seclabel>\n");
    } else {
        virBufferAddLit(buf, "/>\n");
    }
}


static int
virDomainLeaseDefFormat(virBufferPtr buf,
                        virDomainLeaseDefPtr def)
{
    virBufferAddLit(buf, "    <lease>\n");
    virBufferEscapeString(buf, "      <lockspace>%s</lockspace>\n", def->lockspace);
    virBufferEscapeString(buf, "      <key>%s</key>\n", def->key);
    virBufferEscapeString(buf, "      <target path='%s'", def->path);
    if (def->offset)
        virBufferAsprintf(buf, " offset='%llu'", def->offset);
    virBufferAddLit(buf, "/>\n");
    virBufferAddLit(buf, "    </lease>\n");

    return 0;
}

static void
virDomainDiskGeometryDefFormat(virBufferPtr buf,
                               virDomainDiskDefPtr def)
{
    const char *trans =
        virDomainDiskGeometryTransTypeToString(def->geometry.trans);

    if (def->geometry.cylinders > 0 &&
        def->geometry.heads > 0 &&
        def->geometry.sectors > 0) {
        virBufferAsprintf(buf,
                          "      <geometry cyls='%u' heads='%u' secs='%u'",
                          def->geometry.cylinders,
                          def->geometry.heads,
                          def->geometry.sectors);

        if (def->geometry.trans != VIR_DOMAIN_DISK_TRANS_DEFAULT)
            virBufferEscapeString(buf, " trans='%s'", trans);

        virBufferAddLit(buf, "/>\n");
    }
}

static void
virDomainDiskBlockIoDefFormat(virBufferPtr buf,
                              virDomainDiskDefPtr def)
{
    if (def->blockio.logical_block_size > 0 ||
        def->blockio.physical_block_size > 0) {
        virBufferAddLit(buf,"      <blockio");
        if (def->blockio.logical_block_size > 0) {
            virBufferAsprintf(buf,
                              " logical_block_size='%u'",
                              def->blockio.logical_block_size);
        }
        if (def->blockio.physical_block_size > 0) {
            virBufferAsprintf(buf,
                              " physical_block_size='%u'",
                              def->blockio.physical_block_size);
        }
        virBufferAddLit(buf, "/>\n");
    }
}

static int
virDomainDiskSourceDefFormat(virBufferPtr buf,
                             virDomainDiskDefPtr def)
{
    int n;
    const char *startupPolicy = virDomainStartupPolicyTypeToString(def->startupPolicy);

    if (def->src || def->nhosts > 0 || def->srcpool ||
        def->startupPolicy) {
        switch (def->type) {
        case VIR_DOMAIN_DISK_TYPE_FILE:
            virBufferAddLit(buf,"      <source");
            if (def->src)
                virBufferEscapeString(buf, " file='%s'", def->src);
            if (def->startupPolicy)
                virBufferEscapeString(buf, " startupPolicy='%s'",
                                      startupPolicy);
            if (def->nseclabels) {
                virBufferAddLit(buf, ">\n");
                virBufferAdjustIndent(buf, 8);
                for (n = 0; n < def->nseclabels; n++)
                    virSecurityDeviceLabelDefFormat(buf, def->seclabels[n]);
                virBufferAdjustIndent(buf, -8);
                virBufferAddLit(buf, "      </source>\n");
            } else {
                virBufferAddLit(buf, "/>\n");
            }
            break;
        case VIR_DOMAIN_DISK_TYPE_BLOCK:
            virBufferEscapeString(buf, "      <source dev='%s'",
                                  def->src);
            if (def->nseclabels) {
                virBufferAddLit(buf, ">\n");
                virBufferAdjustIndent(buf, 8);
                for (n = 0; n < def->nseclabels; n++)
                    virSecurityDeviceLabelDefFormat(buf, def->seclabels[n]);
                virBufferAdjustIndent(buf, -8);
                virBufferAddLit(buf, "      </source>\n");
            } else {
                virBufferAddLit(buf, "/>\n");
            }
            break;
        case VIR_DOMAIN_DISK_TYPE_DIR:
            virBufferEscapeString(buf, "      <source dir='%s'/>\n",
                                  def->src);
            break;
        case VIR_DOMAIN_DISK_TYPE_NETWORK:
            virBufferAsprintf(buf, "      <source protocol='%s'",
                              virDomainDiskProtocolTypeToString(def->protocol));
            if (def->src) {
                virBufferEscapeString(buf, " name='%s'", def->src);
            }
            if (def->nhosts == 0) {
                virBufferAddLit(buf, "/>\n");
            } else {
                int i;

                virBufferAddLit(buf, ">\n");
                for (i = 0; i < def->nhosts; i++) {
                    virBufferAddLit(buf, "        <host");
                    if (def->hosts[i].name) {
                        virBufferEscapeString(buf, " name='%s'", def->hosts[i].name);
                    }
                    if (def->hosts[i].port) {
                        virBufferEscapeString(buf, " port='%s'",
                                              def->hosts[i].port);
                    }
                    if (def->hosts[i].transport) {
                        virBufferAsprintf(buf, " transport='%s'",
                                          virDomainDiskProtocolTransportTypeToString(def->hosts[i].transport));
                    }
                    if (def->hosts[i].socket) {
                        virBufferEscapeString(buf, " socket='%s'", def->hosts[i].socket);
                    }
                    virBufferAddLit(buf, "/>\n");
                }
                virBufferAddLit(buf, "      </source>\n");
            }
            break;
        case VIR_DOMAIN_DISK_TYPE_VOLUME:
            virBufferAddLit(buf, "      <source");

            if (def->srcpool)
                virBufferAsprintf(buf, " pool='%s' volume='%s'",
                                  def->srcpool->pool, def->srcpool->volume);
            if (def->startupPolicy)
                virBufferEscapeString(buf, " startupPolicy='%s'", startupPolicy);

            if (def->nseclabels) {
                virBufferAddLit(buf, ">\n");
                virBufferAdjustIndent(buf, 8);
                for (n = 0; n < def->nseclabels; n++)
                    virSecurityDeviceLabelDefFormat(buf, def->seclabels[n]);
                virBufferAdjustIndent(buf, -8);
                virBufferAddLit(buf, "      </source>\n");
            } else {
                virBufferAddLit(buf, "/>\n");
            }
            break;
        default:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unexpected disk type %s"),
                           virDomainDiskTypeToString(def->type));
            return -1;
        }
    }

    return 0;
}

static int
virDomainDiskDefFormat(virBufferPtr buf,
                       virDomainDiskDefPtr def,
                       unsigned int flags)
{
    const char *type = virDomainDiskTypeToString(def->type);
    const char *device = virDomainDiskDeviceTypeToString(def->device);
    const char *bus = virDomainDiskBusTypeToString(def->bus);
    const char *cachemode = virDomainDiskCacheTypeToString(def->cachemode);
    const char *error_policy = virDomainDiskErrorPolicyTypeToString(def->error_policy);
    const char *rerror_policy = virDomainDiskErrorPolicyTypeToString(def->rerror_policy);
    const char *iomode = virDomainDiskIoTypeToString(def->iomode);
    const char *ioeventfd = virDomainIoEventFdTypeToString(def->ioeventfd);
    const char *event_idx = virDomainVirtioEventIdxTypeToString(def->event_idx);
    const char *copy_on_read = virDomainVirtioEventIdxTypeToString(def->copy_on_read);
    const char *sgio = virDomainDiskSGIOTypeToString(def->sgio);

    char uuidstr[VIR_UUID_STRING_BUFLEN];

    if (!type) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected disk type %d"), def->type);
        return -1;
    }
    if (!device) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected disk device %d"), def->device);
        return -1;
    }
    if (!bus) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected disk bus %d"), def->bus);
        return -1;
    }
    if (!cachemode) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected disk cache mode %d"), def->cachemode);
        return -1;
    }
    if (!iomode) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected disk io mode %d"), def->iomode);
        return -1;
    }
    if (!sgio) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unexpected disk sgio mode '%d'"), def->sgio);
        return -1;
    }

    virBufferAsprintf(buf,
                      "    <disk type='%s' device='%s'",
                      type, device);
    if (def->rawio_specified) {
        if (def->rawio == 1) {
            virBufferAddLit(buf, " rawio='yes'");
        } else if (def->rawio == 0) {
            virBufferAddLit(buf, " rawio='no'");
        }
    }

    if (def->sgio)
        virBufferAsprintf(buf, " sgio='%s'", sgio);

    if (def->snapshot &&
        !(def->snapshot == VIR_DOMAIN_SNAPSHOT_LOCATION_NONE && def->readonly))
        virBufferAsprintf(buf, " snapshot='%s'",
                          virDomainSnapshotLocationTypeToString(def->snapshot));
    virBufferAddLit(buf, ">\n");

    if (def->driverName || def->format > 0 || def->cachemode ||
        def->ioeventfd || def->event_idx || def->copy_on_read) {
        virBufferAddLit(buf, "      <driver");
        if (def->driverName)
            virBufferAsprintf(buf, " name='%s'", def->driverName);
        if (def->format > 0)
            virBufferAsprintf(buf, " type='%s'",
                              virStorageFileFormatTypeToString(def->format));
        if (def->cachemode)
            virBufferAsprintf(buf, " cache='%s'", cachemode);
        if (def->error_policy)
            virBufferAsprintf(buf, " error_policy='%s'", error_policy);
        if (def->rerror_policy)
            virBufferAsprintf(buf, " rerror_policy='%s'", rerror_policy);
        if (def->iomode)
            virBufferAsprintf(buf, " io='%s'", iomode);
        if (def->ioeventfd)
            virBufferAsprintf(buf, " ioeventfd='%s'", ioeventfd);
        if (def->event_idx)
            virBufferAsprintf(buf, " event_idx='%s'", event_idx);
        if (def->copy_on_read)
            virBufferAsprintf(buf, " copy_on_read='%s'", copy_on_read);
        virBufferAddLit(buf, "/>\n");
    }

    if (def->auth.username) {
        virBufferEscapeString(buf, "      <auth username='%s'>\n",
                              def->auth.username);
        if (def->protocol == VIR_DOMAIN_DISK_PROTOCOL_ISCSI) {
            virBufferAsprintf(buf, "        <secret type='iscsi'");
        } else if (def->protocol == VIR_DOMAIN_DISK_PROTOCOL_RBD) {
            virBufferAsprintf(buf, "        <secret type='ceph'");
        }

        if (def->auth.secretType == VIR_DOMAIN_DISK_SECRET_TYPE_UUID) {
            virUUIDFormat(def->auth.secret.uuid, uuidstr);
            virBufferAsprintf(buf, " uuid='%s'/>\n", uuidstr);
        }
        if (def->auth.secretType == VIR_DOMAIN_DISK_SECRET_TYPE_USAGE) {
            virBufferEscapeString(buf, " usage='%s'/>\n",
                                  def->auth.secret.usage);
        }
        virBufferAddLit(buf, "      </auth>\n");
    }

    if (virDomainDiskSourceDefFormat(buf, def) < 0)
        return -1;
    virDomainDiskGeometryDefFormat(buf, def);
    virDomainDiskBlockIoDefFormat(buf, def);

    /* For now, mirroring is currently output-only: we only output it
     * for live domains, therefore we ignore it on input except for
     * the internal parse on libvirtd restart.  */
    if (def->mirror && !(flags & VIR_DOMAIN_XML_INACTIVE)) {
        virBufferEscapeString(buf, "      <mirror file='%s'", def->mirror);
        if (def->mirrorFormat)
            virBufferAsprintf(buf, " format='%s'",
                              virStorageFileFormatTypeToString(def->mirrorFormat));
        if (def->mirroring)
            virBufferAddLit(buf, " ready='yes'");
        virBufferAddLit(buf, "/>\n");
    }

    virBufferAsprintf(buf, "      <target dev='%s' bus='%s'",
                      def->dst, bus);
    if ((def->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY ||
         def->device == VIR_DOMAIN_DISK_DEVICE_CDROM) &&
        def->tray_status != VIR_DOMAIN_DISK_TRAY_CLOSED)
        virBufferAsprintf(buf, " tray='%s'/>\n",
                          virDomainDiskTrayTypeToString(def->tray_status));
    else
        virBufferAddLit(buf, "/>\n");

    /*disk I/O throttling*/
    if (def->blkdeviotune.total_bytes_sec ||
        def->blkdeviotune.read_bytes_sec ||
        def->blkdeviotune.write_bytes_sec ||
        def->blkdeviotune.total_iops_sec ||
        def->blkdeviotune.read_iops_sec ||
        def->blkdeviotune.write_iops_sec) {
        virBufferAddLit(buf, "      <iotune>\n");
        if (def->blkdeviotune.total_bytes_sec) {
            virBufferAsprintf(buf, "        <total_bytes_sec>%llu</total_bytes_sec>\n",
                              def->blkdeviotune.total_bytes_sec);
        }

        if (def->blkdeviotune.read_bytes_sec) {
            virBufferAsprintf(buf, "        <read_bytes_sec>%llu</read_bytes_sec>\n",
                              def->blkdeviotune.read_bytes_sec);

        }

        if (def->blkdeviotune.write_bytes_sec) {
            virBufferAsprintf(buf, "        <write_bytes_sec>%llu</write_bytes_sec>\n",
                              def->blkdeviotune.write_bytes_sec);
        }

        if (def->blkdeviotune.total_iops_sec) {
            virBufferAsprintf(buf, "        <total_iops_sec>%llu</total_iops_sec>\n",
                              def->blkdeviotune.total_iops_sec);
        }

        if (def->blkdeviotune.read_iops_sec) {
            virBufferAsprintf(buf, "        <read_iops_sec>%llu</read_iops_sec>\n",
                              def->blkdeviotune.read_iops_sec);
        }

        if (def->blkdeviotune.write_iops_sec) {
            virBufferAsprintf(buf, "        <write_iops_sec>%llu</write_iops_sec>\n",
                              def->blkdeviotune.write_iops_sec);
        }

        virBufferAddLit(buf, "      </iotune>\n");
    }

    if (def->readonly)
        virBufferAddLit(buf, "      <readonly/>\n");
    if (def->shared)
        virBufferAddLit(buf, "      <shareable/>\n");
    if (def->transient)
        virBufferAddLit(buf, "      <transient/>\n");
    virBufferEscapeString(buf, "      <serial>%s</serial>\n", def->serial);
    virBufferEscapeString(buf, "      <wwn>%s</wwn>\n", def->wwn);
    virBufferEscapeString(buf, "      <vendor>%s</vendor>\n", def->vendor);
    virBufferEscapeString(buf, "      <product>%s</product>\n", def->product);
    if (def->encryption) {
        virBufferAdjustIndent(buf, 6);
        if (virStorageEncryptionFormat(buf, def->encryption) < 0)
            return -1;
        virBufferAdjustIndent(buf, -6);
    }

    if (virDomainDeviceInfoFormat(buf, &def->info,
                                  flags | VIR_DOMAIN_XML_INTERNAL_ALLOW_BOOT) < 0)
        return -1;

    virBufferAddLit(buf, "    </disk>\n");

    return 0;
}

static const char *
virDomainControllerModelTypeToString(virDomainControllerDefPtr def,
                                     int model)
{
    if (def->type == VIR_DOMAIN_CONTROLLER_TYPE_SCSI)
        return virDomainControllerModelSCSITypeToString(model);
    else if (def->type == VIR_DOMAIN_CONTROLLER_TYPE_USB)
        return virDomainControllerModelUSBTypeToString(model);

    return NULL;
}

static int
virDomainControllerDefFormat(virBufferPtr buf,
                             virDomainControllerDefPtr def,
                             unsigned int flags)
{
    const char *type = virDomainControllerTypeToString(def->type);
    const char *model = NULL;

    if (!type) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected controller type %d"), def->type);
        return -1;
    }

    if (def->model != -1) {
        model = virDomainControllerModelTypeToString(def, def->model);

        if (!model) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unexpected model type %d"), def->model);
            return -1;
        }
    }

    virBufferAsprintf(buf,
                      "    <controller type='%s' index='%d'",
                      type, def->idx);

    if (model) {
        virBufferEscapeString(buf, " model='%s'", model);
    }

    if (def->num_queues)
        virBufferAsprintf(buf, " num_queues='%u'", def->num_queues);

    switch (def->type) {
    case VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL:
        if (def->opts.vioserial.ports != -1) {
            virBufferAsprintf(buf, " ports='%d'",
                              def->opts.vioserial.ports);
        }
        if (def->opts.vioserial.vectors != -1) {
            virBufferAsprintf(buf, " vectors='%d'",
                              def->opts.vioserial.vectors);
        }
        break;

    default:
        break;
    }

    if (virDomainDeviceInfoIsSet(&def->info, flags)) {
        virBufferAddLit(buf, ">\n");
        if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
            return -1;
        virBufferAddLit(buf, "    </controller>\n");
    } else {
        virBufferAddLit(buf, "/>\n");
    }

    return 0;
}


int
virDomainFSIndexByName(virDomainDefPtr def, const char *name)
{
    virDomainFSDefPtr fs;
    int i;

    for (i = 0; i < def->nfss; i++) {
        fs = def->fss[i];
        if (STREQ(fs->dst, name))
            return i;
    }
    return -1;
}


static int
virDomainFSDefFormat(virBufferPtr buf,
                     virDomainFSDefPtr def,
                     unsigned int flags)
{
    const char *type = virDomainFSTypeToString(def->type);
    const char *accessmode = virDomainFSAccessModeTypeToString(def->accessmode);
    const char *fsdriver = virDomainFSDriverTypeTypeToString(def->fsdriver);
    const char *wrpolicy = virDomainFSWrpolicyTypeToString(def->wrpolicy);

    if (!type) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected filesystem type %d"), def->type);
        return -1;
    }

   if (!accessmode) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected accessmode %d"), def->accessmode);
        return -1;
    }


    virBufferAsprintf(buf,
                      "    <filesystem type='%s' accessmode='%s'>\n",
                      type, accessmode);

    if (def->fsdriver) {
        virBufferAsprintf(buf, "      <driver type='%s'", fsdriver);

        /* Don't generate anything if wrpolicy is set to default */
        if (def->wrpolicy) {
            virBufferAsprintf(buf, " wrpolicy='%s'", wrpolicy);
        }

        virBufferAddLit(buf, "/>\n");
    }

    switch (def->type) {
    case VIR_DOMAIN_FS_TYPE_MOUNT:
    case VIR_DOMAIN_FS_TYPE_BIND:
        virBufferEscapeString(buf, "      <source dir='%s'/>\n",
                              def->src);
        break;

    case VIR_DOMAIN_FS_TYPE_BLOCK:
        virBufferEscapeString(buf, "      <source dev='%s'/>\n",
                              def->src);
        break;

    case VIR_DOMAIN_FS_TYPE_FILE:
        virBufferEscapeString(buf, "      <source file='%s'/>\n",
                              def->src);
        break;

    case VIR_DOMAIN_FS_TYPE_TEMPLATE:
        virBufferEscapeString(buf, "      <source name='%s'/>\n",
                              def->src);
        break;

    case VIR_DOMAIN_FS_TYPE_RAM:
        virBufferAsprintf(buf, "      <source usage='%lld' units='KiB'/>\n",
                          def->usage / 1024);
        break;
    }

    virBufferEscapeString(buf, "      <target dir='%s'/>\n",
                          def->dst);

    if (def->readonly)
        virBufferAddLit(buf, "      <readonly/>\n");

    if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
        return -1;


    if (def->space_hard_limit)
        virBufferAsprintf(buf, "      <space_hard_limit unit='bytes'>"
                          "%llu</space_hard_limit>\n", def->space_hard_limit);
    if (def->space_soft_limit) {
        virBufferAsprintf(buf, "      <space_soft_limit unit='bytes'>"
                          "%llu</space_soft_limit>\n", def->space_soft_limit);
    }
    virBufferAddLit(buf, "    </filesystem>\n");

    return 0;
}

static int
virDomainHostdevDefFormatSubsys(virBufferPtr buf,
                                virDomainHostdevDefPtr def,
                                unsigned int flags,
                                bool includeTypeInAddr)
{
    virBufferAddLit(buf, "<source");
    if (def->startupPolicy) {
        const char *policy;
        policy = virDomainStartupPolicyTypeToString(def->startupPolicy);
        virBufferAsprintf(buf, " startupPolicy='%s'", policy);
    }
    if (def->source.subsys.u.usb.autoAddress &&
        (flags & VIR_DOMAIN_XML_MIGRATABLE))
        virBufferAddLit(buf, " autoAddress='yes'");

    if (def->missing &&
        !(flags & VIR_DOMAIN_XML_INACTIVE))
        virBufferAddLit(buf, " missing='yes'");

    virBufferAddLit(buf, ">\n");

    virBufferAdjustIndent(buf, 2);
    switch (def->source.subsys.type)
    {
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB:
        if (def->source.subsys.u.usb.vendor) {
            virBufferAsprintf(buf, "<vendor id='0x%.4x'/>\n",
                              def->source.subsys.u.usb.vendor);
            virBufferAsprintf(buf, "<product id='0x%.4x'/>\n",
                              def->source.subsys.u.usb.product);
        }
        if (def->source.subsys.u.usb.bus ||
            def->source.subsys.u.usb.device) {
            virBufferAsprintf(buf, "<address %sbus='%d' device='%d'/>\n",
                              includeTypeInAddr ? "type='usb' " : "",
                              def->source.subsys.u.usb.bus,
                              def->source.subsys.u.usb.device);
        }
        break;
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI:
        if (virDevicePCIAddressFormat(buf,
                                      def->source.subsys.u.pci,
                                      includeTypeInAddr) != 0)
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("PCI address Formatting failed"));

        if ((flags & VIR_DOMAIN_XML_INTERNAL_PCI_ORIG_STATES) &&
            (def->origstates.states.pci.unbind_from_stub ||
             def->origstates.states.pci.remove_slot ||
             def->origstates.states.pci.reprobe)) {
            virBufferAddLit(buf, "<origstates>\n");
            if (def->origstates.states.pci.unbind_from_stub)
                virBufferAddLit(buf, "  <unbind/>\n");
            if (def->origstates.states.pci.remove_slot)
                virBufferAddLit(buf, "  <removeslot/>\n");
            if (def->origstates.states.pci.reprobe)
                virBufferAddLit(buf, "  <reprobe/>\n");
            virBufferAddLit(buf, "</origstates>\n");
        }
        break;
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected hostdev type %d"),
                       def->source.subsys.type);
        return -1;
    }

    virBufferAdjustIndent(buf, -2);
    virBufferAddLit(buf, "</source>\n");
    return 0;
}

static int
virDomainHostdevDefFormatCaps(virBufferPtr buf,
                              virDomainHostdevDefPtr def)
{
    virBufferAddLit(buf, "<source>\n");

    virBufferAdjustIndent(buf, 2);
    switch (def->source.caps.type)
    {
    case VIR_DOMAIN_HOSTDEV_CAPS_TYPE_STORAGE:
        virBufferEscapeString(buf, "<block>%s</block>\n",
                              def->source.caps.u.storage.block);
        break;
    case VIR_DOMAIN_HOSTDEV_CAPS_TYPE_MISC:
        virBufferEscapeString(buf, "<char>%s</char>\n",
                              def->source.caps.u.misc.chardev);
        break;
    case VIR_DOMAIN_HOSTDEV_CAPS_TYPE_NET:
        virBufferEscapeString(buf, "<interface>%s</interface>\n",
                              def->source.caps.u.net.iface);
        break;
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected hostdev type %d"),
                       def->source.caps.type);
        return -1;
    }

    virBufferAdjustIndent(buf, -2);
    virBufferAddLit(buf, "</source>\n");
    return 0;
}

static int
virDomainActualNetDefFormat(virBufferPtr buf,
                            virDomainActualNetDefPtr def,
                            unsigned int flags)
{
    const char *type;
    const char *mode;

    if (!def)
        return 0;

    type = virDomainNetTypeToString(def->type);
    if (!type) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected net type %d"), def->type);
        return -1;
    }

    virBufferAsprintf(buf, "<actual type='%s'", type);
    if (def->type == VIR_DOMAIN_NET_TYPE_HOSTDEV &&
        def->data.hostdev.def.managed) {
        virBufferAddLit(buf, " managed='yes'");
    }
    virBufferAddLit(buf, ">\n");

    virBufferAdjustIndent(buf, 2);
    switch (def->type) {
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
        virBufferEscapeString(buf, "<source bridge='%s'/>\n",
                              def->data.bridge.brname);
        break;

    case VIR_DOMAIN_NET_TYPE_DIRECT:
        virBufferAddLit(buf, "<source");
        if (def->data.direct.linkdev)
            virBufferEscapeString(buf, " dev='%s'",
                                  def->data.direct.linkdev);

        mode = virNetDevMacVLanModeTypeToString(def->data.direct.mode);
        if (!mode) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unexpected source mode %d"),
                           def->data.direct.mode);
            return -1;
        }
        virBufferAsprintf(buf, " mode='%s'/>\n", mode);
        break;

    case VIR_DOMAIN_NET_TYPE_HOSTDEV:
        if (virDomainHostdevDefFormatSubsys(buf, &def->data.hostdev.def,
                                            flags, true) < 0) {
            return -1;
        }
        break;

    case VIR_DOMAIN_NET_TYPE_NETWORK:
        if (def->class_id)
            virBufferAsprintf(buf, "<class id='%u'/>", def->class_id);
        break;
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected net type %s"), type);
        return -1;
    }

    if (virNetDevVlanFormat(&def->vlan, buf) < 0)
        return -1;
    if (virNetDevVPortProfileFormat(def->virtPortProfile, buf) < 0)
        return -1;
    if (virNetDevBandwidthFormat(def->bandwidth, buf) < 0)
        return -1;

    virBufferAdjustIndent(buf, -2);
    virBufferAddLit(buf, "</actual>\n");
    return 0;
}

static int
virDomainNetDefFormat(virBufferPtr buf,
                      virDomainNetDefPtr def,
                      unsigned int flags)
{
    const char *type = virDomainNetTypeToString(def->type);
    char macstr[VIR_MAC_STRING_BUFLEN];

    if (!type) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected net type %d"), def->type);
        return -1;
    }

    virBufferAsprintf(buf, "    <interface type='%s'", type);
    if (def->type == VIR_DOMAIN_NET_TYPE_HOSTDEV &&
        def->data.hostdev.def.managed) {
        virBufferAddLit(buf, " managed='yes'");
    }
    virBufferAddLit(buf, ">\n");

    virBufferAdjustIndent(buf, 6);
    virBufferAsprintf(buf, "<mac address='%s'/>\n",
                      virMacAddrFormat(&def->mac, macstr));

    switch (def->type) {
    case VIR_DOMAIN_NET_TYPE_NETWORK:
        virBufferEscapeString(buf, "<source network='%s'",
                              def->data.network.name);
        virBufferEscapeString(buf, " portgroup='%s'",
                              def->data.network.portgroup);
        virBufferAddLit(buf, "/>\n");
        if ((flags & VIR_DOMAIN_XML_INTERNAL_ACTUAL_NET) &&
            (virDomainActualNetDefFormat(buf, def->data.network.actual, flags) < 0))
            return -1;
        break;

    case VIR_DOMAIN_NET_TYPE_ETHERNET:
        virBufferEscapeString(buf, "<source dev='%s'/>\n",
                              def->data.ethernet.dev);
        if (def->data.ethernet.ipaddr)
            virBufferAsprintf(buf, "<ip address='%s'/>\n",
                              def->data.ethernet.ipaddr);
        break;

    case VIR_DOMAIN_NET_TYPE_BRIDGE:
        virBufferEscapeString(buf, "<source bridge='%s'/>\n",
                              def->data.bridge.brname);
        if (def->data.bridge.ipaddr) {
            virBufferAsprintf(buf, "<ip address='%s'/>\n",
                              def->data.bridge.ipaddr);
        }
        break;

    case VIR_DOMAIN_NET_TYPE_SERVER:
    case VIR_DOMAIN_NET_TYPE_CLIENT:
    case VIR_DOMAIN_NET_TYPE_MCAST:
        if (def->data.socket.address) {
            virBufferAsprintf(buf, "<source address='%s' port='%d'/>\n",
                              def->data.socket.address, def->data.socket.port);
        } else {
            virBufferAsprintf(buf, "<source port='%d'/>\n",
                              def->data.socket.port);
        }
        break;

    case VIR_DOMAIN_NET_TYPE_INTERNAL:
        virBufferEscapeString(buf, "<source name='%s'/>\n",
                              def->data.internal.name);
        break;

    case VIR_DOMAIN_NET_TYPE_DIRECT:
        virBufferEscapeString(buf, "<source dev='%s'",
                              def->data.direct.linkdev);
        virBufferAsprintf(buf, " mode='%s'",
                          virNetDevMacVLanModeTypeToString(def->data.direct.mode));
        virBufferAddLit(buf, "/>\n");
        break;

    case VIR_DOMAIN_NET_TYPE_HOSTDEV:
        if (virDomainHostdevDefFormatSubsys(buf, &def->data.hostdev.def,
                                            flags, true) < 0) {
            return -1;
        }
        break;

    case VIR_DOMAIN_NET_TYPE_USER:
    case VIR_DOMAIN_NET_TYPE_LAST:
        break;
    }

    if (virNetDevVlanFormat(&def->vlan, buf) < 0)
        return -1;
    if (virNetDevVPortProfileFormat(def->virtPortProfile, buf) < 0)
        return -1;
    virBufferEscapeString(buf, "<script path='%s'/>\n",
                          def->script);
    if (def->ifname &&
        !((flags & VIR_DOMAIN_XML_INACTIVE) &&
          (STRPREFIX(def->ifname, VIR_NET_GENERATED_PREFIX)))) {
        /* Skip auto-generated target names for inactive config. */
        virBufferEscapeString(buf, "<target dev='%s'/>\n", def->ifname);
    }
    if (def->model) {
        virBufferEscapeString(buf, "<model type='%s'/>\n",
                              def->model);
        if (STREQ(def->model, "virtio") &&
            (def->driver.virtio.name || def->driver.virtio.txmode)) {
            virBufferAddLit(buf, "<driver");
            if (def->driver.virtio.name) {
                virBufferAsprintf(buf, " name='%s'",
                                  virDomainNetBackendTypeToString(def->driver.virtio.name));
            }
            if (def->driver.virtio.txmode) {
                virBufferAsprintf(buf, " txmode='%s'",
                                  virDomainNetVirtioTxModeTypeToString(def->driver.virtio.txmode));
            }
            if (def->driver.virtio.ioeventfd) {
                virBufferAsprintf(buf, " ioeventfd='%s'",
                                  virDomainIoEventFdTypeToString(def->driver.virtio.ioeventfd));
            }
            if (def->driver.virtio.event_idx) {
                virBufferAsprintf(buf, " event_idx='%s'",
                                  virDomainVirtioEventIdxTypeToString(def->driver.virtio.event_idx));
            }
            virBufferAddLit(buf, "/>\n");
        }
    }
    if (def->filter) {
        if (virNWFilterFormatParamAttributes(buf, def->filterparams,
                                             def->filter) < 0)
            return -1;
    }

    if (def->tune.sndbuf_specified) {
        virBufferAddLit(buf,   "<tune>\n");
        virBufferAsprintf(buf, "  <sndbuf>%lu</sndbuf>\n", def->tune.sndbuf);
        virBufferAddLit(buf,   "</tune>\n");
    }

    if (def->linkstate) {
        virBufferAsprintf(buf, "<link state='%s'/>\n",
                          virDomainNetInterfaceLinkStateTypeToString(def->linkstate));
    }

    if (virNetDevBandwidthFormat(def->bandwidth, buf) < 0)
        return -1;

    virBufferAdjustIndent(buf, -6);

    if (virDomainDeviceInfoFormat(buf, &def->info,
                                  flags | VIR_DOMAIN_XML_INTERNAL_ALLOW_BOOT
                                  | VIR_DOMAIN_XML_INTERNAL_ALLOW_ROM) < 0)
        return -1;

    virBufferAddLit(buf, "    </interface>\n");

    return 0;
}


/* Assumes that "<device" has already been generated, and starts
 * output at " type='type'>". */
static int
virDomainChrSourceDefFormat(virBufferPtr buf,
                            virDomainChrSourceDefPtr def,
                            bool tty_compat,
                            unsigned int flags)
{
    const char *type = virDomainChrTypeToString(def->type);

    if (!type) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected char type %d"), def->type);
        return -1;
    }

    /* Compat with legacy <console tty='/dev/pts/5'/> syntax */
    virBufferAsprintf(buf, " type='%s'", type);
    if (tty_compat) {
        virBufferEscapeString(buf, " tty='%s'",
                              def->data.file.path);
    }
    virBufferAddLit(buf, ">\n");

    switch (def->type) {
    case VIR_DOMAIN_CHR_TYPE_NULL:
    case VIR_DOMAIN_CHR_TYPE_VC:
    case VIR_DOMAIN_CHR_TYPE_STDIO:
    case VIR_DOMAIN_CHR_TYPE_SPICEVMC:
        /* nada */
        break;

    case VIR_DOMAIN_CHR_TYPE_PTY:
    case VIR_DOMAIN_CHR_TYPE_DEV:
    case VIR_DOMAIN_CHR_TYPE_FILE:
    case VIR_DOMAIN_CHR_TYPE_PIPE:
        if (def->type != VIR_DOMAIN_CHR_TYPE_PTY ||
            (def->data.file.path &&
             !(flags & VIR_DOMAIN_XML_INACTIVE))) {
            virBufferEscapeString(buf, "      <source path='%s'/>\n",
                                  def->data.file.path);
        }
        break;

    case VIR_DOMAIN_CHR_TYPE_UDP:
        if (def->data.udp.bindService &&
            def->data.udp.bindHost) {
            virBufferAsprintf(buf,
                              "      <source mode='bind' host='%s' "
                              "service='%s'/>\n",
                              def->data.udp.bindHost,
                              def->data.udp.bindService);
        } else if (def->data.udp.bindHost) {
            virBufferAsprintf(buf, "      <source mode='bind' host='%s'/>\n",
                              def->data.udp.bindHost);
        } else if (def->data.udp.bindService) {
            virBufferAsprintf(buf, "      <source mode='bind' service='%s'/>\n",
                              def->data.udp.bindService);
        }

        if (def->data.udp.connectService &&
            def->data.udp.connectHost) {
            virBufferAsprintf(buf,
                              "      <source mode='connect' host='%s' "
                              "service='%s'/>\n",
                              def->data.udp.connectHost,
                              def->data.udp.connectService);
        } else if (def->data.udp.connectHost) {
            virBufferAsprintf(buf, "      <source mode='connect' host='%s'/>\n",
                              def->data.udp.connectHost);
        } else if (def->data.udp.connectService) {
            virBufferAsprintf(buf,
                              "      <source mode='connect' service='%s'/>\n",
                              def->data.udp.connectService);
        }
        break;

    case VIR_DOMAIN_CHR_TYPE_TCP:
        virBufferAsprintf(buf,
                          "      <source mode='%s' host='%s' service='%s'/>\n",
                          def->data.tcp.listen ? "bind" : "connect",
                          def->data.tcp.host,
                          def->data.tcp.service);
        virBufferAsprintf(buf, "      <protocol type='%s'/>\n",
                          virDomainChrTcpProtocolTypeToString(
                              def->data.tcp.protocol));
        break;

    case VIR_DOMAIN_CHR_TYPE_UNIX:
        virBufferAsprintf(buf, "      <source mode='%s'",
                          def->data.nix.listen ? "bind" : "connect");
        virBufferEscapeString(buf, " path='%s'/>\n",
                              def->data.nix.path);
        break;
    }

    return 0;
}

static int
virDomainChrDefFormat(virBufferPtr buf,
                      virDomainChrDefPtr def,
                      unsigned int flags)
{
    const char *elementName = virDomainChrDeviceTypeToString(def->deviceType);
    const char *targetType = virDomainChrTargetTypeToString(def->deviceType,
                                                            def->targetType);
    bool tty_compat;
    size_t n;

    int ret = 0;

    if (!elementName) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected char device type %d"),
                       def->deviceType);
        return -1;
    }

    virBufferAsprintf(buf, "    <%s", elementName);
    tty_compat = (def->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE &&
                  def->target.port == 0 &&
                  def->source.type == VIR_DOMAIN_CHR_TYPE_PTY &&
                  !(flags & VIR_DOMAIN_XML_INACTIVE) &&
                  def->source.data.file.path);
    if (virDomainChrSourceDefFormat(buf, &def->source, tty_compat, flags) < 0)
        return -1;

    /* Format <target> block */
    switch (def->deviceType) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL: {
        if (!targetType) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Could not format channel target type"));
            return -1;
        }
        virBufferAsprintf(buf, "      <target type='%s'", targetType);

        switch (def->targetType) {
        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_GUESTFWD: {
            int port = virSocketAddrGetPort(def->target.addr);
            if (port < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Unable to format guestfwd port"));
                return -1;
            }

            const char *addr = virSocketAddrFormat(def->target.addr);
            if (addr == NULL)
                return -1;

            virBufferAsprintf(buf, " address='%s' port='%d'",
                              addr, port);
            VIR_FREE(addr);
            break;
        }

        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO: {
            if (def->target.name) {
                virBufferEscapeString(buf, " name='%s'", def->target.name);
            }
            break;
        }

        }
        virBufferAddLit(buf, "/>\n");
        break;
    }

    case VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE:
        virBufferAsprintf(buf,
                          "      <target type='%s' port='%d'/>\n",
                          virDomainChrTargetTypeToString(def->deviceType,
                                                         def->targetType),
                          def->target.port);
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL:
        if (def->targetTypeAttr) {
            virBufferAsprintf(buf,
                              "      <target type='%s' port='%d'/>\n",
                              virDomainChrTargetTypeToString(def->deviceType,
                                                             def->targetType),
                              def->target.port);
            break;
        }
    default:
        virBufferAsprintf(buf, "      <target port='%d'/>\n",
                          def->target.port);
        break;
    }

    if (virDomainDeviceInfoIsSet(&def->info, flags)) {
        if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
            return -1;
    }

    /* Security label overrides, if any. */
    if (def->seclabels && def->nseclabels > 0) {
        virBufferAdjustIndent(buf, 2);
        for (n = 0; n < def->nseclabels; n++)
            virSecurityDeviceLabelDefFormat(buf, def->seclabels[n]);
        virBufferAdjustIndent(buf, -2);
    }

    virBufferAsprintf(buf, "    </%s>\n", elementName);

    return ret;
}

static int
virDomainSmartcardDefFormat(virBufferPtr buf,
                            virDomainSmartcardDefPtr def,
                            unsigned int flags)
{
    const char *mode = virDomainSmartcardTypeToString(def->type);
    size_t i;

    if (!mode) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected smartcard type %d"), def->type);
        return -1;
    }

    virBufferAsprintf(buf, "    <smartcard mode='%s'", mode);
    switch (def->type) {
    case VIR_DOMAIN_SMARTCARD_TYPE_HOST:
        if (!virDomainDeviceInfoIsSet(&def->info, flags)) {
            virBufferAddLit(buf, "/>\n");
            return 0;
        }
        virBufferAddLit(buf, ">\n");
        break;

    case VIR_DOMAIN_SMARTCARD_TYPE_HOST_CERTIFICATES:
        virBufferAddLit(buf, ">\n");
        for (i = 0; i < VIR_DOMAIN_SMARTCARD_NUM_CERTIFICATES; i++)
            virBufferEscapeString(buf, "      <certificate>%s</certificate>\n",
                                  def->data.cert.file[i]);
        virBufferEscapeString(buf, "      <database>%s</database>\n",
                              def->data.cert.database);
        break;

    case VIR_DOMAIN_SMARTCARD_TYPE_PASSTHROUGH:
        if (virDomainChrSourceDefFormat(buf, &def->data.passthru, false,
                                        flags) < 0)
            return -1;
        break;

    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected smartcard type %d"), def->type);
        return -1;
    }
    if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
        return -1;
    virBufferAddLit(buf, "    </smartcard>\n");
    return 0;
}

static int
virDomainSoundCodecDefFormat(virBufferPtr buf,
                             virDomainSoundCodecDefPtr def)
{
    const char *type = virDomainSoundCodecTypeToString(def->type);

    if (!type) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected codec type %d"), def->type);
        return -1;
    }

    virBufferAsprintf(buf, "      <codec type='%s'/>\n",  type);

    return 0;
}

static int
virDomainSoundDefFormat(virBufferPtr buf,
                        virDomainSoundDefPtr def,
                        unsigned int flags)
{
    const char *model = virDomainSoundModelTypeToString(def->model);
    bool children = false;
    int i;

    if (!model) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected sound model %d"), def->model);
        return -1;
    }

    virBufferAsprintf(buf, "    <sound model='%s'",  model);

    for (i = 0; i < def->ncodecs; i++) {
        if (!children) {
            virBufferAddLit(buf, ">\n");
            children = true;
        }
        virDomainSoundCodecDefFormat(buf, def->codecs[i]);
    }

    if (virDomainDeviceInfoIsSet(&def->info, flags)) {
        if (!children) {
            virBufferAddLit(buf, ">\n");
            children = true;
        }
        if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
            return -1;
    }

    if (children) {
        virBufferAddLit(buf, "    </sound>\n");
    } else {
        virBufferAddLit(buf, "/>\n");
    }

    return 0;
}


static int
virDomainMemballoonDefFormat(virBufferPtr buf,
                             virDomainMemballoonDefPtr def,
                             unsigned int flags)
{
    const char *model = virDomainMemballoonModelTypeToString(def->model);

    if (!model) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected memballoon model %d"), def->model);
        return -1;
    }

    virBufferAsprintf(buf, "    <memballoon model='%s'", model);

    if (virDomainDeviceInfoIsSet(&def->info, flags)) {
        virBufferAddLit(buf, ">\n");
        if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
            return -1;
        virBufferAddLit(buf, "    </memballoon>\n");
    } else {
        virBufferAddLit(buf, "/>\n");
    }

    return 0;
}

static int
virDomainSysinfoDefFormat(virBufferPtr buf,
                          virSysinfoDefPtr def)
{
    int ret;
    virBufferAdjustIndent(buf, 2);
    ret = virSysinfoFormat(buf, def);
    virBufferAdjustIndent(buf, -2);
    return ret;
}


static int
virDomainWatchdogDefFormat(virBufferPtr buf,
                           virDomainWatchdogDefPtr def,
                           unsigned int flags)
{
    const char *model = virDomainWatchdogModelTypeToString(def->model);
    const char *action = virDomainWatchdogActionTypeToString(def->action);

    if (!model) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected watchdog model %d"), def->model);
        return -1;
    }

    if (!action) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected watchdog action %d"), def->action);
        return -1;
    }

    virBufferAsprintf(buf, "    <watchdog model='%s' action='%s'",
                      model, action);

    if (virDomainDeviceInfoIsSet(&def->info, flags)) {
        virBufferAddLit(buf, ">\n");
        if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
            return -1;
        virBufferAddLit(buf, "    </watchdog>\n");
    } else {
        virBufferAddLit(buf, "/>\n");
    }

    return 0;
}


static int
virDomainRNGDefFormat(virBufferPtr buf,
                      virDomainRNGDefPtr def,
                      unsigned int flags)
{
    const char *model = virDomainRNGModelTypeToString(def->model);
    const char *backend = virDomainRNGBackendTypeToString(def->backend);

    virBufferAsprintf(buf, "    <rng model='%s'>\n", model);
    if (def->rate) {
        virBufferAsprintf(buf, "      <rate bytes='%u'", def->rate);
        if (def->period)
            virBufferAsprintf(buf, " period='%u'", def->period);
        virBufferAddLit(buf, "/>\n");
    }
    virBufferAsprintf(buf, "      <backend model='%s'", backend);

    switch ((enum virDomainRNGBackend) def->backend) {
    case VIR_DOMAIN_RNG_BACKEND_RANDOM:
        if (def->source.file)
            virBufferEscapeString(buf, ">%s</backend>\n", def->source.file);
        else
            virBufferAddLit(buf, "/>\n");

        break;

    case VIR_DOMAIN_RNG_BACKEND_EGD:
        virBufferAdjustIndent(buf, 2);
        if (virDomainChrSourceDefFormat(buf, def->source.chardev,
                                        false, flags) < 0)
            return -1;
        virBufferAdjustIndent(buf, -2);
        virBufferAddLit(buf, "      </backend>\n");

    case VIR_DOMAIN_RNG_BACKEND_LAST:
        break;
    }

    virBufferAddLit(buf, "    </rng>\n");

    return 0;
}

void
virDomainRNGDefFree(virDomainRNGDefPtr def)
{
    if (!def)
        return;

    switch ((enum virDomainRNGBackend) def->backend) {
    case VIR_DOMAIN_RNG_BACKEND_RANDOM:
        VIR_FREE(def->source.file);
        break;
    case VIR_DOMAIN_RNG_BACKEND_EGD:
        virDomainChrSourceDefFree(def->source.chardev);
        break;
    case VIR_DOMAIN_RNG_BACKEND_LAST:
        break;
    }

    virDomainDeviceInfoClear(&def->info);
    VIR_FREE(def);
}

static void
virDomainVideoAccelDefFormat(virBufferPtr buf,
                             virDomainVideoAccelDefPtr def)
{
    virBufferAsprintf(buf, "        <acceleration accel3d='%s'",
                      def->support3d ? "yes" : "no");
    virBufferAsprintf(buf, " accel2d='%s'",
                      def->support2d ? "yes" : "no");
    virBufferAddLit(buf, "/>\n");
}


static int
virDomainVideoDefFormat(virBufferPtr buf,
                        virDomainVideoDefPtr def,
                        unsigned int flags)
{
    const char *model = virDomainVideoTypeToString(def->type);

    if (!model) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected video model %d"), def->type);
        return -1;
    }

    virBufferAddLit(buf, "    <video>\n");
    virBufferAsprintf(buf, "      <model type='%s'",
                      model);
    if (def->ram)
        virBufferAsprintf(buf, " ram='%u'", def->ram);
    if (def->vram)
        virBufferAsprintf(buf, " vram='%u'", def->vram);
    if (def->heads)
        virBufferAsprintf(buf, " heads='%u'", def->heads);
    if (def->primary)
        virBufferAddLit(buf, " primary='yes'");
    if (def->accel) {
        virBufferAddLit(buf, ">\n");
        virDomainVideoAccelDefFormat(buf, def->accel);
        virBufferAddLit(buf, "      </model>\n");
    } else {
        virBufferAddLit(buf, "/>\n");
    }

    if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
        return -1;

    virBufferAddLit(buf, "    </video>\n");

    return 0;
}

static int
virDomainInputDefFormat(virBufferPtr buf,
                        virDomainInputDefPtr def,
                        unsigned int flags)
{
    const char *type = virDomainInputTypeToString(def->type);
    const char *bus = virDomainInputBusTypeToString(def->bus);

    if (!type) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected input type %d"), def->type);
        return -1;
    }
    if (!bus) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected input bus type %d"), def->bus);
        return -1;
    }

    virBufferAsprintf(buf, "    <input type='%s' bus='%s'",
                      type, bus);

    if (virDomainDeviceInfoIsSet(&def->info, flags)) {
        virBufferAddLit(buf, ">\n");
        if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
            return -1;
        virBufferAddLit(buf, "    </input>\n");
    } else {
        virBufferAddLit(buf, "/>\n");
    }

    return 0;
}


static int
virDomainTimerDefFormat(virBufferPtr buf,
                        virDomainTimerDefPtr def)
{
    const char *name = virDomainTimerNameTypeToString(def->name);

    if (!name) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected timer name %d"), def->name);
        return -1;
    }
    virBufferAsprintf(buf, "    <timer name='%s'", name);

    if (def->present == 0) {
        virBufferAddLit(buf, " present='no'");
    } else if (def->present == 1) {
        virBufferAddLit(buf, " present='yes'");
    }

    if (def->tickpolicy != -1) {
        const char *tickpolicy
            = virDomainTimerTickpolicyTypeToString(def->tickpolicy);
        if (!tickpolicy) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unexpected timer tickpolicy %d"),
                           def->tickpolicy);
            return -1;
        }
        virBufferAsprintf(buf, " tickpolicy='%s'", tickpolicy);
    }

    if ((def->name == VIR_DOMAIN_TIMER_NAME_PLATFORM)
        || (def->name == VIR_DOMAIN_TIMER_NAME_RTC)) {
        if (def->track != -1) {
            const char *track
                = virDomainTimerTrackTypeToString(def->track);
            if (!track) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("unexpected timer track %d"),
                               def->track);
                return -1;
            }
            virBufferAsprintf(buf, " track='%s'", track);
        }
    }

    if (def->name == VIR_DOMAIN_TIMER_NAME_TSC) {
        if (def->frequency > 0) {
            virBufferAsprintf(buf, " frequency='%lu'", def->frequency);
        }

        if (def->mode != -1) {
            const char *mode
                = virDomainTimerModeTypeToString(def->mode);
            if (!mode) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("unexpected timer mode %d"),
                               def->mode);
                return -1;
            }
            virBufferAsprintf(buf, " mode='%s'", mode);
        }
    }

    if (def->catchup.threshold == 0 && def->catchup.slew == 0 &&
        def->catchup.limit == 0) {
        virBufferAddLit(buf, "/>\n");
    } else {
        virBufferAddLit(buf, ">\n");
        virBufferAddLit(buf, "      <catchup ");
        if (def->catchup.threshold > 0) {
            virBufferAsprintf(buf, " threshold='%lu'", def->catchup.threshold);
        }
        if (def->catchup.slew > 0) {
            virBufferAsprintf(buf, " slew='%lu'", def->catchup.slew);
        }
        if (def->catchup.limit > 0) {
            virBufferAsprintf(buf, " limit='%lu'", def->catchup.limit);
        }
        virBufferAddLit(buf, "/>\n");
        virBufferAddLit(buf, "    </timer>\n");
    }

    return 0;
}

static void
virDomainGraphicsAuthDefFormatAttr(virBufferPtr buf,
                                   virDomainGraphicsAuthDefPtr def,
                                   unsigned int flags)
{
    if (!def->passwd)
        return;

    if (flags & VIR_DOMAIN_XML_SECURE)
        virBufferEscapeString(buf, " passwd='%s'",
                              def->passwd);

    if (def->expires) {
        char strbuf[100];
        struct tm tmbuf, *tm;
        tm = gmtime_r(&def->validTo, &tmbuf);
        strftime(strbuf, sizeof(strbuf), "%Y-%m-%dT%H:%M:%S", tm);
        virBufferAsprintf(buf, " passwdValidTo='%s'", strbuf);
    }

    if (def->connected)
        virBufferEscapeString(buf, " connected='%s'",
                              virDomainGraphicsAuthConnectedTypeToString(def->connected));
}


static void
virDomainGraphicsListenDefFormat(virBufferPtr buf,
                                 virDomainGraphicsListenDefPtr def,
                                 unsigned int flags)
{
    virBufferAddLit(buf, "      <listen");

    if (def->type) {
        virBufferAsprintf(buf, " type='%s'",
                          virDomainGraphicsListenTypeToString(def->type));
    }

    if (def->address &&
        (def->type == VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS ||
         (def->type == VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK &&
          !(flags & VIR_DOMAIN_XML_INACTIVE)))) {
        /* address may also be set to show current status when type='network',
         * but we don't want to print that if INACTIVE data is requested. */
        virBufferAsprintf(buf, " address='%s'", def->address);
    }

    if (def->network &&
        (def->type == VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK)) {
        virBufferEscapeString(buf, " network='%s'", def->network);
    }

    virBufferAddLit(buf, "/>\n");
}


static int
virDomainGraphicsDefFormat(virBufferPtr buf,
                           virDomainGraphicsDefPtr def,
                           unsigned int flags)
{
    const char *type = virDomainGraphicsTypeToString(def->type);
    const char *listenAddr = NULL;
    int children = 0;
    int i;

    if (!type) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected net type %d"), def->type);
        return -1;
    }

    /* find the first listen element of type='address' and duplicate
    * its address attribute as the listen attribute of
    * <graphics>. This is done to improve backward compatibility. */
    for (i = 0; i < def->nListens; i++) {
        if (virDomainGraphicsListenGetType(def, i)
            == VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS) {
            listenAddr = virDomainGraphicsListenGetAddress(def, i);
            break;
        }
    }

    virBufferAsprintf(buf, "    <graphics type='%s'", type);

    switch (def->type) {
    case VIR_DOMAIN_GRAPHICS_TYPE_VNC:
        if (def->data.vnc.socket) {
            if (def->data.vnc.socket)
                virBufferAsprintf(buf, " socket='%s'",
                                  def->data.vnc.socket);
        } else {
            if (def->data.vnc.port &&
                (!def->data.vnc.autoport || !(flags & VIR_DOMAIN_XML_INACTIVE)))
                virBufferAsprintf(buf, " port='%d'",
                                  def->data.vnc.port);
            else if (def->data.vnc.autoport)
                virBufferAddLit(buf, " port='-1'");

            virBufferAsprintf(buf, " autoport='%s'",
                              def->data.vnc.autoport ? "yes" : "no");

            if (listenAddr)
                virBufferAsprintf(buf, " listen='%s'", listenAddr);
        }

        if (def->data.vnc.keymap)
            virBufferEscapeString(buf, " keymap='%s'",
                                  def->data.vnc.keymap);

        virDomainGraphicsAuthDefFormatAttr(buf, &def->data.vnc.auth, flags);
        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_SDL:
        if (def->data.sdl.display)
            virBufferEscapeString(buf, " display='%s'",
                                  def->data.sdl.display);

        if (def->data.sdl.xauth)
            virBufferEscapeString(buf, " xauth='%s'",
                                  def->data.sdl.xauth);
        if (def->data.sdl.fullscreen)
            virBufferAddLit(buf, " fullscreen='yes'");

        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_RDP:
        if (def->data.rdp.port)
            virBufferAsprintf(buf, " port='%d'",
                              def->data.rdp.port);
        else if (def->data.rdp.autoport)
            virBufferAddLit(buf, " port='0'");

        if (def->data.rdp.autoport)
            virBufferAsprintf(buf, " autoport='yes'");

        if (def->data.rdp.replaceUser)
            virBufferAsprintf(buf, " replaceUser='yes'");

        if (def->data.rdp.multiUser)
            virBufferAsprintf(buf, " multiUser='yes'");

        if (listenAddr)
            virBufferAsprintf(buf, " listen='%s'", listenAddr);

        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_DESKTOP:
        if (def->data.desktop.display)
            virBufferEscapeString(buf, " display='%s'",
                                  def->data.desktop.display);

        if (def->data.desktop.fullscreen)
            virBufferAddLit(buf, " fullscreen='yes'");

        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_SPICE:
        if (def->data.spice.port)
            virBufferAsprintf(buf, " port='%d'",
                              def->data.spice.port);

        if (def->data.spice.tlsPort)
            virBufferAsprintf(buf, " tlsPort='%d'",
                              def->data.spice.tlsPort);

        virBufferAsprintf(buf, " autoport='%s'",
                          def->data.spice.autoport ? "yes" : "no");

        if (listenAddr)
            virBufferAsprintf(buf, " listen='%s'", listenAddr);

        if (def->data.spice.keymap)
            virBufferEscapeString(buf, " keymap='%s'",
                                  def->data.spice.keymap);

        if (def->data.spice.defaultMode != VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_ANY)
            virBufferAsprintf(buf, " defaultMode='%s'",
              virDomainGraphicsSpiceChannelModeTypeToString(def->data.spice.defaultMode));

        virDomainGraphicsAuthDefFormatAttr(buf, &def->data.spice.auth, flags);
        break;

    }

    for (i = 0; i < def->nListens; i++) {
        if (virDomainGraphicsListenGetType(def, i)
            == VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NONE)
            continue;
        if (!children) {
            virBufferAddLit(buf, ">\n");
            children = 1;
        }
        virDomainGraphicsListenDefFormat(buf, &def->listens[i], flags);
    }

    if (def->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE) {
        for (i = 0 ; i < VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_LAST ; i++) {
            int mode = def->data.spice.channels[i];
            if (mode == VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_ANY)
                continue;

            if (!children) {
                virBufferAddLit(buf, ">\n");
                children = 1;
            }

            virBufferAsprintf(buf, "      <channel name='%s' mode='%s'/>\n",
                              virDomainGraphicsSpiceChannelNameTypeToString(i),
                              virDomainGraphicsSpiceChannelModeTypeToString(mode));
        }
        if (!children && (def->data.spice.image || def->data.spice.jpeg ||
                          def->data.spice.zlib || def->data.spice.playback ||
                          def->data.spice.streaming || def->data.spice.copypaste ||
                          def->data.spice.mousemode)) {
            virBufferAddLit(buf, ">\n");
            children = 1;
        }
        if (def->data.spice.image)
            virBufferAsprintf(buf, "      <image compression='%s'/>\n",
                              virDomainGraphicsSpiceImageCompressionTypeToString(def->data.spice.image));
        if (def->data.spice.jpeg)
            virBufferAsprintf(buf, "      <jpeg compression='%s'/>\n",
                              virDomainGraphicsSpiceJpegCompressionTypeToString(def->data.spice.jpeg));
        if (def->data.spice.zlib)
            virBufferAsprintf(buf, "      <zlib compression='%s'/>\n",
                              virDomainGraphicsSpiceZlibCompressionTypeToString(def->data.spice.zlib));
        if (def->data.spice.playback)
            virBufferAsprintf(buf, "      <playback compression='%s'/>\n",
                              virDomainGraphicsSpicePlaybackCompressionTypeToString(def->data.spice.playback));
        if (def->data.spice.streaming)
            virBufferAsprintf(buf, "      <streaming mode='%s'/>\n",
                              virDomainGraphicsSpiceStreamingModeTypeToString(def->data.spice.streaming));
        if (def->data.spice.mousemode)
            virBufferAsprintf(buf, "      <mouse mode='%s'/>\n",
                              virDomainGraphicsSpiceMouseModeTypeToString(def->data.spice.mousemode));
        if (def->data.spice.copypaste)
            virBufferAsprintf(buf, "      <clipboard copypaste='%s'/>\n",
                              virDomainGraphicsSpiceClipboardCopypasteTypeToString(def->data.spice.copypaste));
    }

    if (children) {
        virBufferAddLit(buf, "    </graphics>\n");
    } else {
        virBufferAddLit(buf, "/>\n");
    }

    return 0;
}


static int
virDomainHostdevDefFormat(virBufferPtr buf,
                          virDomainHostdevDefPtr def,
                          unsigned int flags)
{
    const char *mode = virDomainHostdevModeTypeToString(def->mode);
    const char *type;

    if (!mode) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected hostdev mode %d"), def->mode);
        return -1;
    }

    switch (def->mode) {
    case VIR_DOMAIN_HOSTDEV_MODE_SUBSYS:
        type = virDomainHostdevSubsysTypeToString(def->source.subsys.type);
        if (!type) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unexpected hostdev type %d"),
                           def->source.subsys.type);
            return -1;
        }
        break;
    case VIR_DOMAIN_HOSTDEV_MODE_CAPABILITIES:
        type = virDomainHostdevCapsTypeToString(def->source.caps.type);
        if (!type) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unexpected hostdev type %d"),
                           def->source.caps.type);
            return -1;
        }
        break;
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected hostdev mode %d"), def->mode);
        return -1;
    }

    virBufferAsprintf(buf, "    <hostdev mode='%s' type='%s'",
                      mode, type);
    if (def->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS)
        virBufferAsprintf(buf, " managed='%s'>\n",
                          def->managed ? "yes" : "no");
    else
        virBufferAddLit(buf, ">\n");

    virBufferAdjustIndent(buf, 6);
    switch (def->mode) {
    case VIR_DOMAIN_HOSTDEV_MODE_SUBSYS:
        if (virDomainHostdevDefFormatSubsys(buf, def, flags, false) < 0)
            return -1;
        break;
    case VIR_DOMAIN_HOSTDEV_MODE_CAPABILITIES:
        if (virDomainHostdevDefFormatCaps(buf, def) < 0)
            return -1;
        break;
    }
    virBufferAdjustIndent(buf, -6);

    if (virDomainDeviceInfoFormat(buf, def->info,
                                  flags | VIR_DOMAIN_XML_INTERNAL_ALLOW_BOOT
                                  | VIR_DOMAIN_XML_INTERNAL_ALLOW_ROM) < 0)
        return -1;

    virBufferAddLit(buf, "    </hostdev>\n");

    return 0;
}

static int
virDomainRedirdevDefFormat(virBufferPtr buf,
                           virDomainRedirdevDefPtr def,
                           unsigned int flags)
{
    const char *bus;

    bus = virDomainRedirdevBusTypeToString(def->bus);

    virBufferAsprintf(buf, "    <redirdev bus='%s'", bus);
    if (virDomainChrSourceDefFormat(buf, &def->source.chr, false, flags) < 0)
        return -1;
    if (virDomainDeviceInfoFormat(buf, &def->info,
                                  flags | VIR_DOMAIN_XML_INTERNAL_ALLOW_BOOT) < 0)
        return -1;
    virBufferAddLit(buf, "    </redirdev>\n");

    return 0;
}

static int
virDomainRedirFilterDefFormat(virBufferPtr buf,
                              virDomainRedirFilterDefPtr filter)
{
    size_t i;

    virBufferAddLit(buf, "    <redirfilter>\n");
    for (i = 0; i < filter->nusbdevs; i++) {
        virDomainRedirFilterUsbDevDefPtr usbdev = filter->usbdevs[i];
        virBufferAddLit(buf, "      <usbdev");
        if (usbdev->usbClass >= 0)
            virBufferAsprintf(buf, " class='0x%02X'", usbdev->usbClass);

        if (usbdev->vendor >= 0)
            virBufferAsprintf(buf, " vendor='0x%04X'", usbdev->vendor);

        if (usbdev->product >= 0)
            virBufferAsprintf(buf, " product='0x%04X'", usbdev->product);

        if (usbdev->version >= 0)
            virBufferAsprintf(buf, " version='%d.%d'",
                                 ((usbdev->version & 0xf000) >> 12) * 10 +
                                 ((usbdev->version & 0x0f00) >>  8),
                                 ((usbdev->version & 0x00f0) >>  4) * 10 +
                                 ((usbdev->version & 0x000f) >>  0));

        virBufferAsprintf(buf, " allow='%s'/>\n", usbdev->allow ? "yes" : "no");

    }
    virBufferAddLit(buf, "    </redirfilter>\n");
    return 0;
}

static int
virDomainHubDefFormat(virBufferPtr buf,
                      virDomainHubDefPtr def,
                      unsigned int flags)
{
    const char *type = virDomainHubTypeToString(def->type);

    if (!type) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected hub type %d"), def->type);
        return -1;
    }

    virBufferAsprintf(buf, "    <hub type='%s'", type);

    if (virDomainDeviceInfoIsSet(&def->info, flags)) {
        virBufferAddLit(buf, ">\n");
        if (virDomainDeviceInfoFormat(buf, &def->info, flags) < 0)
            return -1;
        virBufferAddLit(buf, "    </hub>\n");
    } else {
        virBufferAddLit(buf, "/>\n");
    }

    return 0;
}

/*
 * Return true if no <vcpupin> specified in domain XML
 * (I.e. all vcpus inherit the cpuset from "cpuset" of
 * <vcpu>). Or false otherwise.
 */
static bool
virDomainIsAllVcpupinInherited(virDomainDefPtr def)
{
    int i;

    if (!def->cpumask) {
        if (def->cputune.nvcpupin)
            return false;
        else
            return true;
    } else {
        for (i = 0; i < def->cputune.nvcpupin; i++) {
            if (!virBitmapEqual(def->cputune.vcpupin[i]->cpumask,
                                def->cpumask))
                return false;
        }

        return true;
   }
}

#define DUMPXML_FLAGS                           \
    (VIR_DOMAIN_XML_SECURE |                    \
     VIR_DOMAIN_XML_INACTIVE |                  \
     VIR_DOMAIN_XML_UPDATE_CPU |                \
     VIR_DOMAIN_XML_MIGRATABLE)

verify(((VIR_DOMAIN_XML_INTERNAL_STATUS |
         VIR_DOMAIN_XML_INTERNAL_ACTUAL_NET |
         VIR_DOMAIN_XML_INTERNAL_PCI_ORIG_STATES)
        & DUMPXML_FLAGS) == 0);

/* This internal version can accept VIR_DOMAIN_XML_INTERNAL_*,
 * whereas the public version cannot.  Also, it appends to an existing
 * buffer (possibly with auto-indent), rather than flattening to string.
 * Return -1 on failure.  */
int
virDomainDefFormatInternal(virDomainDefPtr def,
                           unsigned int flags,
                           virBufferPtr buf)
{
    unsigned char *uuid;
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    const char *type = NULL;
    int n;
    int i;
    bool blkio = false;

    virCheckFlags(DUMPXML_FLAGS |
                  VIR_DOMAIN_XML_INTERNAL_STATUS |
                  VIR_DOMAIN_XML_INTERNAL_ACTUAL_NET |
                  VIR_DOMAIN_XML_INTERNAL_PCI_ORIG_STATES,
                  -1);

    if (!(type = virDomainVirtTypeToString(def->virtType))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected domain type %d"), def->virtType);
        goto error;
    }

    if (def->id == -1)
        flags |= VIR_DOMAIN_XML_INACTIVE;

    virBufferAsprintf(buf, "<domain type='%s'", type);
    if (!(flags & VIR_DOMAIN_XML_INACTIVE))
        virBufferAsprintf(buf, " id='%d'", def->id);
    if (def->namespaceData && def->ns.href)
        virBufferAsprintf(buf, " %s", (def->ns.href)());
    virBufferAddLit(buf, ">\n");

    virBufferEscapeString(buf, "  <name>%s</name>\n", def->name);

    uuid = def->uuid;
    virUUIDFormat(uuid, uuidstr);
    virBufferAsprintf(buf, "  <uuid>%s</uuid>\n", uuidstr);

    virBufferEscapeString(buf, "  <title>%s</title>\n", def->title);

    virBufferEscapeString(buf, "  <description>%s</description>\n",
                          def->description);

    if (def->metadata) {
        xmlBufferPtr xmlbuf;
        int oldIndentTreeOutput = xmlIndentTreeOutput;

        /* Indentation on output requires that we previously set
         * xmlKeepBlanksDefault to 0 when parsing; also, libxml does 2
         * spaces per level of indentation of intermediate elements,
         * but no leading indentation before the starting element.
         * Thankfully, libxml maps what looks like globals into
         * thread-local uses, so we are thread-safe.  */
        xmlIndentTreeOutput = 1;
        xmlbuf = xmlBufferCreate();
        if (xmlNodeDump(xmlbuf, def->metadata->doc, def->metadata,
                        virBufferGetIndent(buf, false) / 2 + 1, 1) < 0) {
            xmlBufferFree(xmlbuf);
            xmlIndentTreeOutput = oldIndentTreeOutput;
            goto error;
        }
        virBufferAsprintf(buf, "  %s\n", (char *) xmlBufferContent(xmlbuf));
        xmlBufferFree(xmlbuf);
        xmlIndentTreeOutput = oldIndentTreeOutput;
    }

    virBufferAddLit(buf, "  <memory");
    if (def->mem.dump_core)
        virBufferAsprintf(buf, " dumpCore='%s'",
                          virDomainMemDumpTypeToString(def->mem.dump_core));
    virBufferAsprintf(buf, " unit='KiB'>%llu</memory>\n",
                      def->mem.max_balloon);

    virBufferAsprintf(buf, "  <currentMemory unit='KiB'>%llu</currentMemory>\n",
                      def->mem.cur_balloon);

    /* add blkiotune only if there are any */
    if (def->blkio.weight) {
        blkio = true;
    } else {
        for (n = 0; n < def->blkio.ndevices; n++) {
            if (def->blkio.devices[n].weight) {
                blkio = true;
                break;
            }
        }
    }

    if (blkio) {
        virBufferAddLit(buf, "  <blkiotune>\n");

        if (def->blkio.weight)
            virBufferAsprintf(buf, "    <weight>%u</weight>\n",
                              def->blkio.weight);

        for (n = 0; n < def->blkio.ndevices; n++) {
            if (def->blkio.devices[n].weight == 0)
                continue;
            virBufferAddLit(buf, "    <device>\n");
            virBufferEscapeString(buf, "      <path>%s</path>\n",
                                  def->blkio.devices[n].path);
            virBufferAsprintf(buf, "      <weight>%u</weight>\n",
                              def->blkio.devices[n].weight);
            virBufferAddLit(buf, "    </device>\n");
        }

        virBufferAddLit(buf, "  </blkiotune>\n");
    }

    /* add memtune only if there are any */
    if (def->mem.hard_limit || def->mem.soft_limit || def->mem.min_guarantee ||
        def->mem.swap_hard_limit)
        virBufferAddLit(buf, "  <memtune>\n");
    if (def->mem.hard_limit) {
        virBufferAsprintf(buf, "    <hard_limit unit='KiB'>"
                          "%llu</hard_limit>\n", def->mem.hard_limit);
    }
    if (def->mem.soft_limit) {
        virBufferAsprintf(buf, "    <soft_limit unit='KiB'>"
                          "%llu</soft_limit>\n", def->mem.soft_limit);
    }
    if (def->mem.min_guarantee) {
        virBufferAsprintf(buf, "    <min_guarantee unit='KiB'>"
                          "%llu</min_guarantee>\n", def->mem.min_guarantee);
    }
    if (def->mem.swap_hard_limit) {
        virBufferAsprintf(buf, "    <swap_hard_limit unit='KiB'>"
                          "%llu</swap_hard_limit>\n", def->mem.swap_hard_limit);
    }
    if (def->mem.hard_limit || def->mem.soft_limit || def->mem.min_guarantee ||
        def->mem.swap_hard_limit)
        virBufferAddLit(buf, "  </memtune>\n");

    if (def->mem.hugepage_backed) {
        virBufferStrcat(buf,
                        "  <memoryBacking>\n",
                        "    <hugepages/>\n",
                        "  </memoryBacking>\n", NULL);
    }

    virBufferAddLit(buf, "  <vcpu");
    virBufferAsprintf(buf, " placement='%s'",
                      virDomainCpuPlacementModeTypeToString(def->placement_mode));

    if (def->cpumask && !virBitmapIsAllSet(def->cpumask)) {
        char *cpumask = NULL;
        if ((cpumask = virBitmapFormat(def->cpumask)) == NULL)
            goto error;
        virBufferAsprintf(buf, " cpuset='%s'", cpumask);
        VIR_FREE(cpumask);
    }
    if (def->vcpus != def->maxvcpus)
        virBufferAsprintf(buf, " current='%u'", def->vcpus);
    virBufferAsprintf(buf, ">%u</vcpu>\n", def->maxvcpus);

    if (def->cputune.shares ||
        (def->cputune.nvcpupin && !virDomainIsAllVcpupinInherited(def)) ||
        def->cputune.period || def->cputune.quota ||
        def->cputune.emulatorpin ||
        def->cputune.emulator_period || def->cputune.emulator_quota)
        virBufferAddLit(buf, "  <cputune>\n");

    if (def->cputune.shares)
        virBufferAsprintf(buf, "    <shares>%lu</shares>\n",
                          def->cputune.shares);
    if (def->cputune.period)
        virBufferAsprintf(buf, "    <period>%llu</period>\n",
                          def->cputune.period);
    if (def->cputune.quota)
        virBufferAsprintf(buf, "    <quota>%lld</quota>\n",
                          def->cputune.quota);

    if (def->cputune.emulator_period)
        virBufferAsprintf(buf, "    <emulator_period>%llu"
                          "</emulator_period>\n",
                          def->cputune.emulator_period);

    if (def->cputune.emulator_quota)
        virBufferAsprintf(buf, "    <emulator_quota>%lld"
                          "</emulator_quota>\n",
                          def->cputune.emulator_quota);

    for (i = 0; i < def->cputune.nvcpupin; i++) {
        char *cpumask;
        /* Ignore the vcpupin which inherit from "cpuset of "<vcpu>." */
        if (def->cpumask &&
            virBitmapEqual(def->cpumask,
                           def->cputune.vcpupin[i]->cpumask))
            continue;

        virBufferAsprintf(buf, "    <vcpupin vcpu='%u' ",
                          def->cputune.vcpupin[i]->vcpuid);

        if (!(cpumask = virBitmapFormat(def->cputune.vcpupin[i]->cpumask))) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("failed to format cpuset for vcpupin"));
            goto error;
        }

        virBufferAsprintf(buf, "cpuset='%s'/>\n", cpumask);
        VIR_FREE(cpumask);
    }

    if (def->cputune.emulatorpin) {
        char *cpumask;
        virBufferAsprintf(buf, "    <emulatorpin ");

        if (!(cpumask = virBitmapFormat(def->cputune.emulatorpin->cpumask))) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("failed to format cpuset for emulator"));
                goto error;
        }

        virBufferAsprintf(buf, "cpuset='%s'/>\n", cpumask);
        VIR_FREE(cpumask);
    }
    if (def->cputune.shares ||
        (def->cputune.nvcpupin && !virDomainIsAllVcpupinInherited(def)) ||
        def->cputune.period || def->cputune.quota ||
        def->cputune.emulatorpin ||
        def->cputune.emulator_period || def->cputune.emulator_quota)
        virBufferAddLit(buf, "  </cputune>\n");

    if (def->numatune.memory.nodemask ||
        def->numatune.memory.placement_mode) {
        virBufferAddLit(buf, "  <numatune>\n");
        const char *mode;
        char *nodemask = NULL;
        const char *placement;

        mode = virDomainNumatuneMemModeTypeToString(def->numatune.memory.mode);
        virBufferAsprintf(buf, "    <memory mode='%s' ", mode);

        if (def->numatune.memory.placement_mode ==
            VIR_NUMA_TUNE_MEM_PLACEMENT_MODE_STATIC) {
            nodemask = virBitmapFormat(def->numatune.memory.nodemask);
            if (nodemask == NULL) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("failed to format nodeset for "
                                 "NUMA memory tuning"));
                goto error;
            }
            virBufferAsprintf(buf, "nodeset='%s'/>\n", nodemask);
            VIR_FREE(nodemask);
        } else if (def->numatune.memory.placement_mode) {
            placement = virNumaTuneMemPlacementModeTypeToString(def->numatune.memory.placement_mode);
            virBufferAsprintf(buf, "placement='%s'/>\n", placement);
        }
        virBufferAddLit(buf, "  </numatune>\n");
    }

    if (def->sysinfo)
        virDomainSysinfoDefFormat(buf, def->sysinfo);

    if (def->os.bootloader) {
        virBufferEscapeString(buf, "  <bootloader>%s</bootloader>\n",
                              def->os.bootloader);
        virBufferEscapeString(buf,
                              "  <bootloader_args>%s</bootloader_args>\n",
                              def->os.bootloaderArgs);
    }

    virBufferAddLit(buf, "  <os>\n");

    virBufferAddLit(buf, "    <type");
    if (def->os.arch)
        virBufferAsprintf(buf, " arch='%s'", virArchToString(def->os.arch));
    if (def->os.machine)
        virBufferAsprintf(buf, " machine='%s'", def->os.machine);
    /*
     * HACK: For xen driver we previously used bogus 'linux' as the
     * os type for paravirt, whereas capabilities declare it to
     * be 'xen'. So we convert to the former for backcompat
     */
    if (def->virtType == VIR_DOMAIN_VIRT_XEN &&
        STREQ(def->os.type, "xen"))
        virBufferAsprintf(buf, ">%s</type>\n", "linux");
    else
        virBufferAsprintf(buf, ">%s</type>\n", def->os.type);

    virBufferEscapeString(buf, "    <init>%s</init>\n",
                          def->os.init);
    for (i = 0 ; def->os.initargv && def->os.initargv[i] ; i++)
        virBufferEscapeString(buf, "    <initarg>%s</initarg>\n",
                              def->os.initargv[i]);
    virBufferEscapeString(buf, "    <loader>%s</loader>\n",
                          def->os.loader);
    virBufferEscapeString(buf, "    <kernel>%s</kernel>\n",
                          def->os.kernel);
    virBufferEscapeString(buf, "    <initrd>%s</initrd>\n",
                          def->os.initrd);
    virBufferEscapeString(buf, "    <cmdline>%s</cmdline>\n",
                          def->os.cmdline);
    virBufferEscapeString(buf, "    <dtb>%s</dtb>\n",
                          def->os.dtb);
    virBufferEscapeString(buf, "    <root>%s</root>\n",
                          def->os.root);

    if (!def->os.bootloader) {
        for (n = 0 ; n < def->os.nBootDevs ; n++) {
            const char *boottype =
                virDomainBootTypeToString(def->os.bootDevs[n]);
            if (!boottype) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("unexpected boot device type %d"),
                               def->os.bootDevs[n]);
                goto error;
            }
            virBufferAsprintf(buf, "    <boot dev='%s'/>\n", boottype);
        }

        if (def->os.bootmenu != VIR_DOMAIN_BOOT_MENU_DEFAULT) {
            const char *enabled = (def->os.bootmenu ==
                                   VIR_DOMAIN_BOOT_MENU_ENABLED ? "yes"
                                                                : "no");
            virBufferAsprintf(buf, "    <bootmenu enable='%s'/>\n", enabled);
        }

        if (def->os.bios.useserial || def->os.bios.rt_set) {
            virBufferAddLit(buf, "    <bios");
            if (def->os.bios.useserial)
                virBufferAsprintf(buf, " useserial='%s'",
                                  (def->os.bios.useserial ==
                                   VIR_DOMAIN_BIOS_USESERIAL_YES ? "yes"
                                                                   : "no"));
            if (def->os.bios.rt_set)
                virBufferAsprintf(buf, " rebootTimeout='%d'", def->os.bios.rt_delay);

            virBufferAddLit(buf, "/>\n");
        }
    }

    if (def->os.smbios_mode) {
        const char *mode;

        mode = virDomainSmbiosModeTypeToString(def->os.smbios_mode);
        if (mode == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unexpected smbios mode %d"), def->os.smbios_mode);
            goto error;
        }
        virBufferAsprintf(buf, "    <smbios mode='%s'/>\n", mode);
    }

    virBufferAddLit(buf, "  </os>\n");

    if (def->features) {
        virBufferAddLit(buf, "  <features>\n");
        for (i = 0 ; i < VIR_DOMAIN_FEATURE_LAST ; i++) {
            if (def->features & (1 << i) && i != VIR_DOMAIN_FEATURE_HYPERV) {
                const char *name = virDomainFeatureTypeToString(i);
                if (!name) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("unexpected feature %d"), i);
                    goto error;
                }
                virBufferAsprintf(buf, "    <%s", name);
                if (i == VIR_DOMAIN_FEATURE_APIC && def->apic_eoi) {
                    virBufferAsprintf(buf,
                                      " eoi='%s'",
                                      virDomainFeatureStateTypeToString(def->apic_eoi));
                }
                virBufferAddLit(buf, "/>\n");
            }
        }

        if (def->features & (1 << VIR_DOMAIN_FEATURE_HYPERV)) {
            virBufferAddLit(buf, "    <hyperv>\n");
            for (i = 0; i < VIR_DOMAIN_HYPERV_LAST; i++) {
                switch ((enum virDomainHyperv) i) {
                case VIR_DOMAIN_HYPERV_RELAXED:
                    if (def->hyperv_features[i])
                        virBufferAsprintf(buf, "      <%s state='%s'/>\n",
                                          virDomainHypervTypeToString(i),
                                          virDomainFeatureStateTypeToString(def->hyperv_features[i]));
                    break;

                case VIR_DOMAIN_HYPERV_LAST:
                    break;
                }
            }
            virBufferAddLit(buf, "    </hyperv>\n");
        }

        virBufferAddLit(buf, "  </features>\n");
    }

    virBufferAdjustIndent(buf, 2);
    if (virCPUDefFormatBufFull(buf, def->cpu, flags) < 0)
        goto error;
    virBufferAdjustIndent(buf, -2);

    virBufferAsprintf(buf, "  <clock offset='%s'",
                      virDomainClockOffsetTypeToString(def->clock.offset));
    switch (def->clock.offset) {
    case VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME:
    case VIR_DOMAIN_CLOCK_OFFSET_UTC:
        if (def->clock.data.utc_reset)
            virBufferAddLit(buf, " adjustment='reset'");
        break;
    case VIR_DOMAIN_CLOCK_OFFSET_VARIABLE:
        virBufferAsprintf(buf, " adjustment='%lld' basis='%s'",
                          def->clock.data.variable.adjustment,
                          virDomainClockBasisTypeToString(def->clock.data.variable.basis));
        break;
    case VIR_DOMAIN_CLOCK_OFFSET_TIMEZONE:
        virBufferEscapeString(buf, " timezone='%s'", def->clock.data.timezone);
        break;
    }
    if (def->clock.ntimers == 0) {
        virBufferAddLit(buf, "/>\n");
    } else {
        virBufferAddLit(buf, ">\n");
        for (n = 0; n < def->clock.ntimers; n++) {
            if (virDomainTimerDefFormat(buf, def->clock.timers[n]) < 0)
                goto error;
        }
        virBufferAddLit(buf, "  </clock>\n");
    }

    if (virDomainEventActionDefFormat(buf, def->onPoweroff,
                                      "on_poweroff",
                                      virDomainLifecycleTypeToString) < 0)
        goto error;
    if (virDomainEventActionDefFormat(buf, def->onReboot,
                                      "on_reboot",
                                      virDomainLifecycleTypeToString) < 0)
        goto error;
    if (virDomainEventActionDefFormat(buf, def->onCrash,
                                      "on_crash",
                                      virDomainLifecycleCrashTypeToString) < 0)
        goto error;
    if (def->onLockFailure != VIR_DOMAIN_LOCK_FAILURE_DEFAULT &&
        virDomainEventActionDefFormat(buf, def->onLockFailure,
                                      "on_lockfailure",
                                      virDomainLockFailureTypeToString) < 0)
        goto error;

    if (def->pm.s3 || def->pm.s4) {
        virBufferAddLit(buf, "  <pm>\n");
        if (def->pm.s3) {
            virBufferAsprintf(buf, "    <suspend-to-mem enabled='%s'/>\n",
                              virDomainPMStateTypeToString(def->pm.s3));
        }
        if (def->pm.s4) {
            virBufferAsprintf(buf, "    <suspend-to-disk enabled='%s'/>\n",
                              virDomainPMStateTypeToString(def->pm.s4));
        }
        virBufferAddLit(buf, "  </pm>\n");
    }

    virBufferAddLit(buf, "  <devices>\n");

    virBufferEscapeString(buf, "    <emulator>%s</emulator>\n",
                          def->emulator);

    for (n = 0 ; n < def->ndisks ; n++)
        if (virDomainDiskDefFormat(buf, def->disks[n], flags) < 0)
            goto error;

    for (n = 0 ; n < def->ncontrollers ; n++)
        if (virDomainControllerDefFormat(buf, def->controllers[n], flags) < 0)
            goto error;

    for (n = 0 ; n < def->nleases ; n++)
        if (virDomainLeaseDefFormat(buf, def->leases[n]) < 0)
            goto error;

    for (n = 0 ; n < def->nfss ; n++)
        if (virDomainFSDefFormat(buf, def->fss[n], flags) < 0)
            goto error;

    for (n = 0 ; n < def->nnets ; n++)
        if (virDomainNetDefFormat(buf, def->nets[n], flags) < 0)
            goto error;

    for (n = 0 ; n < def->nsmartcards ; n++)
        if (virDomainSmartcardDefFormat(buf, def->smartcards[n], flags) < 0)
            goto error;

    for (n = 0 ; n < def->nserials ; n++)
        if (virDomainChrDefFormat(buf, def->serials[n], flags) < 0)
            goto error;

    for (n = 0 ; n < def->nparallels ; n++)
        if (virDomainChrDefFormat(buf, def->parallels[n], flags) < 0)
            goto error;

    for (n = 0 ; n < def->nconsoles ; n++) {
        virDomainChrDef console;
        /* Back compat, ignore the console element for hvm guests
         * if it is type == serial
         */
        if (STREQ(def->os.type, "hvm") &&
            (def->consoles[n]->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL ||
             def->consoles[n]->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_NONE) &&
            (n < def->nserials)) {
            memcpy(&console, def->serials[n], sizeof(console));
            console.deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE;
            console.targetType = VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL;
        } else {
            memcpy(&console, def->consoles[n], sizeof(console));
        }
        if (virDomainChrDefFormat(buf, &console, flags) < 0)
            goto error;
    }
    if (STREQ(def->os.type, "hvm") &&
        def->nconsoles == 0 &&
        def->nserials > 0) {
        virDomainChrDef console;
        memcpy(&console, def->serials[n], sizeof(console));
        console.deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE;
        console.targetType = VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL;
        if (virDomainChrDefFormat(buf, &console, flags) < 0)
            goto error;
    }

    for (n = 0 ; n < def->nchannels ; n++)
        if (virDomainChrDefFormat(buf, def->channels[n], flags) < 0)
            goto error;

    for (n = 0 ; n < def->ninputs ; n++)
        if (def->inputs[n]->bus == VIR_DOMAIN_INPUT_BUS_USB &&
            virDomainInputDefFormat(buf, def->inputs[n], flags) < 0)
            goto error;

    if (def->ngraphics > 0) {
        /* If graphics is enabled, add the implicit mouse */
        virDomainInputDef autoInput = {
            VIR_DOMAIN_INPUT_TYPE_MOUSE,
            STREQ(def->os.type, "hvm") ?
            VIR_DOMAIN_INPUT_BUS_PS2 : VIR_DOMAIN_INPUT_BUS_XEN,
            { .alias = NULL },
        };

        if (virDomainInputDefFormat(buf, &autoInput, flags) < 0)
            goto error;

        for (n = 0 ; n < def->ngraphics ; n++)
            if (virDomainGraphicsDefFormat(buf, def->graphics[n], flags) < 0)
                goto error;
    }

    for (n = 0 ; n < def->nsounds ; n++)
        if (virDomainSoundDefFormat(buf, def->sounds[n], flags) < 0)
            goto error;

    for (n = 0 ; n < def->nvideos ; n++)
        if (virDomainVideoDefFormat(buf, def->videos[n], flags) < 0)
            goto error;

    for (n = 0 ; n < def->nhostdevs ; n++) {
        /* If parent.type != NONE, this is just a pointer to the
         * hostdev in a higher-level device (e.g. virDomainNetDef),
         * and will have already been formatted there.
         */
        if (def->hostdevs[n]->parent.type == VIR_DOMAIN_DEVICE_NONE &&
            virDomainHostdevDefFormat(buf, def->hostdevs[n], flags) < 0) {
            goto error;
        }
    }

    for (n = 0 ; n < def->nredirdevs ; n++)
        if (virDomainRedirdevDefFormat(buf, def->redirdevs[n], flags) < 0)
            goto error;

    if (def->redirfilter)
        virDomainRedirFilterDefFormat(buf, def->redirfilter);

    for (n = 0 ; n < def->nhubs ; n++)
        if (virDomainHubDefFormat(buf, def->hubs[n], flags) < 0)
            goto error;

    if (def->watchdog)
        virDomainWatchdogDefFormat(buf, def->watchdog, flags);

    if (def->memballoon)
        virDomainMemballoonDefFormat(buf, def->memballoon, flags);

    if (def->rng)
        virDomainRNGDefFormat(buf, def->rng, flags);

    virBufferAddLit(buf, "  </devices>\n");

    virBufferAdjustIndent(buf, 2);
    for (n = 0; n < def->nseclabels; n++)
        virSecurityLabelDefFormat(buf, def->seclabels[n]);
    virBufferAdjustIndent(buf, -2);

    if (def->namespaceData && def->ns.format) {
        if ((def->ns.format)(buf, def->namespaceData) < 0)
            goto error;
    }

    virBufferAddLit(buf, "</domain>\n");

    if (virBufferError(buf))
        goto no_memory;

    return 0;

 no_memory:
    virReportOOMError();
 error:
    virBufferFreeAndReset(buf);
    return -1;
}

char *
virDomainDefFormat(virDomainDefPtr def, unsigned int flags)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virCheckFlags(DUMPXML_FLAGS, NULL);
    if (virDomainDefFormatInternal(def, flags, &buf) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}


static char *
virDomainObjFormat(virDomainXMLOptionPtr xmlopt,
                   virDomainObjPtr obj,
                   unsigned int flags)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    int state;
    int reason;
    int i;

    state = virDomainObjGetState(obj, &reason);
    virBufferAsprintf(&buf, "<domstatus state='%s' reason='%s' pid='%lld'>\n",
                      virDomainStateTypeToString(state),
                      virDomainStateReasonToString(state, reason),
                      (long long)obj->pid);

    for (i = 0 ; i < VIR_DOMAIN_TAINT_LAST ; i++) {
        if (obj->taint & (1 << i))
            virBufferAsprintf(&buf, "  <taint flag='%s'/>\n",
                              virDomainTaintTypeToString(i));
    }

    if (xmlopt->privateData.format &&
        ((xmlopt->privateData.format)(&buf, obj->privateData)) < 0)
        goto error;

    virBufferAdjustIndent(&buf, 2);
    if (virDomainDefFormatInternal(obj->def, flags, &buf) < 0)
        goto error;
    virBufferAdjustIndent(&buf, -2);

    virBufferAddLit(&buf, "</domstatus>\n");

    if (virBufferError(&buf))
        goto no_memory;

    return virBufferContentAndReset(&buf);

no_memory:
    virReportOOMError();
error:
    virBufferFreeAndReset(&buf);
    return NULL;
}

static bool
virDomainDefHasUSB(virDomainDefPtr def)
{
    int i;

    for (i = 0; i < def->ncontrollers; i++) {
        if (def->controllers[i]->type == VIR_DOMAIN_CONTROLLER_TYPE_USB &&
            def->controllers[i]->model != VIR_DOMAIN_CONTROLLER_MODEL_USB_NONE)
            return true;
    }

    return false;
}

static bool
virDomainDeviceIsUSB(virDomainDeviceDefPtr dev)
{
    int t = dev->type;
    if ((t == VIR_DOMAIN_DEVICE_DISK &&
         dev->data.disk->bus == VIR_DOMAIN_DISK_BUS_USB) ||
        (t == VIR_DOMAIN_DEVICE_CONTROLLER &&
         dev->data.controller->type == VIR_DOMAIN_CONTROLLER_TYPE_USB) ||
        (t == VIR_DOMAIN_DEVICE_INPUT &&
         dev->data.input->type == VIR_DOMAIN_INPUT_BUS_USB) ||
        (t == VIR_DOMAIN_DEVICE_HOSTDEV &&
         dev->data.hostdev->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
         dev->data.hostdev->source.subsys.type ==
         VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB) ||
        (t == VIR_DOMAIN_DEVICE_HUB &&
         dev->data.hub->type == VIR_DOMAIN_HUB_TYPE_USB) ||
        (t == VIR_DOMAIN_DEVICE_REDIRDEV &&
         dev->data.redirdev->bus == VIR_DOMAIN_REDIRDEV_BUS_USB))
        return true;

    return false;
}

int
virDomainDefCompatibleDevice(virDomainDefPtr def,
                             virDomainDeviceDefPtr dev)
{
    if (!virDomainDefHasUSB(def) &&
        virDomainDeviceIsUSB(dev)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Device configuration is not compatible: "
                         "Domain has no USB bus support"));
        return -1;
    }

    return 0;
}

int
virDomainSaveXML(const char *configDir,
                 virDomainDefPtr def,
                 const char *xml)
{
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    char *configFile = NULL;
    int ret = -1;

    if ((configFile = virDomainConfigFile(configDir, def->name)) == NULL)
        goto cleanup;

    if (virFileMakePath(configDir) < 0) {
        virReportSystemError(errno,
                             _("cannot create config directory '%s'"),
                             configDir);
        goto cleanup;
    }

    virUUIDFormat(def->uuid, uuidstr);
    ret = virXMLSaveFile(configFile,
                         virXMLPickShellSafeComment(def->name, uuidstr), "edit",
                         xml);

 cleanup:
    VIR_FREE(configFile);
    return ret;
}

int
virDomainSaveConfig(const char *configDir,
                    virDomainDefPtr def)
{
    int ret = -1;
    char *xml;

    if (!(xml = virDomainDefFormat(def, VIR_DOMAIN_XML_WRITE_FLAGS)))
        goto cleanup;

    if (virDomainSaveXML(configDir, def, xml))
        goto cleanup;

    ret = 0;
cleanup:
    VIR_FREE(xml);
    return ret;
}

int
virDomainSaveStatus(virDomainXMLOptionPtr xmlopt,
                    const char *statusDir,
                    virDomainObjPtr obj)
{
    unsigned int flags = (VIR_DOMAIN_XML_SECURE |
                          VIR_DOMAIN_XML_INTERNAL_STATUS |
                          VIR_DOMAIN_XML_INTERNAL_ACTUAL_NET |
                          VIR_DOMAIN_XML_INTERNAL_PCI_ORIG_STATES);

    int ret = -1;
    char *xml;

    if (!(xml = virDomainObjFormat(xmlopt, obj, flags)))
        goto cleanup;

    if (virDomainSaveXML(statusDir, obj->def, xml))
        goto cleanup;

    ret = 0;
cleanup:
    VIR_FREE(xml);
    return ret;
}


static virDomainObjPtr
virDomainObjListLoadConfig(virDomainObjListPtr doms,
                           virCapsPtr caps,
                           virDomainXMLOptionPtr xmlopt,
                           const char *configDir,
                           const char *autostartDir,
                           const char *name,
                           unsigned int expectedVirtTypes,
                           virDomainLoadConfigNotify notify,
                           void *opaque)
{
    char *configFile = NULL, *autostartLink = NULL;
    virDomainDefPtr def = NULL;
    virDomainObjPtr dom;
    int autostart;
    virDomainDefPtr oldDef = NULL;

    if ((configFile = virDomainConfigFile(configDir, name)) == NULL)
        goto error;
    if (!(def = virDomainDefParseFile(configFile, caps, xmlopt,
                                      expectedVirtTypes,
                                      VIR_DOMAIN_XML_INACTIVE)))
        goto error;

    if ((autostartLink = virDomainConfigFile(autostartDir, name)) == NULL)
        goto error;

    if ((autostart = virFileLinkPointsTo(autostartLink, configFile)) < 0)
        goto error;

    if (!(dom = virDomainObjListAddLocked(doms, def, xmlopt, 0, &oldDef)))
        goto error;

    dom->autostart = autostart;

    if (notify)
        (*notify)(dom, oldDef == NULL, opaque);

    virDomainDefFree(oldDef);
    VIR_FREE(configFile);
    VIR_FREE(autostartLink);
    return dom;

error:
    VIR_FREE(configFile);
    VIR_FREE(autostartLink);
    virDomainDefFree(def);
    return NULL;
}

static virDomainObjPtr
virDomainObjListLoadStatus(virDomainObjListPtr doms,
                           const char *statusDir,
                           const char *name,
                           virCapsPtr caps,
                           virDomainXMLOptionPtr xmlopt,
                           unsigned int expectedVirtTypes,
                           virDomainLoadConfigNotify notify,
                           void *opaque)
{
    char *statusFile = NULL;
    virDomainObjPtr obj = NULL;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    if ((statusFile = virDomainConfigFile(statusDir, name)) == NULL)
        goto error;

    if (!(obj = virDomainObjParseFile(statusFile, caps, xmlopt, expectedVirtTypes,
                                      VIR_DOMAIN_XML_INTERNAL_STATUS |
                                      VIR_DOMAIN_XML_INTERNAL_ACTUAL_NET |
                                      VIR_DOMAIN_XML_INTERNAL_PCI_ORIG_STATES)))
        goto error;

    virUUIDFormat(obj->def->uuid, uuidstr);

    if (virHashLookup(doms->objs, uuidstr) != NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected domain %s already exists"),
                       obj->def->name);
        goto error;
    }

    if (virHashAddEntry(doms->objs, uuidstr, obj) < 0)
        goto error;

    if (notify)
        (*notify)(obj, 1, opaque);

    VIR_FREE(statusFile);
    return obj;

error:
    virObjectUnref(obj);
    VIR_FREE(statusFile);
    return NULL;
}

int
virDomainObjListLoadAllConfigs(virDomainObjListPtr doms,
                               const char *configDir,
                               const char *autostartDir,
                               int liveStatus,
                               virCapsPtr caps,
                               virDomainXMLOptionPtr xmlopt,
                               unsigned int expectedVirtTypes,
                               virDomainLoadConfigNotify notify,
                               void *opaque)
{
    DIR *dir;
    struct dirent *entry;

    VIR_INFO("Scanning for configs in %s", configDir);

    if (!(dir = opendir(configDir))) {
        if (errno == ENOENT)
            return 0;
        virReportSystemError(errno,
                             _("Failed to open dir '%s'"),
                             configDir);
        return -1;
    }

    virObjectLock(doms);

    while ((entry = readdir(dir))) {
        virDomainObjPtr dom;

        if (entry->d_name[0] == '.')
            continue;

        if (!virFileStripSuffix(entry->d_name, ".xml"))
            continue;

        /* NB: ignoring errors, so one malformed config doesn't
           kill the whole process */
        VIR_INFO("Loading config file '%s.xml'", entry->d_name);
        if (liveStatus)
            dom = virDomainObjListLoadStatus(doms,
                                             configDir,
                                             entry->d_name,
                                             caps,
                                             xmlopt,
                                             expectedVirtTypes,
                                             notify,
                                             opaque);
        else
            dom = virDomainObjListLoadConfig(doms,
                                             caps,
                                             xmlopt,
                                             configDir,
                                             autostartDir,
                                             entry->d_name,
                                             expectedVirtTypes,
                                             notify,
                                             opaque);
        if (dom) {
            virObjectUnlock(dom);
            if (!liveStatus)
                dom->persistent = 1;
        }
    }

    closedir(dir);
    virObjectUnlock(doms);
    return 0;
}

int
virDomainDeleteConfig(const char *configDir,
                      const char *autostartDir,
                      virDomainObjPtr dom)
{
    char *configFile = NULL, *autostartLink = NULL;
    int ret = -1;

    if ((configFile = virDomainConfigFile(configDir, dom->def->name)) == NULL)
        goto cleanup;
    if ((autostartLink = virDomainConfigFile(autostartDir,
                                             dom->def->name)) == NULL)
        goto cleanup;

    /* Not fatal if this doesn't work */
    unlink(autostartLink);

    if (unlink(configFile) < 0 &&
        errno != ENOENT) {
        virReportSystemError(errno,
                             _("cannot remove config %s"),
                             configFile);
        goto cleanup;
    }

    ret = 0;

cleanup:
    VIR_FREE(configFile);
    VIR_FREE(autostartLink);
    return ret;
}

char
*virDomainConfigFile(const char *dir,
                     const char *name)
{
    char *ret;

    if (virAsprintf(&ret, "%s/%s.xml", dir, name) < 0)
        virReportOOMError();

    return ret;
}

/* Translates a device name of the form (regex) "[fhv]d[a-z]+" into
 * the corresponding bus,index combination (e.g. sda => (0,0), sdi (1,1),
 *                                               hdd => (1,1), vdaa => (0,26))
 * @param disk The disk device
 * @param busIdx parsed bus number
 * @param devIdx parsed device number
 * @return 0 on success, -1 on failure
 */
int
virDiskNameToBusDeviceIndex(const virDomainDiskDefPtr disk,
                            int *busIdx,
                            int *devIdx) {

    int idx = virDiskNameToIndex(disk->dst);
    if (idx < 0)
        return -1;

    switch (disk->bus) {
        case VIR_DOMAIN_DISK_BUS_IDE:
            *busIdx = idx / 2;
            *devIdx = idx % 2;
            break;
        case VIR_DOMAIN_DISK_BUS_SCSI:
            *busIdx = idx / 7;
            *devIdx = idx % 7;
            break;
        case VIR_DOMAIN_DISK_BUS_FDC:
        case VIR_DOMAIN_DISK_BUS_USB:
        case VIR_DOMAIN_DISK_BUS_VIRTIO:
        case VIR_DOMAIN_DISK_BUS_XEN:
        default:
            *busIdx = 0;
            *devIdx = idx;
            break;
    }

    return 0;
}

virDomainFSDefPtr
virDomainGetRootFilesystem(virDomainDefPtr def)
{
    int i;

    for (i = 0 ; i < def->nfss ; i++) {
        if (STREQ(def->fss[i]->dst, "/"))
            return def->fss[i];
    }

    return NULL;
}


static void
virDomainObjListCountActive(void *payload,
                            const void *name ATTRIBUTE_UNUSED,
                            void *data)
{
    virDomainObjPtr obj = payload;
    int *count = data;
    virObjectLock(obj);
    if (virDomainObjIsActive(obj))
        (*count)++;
    virObjectUnlock(obj);
}

static void
virDomainObjListCountInactive(void *payload,
                              const void *name ATTRIBUTE_UNUSED,
                              void *data)
{
    virDomainObjPtr obj = payload;
    int *count = data;
    virObjectLock(obj);
    if (!virDomainObjIsActive(obj))
        (*count)++;
    virObjectUnlock(obj);
}

int
virDomainObjListNumOfDomains(virDomainObjListPtr doms,
                             int active)
{
    int count = 0;
    virObjectLock(doms);
    if (active)
        virHashForEach(doms->objs, virDomainObjListCountActive, &count);
    else
        virHashForEach(doms->objs, virDomainObjListCountInactive, &count);
    virObjectUnlock(doms);
    return count;
}

struct virDomainIDData {
    int numids;
    int maxids;
    int *ids;
};

static void
virDomainObjListCopyActiveIDs(void *payload,
                              const void *name ATTRIBUTE_UNUSED,
                              void *opaque)
{
    virDomainObjPtr obj = payload;
    struct virDomainIDData *data = opaque;
    virObjectLock(obj);
    if (virDomainObjIsActive(obj) && data->numids < data->maxids)
        data->ids[data->numids++] = obj->def->id;
    virObjectUnlock(obj);
}

int
virDomainObjListGetActiveIDs(virDomainObjListPtr doms,
                             int *ids,
                             int maxids)
{
    struct virDomainIDData data = { 0, maxids, ids };
    virObjectLock(doms);
    virHashForEach(doms->objs, virDomainObjListCopyActiveIDs, &data);
    virObjectUnlock(doms);
    return data.numids;
}

struct virDomainNameData {
    int oom;
    int numnames;
    int maxnames;
    char **const names;
};

static void
virDomainObjListCopyInactiveNames(void *payload,
                                  const void *name ATTRIBUTE_UNUSED,
                                  void *opaque)
{
    virDomainObjPtr obj = payload;
    struct virDomainNameData *data = opaque;

    if (data->oom)
        return;

    virObjectLock(obj);
    if (!virDomainObjIsActive(obj) && data->numnames < data->maxnames) {
        if (!(data->names[data->numnames] = strdup(obj->def->name)))
            data->oom = 1;
        else
            data->numnames++;
    }
    virObjectUnlock(obj);
}


int
virDomainObjListGetInactiveNames(virDomainObjListPtr doms,
                                 char **const names,
                                 int maxnames)
{
    struct virDomainNameData data = { 0, 0, maxnames, names };
    int i;
    virObjectLock(doms);
    virHashForEach(doms->objs, virDomainObjListCopyInactiveNames, &data);
    virObjectUnlock(doms);
    if (data.oom) {
        for (i = 0 ; i < data.numnames ; i++)
            VIR_FREE(data.names[i]);
        virReportOOMError();
        return -1;
    }

    return data.numnames;
}


struct virDomainListIterData {
    virDomainObjListIterator callback;
    void *opaque;
    int ret;
};

static void
virDomainObjListHelper(void *payload,
                       const void *name ATTRIBUTE_UNUSED,
                       void *opaque)
{
    struct virDomainListIterData *data = opaque;

    if (data->callback(payload, data->opaque) < 0)
        data->ret = -1;
}

int
virDomainObjListForEach(virDomainObjListPtr doms,
                        virDomainObjListIterator callback,
                        void *opaque)
{
    struct virDomainListIterData data = {
        callback, opaque, 0,
    };
    virObjectLock(doms);
    virHashForEach(doms->objs, virDomainObjListHelper, &data);
    virObjectUnlock(doms);
    return data.ret;
}


int
virDomainChrDefForeach(virDomainDefPtr def,
                       bool abortOnError,
                       virDomainChrDefIterator iter,
                       void *opaque)
{
    int i;
    int rc = 0;

    for (i = 0 ; i < def->nserials ; i++) {
        if ((iter)(def,
                   def->serials[i],
                   opaque) < 0)
            rc = -1;

        if (abortOnError && rc != 0)
            goto done;
    }

    for (i = 0 ; i < def->nparallels ; i++) {
        if ((iter)(def,
                   def->parallels[i],
                   opaque) < 0)
            rc = -1;

        if (abortOnError && rc != 0)
            goto done;
    }

    for (i = 0 ; i < def->nchannels ; i++) {
        if ((iter)(def,
                   def->channels[i],
                   opaque) < 0)
            rc = -1;

        if (abortOnError && rc != 0)
            goto done;
    }
    for (i = 0 ; i < def->nconsoles ; i++) {
        if ((iter)(def,
                   def->consoles[i],
                   opaque) < 0)
            rc = -1;

        if (abortOnError && rc != 0)
            goto done;
    }

done:
    return rc;
}


int
virDomainSmartcardDefForeach(virDomainDefPtr def,
                             bool abortOnError,
                             virDomainSmartcardDefIterator iter,
                             void *opaque)
{
    int i;
    int rc = 0;

    for (i = 0 ; i < def->nsmartcards ; i++) {
        if ((iter)(def,
                   def->smartcards[i],
                   opaque) < 0)
            rc = -1;

        if (abortOnError && rc != 0)
            goto done;
    }

done:
    return rc;
}


/* Call iter(disk, name, depth, opaque) for each element of disk and
 * its backing chain in the pre-populated disk->backingChain.
 * ignoreOpenFailure determines whether to warn about a chain that
 * mentions a backing file without also having metadata on that
 * file.  */
int
virDomainDiskDefForeachPath(virDomainDiskDefPtr disk,
                            bool ignoreOpenFailure,
                            virDomainDiskDefPathIterator iter,
                            void *opaque)
{
    int ret = -1;
    size_t depth = 0;
    virStorageFileMetadata *tmp;

    if (!disk->src || disk->type == VIR_DOMAIN_DISK_TYPE_NETWORK)
        return 0;

    if (iter(disk, disk->src, 0, opaque) < 0)
        goto cleanup;

    tmp = disk->backingChain;
    while (tmp && tmp->backingStoreIsFile) {
        if (!ignoreOpenFailure && !tmp->backingMeta) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unable to visit backing chain file %s"),
                           tmp->backingStore);
            goto cleanup;
        }
        if (iter(disk, tmp->backingStore, ++depth, opaque) < 0)
            goto cleanup;
        tmp = tmp->backingMeta;
    }

    ret = 0;

cleanup:
    return ret;
}


/* Copy src into a new definition; with the quality of the copy
 * depending on the migratable flag (false for transitions between
 * persistent and active, true for transitions across save files or
 * snapshots).  */
virDomainDefPtr
virDomainDefCopy(virDomainDefPtr src,
                 virCapsPtr caps,
                 virDomainXMLOptionPtr xmlopt,
                 bool migratable)
{
    char *xml;
    virDomainDefPtr ret;
    unsigned int write_flags = VIR_DOMAIN_XML_WRITE_FLAGS;
    unsigned int read_flags = VIR_DOMAIN_XML_READ_FLAGS;

    if (migratable)
        write_flags |= VIR_DOMAIN_XML_INACTIVE | VIR_DOMAIN_XML_MIGRATABLE;

    /* Easiest to clone via a round-trip through XML.  */
    if (!(xml = virDomainDefFormat(src, write_flags)))
        return NULL;

    ret = virDomainDefParseString(xml, caps, xmlopt, -1, read_flags);

    VIR_FREE(xml);
    return ret;
}

virDomainDefPtr
virDomainObjCopyPersistentDef(virDomainObjPtr dom,
                              virCapsPtr caps,
                              virDomainXMLOptionPtr xmlopt)
{
    virDomainDefPtr cur;

    cur = virDomainObjGetPersistentDef(caps, xmlopt, dom);
    return virDomainDefCopy(cur, caps, xmlopt, false);
}


virDomainState
virDomainObjGetState(virDomainObjPtr dom, int *reason)
{
    if (reason)
        *reason = dom->state.reason;

    return dom->state.state;
}


void
virDomainObjSetState(virDomainObjPtr dom, virDomainState state, int reason)
{
    int last = -1;

    switch (state) {
    case VIR_DOMAIN_NOSTATE:
        last = VIR_DOMAIN_NOSTATE_LAST;
        break;
    case VIR_DOMAIN_RUNNING:
        last = VIR_DOMAIN_RUNNING_LAST;
        break;
    case VIR_DOMAIN_BLOCKED:
        last = VIR_DOMAIN_BLOCKED_LAST;
        break;
    case VIR_DOMAIN_PAUSED:
        last = VIR_DOMAIN_PAUSED_LAST;
        break;
    case VIR_DOMAIN_SHUTDOWN:
        last = VIR_DOMAIN_SHUTDOWN_LAST;
        break;
    case VIR_DOMAIN_SHUTOFF:
        last = VIR_DOMAIN_SHUTOFF_LAST;
        break;
    case VIR_DOMAIN_CRASHED:
        last = VIR_DOMAIN_CRASHED_LAST;
        break;
    case VIR_DOMAIN_PMSUSPENDED:
        last = VIR_DOMAIN_PMSUSPENDED_LAST;
        break;
    default:
        last = -1;
    }

    if (last < 0) {
        VIR_ERROR(_("invalid domain state: %d"), state);
        return;
    }

    dom->state.state = state;
    if (reason > 0 && reason < last)
        dom->state.reason = reason;
    else
        dom->state.reason = 0;
}


const char *
virDomainStateReasonToString(virDomainState state, int reason)
{
    switch (state) {
    case VIR_DOMAIN_NOSTATE:
        return virDomainNostateReasonTypeToString(reason);
    case VIR_DOMAIN_RUNNING:
        return virDomainRunningReasonTypeToString(reason);
    case VIR_DOMAIN_BLOCKED:
        return virDomainBlockedReasonTypeToString(reason);
    case VIR_DOMAIN_PAUSED:
        return virDomainPausedReasonTypeToString(reason);
    case VIR_DOMAIN_SHUTDOWN:
        return virDomainShutdownReasonTypeToString(reason);
    case VIR_DOMAIN_SHUTOFF:
        return virDomainShutoffReasonTypeToString(reason);
    case VIR_DOMAIN_CRASHED:
        return virDomainCrashedReasonTypeToString(reason);
    case VIR_DOMAIN_PMSUSPENDED:
        return virDomainPMSuspendedReasonTypeToString(reason);
    case VIR_DOMAIN_LAST:
        break;
    }
    VIR_WARN("Unexpected domain state: %d", state);
    return NULL;
}


int
virDomainStateReasonFromString(virDomainState state, const char *reason)
{
    switch (state) {
    case VIR_DOMAIN_NOSTATE:
        return virDomainNostateReasonTypeFromString(reason);
    case VIR_DOMAIN_RUNNING:
        return virDomainRunningReasonTypeFromString(reason);
    case VIR_DOMAIN_BLOCKED:
        return virDomainBlockedReasonTypeFromString(reason);
    case VIR_DOMAIN_PAUSED:
        return virDomainPausedReasonTypeFromString(reason);
    case VIR_DOMAIN_SHUTDOWN:
        return virDomainShutdownReasonTypeFromString(reason);
    case VIR_DOMAIN_SHUTOFF:
        return virDomainShutoffReasonTypeFromString(reason);
    case VIR_DOMAIN_CRASHED:
        return virDomainCrashedReasonTypeFromString(reason);
    case VIR_DOMAIN_PMSUSPENDED:
        return virDomainPMSuspendedReasonTypeFromString(reason);
    case VIR_DOMAIN_LAST:
        break;
    }
    VIR_WARN("Unexpected domain state: %d", state);
    return -1;
}


/* Some access functions to gloss over the difference between NetDef
 * (<interface>) and ActualNetDef (<actual>). If the NetDef has an
 * ActualNetDef, return the requested value from the ActualNetDef,
 * otherwise return the value from the NetDef.
 */

int
virDomainNetGetActualType(virDomainNetDefPtr iface)
{
    if (iface->type != VIR_DOMAIN_NET_TYPE_NETWORK)
        return iface->type;
    if (!iface->data.network.actual)
        return iface->type;
    return iface->data.network.actual->type;
}

const char *
virDomainNetGetActualBridgeName(virDomainNetDefPtr iface)
{
    if (iface->type == VIR_DOMAIN_NET_TYPE_BRIDGE)
        return iface->data.bridge.brname;
    if (iface->type == VIR_DOMAIN_NET_TYPE_NETWORK &&
        iface->data.network.actual &&
        iface->data.network.actual->type == VIR_DOMAIN_NET_TYPE_BRIDGE) {
        return iface->data.network.actual->data.bridge.brname;
    }
    return NULL;
}

const char *
virDomainNetGetActualDirectDev(virDomainNetDefPtr iface)
{
    if (iface->type == VIR_DOMAIN_NET_TYPE_DIRECT)
        return iface->data.direct.linkdev;
    if (iface->type == VIR_DOMAIN_NET_TYPE_NETWORK &&
        iface->data.network.actual &&
        iface->data.network.actual->type == VIR_DOMAIN_NET_TYPE_DIRECT) {
        return iface->data.network.actual->data.direct.linkdev;
    }
    return NULL;
}

int
virDomainNetGetActualDirectMode(virDomainNetDefPtr iface)
{
    if (iface->type == VIR_DOMAIN_NET_TYPE_DIRECT)
        return iface->data.direct.mode;
    if (iface->type == VIR_DOMAIN_NET_TYPE_NETWORK &&
        iface->data.network.actual &&
        iface->data.network.actual->type == VIR_DOMAIN_NET_TYPE_DIRECT) {
        return iface->data.network.actual->data.direct.mode;
    }
    return 0;
}

virDomainHostdevDefPtr
virDomainNetGetActualHostdev(virDomainNetDefPtr iface)
{
    if (iface->type == VIR_DOMAIN_NET_TYPE_HOSTDEV)
        return &iface->data.hostdev.def;
    if (iface->type == VIR_DOMAIN_NET_TYPE_NETWORK &&
        iface->data.network.actual->type == VIR_DOMAIN_NET_TYPE_HOSTDEV) {
        return &iface->data.network.actual->data.hostdev.def;
    }
    return NULL;
}

virNetDevVPortProfilePtr
virDomainNetGetActualVirtPortProfile(virDomainNetDefPtr iface)
{
    switch (iface->type) {
    case VIR_DOMAIN_NET_TYPE_DIRECT:
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
    case VIR_DOMAIN_NET_TYPE_HOSTDEV:
        return iface->virtPortProfile;
    case VIR_DOMAIN_NET_TYPE_NETWORK:
        if (!iface->data.network.actual)
            return NULL;
        switch (iface->data.network.actual->type) {
        case VIR_DOMAIN_NET_TYPE_DIRECT:
        case VIR_DOMAIN_NET_TYPE_BRIDGE:
        case VIR_DOMAIN_NET_TYPE_HOSTDEV:
            return iface->data.network.actual->virtPortProfile;
        default:
            return NULL;
        }
    default:
        return NULL;
    }
}

virNetDevBandwidthPtr
virDomainNetGetActualBandwidth(virDomainNetDefPtr iface)
{
    if (iface->type == VIR_DOMAIN_NET_TYPE_NETWORK &&
        iface->data.network.actual && iface->data.network.actual->bandwidth) {
        return iface->data.network.actual->bandwidth;
    }
    return iface->bandwidth;
}

virNetDevVlanPtr
virDomainNetGetActualVlan(virDomainNetDefPtr iface)
{
    if (iface->type == VIR_DOMAIN_NET_TYPE_NETWORK &&
        iface->data.network.actual &&
        iface->data.network.actual->vlan.nTags > 0)
        return &iface->data.network.actual->vlan;
    if (iface->vlan.nTags > 0)
        return &iface->vlan;
    return 0;
}

/* Return listens[ii] from the appropriate union for the graphics
 * type, or NULL if this is an unsuitable type, or the index is out of
 * bounds. If force0 is TRUE, ii == 0, and there is no listen array,
 * allocate one with a single item. */
static virDomainGraphicsListenDefPtr
virDomainGraphicsGetListen(virDomainGraphicsDefPtr def, size_t ii, bool force0)
{
    if (def->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC ||
        def->type == VIR_DOMAIN_GRAPHICS_TYPE_RDP ||
        def->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE) {

        if (!def->listens && (ii == 0) && force0) {
            if (VIR_ALLOC(def->listens) < 0)
                virReportOOMError();
            else
                def->nListens = 1;
        }

        if (!def->listens || (def->nListens <= ii))
            return NULL;

        return &def->listens[ii];
    }

    /* it's a type that has no listens array */
    return NULL;
}


/* Access functions for the fields in a virDomainGraphicsDef's
 * "listens" array.
 *
 * NB: For simple backward compatibility with existing code, any of
 * the "Set" functions will auto-create listens[0] to store the new
 * setting, when necessary. Auto-creation beyond the first item is not
 * supported.
 *
 * Return values: All "Get" functions return the requested item, or
 * 0/NULL. (in the case of returned const char *, the caller should
 * make a copy if they want to keep it around). All "Set" functions
 * return 0 on success, -1 on failure. */

int
virDomainGraphicsListenGetType(virDomainGraphicsDefPtr def, size_t ii)
{
    virDomainGraphicsListenDefPtr listenInfo
        = virDomainGraphicsGetListen(def, ii, false);

    if (!listenInfo)
        return VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NONE;
    return listenInfo->type;
}


/* NB: This function assumes type has not previously been set. It
 * *will not* free any existing address or network based on a change
 * in value of type. */
int
virDomainGraphicsListenSetType(virDomainGraphicsDefPtr def, size_t ii, int val)
{
    virDomainGraphicsListenDefPtr listenInfo
        = virDomainGraphicsGetListen(def, ii, true);

    if (!listenInfo)
        return -1;
    listenInfo->type = val;
    return 0;
}


const char *
virDomainGraphicsListenGetAddress(virDomainGraphicsDefPtr def, size_t ii)
{
    virDomainGraphicsListenDefPtr listenInfo
        = virDomainGraphicsGetListen(def, ii, false);

    if (!listenInfo ||
        (listenInfo->type != VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS))
        return NULL;
    return listenInfo->address;
}


/* Make a copy of up to len characters of address, and store it in
 * listens[ii].address. If setType is true, set the listen's type
 * to 'address', otherwise leave type alone. */
int
virDomainGraphicsListenSetAddress(virDomainGraphicsDefPtr def,
                                  size_t ii, const char *address,
                                  int len, bool setType)
{
    virDomainGraphicsListenDefPtr listenInfo
        = virDomainGraphicsGetListen(def, ii, true);

    if (!listenInfo)
        return -1;

    if (setType)
        listenInfo->type = VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS;

    if (!address) {
        listenInfo->address = NULL;
        return 0;
    }

    listenInfo->address = (len == -1) ? strdup(address) : strndup(address, len);
    if (!listenInfo->address) {
        virReportOOMError();
        return -1;
    }

    return 0;
}


const char *
virDomainGraphicsListenGetNetwork(virDomainGraphicsDefPtr def, size_t ii)
{
    virDomainGraphicsListenDefPtr listenInfo
        = virDomainGraphicsGetListen(def, ii, false);

    if (!listenInfo ||
        (listenInfo->type != VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK))
        return NULL;
    return listenInfo->network;
}


/* Make a copy of up to len characters of address, and store it in
 * listens[ii].network */
int
virDomainGraphicsListenSetNetwork(virDomainGraphicsDefPtr def,
                                  size_t ii, const char *network, int len)
{
    virDomainGraphicsListenDefPtr listenInfo
        = virDomainGraphicsGetListen(def, ii, true);

    if (!listenInfo)
        return -1;

    listenInfo->type = VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK;

    if (!network) {
        listenInfo->network = NULL;
        return 0;
    }

    listenInfo->network = (len == -1) ? strdup(network) : strndup(network, len);
    if (!listenInfo->network) {
        virReportOOMError();
        return -1;
    }

    return 0;
}

/**
 * virDomainNetFind:
 * @def: domain's def
 * @device: could be the interface name or MAC address
 *
 * Finds a domain's net def, given the interface name or MAC address
 *
 * Returns a pointer to the net def or NULL if not found.
 */
virDomainNetDefPtr
virDomainNetFind(virDomainDefPtr def, const char *device)
{
    bool isMac = false;
    virDomainNetDefPtr net = NULL;
    virMacAddr mac;
    int i;

    if (virMacAddrParse(device, &mac) == 0)
        isMac = true;

    if (isMac) {
        for (i = 0; i < def->nnets; i++) {
            if (virMacAddrCmp(&mac, &def->nets[i]->mac) == 0) {
                net = def->nets[i];
                break;
            }
        }
    } else { /* ifname */
        for (i = 0; i < def->nnets; i++) {
            if (STREQ_NULLABLE(device, def->nets[i]->ifname)) {
                net = def->nets[i];
                break;
            }
        }
    }

    return net;
}

/**
 * virDomainDeviceDefCopy:
 * @caps: Capabilities
 * @def: Domain definition to which @src belongs
 * @src: source to be copied
 *
 * virDomainDeviceDefCopy does a deep copy of only the parts of a
 * DeviceDef that are valid when just the flag VIR_DOMAIN_XML_INACTIVE is
 * set. This means that any part of the device xml that is conditionally
 * parsed/formatted based on some other flag being set (or on the INACTIVE
 * flag being reset) *will not* be copied to the destination. Caveat emptor.
 *
 * Returns a pointer to copied @src or NULL in case of error.
 */
virDomainDeviceDefPtr
virDomainDeviceDefCopy(virDomainDeviceDefPtr src,
                       const virDomainDefPtr def,
                       virCapsPtr caps,
                       virDomainXMLOptionPtr xmlopt)
{
    virDomainDeviceDefPtr ret = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    int flags = VIR_DOMAIN_XML_INACTIVE;
    char *xmlStr = NULL;
    int rc = -1;

    switch ((virDomainDeviceType) src->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        rc = virDomainDiskDefFormat(&buf, src->data.disk, flags);
        break;
    case VIR_DOMAIN_DEVICE_LEASE:
        rc = virDomainLeaseDefFormat(&buf, src->data.lease);
        break;
    case VIR_DOMAIN_DEVICE_FS:
        rc = virDomainFSDefFormat(&buf, src->data.fs, flags);
        break;
    case VIR_DOMAIN_DEVICE_NET:
        rc = virDomainNetDefFormat(&buf, src->data.net, flags);
        break;
    case VIR_DOMAIN_DEVICE_INPUT:
        rc = virDomainInputDefFormat(&buf, src->data.input, flags);
        break;
    case VIR_DOMAIN_DEVICE_SOUND:
        rc = virDomainSoundDefFormat(&buf, src->data.sound, flags);
        break;
    case VIR_DOMAIN_DEVICE_VIDEO:
        rc = virDomainVideoDefFormat(&buf, src->data.video, flags);
        break;
    case VIR_DOMAIN_DEVICE_HOSTDEV:
        rc = virDomainHostdevDefFormat(&buf, src->data.hostdev, flags);
        break;
    case VIR_DOMAIN_DEVICE_WATCHDOG:
        rc = virDomainWatchdogDefFormat(&buf, src->data.watchdog, flags);
        break;
    case VIR_DOMAIN_DEVICE_CONTROLLER:
        rc = virDomainControllerDefFormat(&buf, src->data.controller, flags);
        break;
    case VIR_DOMAIN_DEVICE_GRAPHICS:
        rc = virDomainGraphicsDefFormat(&buf, src->data.graphics, flags);
        break;
    case VIR_DOMAIN_DEVICE_HUB:
        rc = virDomainHubDefFormat(&buf, src->data.hub, flags);
        break;
    case VIR_DOMAIN_DEVICE_REDIRDEV:
        rc = virDomainRedirdevDefFormat(&buf, src->data.redirdev, flags);
        break;
    case VIR_DOMAIN_DEVICE_RNG:
        rc = virDomainRNGDefFormat(&buf, src->data.rng, flags);
        break;
    case VIR_DOMAIN_DEVICE_NONE:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_CHR:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_LAST:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Copying definition of '%d' type "
                         "is not implemented yet."),
                       src->type);
        goto cleanup;
    }

    if (rc < 0)
        goto cleanup;

    xmlStr = virBufferContentAndReset(&buf);
    ret = virDomainDeviceDefParse(xmlStr, def, caps, xmlopt, flags);

cleanup:
    VIR_FREE(xmlStr);
    return ret;
}

struct virDomainListData {
    virConnectPtr conn;
    virDomainPtr *domains;
    unsigned int flags;
    int ndomains;
    bool error;
};

#define MATCH(FLAG) (data->flags & (FLAG))
static void
virDomainListPopulate(void *payload,
                      const void *name ATTRIBUTE_UNUSED,
                      void *opaque)
{
    struct virDomainListData *data = opaque;
    virDomainObjPtr vm = payload;
    virDomainPtr dom;

    if (data->error)
        return;

    virObjectLock(vm);
    /* check if the domain matches the filter */

    /* filter by active state */
    if (MATCH(VIR_CONNECT_LIST_DOMAINS_FILTERS_ACTIVE) &&
        !((MATCH(VIR_CONNECT_LIST_DOMAINS_ACTIVE) &&
           virDomainObjIsActive(vm)) ||
          (MATCH(VIR_CONNECT_LIST_DOMAINS_INACTIVE) &&
           !virDomainObjIsActive(vm))))
        goto cleanup;

    /* filter by persistence */
    if (MATCH(VIR_CONNECT_LIST_DOMAINS_FILTERS_PERSISTENT) &&
        !((MATCH(VIR_CONNECT_LIST_DOMAINS_PERSISTENT) &&
           vm->persistent) ||
          (MATCH(VIR_CONNECT_LIST_DOMAINS_TRANSIENT) &&
           !vm->persistent)))
        goto cleanup;

    /* filter by domain state */
    if (MATCH(VIR_CONNECT_LIST_DOMAINS_FILTERS_STATE)) {
        int st = virDomainObjGetState(vm, NULL);
        if (!((MATCH(VIR_CONNECT_LIST_DOMAINS_RUNNING) &&
               st == VIR_DOMAIN_RUNNING) ||
              (MATCH(VIR_CONNECT_LIST_DOMAINS_PAUSED) &&
               st == VIR_DOMAIN_PAUSED) ||
              (MATCH(VIR_CONNECT_LIST_DOMAINS_SHUTOFF) &&
               st == VIR_DOMAIN_SHUTOFF) ||
              (MATCH(VIR_CONNECT_LIST_DOMAINS_OTHER) &&
               (st != VIR_DOMAIN_RUNNING &&
                st != VIR_DOMAIN_PAUSED &&
                st != VIR_DOMAIN_SHUTOFF))))
            goto cleanup;
    }

    /* filter by existence of managed save state */
    if (MATCH(VIR_CONNECT_LIST_DOMAINS_FILTERS_MANAGEDSAVE) &&
        !((MATCH(VIR_CONNECT_LIST_DOMAINS_MANAGEDSAVE) &&
           vm->hasManagedSave) ||
          (MATCH(VIR_CONNECT_LIST_DOMAINS_NO_MANAGEDSAVE) &&
           !vm->hasManagedSave)))
            goto cleanup;

    /* filter by autostart option */
    if (MATCH(VIR_CONNECT_LIST_DOMAINS_FILTERS_AUTOSTART) &&
        !((MATCH(VIR_CONNECT_LIST_DOMAINS_AUTOSTART) && vm->autostart) ||
          (MATCH(VIR_CONNECT_LIST_DOMAINS_NO_AUTOSTART) && !vm->autostart)))
        goto cleanup;

    /* filter by snapshot existence */
    if (MATCH(VIR_CONNECT_LIST_DOMAINS_FILTERS_SNAPSHOT)) {
        int nsnap = virDomainSnapshotObjListNum(vm->snapshots, NULL, 0);
        if (!((MATCH(VIR_CONNECT_LIST_DOMAINS_HAS_SNAPSHOT) && nsnap > 0) ||
              (MATCH(VIR_CONNECT_LIST_DOMAINS_NO_SNAPSHOT) && nsnap <= 0)))
            goto cleanup;
    }

    /* just count the machines */
    if (!data->domains) {
        data->ndomains++;
        return;
    }

    if (!(dom = virGetDomain(data->conn, vm->def->name, vm->def->uuid))) {
        data->error = true;
        goto cleanup;
    }

    dom->id = vm->def->id;

    data->domains[data->ndomains++] = dom;

cleanup:
    virObjectUnlock(vm);
    return;
}
#undef MATCH

int
virDomainObjListExport(virDomainObjListPtr doms,
                       virConnectPtr conn,
                       virDomainPtr **domains,
                       unsigned int flags)
{
    int ret = -1;
    int i;

    struct virDomainListData data = { conn, NULL, flags, 0, false };

    virObjectLock(doms);
    if (domains) {
        if (VIR_ALLOC_N(data.domains, virHashSize(doms->objs) + 1) < 0) {
            virReportOOMError();
            goto cleanup;
        }
    }

    virHashForEach(doms->objs, virDomainListPopulate, &data);

    if (data.error)
        goto cleanup;

    if (data.domains) {
        /* trim the array to the final size */
        ignore_value(VIR_REALLOC_N(data.domains, data.ndomains + 1));
        *domains = data.domains;
        data.domains = NULL;
    }

    ret = data.ndomains;

cleanup:
    if (data.domains) {
        int count = virHashSize(doms->objs);
        for (i = 0; i < count; i++)
            virObjectUnref(data.domains[i]);
    }

    VIR_FREE(data.domains);
    virObjectUnlock(doms);
    return ret;
}

virSecurityLabelDefPtr
virDomainDefGetSecurityLabelDef(virDomainDefPtr def, const char *model)
{
    int i;
    virSecurityLabelDefPtr seclabel = NULL;

    if (def == NULL || model == NULL)
        return NULL;

    for (i = 0; i < def->nseclabels; i++) {
        if (def->seclabels[i]->model == NULL)
            continue;
        if (STREQ(def->seclabels[i]->model, model))
            return def->seclabels[i];
    }

    return seclabel;
}

virSecurityDeviceLabelDefPtr
virDomainDiskDefGetSecurityLabelDef(virDomainDiskDefPtr def, const char *model)
{
    int i;

    if (def == NULL)
        return NULL;

    for (i = 0; i < def->nseclabels; i++) {
        if (STREQ_NULLABLE(def->seclabels[i]->model, model))
            return def->seclabels[i];
    }
    return NULL;
}

virSecurityDeviceLabelDefPtr
virDomainChrDefGetSecurityLabelDef(virDomainChrDefPtr def, const char *model)
{
    int i;

    if (def == NULL)
        return NULL;

    for (i = 0; i < def->nseclabels; i++) {
        if (STREQ_NULLABLE(def->seclabels[i]->model, model))
            return def->seclabels[i];
    }
    return NULL;
}

virSecurityLabelDefPtr
virDomainDefGenSecurityLabelDef(const char *model)
{
    virSecurityLabelDefPtr seclabel = NULL;

    if (VIR_ALLOC(seclabel) < 0 ||
        (model && !(seclabel->model = strdup(model)))) {
        virReportOOMError();
        virSecurityLabelDefFree(seclabel);
        seclabel = NULL;
    }

    return seclabel;
}

virSecurityDeviceLabelDefPtr
virDomainDiskDefGenSecurityLabelDef(const char *model)
{
    virSecurityDeviceLabelDefPtr seclabel = NULL;

    if (VIR_ALLOC(seclabel) < 0 ||
        (model && !(seclabel->model = strdup(model)))) {
        virReportOOMError();
        virSecurityDeviceLabelDefFree(seclabel);
        seclabel = NULL;
    }

    return seclabel;
}
