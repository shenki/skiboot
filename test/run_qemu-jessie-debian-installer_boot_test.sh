#!/bin/bash


if [ -z "$QEMU" ]; then
    QEMU="qemu-system-ppc64"
fi

if [ ! `command -v qemu-system-ppc64` ]; then
    echo 'Could not find executable QEMU. Skipping hello_world test';
    exit 0;
fi

if [ -n "$KERNEL" ]; then
    echo 'Please rebuild skiboot without KERNEL set. Skipping boot test';
    exit 0;
fi

if [ ! `command -v expect` ]; then
    echo 'Could not find expect binary. Skipping boot test';
    exit 0;
fi

if [ ! -f debian-jessie-vmlinux ]; then
    echo 'No debian-jessie-vmlinux kernel! Run opal-ci/fetch-debian-jessie-installer.sh : Skipping test.';
    exit 0;
fi

if [ ! -f debian-jessie-initrd.gz ]; then
    echo 'No debian-jessie-initrd.gz! Run opal-ci/fetch-debian-jessie-installer.sh : Skipping test';
    exit 0;
fi

T=`mktemp  --tmpdir skiboot_qemu_debian-jessie-boot_test.XXXXXXXXXX`
#D=`mktemp  --tmpdir debian-jessie-install.qcow2.XXXXXXXXXX`

# In future we should do full install:
# FIXME: -append "DEBIAN_FRONTEND=text locale=en_US keymap=us hostname=OPALtest domain=unassigned-domain rescue/enable=true"

#$QEMU_PATH/../qemu-img  create -f qcow2 $D 128G 2>&1 > $T

( cat <<EOF | expect
set timeout 600
spawn $QEMU -m 2G -M powernv -kernel debian-jessie-vmlinux -initrd debian-jessie-initrd.gz -nographic -device ipmi-bmc-sim,id=ipmi0 -device isa-ipmi-bt,bmc=ipmi0
expect {
timeout { send_user "\nTimeout waiting for petitboot\n"; exit 1 }
eof { send_user "\nUnexpected EOF\n;" exit 1 }
"Machine Check Stop" { exit 1;}
"Kernel panic - not syncing" { exit 2;}
"Starting system log daemon"
}
close
wait
exit 0
EOF
) 2>&1 >> $T
E=$?

if [ $E -eq 0 ]; then
    rm $T $D
else
    cat $T
    echo "Boot Test FAILED. Results in $T, Disk $D";
fi

exit $E;
