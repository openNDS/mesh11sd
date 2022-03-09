## 0. The mesh11sd project

  mesh11sd is an 802.11s mesh, dynamic parameter configuration daemon, originally designed to leverage 802.11s mesh networking at Captive Portal venues.

  This is the open source version and it enables easy and automated mesh network operation with multiple mesh nodes.

  It allows all mesh parameters supported by the wireless driver to be set in the uci config file.

  Settings take effect immediately without having to restart the wireless network.

  Default settings give rapid and reliable layer 2 mesh convergence.

  Without mesh11sd, many mesh parameters cannot be set in the uci wireless config file as the mesh interface must be up before the parameters can be set.

  Some of those that are supported, would fail to be implemented when the network is (re)started resulting in errors or dropped nodes.

  The mesh11sd daemon dynamically checks configured parameters and sets them as required.

  This version does not require a Captive Portal to be running.

## 1. Overview

**mesh11sd**

                     ----ToDo----