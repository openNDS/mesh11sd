## 1. The mesh11sd project

  Mesh11sd is a dynamic parameter configuration daemon for 802.11s mesh networks.

  It was originally designed to leverage 802.11s mesh networking at Captive Portal venues.

  This is the open source version and it enables easy and automated mesh network operation with multiple mesh nodes.

  This version does not require a Captive Portal to be running.

## 2. Overview

**What is a Mesh?**

A mesh network is a multi point to multi point layer 2 mac-routing backhaul used to interconnect mesh peers. Mesh peers are generally non-user devices, such as routers, access points, CPEs etc..

A normal user device, such as a phone, tablet, laptop etc., cannot connect to a mesh network. Instead, connection is achieved via a mesh gateway, a special type of mesh peer.

**Are you sure you want a mesh?**

A mesh has nothing to do with a solution to enable your user devices to seamlessly roam from one access point to another. If this is what you want, **YOU ARE NOT LOOKING FOR A MESH**.

*It is unfortunate that some manufacturers have used the word “Mesh” for marketing purposes to describe their non-standard, closed source, proprietary “roaming” functionality and this causes great confusion to many people when they enter the world of international standards and open source firmware for their network infrastructure.*

    The accepted standard for mesh networks is ieee802.11s.
    The accepted standard for fast roaming of user devices is ieee802.11r.

***These are two completely unrelated standards.***

## 3. Major Features:
 1. Auto configuration of 802.11s mesh backhaul
 2. Optional Bridge Portal mode supporting VLAN trunking over the mesh backhaul.
 3. Optional Trunk Peer Mode providing ethernet downstream VLAN support.
 4. Optional Customer/Client Premises Mode (CPE)
 5. Default support for Opportunistic Wireless Encryption (OWE), with OWE Transition.
 6. Optional portal node to multi point peer group, enabling "guest" networking over mesh backhaul without the need for setting up a VLAN.

## 4. Getting Started:
To get started, you will need at least two mesh capable devices to use as meshnodes.

Reflash the first one of these with the standard OpenWrt image and allow it to boot up making sure it is connected to your upstream Internet feed **using its wan port** and that you get Internet access when connected to (one of) its lan ports.

**Note**: The OpenWrt default is for all wireless interfaces to be disabled. You do not have to enable them here as the mesh11sd daemon will do it for you.

**Note**: If you cannot get Internet access you may need to change the ipv4 subnet of either this meshnode or your isp router.

You are now ready to install mesh11sd and its supporting packages. None of these support packages are essential, but without them there will be much reduced functionality in the resulting mesh network. For example, the mesh backhaul will NOT be encrypted.

Log in to a terminal session on the meshnode using ssh.
Using the normal OpenWrt method (opkg or apk), update the repository databases (``opkg update`` or ``apk update``).

Now do the following:

 1. Remove the wpad-basic-mbedtls package.
 2. Install the wpad-mbedtls package.
 3. Install the ip-full package.
 4. Install the kmod-nft-bridge package.
 5. Install the vxlan package
 6. Install the uhttpd and px5g-mbedtls packages (may already be installed along with the LuCi UI).
 7. Finally - Install the mesh11sd package.

The mesh11sd daemon should now be running in a "safe" state (aka "manual mode").

To continue we now need to activate the new wpad daemon we installed at point 2 above.

Use the command:


``service wpad restart``

Before activating the mesh11sd daemon, there are a few important considerations to bare in mind.

 1. Mesh11sd uses the uci utility to manage dynamic configuration changes, moreover autoconfiguration is done on every startup and is not a one off process.
 2. In normal operation, configuration changes are not written to the config files in /etc/config but are kept in volatile storage by way of the uci utility.
 3. Directly editing a config file will very likely break something, all changes should be done with the uci utility.
 4. The OpenWrt Luci web interface does not support mesh11sd configuration and will probably not even show its effects. To this end, Luci is by default disabled by the mesh11sd daemon. Advanced users can re-enable it later if required (See the mesh11sd command line (CLI) reference later in this document).


## 5. Meshnode Types:

The mesh can have numerous types of meshnodes.

  1. **Peer Node** - the basic mesh peer - capable of mac-routing layer 2 packets in the mesh network.
  2. **Gateway Node** - a peer node that also hosts an access point (AP) radio for normal client devices to connect to. Also known as a gate. A gate can also function as a CPE (Customer [or Client] Premises Equipment), hosting a downstream layer 3 network with its own unique ipv4 subnet.
  3. **Gateway Leech Node** - a special type of Gateway Node that connects to the mesh backhaul but neither contributes to it nor advertises itself on it.
  4. **Portal Node** - a peer node that also hosts a layer 3 routed upstream connection (eg an Internet feed)
  5. **CPE Gateway Node**
  6. **Portal Bridge Node**
  7. **Trunk Node**

It is possible for a Portal node to also be a Gateway node (ie it hosts an AP as well as an upstream connection) as well as other combinations.

**Auto Channel Tracking:**

