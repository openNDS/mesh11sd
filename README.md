## 1. The mesh11sd project

  Mesh11sd is a dynamic parameter configuration daemon for 802.11s mesh networks.

  It was originally designed to leverage 802.11s mesh networking at Captive Portal venues.

  This is the open source version and it enables easy and automated mesh network operation with multiple mesh nodes.

  This version does not require a Captive Portal to be running.

## 2. Overview

**Auto_configuration:**

  From version 3 onwards, by default, Mesh11sd checks the wireless configuration for mesh options including a mesh capable version of the wpad package.

  If a suitable version of wpad is installed (eg wpad-mesh-mbedtls), mesh11sd adds options to the wireless configuration to bring up 802.11s mesh interfaces, if they are not already present.

**Mesh Parameters:**

  Mesh11sd allows all mesh parameters that are supported by the wireless driver to be set in the mesh11sd uci config file.

  Changes to parameter settings take effect immediately without having to restart the wireless network and the default settings give rapid and reliable layer 2 mesh convergence.

  The settings in the uci wireless config are acted upon at boot time or when the wireless network is restarted and are implemented *before* the mesh interface has reached an active state

  However, many mesh configuration parameters can only be set *after* the mesh interface becomes active.

  Without mesh11sd, many mesh parameters cannot be set in the uci wireless config file as the mesh interface must be up before the parameters can be set. Some of those that are supported, would fail to be implemented when the network is (re)started resulting in errors or dropped nodes.

  The mesh11sd daemon dynamically checks and sets parameters set in its config file, overriding any that are set in the wireless config. Parameters must be be set in the mesh11sd config.

Depending on the wireless drivers/hardware, mesh parameters set in the wireless config may not only fail on startup, but may also generate errors and in the worst case may cause the wireless driver to crash.
For this reason it is recommended that all parameter settings are moved from the wireless config and placed into the mesh11sd config.

**Meshnode Types:**

The mesh contains three types of meshnodes.

  1. **Peer Node** - the basic mesh peer - capable of mac-routing layer 2 packets in the mesh network.
  2. **Gateway Node** - a peer node that also hosts an access point (AP) radio for normal client devices to connect to.
  3. **Portal Node** - a peer node that also hosts a layer 3 routed upstream connection (eg an Internet feed)

