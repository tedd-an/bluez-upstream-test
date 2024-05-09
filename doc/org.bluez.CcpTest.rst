=================
org.bluez.CCPTest
=================

-------------------------------------
BlueZ D-Bus CCPTest API documentation
-------------------------------------

:Version: BlueZ
:Date: May 2024
:Manual section: 5
:Manual group: Linux System Administration

Interface
=========

:Service:	org.bluez
:Interface:	org.bluez.CCPTest1
:Object path:	[variable prefix]/{hci0,hci1,...}/dev_XX_XX_XX_XX_XX_XX/CallerX

Methods
-------

void Answer()
``````````````

	This method can be called to answer an incoming call in progress.

	Possible errors:

	:org.bluez.Error.Failed:
	:org.bluez.Error.NotConnected:

void Reject()
`````````````````

	This Method can be called to reject a call, which can be an active call or a call on hold state.

	Possible errors:

	:org.bluez.Error.Failed:
	:org.bluez.Error.NotConnected:

Properties
----------

uint32 CallState [readonly]
```````````````````````````

	call index defined by CCP profile to denote the active call.