All Peer and Gateway nodes will track the wireless channel that the Portal node is using. If the Portal node changes its working channel, this will be detected and tracked autonomously by downstream meshnodes.

The default radio will be on 2.4 GHz but can be changed by means of a simple config option.

2.4 GHz is chosen as it gives the most reliable mesh backhaul due to the range and penetration of the 2.4 GHz spectrum and the fact that it is not effected by the DFS restrictions of other bands. It can also be used unlicensed almost everywhere in outdoor venues. This comes of course with a likely bandwidth compromise, but in practice is often acceptable, particularly in a normal domestic or public environment.

Mesh11sd will add required wireless mesh configuration autonomously and it can be viewed using the uci utility but will not be present in the /etc/config/wireless file.

If the mesh network interface is defined in the wireless configuration file, mesh11sd will attempt to use it, but be warned, this may have very unpredictable results and is not normally recommended.

**NOTE:** ***Mesh11sd cannot be configured using the OpenWrt Luci UI, and its configuration will not appear in the Luci wireless pages, even when mesh11sd is active.***

**NOTE:** It is essential that all meshnodes are configured to use the same radio channel, the same key and the same mesh_id. By default, Mesh11sd will do this for you.

###Autoconfig Essentials

***Note: Use the same configuration for all nodes, including the base ipv4 address.***

By simply enabling auto_config, mesh11sd will bring up a working meshnode, but there are several essentials that should be configured as the defaults may not be appropriate.

 1. The country code default setting is DFS-ETSI as it is the "safest", but of course you are legally obliged to set the country code for your locality.
 2. Set the base ipv4 address of the meshnode, defining the subnet to be used on the mesh.
 3. Mesh11sd sets a hashed meshID and meshKey to encrypt the mesh backhaul, but you should set your own seed values to be used, to ensure only your meshnodes can join your mesh. This must be the same on every meshnode.
 4. You should set a WiFi access code and if desired, your own ssid on each gateway node.


Before proceeding to set these essentials, you must first connect each node in turn to an upstream Internet connection, connecting its "wan" port to a "lan" port of your isp router.

Connect your computer by ethernet to a "lan" port of the meshnode you are configuring.

Open a terminal session on the node using SSH to the default ip address of 192.168.1.1

You must now stop the mesh11sd service using the following command:

```
	service mesh11sd stop
```

**Country Code**

This should be set in the normal OpenWrt way, using either the uci utility or the Luci Web UI.

**Base IP Addrress and Subnet Mask**

This should be set in the normal OpenWrt way, using either the uci utility or the Luci Web UI.

**Mesh Encryption Seed Values**

A mesh ID seed value should be set. For example, using the string "MyMeshIDSeed", run the following command:

```
	uci set mesh11sd.setup.auto_mesh_id='MyMeshIDSeed'
```

Optionally add a mesh key seed, eg "MyMeshKeySeed"

```
	uci set mesh11sd.setup.auto_mesh_key='MyMeshKeySeed'
```

**Gateway SSID**

This should be set in the normal OpenWrt way, using either the uci utility or the Luci Web UI.

**Gateway Encryption**

First, select the desired encryption type from the following:

`0 (none/owe-transition), 1 (sae, aka wpa3), 2 (sae-mixed, aka wpa2/wpa3), 3 (psk2, aka wpa2) or 4 (owe - Opportunistic Wireless Encryption)`

**Note: Opportunistic Wireless Encryption requires a full version of wpad eg wpad-mbedtls.**

Example, set to psk2 encryption:

```
	uci set mesh11sd.setup.mesh_gate_encryption='3'
```

Now set the desired access code, eg "mysecretaccesscode":

```
	uci set mesh11sd.setup.mesh_gate_key='mysecretaccesscode'
```

**Save the Changes**

Finally, save the changes:

```
	uci commit mesh11sd

```

The node can now be moved to the desired location and the next one configured.

Power up all nodes in any order, having one only connected to your isp router as the portal node.

### Default configuration file (/etc/config/mesh11sd):

