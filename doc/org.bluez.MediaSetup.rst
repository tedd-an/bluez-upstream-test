=======================
org.bluez.MediaSetup
=======================

-------------------------------------------
BlueZ D-Bus MediaSetup API documentation
-------------------------------------------

:Version: BlueZ
:Date: June 2024
:Manual section: 5
:Manual group: Linux System Administration

Interface
=========

:Service:	org.bluez
:Interface:	org.bluez.MediaSetup1
:Object path:	/org/bluez/{hci0,hci1,...}/src_XX_XX_XX_XX_XX_XX/dev_YY_YY_YY_YY_YY_YY/bisZ

Methods
-------

void Select()
````````````````````````````````````````````````````````

	Send stream information to the remote device.

void SetBcode(array{byte} broadcast_code)
`````````````````````````````````````````````````````````

	Send Broadcast_Code to the remote device.

Properties
----------

string State [readonly]
```````````````````````

	Indicates the state of the setup. Possible values are:

	:"idle": setup created
	:"pending": setup selected
	:"requesting": remote device requires Broadcast_Code
	:"active": remote device started receiving stream