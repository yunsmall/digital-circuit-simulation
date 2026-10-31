/* 三段分散布局，跨度约 2.1MB —— 远低于 ElfLoader 的 256MB 上限，
 * 因此整套东西能直接塞进 dcs --elf 的内存映像里。
 * 地址都是物理地址：内核 Image 本身就按 0x80200000 链接，
 * DTB 放在内核之前是 OpenSBI 的惯例，避免和内核 BSS 撞车。 */
ENTRY(_start)

SECTIONS
{
    . = 0x80000000;
    .boot : { *(.text.boot) }

    . = 0x80100000;
    .dtb : { *(.dtb) }

    . = 0x80200000;
    .kernel : { *(.kernel) }

    /* initramfs 放在内核映像之后，地址要和 DTB 的 linux,initrd-start 一致。
     * defconfig 内核约 20MB，会一直长到 0x8152xxxx，这里留足余量放 0x82000000 */
    . = 0x82000000;
    .initrd : { *(.initrd) }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