It is possible for a Portal node to also be a Gateway node (ie it hosts an AP as well as an upstream connection.

**Auto Channel Tracking:**

From version 3 onwards, all Peer and Gateway nodes will track the wireless channel that the Portal node is using. If the Portal node changes its working channel, this will be detected and tracked autonomously by downstream meshnodes.

## 3. Installation

It is assumed that the additional dependencies for encrypted mesh are also installed, ie:

    Remove - wpad-basic-mbedtls (or wpad-basic or wpad-basic-wolfssl)

    Install - wpad-mesh-mbedtls
         or - wpad-mbedtls
         or - wpad-mesh-wolfssl
         or - wpad-wolfssl
         or - wpad-mesh-openssl
         or - wpad-openssl

Installation is achieved in the usual way for OpenWrt, either using the Luci UI, or the opkg command line utility.

Example:
```
    opkg update
    opkg install mesh11sd
```

## 4. Configuration
No configuration is necessary for a basic mesh network.
The mesh11sd package is designed to work immediately the package is installed on a fresh reflash of OpenWrt firmware.

Mesh11sd will add required wireless mesh configuration autonomously. It is recommended that this is not done manually.

However if the mesh network interface is defined in the wireless configuration, this will be used. If it is not defined, Mesh11sd will add the required options dynamically.

By default, auto-configuration is enabled.

**NOTE:** ***Mesh11sd cannot be configured using the OpenWrt Luci UI, and its configuration will not appear in the Luci wireless pages, even when mesh11sd is active.***

A *typical* and ***optional*** manual mesh interface configuration in /etc/config/wireless would look something like:

    config wifi-iface 'mesh0'
        option device 'radio0'
	    option mode 'mesh'
	    option encryption 'sae'
	    option key 'secretmeshkey'
	    option disabled '0'
	    option network 'lan'
	    option mesh_id 'PublicFreeMesh'

**NOTE:** It is essential that all meshnodes are configured to use the same radio channel, the same key and the same mesh_id. By default, Mesh11sd will do this for you.

**NOTE:** Adding your own mesh interface configuration is optional. Mesh11sd will create one for you automatically generating a randomised mesh ID and sha256 key as mentioned in NOTE1 above.

### Default configuration file (/etc/config/mesh11sd):

```
config mesh11sd 'setup'
	###########################################################################################
	# enabled (optional)
	# Enables or disables the mesh11sd daemon
	# Default: 1 (enabled)
	#
	# Set to 0 to disable
	#
	#option enabled '1'

	###########################################################################################
	# debuglevel (optional)
	# Sets the debuglevel
	# Default: 1 (Notification)
	# Options are 0, silent, 1 notification, 2 info and 3 debug
	#
	#option debuglevel '2'

	###########################################################################################
	# checkinterval (optional)
	# Sets the dynamic configuration checkinterval in seconds
	# Default: 10 (seconds)
	#
	#option checkinterval '15'

	###########################################################################################
	# interface_timeout (optional)
	# Sets the interface timeout interval in seconds
	# Default: 10 (seconds)
	#
	#option interface_timeout '10'

	###########################################################################################
	# mesh_basename (optional)
	# The first 4 characters after non alphanumerics are removed are used as the mesh_basename
	# The mesh_basename is used to construct a unique mesh interface name of the form m-xxxx-n
	# Default: 11s
	# Results in ifname=m-11s-0 for the first mesh interface
	# Example: link
	# Results in ifname=m-link-0
	#
	#option mesh_basename 'link'

	###########################################################################################
	# mesh_gate_enable (optional)
	# Determines whether this node will be a gate
	#
	# Default: 1 (enabled)
	# Set to 0 to disable
	#
	#option mesh_gate_enable '0'

	###########################################################################################
	# mesh_path_cost (optional)
	# sets the STP cost of the mesh network
	# Default: 0
	# Can be set to any value from 0 to 65534
	# Setting to 0 disables STP
	#
	# Example:
	#option mesh_path_cost '100'

	###########################################################################################
	# portal_detect (optional)
	# Detect if the meshnode is a portal, meaning it has an upstream wan link.
	# If the upstream link is active, the router hosting the meshnode will serve ipv4 dhcp into the mesh network.
	# If the upstream link is not connected, dhcp will be disabled and the meshnode will function as a level 2 bridge on the mesh network.
	# If portal_detect is disabled, the meshnode will be forced into portal mode.
	# Default 1 (enabled). Set to 0 to disable.
	#
	#option portal_detect '0'

	###########################################################################################
	# portal_channel (optional) Applies to 2.4 GHz band only
	#
	# If portal_detect is disabled (0), portal_channel can be set to:
	# 1. auto - a channel is auto selected
	# 2. default - the channel defined in /etc/config/wireless is used
	# 3. A valid 2.4 GHz channel (1 to 13, depending on the country setting)
	#
	# Default: default
	#
	# All mesh peer and mesh gate nodes will autonomously track the mesh portal channel
	# regardless of the configured auto_mesh_band
	# 
	#option portal_channel 'auto'
	# or
	#option portal_channel '4'

	###########################################################################################
	# auto_config (optional)
	# Auto configure mesh interfaces in the wireless configuration.
	# Default 1 (enabled). Set to 0 to disable.
	#
	#option auto_config '0'

	###########################################################################################
	# auto_mesh_id (optional)
	# Configure the mesh_id of the wireless interface(s) when auto_config is enabled
	# Default --__
	#
	# If set, it must also be set to the same value on every mesh node
	#
	#option auto_mesh_id 'MyMeshID'

	###########################################################################################
	# auto_mesh_band (optional)
	# Configure the band to use for the mesh network
	# Valid values: 2g, 5g, 6g, 60g
	# Default 2g
	#
	# If set, it must also be set to the same value on every mesh node
	#
	# All mesh peer and mesh gate nodes will autonomously track the mesh portal channel
	# regardless of the configured auto_mesh_band
	#
	#option auto_mesh_band '5g'

	###########################################################################################
	# auto_mesh_key (optional)
	# Defaults to a sha256 key to be automatically used on all members of this mesh when auto_config is enabled
	# Generates a sha256 key from the value set in this option.
	#
	# If set, it must also be set to the same value on every mesh node
	#
	#option auto_mesh_key "MySecretKey"

	###########################################################################################
	# auto_mesh_network (optional)
	# Set the network the mesh interface will bind to (eg lan, guestlan etc) when auto_config is enabled
	# Default lan
	#
	#option auto_mesh_network 'guest'

	###########################################################################################
	# txpower (optional)
	# Set the mesh radio transmit power in dBm.

	# Default - use driver default or value set in wireless config
	# Values outside the limits defined by the regulatory domain will be ignored
	#
	#option txpower '15'

	###########################################################################################
	# ssid_suffix_enable (optional)
	# Add a 4 digit suffix to the ssid
	# The 4 digits are the last 4 digits of the mac address of the mesh interface
	# Default 1 (enabled)
	#
	#option ssid_suffix_enable '0'

config mesh11sd 'mesh_params'
	# A minimum set of parameters is automatically set for initial startup and do not have to be configured here
	#
	# Any mesh parameter supported by the wireless driver can be specified here
	# and will be dynamically set and continuously checked every "checkinterval"
	# 
	# Examples:
	#option mesh_rssi_threshold '-70'
	#option mesh_max_peer_links '20'

	#
	# The command: "mesh11sd status" gives a full list of supported parameters.
```
All mesh parameter settings in the config file are dynamic and will take effect immediately.

**NOTE:** From version 3 onwards, the setup option `portal_detect` is enabled by default.

**NOTE:** If the setup option `portal_detect` is disabled, the meshnode will be forced into Portal mode. ie it will act as a layer 3 router between its wan and lan ports regardless of the availability of an upstream feed.

The option portal_detect much simplifies the setup of the meshnodes of a network. Each can be configured as a basic router with a mesh interface defined as above. Once mesh11sd is installed, portal detection will be activated and with the upstream wan port connected, the meshnode will continue to function as a router with the additional functionality of a mesh portal.

When the upstream wan connection is disconnected, the meshnode will automatically reconfigure itself as a layer 2 peer meshnode.

This means that all meshnodes can be the same basic router configuration and once moved to the required location, will autonomously reconfigure.

Access to the remote meshnode peers will not be possible using the ipv4 address as this will be disabled. Remote management can be achieved by using the `mesh11sd connect` and `mesh11sd copy` commands, or alternatively by reconnecting the wan port to an upstream feed.


## 5. Setup Options
* enabled - 0=disabled, 1=enabled. Default 1

* debuglevel - 0=silent, 1=notice, 2=info, 3=debug. Default 1

* checkinterval - the interval in seconds after which changes in parameters are detected and activated. Default 10 seconds

* portal_detect - (optional) - Detect if the meshnode is a portal, meaning it has an upstream wan link. If the upstream link is active, the router hosting the meshnode will serve ipv4 dhcp into the mesh network. If the upstream link is not connected, dhcp will be disabled and the meshnode will function as a level 2 bridge on the mesh network. Default 1 (enabled). Set to 0 to disable.

* portal_channel - valid only when portal detect is disabled.

* interface_timeout - the time in seconds that mesh11sd will wait for a mesh interface to establish before continuing. Default 10 seconds

* mesh_path_cost - sets the STP cost of the mesh network. Default: 0. Can be set to any value from 0 to 65534. Setting to 0 disables STP

* auto_config - (optional) - autonomously configures the mesh network. Default 1 (enabled). Set to 0 to disable

* auto_mesh_id - (optional) - specifies a string used to generate the mesh id hash. If set, this must be the same on all mesh nodes. Default --__

* auto_mesh_band - (optional) - specifies the wireless band to be used for the mesh network. Possible values are 2g, 5g, 6g and 60g. Default 2g

* auto_mesh_key - specifies a seed value string to be used in generating the mesh key hash used for mesh encryption. If set, this must be the same on all meshnodes. Default not set

* auto_mesh_network - (optional) - specifies the firewall zone used for the mesh. Typical values "lan", "guest" etc. This can be set differently on each meshnode as required. Firewall zone "wan" is not valid. Default lan

* mesh_basename - (optional) The first 4 characters after non alphanumerics (ie special characters) are removed are used as the mesh_basename. The mesh_basename is used to construct a unique mesh interface name of the form m-xxxx-n. Default: 11s

* mesh_gate_enable - enables any access points configured on the meshnode. Default 1 (enabled). Set to 0 to disable. **Note:** If there is an interface level "disable option" (in wireless config), mesh11sd will use that setting.

* txpower - set the mesh radio transmit power in dBm. Takes effect immediately.

* ssid_suffix_enable - Add a 4 digit suffix to the ssid. The 4 digits are the last 4 digits of the mac address of the mesh interface.

**Example**:
Set the debuglevel to 2

        uci set mesh11sd.setup.debuglevel='2'

Debuglevel will be set immediately to level 2 "info" and will remain set until changed again or a reboot occurs.

Changes can be made permanent with the following command:

        uci commit mesh11sd

## 6. Mesh Parameter Options


Mesh parameters can be changed only while the mesh is active.

Here is a list of available parameters and their function:

 * mesh_retry_timeout: the initial retry timeout in millisecond units used by the Mesh Peering Open message

 * mesh_confirm_timeout: the initial retry timeout in millisecond units used by the Mesh Peering Open message

 * mesh_holding_timeout: the confirm timeout in millisecond units used by the mesh peering management to close a mesh peering

 * mesh_max_peer_links: the maximum number of peer links allowed on this mesh interface

 * mesh_max_retries: the maximum number of peer link open retries that can be sent to establish a new peer link instance in a mesh

 * mesh_ttl: the value of TTL field set at a source mesh STA (STAtion)

 * mesh_element_ttl: the value of TTL field set at a mesh STA for path selection elements

 * mesh_auto_open_plinks: whether peer links should be automatically opened when compatible mesh peers are detected [deprecated - most implementations hard coded to enabled]

 * mesh_sync_offset_max_neighor: (note the odd spelling)- the maximum number of neighbors to synchronize to

 * mesh_hwmp_max_preq_retries: the number of action frames containing a PREQ (PeerREQuest) that an originator mesh STA can send to a particular path target

 * mesh_path_refresh_time: how frequently to refresh mesh paths in milliseconds

 * mesh_min_discovery_timeout: the minimum length of time to wait until giving up on a path discovery in milliseconds

 * mesh_hwmp_active_path_timeout: the time in milliseconds for which mesh STAs receiving a PREQ shall consider the forwarding information from the root to be valid.

 * mesh_hwmp_preq_min_interval: the minimum interval of time in milliseconds during which a mesh STA can send only one action frame containing a PREQ element

 * mesh_hwmp_net_diameter_traversal_time: the interval of time in milliseconds that it takes for an HWMP (Hybrid Wireless Mesh Protocol) information element to propagate across the mesh

 * mesh_hwmp_rootmode: the configuration of a mesh STA as root mesh STA

 * mesh_hwmp_rann_interval: the interval of time in milliseconds between root announcements (rann - RootANNouncement)

 * mesh_gate_announcements: whether to advertise that this mesh station has access to a broader network beyond the MBSS (Mesh Basic Service Set, a self-contained network of mesh stations that share a mesh profile)

 * mesh_fwding: whether the Mesh STA is forwarding or non-forwarding

 * mesh_rssi_threshold: the threshold for average signal strength of candidate station to establish a peer link

 * mesh_hwmp_active_path_to_root_timeout: The time in milliseconds for which mesh STAs receiving a proactive PREQ shall consider the forwarding information to the root mesh STA to be valid

 * mesh_hwmp_root_interval: The interval of time in milliseconds between proactive PREQs

 * mesh_hwmp_confirmation_interval: The minimum interval of time in milliseconds during which a mesh STA can send only one Action frame containing a PREQ element for root path confirmation

 * mesh_power_mode: The default mesh power save mode which will be the initial setting for new peer links

 * mesh_awake_window: The duration in milliseconds the STA will remain awake after transmitting its beacon

 * mesh_plink_timeout: If no tx activity is seen from a peered STA for longer than this time (in seconds), then remove it from the STA's list of peers.  Default is 0, equating to 30 minutes

 * mesh_connected_to_as: if set to true then this mesh STA will advertise in the mesh station information field that it is connected to a captive portal authentication server, or in the simplest case, an upstream router

 * mesh_connected_to_gate: if set to true then this mesh STA will advertise in the mesh station information field that it is connected to a separate network infrastucture such as a wireless network or downstream router

 * mesh_nolearn: Try to avoid multi-hop path discovery if the destination is a direct neighbour. Note that this will not be optimal as multi-hop mac-routes will not be discovered. If using this setting, disable mesh forwarding and use another mesh routing protocol

**Acronyms used**

TTL - Time To Live

STA - STAtion

PREQ - PeerREQuest

HWMP - Hybrid Wireless Mesh Protocol

RANN - Root ANNouncement

RSSI - Received Signal Strength Indication


**The Config File**

Basic mesh parameters are already included in the default config file. These basic parameters ensure the mesh interface will be able to either seed a new mesh or join an existing one of the same mesh id.

**Example of Setting a Config Option**: Set mesh_rssi_threshold to -75 dBm (decibels relative to one milliwatt)

        uci set mesh11sd.mesh_params.mesh_rssi_threshold='-75'

The mesh_rssi_threshold will be set immediately to this new value and will remain set until changed again or a reboot occurs.

Changes can be made permanent with the following command:

        uci commit mesh11sd

See "Command Line Interface" for obtaining a list of possible mesh parameters for a mesh interface.

## 7. Command Line Interface
Mesh11sd is an OpenWrt service daemon and runs continuously in the background. It does however also have a CLI interface:

      Usage: mesh11sd [option] [argument...]]

        Option: -h --help help
          Returns: This help information

        Option: -v --version version
          Returns: The mesh11sd version

        Option: debuglevel
          Argument: 0 - silent, 1 - notice, 2 - info, 3 - debug
          Returns: The mesh11sd debug level

        Option: enable
          Returns: "1" and exit code 0 if successful, exit code 1 if was already enabled

        Option: disable
          Returns: "0" and exit code 0 if successful, exit code 1 if was already disabled

        Option: status
          Returns: the mesh status in json format

  	    Option: connect
		  Connect a remote terminal session on a remote meshnode
		  Usage: mesh11sd connect [remote_meshnode_macaddress]
 		 	If the remote meshnode mac address is omitted, a list of meshnode mac addresses available for connection is listed.

        Option: copy
		  Copy a file to /tmp/ on a remote meshnode
		  Usage: mesh11sd copy [remote_meshnode_macaddress] [path_of_source_file]
			If the remote meshnode mac address is null, or both arguments are omitted, a list of meshnode mac addresses available for copy is listed.

