/* AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查） */
#ifndef FS_H
#define FS_H

int  fs_read(const char *path, unsigned char **buf, unsigned *len);
void fs_free(void *p);
int  fs_is_dir(const char *path);
int  fs_touch(const char *path);
int  fs_mkdir(const char *path);
int  fs_rmdir(const char *path);
int  fs_remove(const char *path);
int  fs_rename(const char *from, const char *to);
int  fs_copy(const char *from, const char *to);
/* 列目录；cb 返回非 0 时提前结束。返回条目数，失败 -1 */
int  fs_list(const char *dir, int (*cb)(const char *name, unsigned size,
                                        int is_dir, void *ud), void *ud);

#endif