```


config mesh11sd 'setup'
	###########################################################################################
	# debuglevel (optional)
	# Sets the debuglevel
	# Default: 1 (Notification)
	# Options are 0, silent, 1 notification, 2 info and 3 debug
	#
	#option debuglevel '2'

	###########################################################################################
	# enabled (optional)
	# Enables or disables the mesh11sd daemon
	# Default: 1 (enabled)
	#
	# Set to 0 to disable
	#
	#option enabled '1'

	###########################################################################################
	# checkinterval (optional)
	# Sets the dynamic configuration checkinterval in seconds
	# Default: 10 (seconds)
	#
	#option checkinterval '15'

	###########################################################################################
	# portal_detect (optional)
	# Ignored if auto_config is disabled.
	#
	# Default 1
	#
	# Possible values:
	#  0  - Force Portal mode regardless of an upstream connection.
	#  1  - Detect if the meshnode is a portal, meaning it has an upstream wan link.
	# 	If the upstream link is active, the router hosting the meshnode will serve ipv4 dhcp into the mesh network.
	# 	If the upstream link is not connected, dhcp will be disabled and the meshnode will function as a layer 2 bridge on the mesh network.
	#  2  - Deprecated - no longer used.
	#  3  - Force CPE mode (Customer Premises Equipment)
	#  	This is a peer mode but treats the mesh backhaul as an upstream wan connection.
	#  	A nat routed lan is created with its own ipv4 subnet.
	#  4  - bridge vxlan trunk portal
	#	Functions as 0, but adds wan ethernet port to the vxtunnel bridge (default br-tun69)
	#	The wan port will be an ethernet end point into the vxtunnel, supporting vlans if required
	#	This mode should be used if a bridged connection to the upstream ISP router is required (ie bridged/no-nat ipv4 ).
	#	The wan port and lan port(s) form independent layer 2 networks carried by the mesh backhaul to all peer meshnodes.
	#	For normal use, both the wan and a lan port should be patched to the upstream router or an intermediate switch.
	#
	#  5  - bridge vxlan trunk node
	#	Functions as 1, but adds wan ethernet port to the vxtunnel bridge (default br-tun69)
	#	The wan port will be an ethernet end point into the vxtunnel, supporting vlans if required.
	#	Compatible woth portal nosdes configured with portal_detect 0, 1 or 4.
	#
	#
	# Has no effect if auto_config is disabled.
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
	# channel_tracking_checkinterval (optional)
	#
	# The minimum interval in seconds after which channel tracking begins on peer nodes
	# Values less than checkinterval are ignored
	#
	# Default: 30 seconds
	#
	# Example:
	#option channel_tracking_checkinterval '60'

	###########################################################################################
	# portal_detect_threshold (optional)
	#
	# This is the portal detect watchdog.
	#
	# The number of checkintervals before the portal detect watchdog begins actions to (re)establish a reconnection to a portal.
	#
	# Default 10 (watchdog is triggered after 10 iterations)
	# If set to 0, the watchdog is never triggered
	# Ignored if auto_config is disabled.
	#
	# Each time the peer node fails to detect the portal, a counter is incremented.
	# If the threshold is reached, the node will take various actions in an attempt to find the portal.
	# If the portal is still not detected, the watchdog will reboot the peer node.
	#
	# Example - Set threshold to 10 chekintervals:
	#option portal_detect_threshold '10'

	###########################################################################################
	# mesh_path_cost (optional)
	#
	# sets the STP cost of the mesh network
	# Default: 10
	# Can be set to any value from 0 to 65534
	# Setting to 0 disables STP
	#
	# Example:
	#option mesh_path_cost '100'

	###########################################################################################
	# interface_timeout (optional)
	#
	# Sets the interface timeout interval in seconds
	# Default: 10 (seconds)
	#
	#option interface_timeout '10'

	###########################################################################################
	# auto_config (optional)
	#
	# Enables autonomous dynamic mesh configuration.
	# Auto configure mesh interfaces in the wireless configuration.
	# Default 0 (disabled). Set to 1 to enable.
	#
	# When set to 0, the mesh11sd daemon will check for an existing mesh configuration.
	#
	# Warning: If an existing mesh configuration is found, it will be honoured even if it is incorrect.
	#	Manually configuring a mesh can soft brick the router if incorrectly done.
	#
	# Auto config can be tested using the command line function 'mesh11sd auto_config test'
	#	See the documentation for further information (Hint: try 'mesh11sd --help')
	#
	#option auto_config '1'

	###########################################################################################
	# auto_mesh_id (optional)
	#
	# Configure the mesh_id of the wireless interface(s) when auto_config is enabled
	# Default --__
	#
	# This string will be hashed to produce a secure mesh id
	# If set, it must also be set to the same value on every mesh node
	#
	#option auto_mesh_id 'MyMeshID'

	###########################################################################################
	# auto_mesh_band (optional)
	#
	# Configure the band to use for the mesh network
	# Valid values: 2g, 2g40, 5g, 6g, 60g
	# Default 2g40
	#
	# If set, it must also be set to the same value on every mesh node
	#
	# All mesh peer and mesh gate nodes will autonomously track the mesh portal channel
	# regardless of the configured auto_mesh_band
	#
	#option auto_mesh_band '5g'

	###########################################################################################
	# auto_mesh_key (optional)
	#
	# Defaults to a sha256 key to be automatically used on all members of this mesh when auto_config is enabled
	# Generates a secure sha256 key from the string value set in this option.
	#
	# If set, it must also be set to the same value on every mesh node
	#
	#option auto_mesh_key 'MySecretKey'

	###########################################################################################
	# auto_mesh_network (optional)
	#
	# Set the network the mesh interface will bind to (eg lan, guestlan etc) when auto_config is enabled.
	# Network wan is not accepted
	# Default lan
	#
	#option auto_mesh_network 'guestlan'

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
	# mesh_gate_encryption (optional)
	# Determines whether this node's gate (Access Point) will be a encrypted
	#
	# Default: 0 (disabled)
	# Set to 0 (none/owe-transition), 1 (sae, aka wpa3), 2 (sae-mixed, aka wpa2/wpa3), 3 (psk2, aka wpa2)
	# or 4 (Opportunistic Wireless Encryption - owe)
	#
	# Example - enable psk2 encryption
	#option mesh_gate_encryption '3'

	###########################################################################################
	# mesh_gate_key (optional)
	# Determines the encryption key for this node's gate.
	#
	# Default: not set (encryption disabled)
	# Set to a secret string value to use for encrypting the node's gate
	# Ignored if mesh_gate_encryption is set to 0 or 4
	#
	# Example - set a key string
	#option mesh_gate_key 'mysecretencryptionkey'

	###########################################################################################
	# mesh_gate_enable (optional)
	# Determines whether this node will be a gate
	#
	# Default: 1 (enabled)
	# Set to 0 to disable (turns off the node's gate interface ie its access point and SSID)
	#
	#option mesh_gate_enable '0'

	###########################################################################################
	# mesh_leechmode_enable (optional)
	# Determines whether this node will be a gate only leech node
	# A gate only leech node acts as an access point with a mesh backhaul connection, but does not contribute to the mesh
	#
	# This is useful when a node is well within the coverage of 2 or more peer nodes,
	# as otherwise it could create unstable multi hop paths within the backhaul.
	#
	# Can also be set dynamically using the command line option 'mesh11sd mesh_leechmode [enable/disable]'
	#	See the documentation for further information (Hint: try 'mesh11sd --help')
	#
	# Default: 0 (disabled)
	# Set to 1 to enable (turns off the node's mesh forwarding and active HWMP mac-routing)
	#
	# Example - enable leach mode
	#option mesh_leechmode_enable '1'

	###########################################################################################
	# txpower (optional)
	# Set the mesh radio transmit power in dBm.

	# Default - use driver default or value set in wireless config
	# Values outside the limits defined by the regulatory domain will be ignored
	#
	# Example - Set tx power to 15 dBm:
	#option txpower '15'

	###########################################################################################
	# ssid_suffix_enable (optional)
	# Add a 4 digit suffix to the ssid
	# The 4 digits are the last 4 digits of the mac address of the mesh interface
	# Default 1 (enabled)
	#
	#option ssid_suffix_enable '0'

	###########################################################################################
	# watchdog_nonvolatile_log (optional - FOR DEBUGGING PURPOSES ONLY)
	#
	# This enables logging of the portal detect watchdog actions in non-volatile storage.
	# The log file /mesh11sd_log/mesh11sd.log is created
	#
	# ##########WARNING##########
	# THIS OPTION IS FOR PORTAL DETECT WATCHDOG DEBUGGING PURPOSES ONLY
	# IF LEFT ENABLED FOR A LENGTH OF TIME IT MAY CAUSE NONE REPAIRABLE FLASH MEMORY WEAR AND USE UP FREE STORAGE SPACE
	# DISABLE IMMEDIATELY AFTER DEBUGGING OPERATIONS ARE COMPLETE
	# ##########WARNING##########

	###########################################################################################
	# mesh_path_stabilisation (optional)
	#
	# This enables mesh path stabilisation, preventing multi hop path changes due to multipath signal strength jitter
	#
	# Default: 1 (enabled)
	#
	# To disable, set to zero:
	#option mesh_path_stabilisation '0'

	###########################################################################################
	# reactive_path_stabilisation_threshold (optional)
	#
	# If an unstable path to an immediate neighbour node is detected, a counter is incremented each checkinterval while the unstable condition continues.
	# 	Mesh path stabilisation is activated once the counter exceeds the threshold.
	#
	# Default: 10 checkinterval periods.
	#
	#option reactive_path_stabilisation_threshold '5'

	###########################################################################################
	# mesh_mac_forced_forwarding (optional)
	#
	# This enables mac forced forewarding on the mesh interface
	#
	# Default: 1 (enabled)
	#
	# To disable, set to zero:
	#option mesh_mac_forced_forwarding '0'

	###########################################################################################
	# gateway_proxy_arp (optional)
	#
	# This enables proxy arp on the gateway bridge interface
	#
	# Default: 1 (enabled)
	#
	# To disable, set to zero:
	#option gateway_proxy_arp '0'

	###########################################################################################
	# stop_on_error (optional)
	#
	# If the watchdog detects a failure of ipv4 communication with a portal, the daemon will go into idle mode
	# This is useful if the meshnode does not have a reset button and a critical error occurs, blocking access
	#
	# Default: 0 (disabled)
	#
	# To enable, set to 1. This setting will overide the reboot_on_error setting
	#option stop_on_error '0'

	###########################################################################################
	# reboot_on_error (optional)
	#
	# If the watchdog detects a failure of ipv4 communication with a portal, the daemon will reboot the node
	#
	# Default: 1 (enabled)
	#
	# To disable, set to zero:
	#option reboot_on_error '0'

	###########################################################################################
	# vtun_enable (optional)
	#
	# Note: All vtun options require the ip-full and vxlan packages to be installed, otherwise the options will be ignored
	#
	# Enables point to multi-point vxlan tunneling from portal to all compatible nodes
	#
	# Default: 1 (enabled)
	#
	# To disable, set to zero:
	#option vtun_enable '0'

	###########################################################################################
	# tun_id (optional)
	#
	# Note: All vtun options require the ip-full and vxlan packages to be installed, otherwise the options will be ignored
	#
	# Sets the vxtunnel id, a decimal number between 1 and 16777216 (24 bits)
	#
	# Default: 69
	#
	#option tun_id '42069'

	###########################################################################################
	# vtun_ip (optional)
	#
	# Note: All vtun options require the ip-full and vxlan packages to be installed, otherwise the options will be ignored
	#
	# Sets the vxtunnel ipv4 gateway address to be used in the vxtunnel.
	# Becomes active if the node becomes a portal (portal_detect 0 or 1).
	#
	# Default: 192.168.168.1
	#
	#option vtun_ip '10.69.1.1'

	###########################################################################################
	# vtun_mask (optional)
	#
	# Note: All vtun options require the ip-full and vxlan packages to be installed, otherwise the options will be ignored
	#
	# Sets the vxtunnel ipv4 address mask to be used in the vxtunnel.
	# Becomes active if the node becomes a portal (portal_detect 0 or 1).
	#
	# Default: 255.255.255.0
	#
	#option vtun_mask '255.255.0.0'

	###########################################################################################
	# vtun_gate_encryption (optional)
	#
	# Note: All vtun options require the ip-full and vxlan packages to be installed, otherwise the options will be ignored
	#
	# Sets the vxtunnel gate encryption to be used on node gates (access points) connected to the vxtunnel.
	#	Valid values are: 0 (none/owe_transition), 1 (sae, aka wpa3), 2 (sae-mixed, aka wpa2/wpa3), 3 (psk2, aka wpa2) or 4 (owe)"
	#
	# Default: 0 (none/owe transition)
	#
	#option vtun_gate_encryption '3'

	###########################################################################################
	# vtun_gate_key (optional)
	#
	# Note: All vtun options require the ip-full and vxlan packages to be installed, otherwise the options will be ignored
	#
	# Sets the vxtunnel gate encryption key to be used on node gates (access points) connected to the vxtunnel.
	#	Must be a minimum of 8 characters in length
	#
	# Default: not set
	#
	#option vtun_gate_key 'mysecretkey'

	###########################################################################################
	# vtun_base_ssid (optional)
	#
	# Note: All vtun options require the ip-full and vxlan packages to be installed, otherwise the options will be ignored
	#
	# Sets the vxtunnel base ssid string
	#	If ssid_suffix_enable is set to 0, must be a maximum of 30 characters in length
	#	If ssid_suffix_enable is set to 1, must be a maximum of 22 characters in length
	#	Excess characters will be truncated
	#
	# Default: Guest
	#
	#option vtun_base_ssid 'CustomerNetwork'

	###########################################################################################
	# vtun_path_cost (optional)
	#
	# Note: All vtun options require the ip-full and vxlan packages to be installed, otherwise the options will be ignored
	#
	# sets the STP cost of the vxtunnel network
	# Default: 10
	# Can be set to any value from 0 to 65534
	# Setting to 0 disables STP
	#
	# Example:
	#option vtun_path_cost '100'

	###########################################################################################
	# apmond_enable (optional)
	#
	# Enables the access point monitoring daemon
	# 	Assumes the uhttpd and px5g-mbedtls packages are installed.
	#	But other portal based https web servers can be used.
	# Default: 1 (enabled)
	#
	# Data is collected from access point interfaces on this node and sent to the portal node
	#
	# Example:
	#option apmond_enable '0'

	###########################################################################################
	# apmond_cgi_dir (optional)
	#
	# Sets the apmond cgi directory
	#	Takes effect when this node becomes a portal (portal detect 0, 1 and 4)
	# Default: /www/cgi-bin
	#
	# Example:
	#option apmond_cgi_dir '/webroot/cgi'

	###########################################################################################
	# mesh_backhaul_led (optional)
	#
	# Enables the mesh backhaul heartbeat led indicator
	#	The led indicator will be on when the mesh inteface is up, changing to the Linux heartbeat signal when peer nodes are connected
	# Default: auto
	#	By default, the power or system led will be used if present.
	#
	#	Other leds can be found listed in /sys/class/leds with the format "color:function"
	# Disable this option by setting its value to "none"
	#
	# Example, enable the "blue:run" led:
	#option mesh_backhaul_led 'blue:run'
	#
	# Example, disable the mesh backhaul led:
	#option mesh_backhaul_led 'none'

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

**NOTE:** The setup option `portal_detect` is set to 1 (enabled) by default.

**NOTE:** If the setup option `portal_detect` is set to 0, the meshnode will be forced into Portal mode. ie it will act as a layer 3 router between its wan and lan ports regardless of the availability of an upstream wan feed.

**NOTE:** If the setup option `portal_detect` is set to 2, the meshnode will be forced into Peer mode, ignoring any any upstream wan feed.

**NOTE:** If the setup option `portal_detect` is set to 3, the meshnode will be forced into CPE (Customer Premises Equipment) mode, The mesh backhaul will be used as a wan connection and the meshnode will function as an ipv4 NAT router with a unique ipv4 subnet on its lan and mesh gate (access point).

The option portal_detect much simplifies the setup of the meshnodes of a network. Each can be configured as a basic router with a mesh interface defined as above. Once mesh11sd is installed, portal detection will be activated and with the upstream wan port connected, the meshnode will continue to function as a router with the additional functionality of a mesh portal.

When the upstream wan connection is disconnected, the meshnode will automatically reconfigure itself as a layer 2 peer meshnode.

This means that all meshnodes can be the same basic router configuration and once moved to the required location, will autonomously reconfigure.

Access to the remote meshnode peers will not be possible using the default ipv4 address as this will be disabled. Remote management can be achieved by using the `mesh11sd connect` and `mesh11sd copy` commands, or alternatively by reconnecting the wan port to an upstream feed.


## 7. Setup Options
* enabled - 0=disabled, 1=enabled. Default 1

* debuglevel - 0=silent, 1=notice, 2=info, 3=debug. Default 1

* checkinterval - the interval in seconds after which changes in parameters are detected and activated. Default 10 seconds

* portal_detect - (optional, default 1)  Detect if the meshnode is a portal, meaning it has an upstream wan link. If the upstream link is active, the router hosting the meshnode will serve ipv4 dhcp into the mesh network. If the upstream link is not connected, dhcp will be disabled and the meshnode will function as a level 2 bridge on the mesh network. Default 1 (enabled). Set to 0 to force Portal mode, 2 to force Peer mode, or 3 to enable CPE mode.

* portal_channel - valid only when the meshnode is a portal. Specifies the mesh network working channel. All other meshnodes will track this channel.

* interface_timeout - the time in seconds that mesh11sd will wait for a mesh interface to establish before continuing. Default 10 seconds

* mesh_path_cost - sets the STP cost of the mesh network. Default: 10. Can be set to any value from 0 to 65534. Setting to 0 disables STP

* auto_config - (optional) - autonomously configures the mesh network. Default 1 (enabled). Set to 0 to disable

* auto_mesh_id - (optional) - specifies a string used to generate the mesh id hash. If set, this must be the same on all mesh nodes. Default --__

* auto_mesh_band - (optional) - specifies the wireless band to be used for the mesh network. Possible values are 2g, 5g, 6g and 60g. Default 2g

* auto_mesh_key - specifies a seed value string to be used in generating the mesh key hash used for mesh encryption. If set, this must be the same on all meshnodes. Default not set

* auto_mesh_network - (optional) - specifies the firewall zone used for the mesh. Typical values "lan", "guest" etc. This can be set differently on each meshnode as required. Firewall zone "wan" is not valid. Default lan

* mesh_basename - (optional) The first 4 characters after non alphanumerics (ie special characters) are removed are used as the mesh_basename. The mesh_basename is used to construct a unique mesh interface name of the form m-xxxx-n. Default: 11s

* mesh_gate_enable - enables any access points configured on the meshnode. Default 1 (enabled). Set to 0 to disable. **Note:** If there is an interface level "disable option" (in wireless config), mesh11sd will use that setting.

* mesh_gate_encryption - Determines whether this node's gate will be a encrypted. Default: 0 (disabled). Set to 0 (none/owe-transition), 1 (sae, aka wpa3), 2 (sae-mixed, aka wpa2/wpa3), 3 (psk2, aka wpa2) or 4 (Opportunistic Wireless Encryption - owe).

* mesh_gate_key - Determines the encryption key for this node's gate. Default: not set (encryption disabled or owe). Set to a secret string value to use for encrypting the node's gate

* mesh_leechmode_enable - Determines whether this node will be a gate only leech node. A gate only leech node acts as an access point with a mesh backhaul connection, but does not contribute to the mesh. This is useful when a node is well within the coverage of 2 or more peer nodes, as otherwise it could create unstable multi hop paths within the backhaul. Default: 0 (disabled). Set to 1 to enable (turns off the node's mesh forwarding and HWMP mac-routing).

* mesh_path_stabilisation - This enables mesh path stabilisation, preventing multi hop path changes due to multipath signal strength jitter. Default: 1 (enabled). To disable, set to zero.

* txpower - set the mesh radio transmit power in dBm. Takes effect immediately.

* ssid_suffix_enable - Add a 4 digit suffix to the ssid. The 4 digits are the last 4 digits of the mac address of the mesh interface.

* watchdog_nonvolatile_log - (optional - FOR DEBUGGING PURPOSES ONLY). This enables logging of the portal detect watchdog actions in non-volatile storage. The log file /mesh11sd_log/mesh11sd.log is created. THIS OPTION IS FOR PORTAL DETECT WATCHDOG DEBUGGING PURPOSES ONLY. IF LEFT ENABLED FOR A LENGTH OF TIME IT MAY CAUSE NONE REPAIRABLE FLASH MEMORY WEAR AND USE UP FREE STORAGE SPACE. DISABLE IMMEDIATELY AFTER DEBUGGING OPERATIONS ARE COMPLETE.


**Example**:
Set the debuglevel to 2

        uci set mesh11sd.setup.debuglevel='2'

Debuglevel will be set immediately to level 2 "info" and will remain set until changed again or a reboot occurs.

Changes can be made permanent with the following command:

        uci commit mesh11sd

## 8. Mesh Parameter Options


Mesh parameters can be changed only while the mesh is active.

Here is a list of available parameters and their function:

 * mesh_retry_timeout: the initial retry timeout in millisecond units used by the Mesh Peering Open message

 * mesh_confirm_timeout: the initial confirm timeout in millisecond units used by the Mesh Peering Open message

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


## 9. Command Line Interface
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

        Option: auto_config
          Configure auto config mode
          Usage:
            mesh11sd auto_config test | enable
              Takes immediate effect, any current connection is likely to be lost, requiring re-connection on possibly a different ipv4 address
              test - turn on auto config test mode. A reboot will revert the auto configuration.
              enable - configure auto config mode.
            Valid only when auto_config is disabled (default)

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

        Option: txpower
          Change the mesh transmit power
          Usage: mesh11sd txpower [+|-]
          where \"+\" increments by 3dBm and \"-\" decrements by 3dBm
          Takes effect immediately

        Option: mesh_leechmode
          Change leechmode status
          Usage: mesh11sd mesh_leechmode [enable/disable]
          Takes effect immediately

        Option: stations
          List all mesh peer stations directly connected to this mesh peer station (one hop)
          Usage: mesh11sd stations

        Option: mesh_rssi_threshold
          Change the mesh rssi threshold
          Usage: mesh11sd mesh_rssi_threshold [+|-] [force]
          where \"+\" increments by 3dBm and \"-\" decrements by 3dBm
          Takes effect immediately on NEW connections
          The keyword \"force\" forces the new threshold on all connected peers
          Warning - \"force\" will briefly remove this node from the mesh network, taking a few seconds to rejoin

        Option: commit_changes
          Usage: mesh11sd commit_changes
          Commits changes to mesh_leechmode, txpower and rssi_threshold to non volatile configuration (make permanent)

        Option: commit_all
          Usage: mesh11sd commit_all commit
          Writes all changes made by auto_config to configuration file
          Active only when auto_config is enabled

        Option: revert_all
          Usage: mesh11sd revert_all revert
          Reverts all changes made by commit_all
          Active only when auto_config is enabled

        Option: force_ipv4_download
          Usage: mesh11sd force_ipv4_download
          Forces opkg to use ipv4 for its downloads

        Option: download_revert_to_default
          Usage: mesh11sd download_revert_to_default
          Reverts opkg to default for its downloads

        Option: dhcp4_renew
          Usage: mesh11sd dhcp4_renew
          Renews the current dhcp4 lease

       Option: is_installed
         Usage: mesh11sd is_installed [packagename]
         Checks if \"packagename\" is installed
         Returns the installation status of the package and exit code 0 if installed

       Option: get_portal_ula
         Usage: mesh11sd get_portal_ula
         Gets the portal unique local ipv6 address (ula)

       Option: set_ula_prefix
         Usage: mesh11sd set_ula_prefix [get|set|revert]
         get - gets the current ula prefix
         set - sets the ula prefix for the mesh backhaul based on the mesh id
         revert - reverts a previously set ula prefix

       Option: str_to_hex
         Usage: mesh11sd str_to_hex [access_point_data_string]
         Run length hex encodes access_point_data from apmond format

       Option: hex_to_str
         Usage: mesh11sd hex_to_str [hex encoded_access_point_data_string]
         Decode run length hex encoded access_point_data to apmond format

       Option: write_node_data
         Usage: mesh11sd write_node_data [node_id] [querystring]
         Called from apmond cgi script. Writes node data received by web server

       Option: send_ap_data
         Usage: mesh11sd send_ap_data
         Sends hex encoded apmon data to portal/apmond web server

       Option: get_ap_data
         Usage: mesh11sd get_ap_data
         Get json format access point data for this node

       Option: show_ap_data
         Usage: mesh11sd show_ap_data [node_id | all]
         Shows json format access point data for requested node | all nodes
         Valid on portal/apmond node

**Example status output:**

```
{
  "setup":{
    "version":"5.0.0",
    "enabled":"1",
    "procd_status":"running",
    "portal_detect":"1",
    "portal_detect_threshold":"10",
    "portal_channel":"default",
    "channel_tracking_checkinterval":"30",
    "mesh_mac_forced_forwarding":"1",
    "gateway_proxy_arp":"1",
    "reboot_on_error":"1",
    "stop_on_error":"0",
    "watchdog_nonvolatile_log":"0",
    "mesh_basename":"m-11s-",
    "auto_config":"1",
    "auto_mesh_network":"lan",
    "auto_mesh_band":"2g40",
    "auto_mesh_id":"92d490daf46cfe534c56ddd669297e",
    "mesh_gate_enable":"1",
    "mesh_leechmode_enable":"0",
    "mesh_gate_encryption":"0",
    "mesh_backhaul_led":"auto",
    "vtun_enable":"1",
    "tun_id":"69",
    "vtun_ip":"192.168.168.1",
    "vtun_mask":"255.255.255.0",
    "vtun_gate_encryption":"0",
    "vtun_base_ssid":"Guest",
    "vtun_path_cost":"10",
    "txpower":"20",
    "mesh_path_cost":"10",
    "mesh_path_stabilisation":"1",
    "reactive_path_stabilisation_threshold":"10",
    "checkinterval":"10",
    "interface_timeout":"10",
    "ssid_suffix_enable":"1",
    "apmond_enable":"1",
    "apmond_cgi_dir":"/www/cgi-bin/",
    "debuglevel":"3"
  },
  "mesh_interfaces":{
    "m-11s-0":{
      "mesh_retry_timeout":"255",
      "mesh_confirm_timeout":"255",
      "mesh_holding_timeout":"255",
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
      "mesh_rssi_threshold":"-65",
      "mesh_hwmp_active_path_to_root_timeout":"6000",
      "mesh_hwmp_root_interval":"5000",
      "mesh_hwmp_confirmation_interval":"2000",
      "mesh_power_mode":"active",
      "mesh_awake_window":"10",
      "mesh_plink_timeout":"10",
      "mesh_connected_to_gate":"1",
      "mesh_nolearn":"0",
      "mesh_connected_to_as":"1",
      "mesh_id":"92d490daf46cfe534c56ddd669297e",
      "device":"radio0",
      "channel":"1",
      "tx_packets":"1545600",
      "tx_bytes":"1963833436",
      "rx_packets":"1341979",
      "rx_bytes":"179684310",
      "this_node":"94:83:c4:a2:8e:cb",
      "active_peers":"2",
      "peers":{
        "96:83:c4:30:12:8d":{
          "next_hop":"96:83:c4:30:12:8d",
          "hop_count":"1",
          "path_change_count":"1",
          "metric":"50"
        },
        "96:83:c4:24:1a:a5":{
          "next_hop":"96:83:c4:24:1a:a5",
          "hop_count":"1",
          "path_change_count":"5",
          "metric":"1051"
        }
      },
      "active_stations":"3",
      "stations":{
        "ce:fe:af:7f:3b:e1":{
          "proxy_node":"94:83:c4:a2:8e:cb"
        },
        "94:83:c4:30:12:8d":{
          "proxy_node":"96:83:c4:30:12:8d"
        },
        "94:83:c4:24:1a:a5":{
          "proxy_node":"96:83:c4:24:1a:a5"
        }
      }
    }
  },
  "layer2_connections":{
    "br-lan":{
      "94:83:c4:24:1a:a5":"m-11s-0",
      "94:83:c4:30:12:8d":"m-11s-0",
      "96:83:c4:24:1a:a5":"m-11s-0",
      "00:e0:4c:68:08:a2":"lan3"
    },
    "br-tun69":{
      "ce:fe:af:7f:3b:e1":"owe2-0",
      "96:83:c4:2d:1a:a5":"vxlan69"
    }
  }
}