**Example status output:**

```
{
  "setup":{
    "version":"3.0.0beta",
    "enabled":"1",
    "procd_status":"running",
    "portal_detect":"1",
    "portal_channel":"default",
    "mesh_basename":"m-11s-",
    "auto_config":"1",
    "auto_mesh_network":"lan",
    "auto_mesh_band":"2g40",
    "auto_mesh_id":"92d490daf46cfe534c56ddd669297e",
    "mesh_gate_enable":"1",
    "mesh_path_cost":"1",
    "checkinterval":"10",
    "interface_timeout":"10",
    "debuglevel":"3"
  }
  "interfaces":{
    "m-11s-0":{
      "mesh_retry_timeout":"100",
      "mesh_confirm_timeout":"100",
      "mesh_holding_timeout":"100",
      "mesh_max_peer_links":"16",
      "mesh_max_retries":"3",
      "mesh_ttl":"31",
      "mesh_element_ttl":"31",
      "mesh_auto_open_plinks":"0",
      "mesh_hwmp_max_preq_retries":"4",
      "mesh_path_refresh_time":"1000",
      "mesh_min_discovery_timeout":"100",
      "mesh_hwmp_active_path_timeout":"5000",
      "mesh_hwmp_preq_min_interval":"10",
      "mesh_hwmp_net_diameter_traversal_time":"50",
      "mesh_hwmp_rootmode":"4",
      "mesh_hwmp_rann_interval":"5000",
      "mesh_gate_announcements":"1",
      "mesh_fwding":"1",
      "mesh_sync_offset_max_neighor":"50",
      "mesh_rssi_threshold":"-80",
      "mesh_hwmp_active_path_to_root_timeout":"6000",
      "mesh_hwmp_root_interval":"5000",
      "mesh_hwmp_confirmation_interval":"2000",
      "mesh_power_mode":"active",
      "mesh_awake_window":"10",
      "mesh_plink_timeout":"0",
      "mesh_connected_to_gate":"1",
      "mesh_nolearn":"0",
      "mesh_connected_to_as":"1",
      "mesh_id":"92d490daf46cfe534c56ddd669297e",
      "device":"radio0",
      "channel":"1",
      "tx_packets":"83312",
      "tx_bytes":"98170667",
      "rx_packets":"46822",
      "rx_bytes":"4359233",
      "this_node":"94:83:c4:36:85:ea",
      "active_peers":"3",
      "peers":{
        "94:83:c4:08:09:f9":{
          "next_hop":"94:83:c4:08:09:f9"
        },
        "e4:95:6e:42:4f:93":{
          "next_hop":"e4:95:6e:42:4f:93"
        },
        "94:83:c4:09:5c:00":{
          "next_hop":"94:83:c4:09:5c:00"
        }
      }
      "active_stations":"3",
      "stations":{
        "dc:0e:a1:35:5c:43":{
          "proxy_node":"94:83:c4:08:09:f9"
        },
        "74:de:2b:b1:70:33":{
          "proxy_node":"e4:95:6e:42:4f:93"
        },
        "84:38:38:98:38:1a":{
          "proxy_node":"94:83:c4:08:09:f9"
        }
      }
    }
  }
}
```

