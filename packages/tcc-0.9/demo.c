/* AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
 *
 * demo.c —— 给 `cc` 用的示例源码：在计算器上画一条横幅。
 * 编译：cc C:\DATA\pkg\tcc-0.9\demo.c -o C:\DATA\demo.elf
 * 运行：runh C:\DATA\demo.elf
 */
#include "prime.h"
#include "hp_gfx.h"

int main(void)
{
    hp_gfx_init();
    hp_clear(0xff101820u);
    hp_fill_rect(0, 90, 320, 60, 0xff2f81f7u);
    hp_text(18, 108, "built by tcc", 0xffffffffu);
    hp_text(18, 126, "on the calculator", 0xffc8d4e0u);
    return 0;
}