```

**Example of using "show_ap_data all" on a portal node:**

```
root@meshnode-8ecb:~# mesh11sd show_ap_data all
{
  "Guest-2g-128d@owe2-0@94:83:c4:30:12:8d":{
    "ba:54:32:31:32:bb":{
      "rx_bytes":"41924",
      "tx_bytes":"140877",
      "tx_retries":"205",
      "signal_avg":"-72_[-74,_-76]_dBm",
      "tx_bitrate":"39.0_MBit/s_MCS_4",
      "rx_bitrate":"13.0_MBit/s_MCS_1",
      "connected_time":"14_seconds",
      "timestamp":"1736663326462_ms",
      "date_time":"Sun_Jan_12_06:28:46_UTC_2025"
    }
  },
  "Guest-2g-8ecb@owe2-0@94:83:c4:a2:8e:cb":{
    "ce:fe:af:7f:3b:e1":{
      "rx_bytes":"522776",
      "tx_bytes":"2831390",
      "tx_retries":"449",
      "signal_avg":"-22_[-28,_-23,_-33,_-29]_dBm",
      "tx_bitrate":"6.0_MBit/s",
      "rx_bitrate":"65.0_MBit/s_MCS_7",
      "avg_ack_signal":"-21_dBm",
      "connected_time":"5836_seconds",
      "timestamp":"1736671236495_ms",
      "date_time":"Sun_Jan_12_08:40:36_GMT_2025"
    }
  }
}
root@meshnode-8ecb:~#
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
root@meshnode-1483:~#

root@meshnode-1483:~#



```
