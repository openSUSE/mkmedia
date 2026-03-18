# mkmedia

mkmedia (formerly mksusecd) is a tool to create (open)SUSE Linux installation ISOs - either from scratch or
by modifying existing ISOs. It is **not** intended to create an installation repository or
completely new product media. For this, use [kiwi](https://opensuse.github.io/kiwi).

mkmedia makes it easy to apply modifications to existing install media, like:

- integrate driver updates ([DUD](http://ftp.suse.com/pub/people/hvogel/Update-Media-HOWTO/html))
- modify the installation system
- replace kernel and modules used during installation
- add boot options
- create and integrate add-on repositories
- change the default repositories used during installation
- create a mini ('network') boot iso from a regular dvd

The images mkmedia creates can be put both on a cd/dvd or a (usb-)disk. For this mkmedia creates
so-called 'isohybrid' images. If you are interested in the technical details,
you can read more about it [here](layout.md).

## Downloads

Packages for openSUSE and SLES are built at the [openSUSE Build Service](https://build.opensuse.org). You can grab

- [official releases](https://software.opensuse.org/package/mksusecd) or

- [latest stable versions](https://software.opensuse.org/download/package?project=home:snwint:ports&package=mksusecd)
  from my [ports](https://build.opensuse.org/package/show/home:snwint:ports/mksusecd) project

## Blog

See also my mini-series of articles around SUSE installation media and driver updates that highlight specific use-cases:

- [Make your own SUSE DVD – Tooling for SUSE and Multi-Linux installation / Live media](https://www.suse.com/c/make-your-own-suse-dvd-tooling-for-suse-and-multi-linux-installation-live-media)
- [Update the update process!](https://lizards.opensuse.org/2017/02/16/fun-things-to-do-with-driver-updates)
- [But what if I need a new kernel?](https://lizards.opensuse.org/2017/03/16/fun-things-to-do-with-driver-updates-2)
- [And what if I want to **remove** some files?](https://lizards.opensuse.org/2017/04/25/fun-things-to-do-with-driver-updates-3)
- [Encrypted installation media](https://lizards.opensuse.org/2017/11/17/encrypted-installation-media)

## Simple use cases

> **Note**
>
> _In all the examples below, when you use an iso image as source (and not a directory with the unpacked iso),
you must run mkmedia with root permissions as it will need to mount the iso image temporarily._

### 1. Update a package that is used in the installation system

We have a patch for yast2-core that is needed during installation and the
final system, e.g. for an AutoYaST installation. The patch is included in
yast2-core-3.1.12-0.x86_64.rpm.

- Create a Driver Update Disk (DUD) from yast2-core.rpm using [mkdud](https://github.com/openSUSE/mkdud?tab=readme-ov-file#mkdud):

    ```sh
    mkdud --create bug-free.dud --dist 13.2 yast2-core-3.1.12-0.x86_64.rpm
    ```

- Create a new ISO from the original openSUSE 13.2 ISO and the DUD:

   ```sh
   mkmedia --create bug-free.iso --initrd bug-free.dud openSUSE-13.2-DVD-x86_64.iso
   ```

Now you can use bug-free.iso as a replacement for openSUSE-13.2-DVD-x86_64.iso.

### 2 .Create a network install iso

Say, you need to walk around and install a lot from `http://foo/bar`, then do

```sh
mkmedia --create foo.iso --nano --net=http://foo/bar openSUSE-13.2-DVD-x86_64.iso
```

`--nano` puts only the necessary files for booting on the media (not the installer itself, for example).

### 3. Add some boot options

If you need to repeatedly enter the same boot options, create an iso that already has them:

```sh
mkmedia --create foo.iso --boot "sshd=1 password=*****" openSUSE-13.2-DVD-x86_64.iso
```

This would start an ssh daemon you can login to during installation.

### 4. Modify some other things on the install iso

- Unpack the iso into some temporary directory:

    ```sh
    mount -oloop,ro openSUSE-13.2-DVD-x86_64.iso /mnt
    mkdir /tmp/foo
    cp -a /mnt/* /tmp/foo
    chmod -R u+w /tmp/foo
    umount /mnt
    ```

- Do any changes you like in `/tmp/foo`

- Build a new iso

    ```sh
    mkmedia --create foo.iso /tmp/foo
    ```

## Installation

Often you will need [mkdud][1] along with mksusecd. Both mksusecd and mkdud are
included in openSUSE Tumbleweed. So on openSUSE Tumbleweed installation is as
simple as

```
zypper in mksusecd mkdud
```

[1]: https://github.com/openSUSE/mkdud

## openSUSE Development

To build, simply run `make`. Install with `make install`.

Basically every new commit into the master branch of the repository will be auto-submitted
to all current SUSE products. No further action is needed except accepting the pull request.

Submissions are managed by a SUSE internal [jenkins](https://jenkins.io) node in the InstallTools tab.

Each time a new commit is integrated into the master branch of the repository,
a new submit request is created to the openSUSE Build Service. The devel project
is [system:install:head](https://build.opensuse.org/package/show/system:install:head/mksusecd).

`*.changes` and version numbers are auto-generated from git commits, you don't have to worry about this.

The spec file is maintained in the Build Service only. If you need to change it for the `master` branch,
submit to the
[devel project](https://build.opensuse.org/package/show/system:install:head/mksusecd)
in the build service directly.

Development happens exclusively in the `master` branch. The branch is used for all current products.

You can find more information about the changes auto-generation and the
tools used for jenkis submissions in the [linuxrc-devtools
documentation](https://github.com/openSUSE/linuxrc-devtools#opensuse-development).
