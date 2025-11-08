Jookia's Linux
--------------

This is my fork of Linux with out of tree features I use or have developed:

Video features:

- Fascontek FS305VG158 panel
- Fascontek FS028VG047 panel
- Setting NV3052C and ST7701 panel pixel formats

Audio features:

- CS5368 sound codec support
- sound-card-test continous rate support

Allwinner features:

- Fractional SDM clock rates (see [clocking docs](./jookia/docs/clocking/))
- Arbitrary I2S rates

Allwinner D1/T113 features:

- Thermal sensor enabled
- PWM support (by Aleksandr Shubin)
- I2S multiple DIN pins support
- LCD tint or green display fix
- Watchdog persists after boot (by Regis Dargent)

This branch tracks Linux v6.12. To see a list of commits from this branch
only use this command:

```
git log --left-only --oneline jookia_main...v6.12
```

I regularly sign my Git commits with my hardware SSH key, if you would like to
verify them run this and subtitute TAG or COMMIT:

```
git -c gpg.ssh.allowedSignersFile=jookia/allowed_signers verify-tag TAG
git -c gpg.ssh.allowedSignersFile=jookia/allowed_signers log --oneline --show-signature COMMIT
```

My key fingerprint is ```SHA256:/gEvgms/9HpbgpcH+K7O4GYXmqkP7siJx9zHeEWRZTg```.

Please verify it by comparing it to my website: https://www.jookia.org/wiki/Keys

Testing notes
-------------

I try to test my custom features with each release, but this isn't always
feasible. Here's a list of features I've tested and which commit I tested.

Video features:

- Fascontek FS305VG158 panel: ea6f5abeb4676112 on 2025-10-31
- Fascontek FS028VG047 panel: ea6f5abeb4676112 on 2025-10-31
- Setting NV3052C and ST7701 panel pixel formats: ea6f5abeb4676112 on 2025-10-31

Audio features:

- CS5368 sound codec: d607727261d85442 on 2025-03-22
- sound-card-test continous rate support: Not tested recently

Allwinner features:

- Fractional SDM clock rates: ff6d78f6852e182f on 2025-08-31
- Arbitrary I2S rates: ff6d78f6852e182f on 2025-08-31

Allwinner D1/T113 features:

- T113 thermal sensor: d607727261d85442 on 2025-03-23
- T113 PWM: d607727261d85442 on 2025-03-20
- I2S multiple DIN pins: Not tested recently
- LCD tint or green display fix: d607727261d85442 on 2025-03-20
- Watchdog persists after boot: d607727261d85442 on 2025-03-13

Mainline README
---------------

```
Linux kernel
============

There are several guides for kernel developers and users. These guides can
be rendered in a number of formats, like HTML and PDF. Please read
Documentation/admin-guide/README.rst first.

In order to build the documentation, use ``make htmldocs`` or
``make pdfdocs``.  The formatted documentation can also be read online at:

    https://www.kernel.org/doc/html/latest/

There are various text files in the Documentation/ subdirectory,
several of them using the reStructuredText markup notation.

Please read the Documentation/process/changes.rst file, as it contains the
requirements for building and running the kernel, and information about
the problems which may result by upgrading your kernel.
```
