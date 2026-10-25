/* AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
 *
 * fw_svc.S —— 固件 SVC 包装（约定：push {r0}; push {lr}; svc N）。
 * 固件返回时弹出这两个字直接回到调用者，所以包装函数没有收尾指令。
 */
    .arm
    .syntax unified
    .text

    .globl fw_malloc
fw_malloc:
    push {r0}
    push {lr}
    svc 0x10037

    .globl fw_free
fw_free:
    push {r0}
    push {lr}
    svc 0x1003a

    .globl fw_fopen
fw_fopen:
    push {r0}
    push {lr}
    svc 0x1026f

    .globl fw_fclose
fw_fclose:
    push {r0}
    push {lr}
    svc 0x100ca

    .globl fw_fread
fw_fread:
    push {r0}
    push {lr}
    svc 0x100d4

    .globl fw_filesize
fw_filesize:
    push {r0}
    push {lr}
    svc 0x100cb

    .globl fw_find_first
fw_find_first:
    push {r0}
    push {lr}
    svc 0x10270

    .globl fw_find_next
fw_find_next:
    push {r0}
    push {lr}
    svc 0x10271

    .globl fw_find_close
fw_find_close:
    push {r0}
    push {lr}
    svc 0x100da

    .globl fw_get_attr
fw_get_attr:
    push {r0}
    push {lr}
    svc 0x10272

    .globl fw_mkdir
fw_mkdir:
    push {r0}
    push {lr}
    svc 0x10277

    .globl fw_rmdir
fw_rmdir:
    push {r0}
    push {lr}
    svc 0x10278

    .globl fw_remove
fw_remove:
    push {r0}
    push {lr}
    svc 0x10274

    .globl fw_rename
fw_rename:
    push {r0}
    push {lr}
    svc 0x10275

    .globl fw_copy
fw_copy:
    push {r0}
    push {lr}
    svc 0x10276
