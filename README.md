Jookia's Linux
--------------

This is my fork of Linux with out of tree features I use or have developed:

Audio features:

- CS5368 sound codec support
- sound-card-test continous rate support

Allwinner D1/T113 features:

- PWM support
- I2S multiple DIN pins support
- LCD tint or green display fix

This branch tracks mainline Linux. To see a list of commits from this branch
only use this command:

```
git log --left-only --oneline jookia_main...master
```

I regularly sign my Git commits with my hardware SSH key, if you would like to
verify them run this and subtitute TAG or COMMIT:

```
git -c gpg.ssh.allowedSignersFile=jookia/allowed_signers verify-tag TAG
git -c gpg.ssh.allowedSignersFile=jookia/allowed_signers log --oneline --show-signature COMMIT
```

My key fingerprint is ```SHA256:/gEvgms/9HpbgpcH+K7O4GYXmqkP7siJx9zHeEWRZTg```.

Please verify it by comparing it to my website: https://www.jookia.org/wiki/Keys

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
