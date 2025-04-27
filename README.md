# Crystal HD Hardware Decoder Driver on Ubuntu 13.04 Linux kernel 6.8.0-58
## Broadcom BCM70012 & BCM70015

After a lot a retries to get the rigth experience with the Crystal HD on Ubuntu, 

**1. Install required files**

    sudo apt-get install checkinstall git-core autoconf build-essential subversion dpkg-dev fakeroot pbuilder build-essential dh-make debhelper devscripts patchutils quilt git-buildpackage pristine-tar git yasm zlib1g-dev zlib1g libzip-dev libx11-dev libx11-dev libxv-dev vstream-client-dev libgtk2.0-dev libpulse-dev libxxf86dga-dev x11proto-xf86dga-dev git libgstreamermm-1.0-dev libgstreamer1.0-dev automake libtool python-appindicator flex bision
    
**2. Get the source**

Get the driver source code from the git repository.

    git clone https://github.com/dbason/crystalhd.git

_The original repo source is available at git://git.linuxtv.org/jarod/crystalhd.git_
    
**3. Compile driver, install libraries, and load driver**

Use make command to compile driver. If you have multiple core processor then use the "-j2" or "-j4" option (2 or 4 is the number of cores). This will speed up the make process.

    cd crystalhd/driver/linux
    autoconf
    ./configure
    make -j2
    sudo make install
    
**4. Install the libraries.**

    cd ../../linux_lib/libcrystalhd/
    make -j2
    sudo make install 
    
**5. Load the driver.**

    sudo modprobe crystalhd
    
**6. Reboot your system** , then check if 'crystalhd' is listed in the output of the following commands.

    lsmod
    dmesg | grep crystalhd
    
 Then you should see something like this:
 
    [    4.349765] Loading crystalhd v3.10.0
    [    4.349823] crystalhd 0000:02:00.0: Starting Device:0x1615
    [    4.351848] crystalhd 0000:02:00.0: irq 43 for MSI/MSI-X
    [  108.512135] crystalhd 0000:02:00.0: Opening new user[0] handle
    [  258.976583] crystalhd 0000:02:00.0: Closing user[0] handle via ioctl with mode 10200

Now is time to enjoy our FullHD content. 

I'm using XMBC , VLC (2.1.0), Mplayer2, GStreamer because they are using (they should) the Crystal HD decoder libraries.

For example , lets try VLC :

    vlc --codec=crystalhd ourgreatfullhdmedia.mkv
    
Now runs smoothly rigth ?

# After kernel update

Reinstall the driver.

    cd crystalhd/driver/linux
    sudo make install


Btw this instructions referred to http://knowledge.evot.biz/documentation/how-to-compile-and-install-the-broadcom-crystal-hd-hardware-decoder-bcm70012-70015-driver-on-ubuntu and fixed some issues appeared using a patch from M25 user at https://bbs.archlinux.org/viewtopic.php?pid=1253622#p1253622

So, the sources on this repository are updated with the fixes and patches in order to make your life easier.

## Recent Updates (2024)

### Compilation Fixes by [Assistant]

Fixed several compilation and linking issues to support modern systems:

1. **Sleep Function Fix**
   - Issue: `usleep` undefined in examples/hellobcm.cpp
   - Solution: Replaced with existing `msleep(1)` function
   - Affected file: examples/hellobcm.cpp

2. **Library Linking Order**
   - Issue: Undefined references to DtsDeviceOpen and other library functions
   - Original command:
     ```bash
     g++ -I../include/ -I../linux_lib/libcrystalhd/ -D__LINUX_USER__ -lcrystalhd -lpthread -o hellobcm hellobcm.cpp
     ```
   - Fixed command:
     ```bash
     g++ -I../include/ -I../linux_lib/libcrystalhd/ -D__LINUX_USER__ hellobcm.cpp -o hellobcm -lcrystalhd -lpthread
     ```
   - Note: Library flags must come after source files for proper linking

### Testing
- Tested on Ubuntu with kernel 6.8.0-58
- Successfully compiled example code
- Verified basic device initialization and status checking

## History

See [HISTORY.md](HISTORY.md) for a rough history of the various versions of this driver floating around the web.

## Troubleshooting Common Issues

### Compilation Fixes

1. **usleep undefined error**
   - Solution: Replace `usleep(1000)` with `msleep(1)` using the existing msleep function
   - File modified: examples/hellobcm.cpp

2. **Undefined reference to DtsDeviceOpen and other library functions**
   - Problem: Library linking order issue
   - Original command:
     ```bash
     g++ -I../include/ -I../linux_lib/libcrystalhd/ -D__LINUX_USER__ -lcrystalhd -lpthread -o hellobcm hellobcm.cpp
     ```
   - Fixed command:
     ```bash
     g++ -I../include/ -I../linux_lib/libcrystalhd/ -D__LINUX_USER__ hellobcm.cpp -o hellobcm -lcrystalhd -lpthread
     ```
   - Note: Library flags (-l) must come after the source file

These changes allow successful compilation of the example code on modern systems.

## Contributing
If you find issues or have improvements, please feel free to submit a pull request or open an issue.

Last updated: [Current Date]
