========================
org.bluez.MediaAssistant
========================

--------------------------------------------
BlueZ D-Bus MediaAssistant API documentation
--------------------------------------------

:Version: BlueZ
:Date: June 2024
:Manual section: 5
:Manual group: Linux System Administration

Interface
=========

:Service:	org.bluez
:Interface:	org.bluez.MediaAssistant1
:Object path:	/org/bluez/{hci0,hci1,...}/src_XX_XX_XX_XX_XX_XX/dev_YY_YY_YY_YY_YY_YY/bisZ

Methods
-------

void Push(array{byte} Broadcast_Code)
````````````````````````````````````````````````````````

	Send stream information to the remote device. If the stream
	is unencrypted, the Broadcast_Code is set to 0. Otherwise,
	it contains the key to decrypt the stream.

Properties
----------

string State [readonly]
```````````````````````

	Indicates the state of the assistant object. Possible values are:

	:"idle": assistant object was created for the stream
	:"pending": assistant object was pushed (stream information was sent to the peer)
	:"requesting": remote device requires Broadcast_Code
	:"active": remote device started receiving stream

dict QoS [readonly, ISO only, experimental]
`````````````````````````````````````````````````````

	Indicates QoS capabilities.

	Values:

	:byte BIG:

		Indicates BIG id.

	:byte Encryption:

		Indicates whether the stream is encrypted.

	:array{byte} BCode

		Indicates Broadcast_Code to decrypt stream.

	:byte Options:

		Indicates configured broadcast options.

	:uint16 Skip:

		Indicates configured broadcast skip.

	:byte SyncTimeout:

		Indicates configured broadcast sync timeout.

	:byte SyncType:

		Indicates configured broadcast sync CTE type.

	:byte MSE:

		Indicates configured broadcast MSE.

	:uint16 Timeout:

		Indicates configured broadcast timeout.

	:uint32 Interval:

		Indicates configured ISO interval (us).

	:uint16 Latency:

		Indicates configured transport latency (ms).

	:uint16 SDU:

		Indicates configured maximum SDU.