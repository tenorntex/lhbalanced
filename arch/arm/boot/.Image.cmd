cmd_arch/arm/boot/Image := arm-linux-androideabi-objcopy -O binary -R .note -R .note.gnu.build-id -R .comment -S  vmlinux arch/arm/boot/Image
