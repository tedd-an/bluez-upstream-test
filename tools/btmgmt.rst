======
btmgmt
======

-------------------------------------
interactive bluetooth management tool
-------------------------------------

:Version: BlueZ
:Copyright: Free use of this software is granted under ther terms of the GNU
            Lesser General Public Licenses (LGPL).
:Date: July 2023
:Manual section: 1
:Manual group: Linux System Administration

SYNOPSIS
========

**btmgmt** [--options] [commands]

DESCRIPTION
===========

**btmgmt(1)** interactive bluetooth management tool. The tool issues commands
to the Kernel using the Bluetooth Management socket, some commands may require
net-admin capability in order to work.


OPTIONS
=======

.. csv-table::
   :header: "Options", "Description"
   :align: left

   *-i/--index*, Specify adapter index
   *-m-/-monitor*, Enable monitor output
   *-t/--timeout*, Timeout in seconds for non-interactive mode
   *-v/--version*, Display version
   *-i/--init-script*, Init script file
   *-*h/--help*, Display help

COMMANDS
========

.. csv-table::
   :header: "Command", "Arguments", "Description"
   :align: left

   *select*, "<index>", Select a different index
   *revision*, , Get the MGMT Revision
   *commands*, , List supported commands
   *config*, , Show configuration info
   *info*, , Show controller info
   *extinfo*, , Show extended controller info
   *auto-power*, , Power all available features
   *power*, <on/off>, Toggle powered state
   *discov*, <yes/no/limited> [timeout], Toggle discoverable state
   *connectable*, <on/off>, Toggle connectable state
   *fast-conn*, <on/off>, Toggle fast connectable state
   *bondable*, <on/off>, Toggle bondable state
   *pairable*, <on/off>, Toggle bondable state
   *linksec*, <on/off>, Toggle link level security
   *ssp*, <on/off>, Toggle SSP mode
   *sc*, <on/off/only>, Toggle SC support
   *hs*, <on/off>, Toggle HS support
   *le*, <on/off>, Toggle LE support
   *advertising*, <on/off>, Toggle LE advertising
   *bredr*, <on/off>, Toggle BR/EDR support
   *privacy*, <on/off> [irk], Toggle privacy support
   *class*, <major> <minor>, Set device major/minor class
   *disconnect*, [-t type] <remote address>, Disconnect device
   *con*, , List connections
   *find*, [-l|-b] [-L], Discover nearby devices
   *find-service*, [-u UUID] [-r RSSI_Threshold] [-l|-b], Discover nearby service
   *stop-find*, [-l|-b], Stop discovery
   *name*, <name> [shortname], Set local name
   *pair*, [-c cap] [-t type] <remote address>, Pair with a remote device
   *cancelpair*, [-t type] <remote address>, Cancel pairing
   *unpair*, [-t type] <remote address>, Unpair device
   *keys*, ,Load Link Keys
   *ltks*, ,Load Long Term Keys
   *irks*, [--local index] [--file file path], Load Identity Resolving Keys
   *block*, [-t type] <remote address> Block Device
   *unblock*, [-t type] <remote address>, Unblock Device
   *add-uuid*, <UUID> <service class hint>, Add UUID
   *rm-uuid*, <UUID>, Remove UUID
   *clr-uuids*, ,Clear UUIDs
   *local-oob*, ,Local OOB data
   *remote-oob*, [-t <addr_type>] [-r <rand192>] [-h <hash192>] [-R <rand256>] [-H <hash256>] <addr>, Remote OOB data
   *did*, <source>:<vendor>:<product>:<version>, Set Device ID
   *static-addr*, <address>, Set static address
   *public-addr*, <address>, Set public address
   *ext-config*, <on/off>, External configuration
   *debug-keys*, <on/off>, Toggle debug keys
   *conn-info*, [-t type] <remote address>, Get connection information
   *io-cap*, <cap>, Set IO Capability
   *scan-params*, <interval> <window>, Set Scan Parameters
   *get-clock*, [address], Get Clock Information
   *add-device*, [-a action] [-t type] <address>, Add Device
   *del-device*, [-t type] <address>, Remove Device
   *clr-devices*, ,Clear Devices
   *bredr-oob*, ,Local OOB data (BR/EDR)
   *le-oob*, ,Local OOB data (LE)
   *advinfo*, ,Show advertising features
   *advsize*, [options] <instance_id>, Show advertising size info
   *add-adv*, [options] <instance_id>, Add advertising instance
   *rm-adv*, <instance_id>, Remove advertising instance
   *clr-adv*, ,Clear advertising instances
   *add-ext-adv-params*, [options] <instance_id>, Add extended advertising params
   *add-ext-adv-data*, [options] <instance_id>, Add extended advertising data
   *appearance*, <appearance>, Set appearance
   *phy*, [LE1MTX] [LE1MRX] [LE2MTX] [LE2MRX] [LECODEDTX] [LECODEDRX] [BR1M1SLOT] [BR1M3SLOT] [BR1M5SLOT][EDR2M1SLOT] [EDR2M3SLOT] [EDR2M5SLOT][EDR3M1SLOT] [EDR3M3SLOT] [EDR3M5SLOT], Get/Set PHY Configuration
   *wbs*, <on/off>, Toggle Wideband-Speech support
   *secinfo*, ,Show security information
   *expinfo*, ,Show experimental features
   *exp-debug*, <on/off>, Set debug feature
   *exp-privacy*, <on/off>, Set LL privacy feature
   *exp-quality*, <on/off>, Set bluetooth quality report feature
   *exp-offload*, <on/off>, Toggle codec support
   *read-sysconfig*, ,Read System Configuration
   *set-sysconfig*, <-v|-h> [options...], Set System Configuration
   *get-flags*, [-t type] <address>, Get device flags
   *set-flags*, [-f flags] [-t type] <address>, Set device flags
   *menu*, <name>, Select submenu
   *version*, ,Display version
   *quit*, , Quit program
   *exit*, , Quit program
   *help*, , Display help about this program
   *export*, ,Print environment variables

AUTOMATION
==========
Two common ways to automate the tool are to pass the commands directly like in
the follow example:

::

   btmgmt <<EOF
   list
   show
   EOF

Or create a script and give it as init-script:

::

  vi test-script.bt
  list
  show
  quit
  :wq
  btmgmt --init-script=test-script

RESOURCES
=========

http://www.bluez.org

REPORTING BUGS
==============

linux-bluetooth@vger.kernel.org