**Example of using copy and connect:**

***Get a list of meshnodes:***

```
root@meshnode-c525:~# mesh11sd connect
===========================================================================
 Connect a remote terminal session on a remote meshnode
    Usage: mesh11sd connect [remote_meshnode_macaddress]

 Waiting for node list to build * * * * *

 If the node you are looking for is not in the list - re-run this command.
===========================================================================
 The following meshnodes are available for remote connection:
	e4-95-6e-44-60-2e	[ ip address: fe80::e695:6eff:fe44:602e ]
	94-83-c4-20-77-4e	[ ip address: fe80::9683:c4ff:fe20:774e ]
	94-83-c4-29-f5-a4	[ ip address: fe80::9683:c4ff:fe29:f5a4 ]
	94-83-c4-08-14-83	[ ip address: fe80::9683:c4ff:fe08:1483 ]
	94-83-c4-17-16-ad	[ ip address: fe80::9683:c4ff:fe17:16ad ]
	94-83-c4-2e-ef-d0	[ ip address: fe80::9683:c4ff:fe2e:efd0 ]
	e4-95-6e-4a-43-e5	[ ip address: fe80::e695:6eff:fe4a:43e5 ]
	e4-95-6e-45-1a-14	[ ip address: fe80::e695:6eff:fe45:1a14 ]
	94-83-c4-36-85-ea	[ ip address: fe80::9683:c4ff:fe36:85ea ]
===========================================================================
root@meshnode-c525:~#


```

