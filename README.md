Kernelflinger
=============

Overview
--------

Kernelflinger is the Intel UEFI bootloader for
Android<sup>TM</sup>/Brillo<sup>TM</sup>. It is compatible with the
[UEFI 2.4 specification](http://www.uefi.org/sites/default/files/resources/2_4_Errata_B.pdf).

Kernelflinger implements the Google Bootloader requirements for
Android<sup>TM</sup> L and M desserts.

The key features are:

1. [Google verified boot](https://source.android.com/security/verifiedboot/verified-boot.html)
   support.
2. [Fastboot](./doc/fastboot.md) support over USB and TCP.
3. [Installer](./doc/installer.md): Standalone EFI application that
   can be used to flash a device from the EFI shell using an external
   storage.
4. [Crashmode](./doc/crashmode.md): provides a simple access using adb
   commmand to retrieve data from memory, partitions, EFI variables or
   ACPI tables in case of OS crash.

Basic architecture
------------------

* libkernelflinger: library that provides all the tools necessary to
  access ACPI and SMBIOS tables, run image verification, use storage
  (SATA, eMMC, SDCard and UFS) and draw graphic widgets.
* [libfastboot](./doc/fastboot.md): Fastboot protocol implementation.
  [fastboot protocol](https://android.googlesource.com/platform/system/core/+/master/fastboot/)
* libadb: used by [Crashmode](./doc/crashmode.md).
* libefiusb: based on the non-standard DeviceMode protocol it provides
  easy to use USB configuration, read and write functions and TX/RX
  events callbacks.
* libefitcp: based on the standard UEFI TCP protocol, it provides easy
  to use TCP configuration, read and write functions and TX/RX events
  callbacks.
* libtransport: is a framework to abstract the transport layer.  Used
  by both libfastboot and libadb to support USB and TCP transport.
* kernelflinger.c: main program that implements the boot flow.
* installer.c: main program of the [Installer](./doc/installer.md)

Dependencies
------------

Kernelflinger depends on the following libraries:
* gnu-efi (TODO: github link)
* openssl (TODO: github link)

Kernelflinger's compilation requires the following tools:
* [sbsigntool](https://github.com/android-ia/platform_external_sbsigntool):
  EFI binary signer.
* [vendor\_intel\_build](https://github.com/android-ia/vendor_intel_build):
  EFI compilation definitions for Android<sup>TM</sup>.

Compilation
-----------

Kernelflinger's compilation relies on the Android<sup>TM</sup>
compilation system.  In an Android<sup>TM</sup> tree, with all the
dependencies checked out, run the following command to build
`$OUT/efi/kernelflinger.efi`.

```bash
$ make kernelflinger-$TARGET_BUILD_VARIANT
```

Run the following command to build `$OUT/efi/installer.efi`:

```bash
$ make installer-$TARGET_BUILD_VARIANT
```

Kerneflinger specific configuration flags:

* `TARGET_NO_DEVICE_UNLOCK`: if true, any attempt to unlock the device
  (`fastboot flashing unlock`) will systematically fail.
* `HAL_AUTODETECT`: Cf. [Autodetect](./doc/autodetect.md).
* `TARGET_BOOTLOADER_POLICY`:
  Cf. [Bootloader Policy and Factory Reset Protection](./doc/FRP.md)
* `KERNELFLINGER_ALLOW_UNSUPPORTED_ACPI_TABLE`: makes kernelflinger
   ignore ACPI table oem\_id, oem\_table\_id and revision fields.
* `KERNELFLINGER_USE_POWER_BUTTON`: makes kernelflinger use the power
   key as an input source.
* `KERNELFLINGER_USE_WATCHDOG`: makes kernelflinger start the "kernel"
   watchdog prior booting the kernel.
* `KERNELFLINGER_USE_CHARGING_APPLET`: makes Kernelflinger use the
   non-standard ChargingApplet protocol to get the battery and charger
   status, and modify the boot flow in consequence.
* `KERNELFLINGER_IGNORE_RSCI`: makes Kernelflinger ignore the
   non-standard RSCI ACPI table.  This APCI table provides the reset
   and wake source reasons.
* `KERNELFLINGER_IGNORE_NOT_APPLICABLE_RESET`: makes Kernelflinger
   ignore the ACPI table RSCI reset source "not_applicable" when
   setting the bootreason.
* `KERNELFLINGER_SSL_LIBRARY`: either 'openssl' or 'boringssl', makes
   Kernelflinger build against the OpenSSL library, respectively, the
   BoringSSL library.  Note: the `TARGET_BOOTLOADER_POLICY` flag
   cannot be used if `KERNELFLINGER_SSL_LIBRARY` is set to 'boringssl'
   because the BoringSSL does not support the PKCS7 message format
   which is used by the RMA force unlock feature
   (Cf. [Bootloader Policy and Factory Reset Protection](./doc/FRP.md)).

Command line parameters
-----------------------

* `-f`: enforce kernelfliner to enter Fastboot mode
* `-U` [test-suite-name]: run unittest test (see
  [unittest.c](./unittest.c)).
