## 1. The mesh11sd project

  Mesh11sd is a dynamic parameter configuration daemon for 802.11s mesh networks.

  It was originally designed to leverage 802.11s mesh networking at Captive Portal venues.

  This is the open source version and it enables easy and automated mesh network operation with multiple mesh nodes.

  This version does not require a Captive Portal to be running.

## 2. Overview

  Mesh11sd allows all mesh parameters that are supported by the wireless driver to be set in the mesh11sd uci config file.

  Settings take effect immediately without having to restart the wireless network and the default settings give rapid and reliable layer 2 mesh convergence.

  The settings in the uci wireless config are acted upon at boot time or when the wireless network is restarted and are implemented *before* the mesh interface has reached an active state

  However, many mesh configuration parameters can only be set *after* the mesh interface becomes active.

  Without mesh11sd, many mesh parameters cannot be set in the uci wireless config file as the mesh interface must be up before the parameters can be set. Some of those that are supported, would fail to be implemented when the network is (re)started resulting in errors or dropped nodes.

  The mesh11sd daemon dynamically checks parameters that are set in the wireless config and sets them as required. Parameters can also be set in the mesh11sd config and these settings will override any mesh parameter settings in the wireless config.

Depending on the wireless drivers/hardware, mesh parameters set in the wireless config may not only fail on startup, but may also generate errors and in the worst case may cause the wireless driver to crash.
For this reason it is recommended that all parameter settings are moved from the wireless config and placed instead into the mesh11sd config.

## 3. Installation
Installation is achieved in the usual way for OpenWrt, either using the Luci UI, or the opkg command line utility.

Example:

    opkg update
    opkg install mesh11sd


## 4. Configuration
It is assumed that the mesh network interface is defined in the wireless configuration.

It is also assumed that the additional dependencies for encrypted mesh are also installed, ie:

    Remove - wpad-basic-wolfssl (or wpad-basic or wpad-basic-mbedtls)

    Install - wpad-mesh-mbedtls
         or - wpad-mbedtls
         or - wpad-mesh-wolfssl
         or - wpad-wolfssl
         or - wpad-mesh-openssl
         or - wpad-openssl

A *typical* mesh interface configuration in /etc/config/wireless would look something like:

    config wifi-iface 'mesh0'
        option device 'radio0'
	    option mode 'mesh'
	    option encryption 'sae'
	    option key 'secretmeshkey'
	    option disabled '0'
	    option network 'lan'
	    option mesh_id 'PublicFreeMesh'

**NOTE:** It is essential that all meshnodes are configured to use the same radio channel, the same key and the same mesh_id.

Default configuration file (/etc/config/mesh11sd):

    config mesh11sd 'setup'
	    option enabled '1'
	    option debuglevel '1'
	    option checkinterval '10'
	    option interface_timeout '10'

    config mesh11sd 'mesh_params'
	    option mesh_fwding '1'
	    option mesh_rssi_threshold '-70'
	    option mesh_gate_announcements '1'
	    option mesh_hwmp_rootmode '3'
	    option mesh_max_peer_links '8'

All settings in the config file are dynamic and will take effect immediately.

**NOTE:** From version 2 onwards, the setup option `portal_detect` is enabled by default.

This much simplifies the setup of the meshnodes of a network. Each can be configured as a basic router with a mesh interface defined as above. Once mesh11sd is installed, portal detection will be activated and with the upstream wan port connected, the meshnode will continue to function as a router with the additional functionality of a mesh portal.

When the upstream wan connection is disconnected, the meshnode will automatically reconfigure itself as a layer 2 peer meshnode.

This means that all meshnodes can be the same basic router configuration and once moved to the required location, will autonomously reconfigure.

Access to the remote meshnode peers will not be possible using the ipv4 address as this will be disabled. Remote management can be achieved by using the `mesh11sd connect` and `mesh11sd copy` commands, or alternatively by reconnecting the wan port to an upstream feed.


## 5. Setup Options
* enabled - 0=disabled, 1=enabled. Default 1

* debuglevel - 0=silent, 1=notice, 2=info, 3=debug. Default 1

* checkinterval - the interval in seconds after which changes in parameters are detected and activated. Default 10 seconds

* interface_timeout - the time in seconds that mesh11sd will wait for a mesh interface to establish before continuing. Default 10 seconds

* mesh_basename - (optional) The first 4 characters after non alphanumerics (ie special characters) are removed are used as the mesh_basename. The mesh_basename is used to construct a unique mesh interface name of the form m-xxxx-n. Default: 11s

* portal_detect - (optional) - Detect if the meshnode is a portal, meaning it has an upstream wan link. If the upstream link is active, the router hosting the meshnode will serve ipv4 dhcp into the mesh network. If the upstream link is not connected, dhcp will be disabled and the meshnode will function as a level 2 bridge on the mesh network. Default 1 (enabled). Set to 0 to disable.

**Example**:
Set the debuglevel to 2

        uci set mesh11sd.setup.debuglevel='2'

Debuglevel will be set immediately to level 2 "info" and will remain set until changed again or a reboot occurs.

Changes can be made permanent with the following command:

        uci commit mesh11sd

## 6. Mesh Parameter Options
Basic mesh parameters are already included in the default config file. These basic parameters ensure the mesh interface will be able to either seed a new mesh or join an existing one of the same mesh id.

* mesh_fwding - packets will be forwarded to peer mesh nodes

* mesh_rssi_threshold - the minimum received signal strength from a peer meshnode for a connection to be established.

* mesh_gate_announcements - 0=disabled, 1=enabled. At least one meshnode must make gate announcements. It is safe to enable this on all nodes. Background announcement traffic can be reduced in large networks by disabling this on a proportion of nodes if required.

* mesh_hwmp_rootmode - Set to 3 for fast mesh convergence in most situations. Can be set to 0, 1, 2, 3 or 4.

* mesh_max_peer_links - sets the maximum number of peer nodes that can connect. Can be set from 1 - 255.

**Example**: Set mesh_rssi_threshold to -75 dBm (decibels relative to one milliwatt)

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

    {
      "setup":{
        "version":"2.1.0",
        "enabled":"1",
        "procd_status":"running",
        "portal_detect":"1",
        "checkinterval":"10",
        "interface_timeout":"10",
        "debuglevel":"2"
      }
      "interfaces":{
        "mesh0":{
          "mesh_retry_timeout":"100",
          "mesh_confirm_timeout":"100",
          "mesh_holding_timeout":"100",
          "mesh_max_peer_links":"10",
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
          "mesh_hwmp_rootmode":"3",
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
          "mesh_connected_to_as":"0",
          "mesh_id":"-_-",
          "device":"radio0",
          "channel":"9",
          "tx_packets":"1789996",
          "tx_bytes":"1643600110",
          "rx_packets":"1996691",
          "rx_bytes":"144717512",
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
