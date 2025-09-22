#!/bin/bash

ANY_KERNEL3_DIR="$HOME/tools/AnyKernel3"
PARENT_DIR=`readlink -f ..`
OUT_DIR=`readlink -f .`/out

if [ ! -d $ANY_KERNEL3_DIR ]; then
    echo 'clone AnyKernel3 - Flashable Zip Template'
    git clone https://github.com/osm0sis/AnyKernel3 $ANY_KERNEL3_DIR
fi

if [ -e $OUT_DIR/arch/arm64/boot/Image ]; then
    cd $ANY_KERNEL3_DIR
    git reset --hard
    cp $OUT_DIR/arch/arm64/boot/Image zImage
    sed -i "s/ExampleKernel by osm0sis/kalama ksu kernel/g" anykernel.sh
    sed -i "s/do\.devicecheck=1/do\.devicecheck=0/g" anykernel.sh
    sed -i "s/=maguro/=/g" anykernel.sh
    sed -i "s/=toroplus/=/g" anykernel.sh
    sed -i "s/=toro/=/g" anykernel.sh
    sed -i "s/=tuna/=/g" anykernel.sh
    sed -i "s/platform\/omap\/omap_hsmmc\.0\/by-name\/boot/bootdevice\/by-name\/boot/g" anykernel.sh
    sed -i "s/backup_file/#backup_file/g" anykernel.sh
    sed -i "s/replace_string/#replace_string/g" anykernel.sh
    sed -i "s/insert_line/#insert_line/g" anykernel.sh
    sed -i "s/append_file/#append_file/g" anykernel.sh
    sed -i "s/patch_fstab/#patch_fstab/g" anykernel.sh
    sed -i "s/dump_boot/split_boot/g" anykernel.sh
    sed -i "s/write_boot/flash_boot/g" anykernel.sh
    zip -r9 $PARENT_DIR/kalama-gki_kernel_`cat $OUT_DIR/include/config/kernel.release`_`date '+%Y_%m_%d'`.zip * -x .git README.md *placeholder
    echo 'Success!'
else
    echo 'Build kernel first'
fi