***Select a meshnode and a new package version:***


```

root@meshnode-c525:~# mesh11sd copy 94-83-c4-08-14-83 /tmp/mesh11sd_3.0.1beta-1_all.ipk
===========================================================================
 Copy a file to /tmp on a remote meshnode
    Usage: mesh11sd copy [remote_meshnode_macaddress] [path_of_source_file]

 Waiting for node list to build * * * * *


Trying to copy to meshnode "94-83-c4-08-14-83".....
root@fe80::9683:c4ff:fe08:1483%br-lan's password:

mesh11sd_3.0.1beta-1_all.ipk    100%   15KB  14.7KB/s   00:00

Disconnected from meshnode "94-83-c4-08-14-83"

===========================================================================

root@meshnode-c525:~#



```

***Connect to the selected node and check the file was transferred:***

```

root@meshnode-c525:~# mesh11sd connect 94-83-c4-08-14-83
===========================================================================
 Connect a remote terminal session on a remote meshnode
    Usage: mesh11sd connect [remote_meshnode_macaddress]

 Waiting for node list to build * * * * *


Trying to connect to meshnode "94-83-c4-08-14-83".....
root@fe80::9683:c4ff:fe08:1483%br-lan's password:


BusyBox v1.36.1 (2023-11-14 13:38:11 UTC) built-in shell (ash)

  _______                     ________        __
 |       |.-----.-----.-----.|  |  |  |.----.|  |_
 |   -   ||  _  |  -__|     ||  |  |  ||   _||   _|
 |_______||   __|_____|__|__||________||__|  |____|
          |__| W I R E L E S S   F R E E D O M
 -----------------------------------------------------
 OpenWrt 23.05.2, r23630-842932a63d
 -----------------------------------------------------
root@meshnode-1483:~# ll /tmp
drwxrwxrwt   18 root     root           540 Feb 11 09:46 ./
drwxr-xr-x    1 root     root             0 Jan  1  1970 ../
drwx------    2 root     root           120 Jan 19 18:54 .uci/
----------    1 root     root             0 Jan 19 18:58 .ujailnoafile
-rw-r--r--    1 root     root             4 Jan 19 18:58 TZ
-rw-r--r--    1 root     root           985 Jan  1  1970 board.json
-rw-r--r--    1 root     root            48 Feb 11 09:46 devicemac
-rw-r--r--    1 root     root             0 Jan 19 18:54 dhcp.leases
-rw-r--r--    1 root     root            51 Feb 11 09:46 dhcp6probe
-rw-r--r--    1 root     root           877 Feb 11 09:45 dhcp6probe.prev
drwxr-xr-x    2 root     root            40 Jan 19 18:54 dnsmasq.d/
drwxr-xr-x    3 root     root            80 Jan 19 18:58 etc/
drwxr-xr-x    2 root     root            60 Jan 19 18:58 hosts/
drwxr-xr-x    3 root     root            60 Jan 19 18:54 lib/
drwxrwxrwt    2 root     root           420 Jan 19 18:54 lock/
drwxr-xr-x    2 root     root            80 Jan 19 18:54 log/
-rw-r--r--    1 root     root         15072 Feb 11 09:41 mesh11sd_3.0.1beta-1_all.ipk
drwxr-xr-x    2 root     root            40 Jan 19 18:54 opkg-lists/
drwxr-xr-x    2 root     root            40 Jan  1  1970 overlay/
-rw-r--r--    1 root     root            47 Jan 19 18:58 resolv.conf
drwxr-xr-x    2 root     root            60 Jan 19 18:57 resolv.conf.d/
drwxr-xr-x    7 root     root           340 Jan 19 18:57 run/
drwxrwxrwt    2 root     root            40 Jan  1  1970 shm/
drwxr-xr-x    2 root     root            80 Feb 10 10:07 state/
drwxr-xr-x    2 root     root            80 Jan  1  1970 sysinfo/
drwxr-xr-x    2 root     root            40 Jan 19 18:54 tmp/
drwxr-xr-x    3 root     root            60 Jan 19 18:54 usr/
root@meshnode-1483:~# ^C

root@meshnode-1483:~#



```
