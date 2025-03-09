#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

int usage()
{
    printf("hardlinker [-noxattr] <source> <destination> <reference>\n");
    printf("           recursively copy all from <source> to <destination>\n");
    printf("           making hardlinks to <reference> wherever possible\n");
    printf("hardlinker [-noxattr] -static <directory> <reference>\n");
    printf("           recursively scan <directory> looking for duplicates\n");
    printf("           in <reference> and replacing them with hardlinks\n");
}

enum
{
    FAIL_CHMOD      =          1,     
    FAIL_CHOWN      =          2,
    FAIL_CREAT      =          4,
    FAIL_DIFF       =          8,
    FAIL_HL         =         16,
    FAIL_MKNOD      =         32,
    FAIL_MMAP       =         64,
    FAIL_OPENDIR    =        128,
    FAIL_READLINK   =        256,
    FAIL_COPY       =        512,
    FAIL_XATTR      =       1024,

    FAIL_MUST       = 0x7fffffff,
};

char * src_path;
char * dst_path;
char * ref_path;

int xattr_max = 0x10000;
char * xattr_name_buf[2];
char ** xattr_pname_buf[2];
char * xattr_value_buf[2];
int xattr_names_size[2];
int xattr_n_names[2];

int opt_debug = 0;
int opt_static = 0;
int opt_noxattr = 0;
int opt_fail = 0;
int opt_verbose = 0;
int opt_help = 0;
int opt_off = 0;
int is_root = 0;

char compath[PATH_MAX];
int compath_i = 0;

void debug(const char *format, ...)
{
    if ( opt_debug )
    {
        va_list ap;
        va_start(ap, format);
        vfprintf(stderr, format, ap);
        va_end(ap);
    }
}

int compath_push(const char *name)
{
    int frame = compath_i;
    compath_i += snprintf(compath + compath_i, sizeof(compath) - compath_i, "/%s", name);
    compath[compath_i] = 0;
    return frame;
}

void compath_pop(int frame)
{
    compath_i = frame;
    compath[compath_i] = 0;
}

void errhandle(const char *prefix, const char * fn, const char *path, int fail)
{
    if ( prefix )
    {
        fprintf(stderr, "ERROR: %s%s/%s: %s: %m\n", prefix, compath, path, fn);
        if ((fail & opt_fail))
        {
            exit(1);
        }
    }
}

void metacopy(int src_fd, int dst_fd)
{
    struct stat src_stat, dst_stat;
}

void debug_stat(int res, const struct stat *st)
{
    if ( res == 0 )
    {
        debug("%5d %5d %6o|", st->st_uid, st->st_gid, st->st_mode);
    }
    else
    {
        debug("      %-12s|", strerrorname_np(res));
    }
}

static inline int ndirfd(DIR * dir)
{
    return dir ? dirfd(dir) : AT_FDCWD;
}

int wrap_stat(DIR * dir, const char *name, struct stat *st)
{
    return fstatat(ndirfd(dir), name, st, AT_NO_AUTOMOUNT | AT_SYMLINK_NOFOLLOW) == 0 ? 0 : errno;
}

int wrap_open(const char *prefix, DIR * dir, const char *name, int mode, int fail)
{
    int fd = openat(ndirfd(dir), name, mode);
    if ( fd == -1 )
    {
        errhandle(prefix, "open", name, fail);
    }
    return fd;
}

int wrap_creat(const char *prefix, DIR * dir, const char *name, mode_t mode)
{
    int fd = openat(ndirfd(dir), name, O_WRONLY | O_TRUNC | O_CREAT, mode);
    if ( fd == -1 )
    {
        errhandle(prefix, "creat", name, FAIL_CREAT);
    }
    return fd;
}

DIR *wrap_opendir_root(const char *path)
{
    DIR *ret = opendir(path);
    if (!ret)
    {
        errhandle("", "opendir", path, FAIL_MUST);
    }
    return ret;
}

DIR *wrap_opendir(const char *prefix, DIR * dir, const char *path)
{
    int fd = wrap_open(prefix, dir, path, O_RDONLY, FAIL_OPENDIR);
    if (fd == -1)
    {
        return NULL;
    }
    DIR *ret = fdopendir(fd);
    if (!ret)
    {
        errhandle(prefix, "opendir", path, FAIL_OPENDIR);
    }
    return ret;
}

void *wrap_mmap(const char *prefix, size_t size, int fd, off_t offset, const char *name)
{
    void *ret = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, offset);
    if (ret == MAP_FAILED)
    {
        fprintf(stderr, "    %ld, ... %d, %ld\n", size, fd, offset);
        errhandle(prefix, "mmap", name, FAIL_MMAP);
    }
    return ret;
}

int wrap_link(const char *prefix, DIR * src_dir, DIR * dst_dir, const char *name)
{
    int result = linkat(ndirfd(src_dir), name, ndirfd(dst_dir), name, 0);
    if ( result == -1 )
    {
        errhandle(prefix, "link", name, FAIL_HL);
    }
    return result;
}

int wrap_mkdir_p(const char *prefix, DIR * dir, const char *name, mode_t mode)
{
    int result = mkdirat(ndirfd(dir), name, mode);
    if ( result == -1 )
    {
        if ( errno == EEXIST )
        {
            result = 0;
            errno = 0;
        }
        else
        {
            errhandle(prefix, "mkdir", name, FAIL_MUST);
        }
    }
    return result;
}

int wrap_mknod(const char *prefix, DIR * dir, const char *name, mode_t mode, dev_t dev)
{
    int result = mknodat(ndirfd(dir), name, mode, dev);
    if ( result == -1 )
    {
        errhandle(prefix, "mknod", name, FAIL_MKNOD);
    }
    return result;
}

int wrap_readlink(const char *prefix, DIR * dir, const char *name, char * buf, size_t bufsize)
{
    int result = readlinkat(ndirfd(dir), name, buf, bufsize);
    if ( result == -1 )
    {
        errhandle(prefix, "readlink", name, FAIL_READLINK);
    }
    else
    {
        if ( result >= bufsize )
        {
            result = bufsize - 1;
        }
        buf[result] = 0;
    }
    return result;
}

int wrap_symlink(const char *prefix, const char * target, DIR * dir, const char *name)
{
    int result = symlinkat(target, ndirfd(dir), name);
    if ( result == -1 )
    {
        errhandle(prefix, "symlink", name, FAIL_MUST);
    }
    return result;
}

int wrap_remove(const char *prefix, DIR * dir, const char *name)
{
    int result = unlinkat(ndirfd(dir), name, 0);
    if ( result == -1 )
    {
        errhandle(prefix, "unlink", name, FAIL_MUST);
    }
    return result;
}

int void_strcmp(const void *a, const void *b)
{
    return strcmp((const char*)a, (const char*)b);
}

void load_xattr_names(const char *prefix, int fd, int reg)
{
    ssize_t result = flistxattr(fd, xattr_name_buf[reg], xattr_max);
    xattr_n_names[reg] = 0;
    if ( result < 0 )
    {
        errhandle(prefix, "listxattr", "", FAIL_XATTR);
        xattr_name_buf[reg][0] = 0;
        xattr_names_size[reg] = 0;
        return;
    }
    xattr_names_size[reg] = result;
    size_t buflen = result;
    char *key = xattr_name_buf[reg];
    while ( buflen > 0 )
    {
        xattr_pname_buf[reg][xattr_n_names[reg]++] = key;
        int keylen = strlen(key) + 1;
        buflen -= keylen;
        key += keylen;
    }
    qsort(xattr_pname_buf[reg], xattr_n_names[reg], sizeof(char*), void_strcmp);
}

int cmp_xattr_names()
{
    if (xattr_names_size[0] != xattr_names_size[1])
    {
        return 1;
    }
    return memcmp(xattr_name_buf[0], xattr_name_buf[1], xattr_names_size[0]);
}

int cmp_xattr_values(const char *prefix, int src_fd, int ref_fd)
{
    int n = xattr_n_names[0];
    for ( int i = 0; i < n; ++i )
    {
        int src_result = fgetxattr( src_fd, xattr_pname_buf[0][i], xattr_value_buf[0], xattr_max );
        if ( src_result < 0 )
        {
            errhandle(prefix, "getxattr", "", FAIL_XATTR);
            continue;
        }
        int ref_result = fgetxattr( ref_fd, xattr_pname_buf[0][i], xattr_value_buf[1], xattr_max );
        if ( ref_result < 0 )
        {
            errhandle(prefix, "getxattr", "", FAIL_XATTR);
            continue;
        }
        if ( src_result != ref_result )
        {
            return 1;
        }
        if ( memcmp( xattr_value_buf[0], xattr_value_buf[1], src_result ) )
        {
            return 1;
        }
    }
    return 0;
}

int transfer_mode(const char *prefix, const struct stat * st, DIR * dir, const char * name)
{
    if (S_ISLNK(st->st_mode))
    {
        return 0;
    }
    int result;
    result = fchmodat(ndirfd(dir), name, st->st_mode & 07777, 0);
    if ( result == -1 )
    {
        errhandle(prefix, "chmod", name, FAIL_CHMOD);
    }
    return result;
}

int transfer_owner(const char *prefix, const struct stat * st, DIR * dir, const char * name)
{
    int result;
    result = fchownat(ndirfd(dir), name, st->st_uid, st->st_gid, AT_SYMLINK_NOFOLLOW);
    if ( result == -1 )
    {
        errhandle(prefix, "chown", name, FAIL_CHOWN);
    }
    return result;
}

int diff_content(DIR * src_dir, DIR * ref_dir, const char *name, size_t size)
{
    int ret = 0;
    int src_fd = wrap_open(src_path, src_dir, name, O_RDONLY, FAIL_DIFF);
    if ( src_fd < 0 )
    {
        ret |= 8;
        goto fail_src_fd;
    }
    int ref_fd = wrap_open(ref_path, ref_dir, name, O_RDONLY, FAIL_DIFF);
    if ( ref_fd < 0 )
    {
        ret |= 8;
        goto fail_ref_fd;
    }
    if ( size )
    {
        void * src_map = wrap_mmap(src_path, size, src_fd, 0, name);
        if ( src_map == MAP_FAILED )
        {
            ret |= 8;
            goto fail_src_map;
        }
        void * ref_map = wrap_mmap(ref_path, size, ref_fd, 0, name);
        if ( ref_map == MAP_FAILED )
        {
            ret |= 8;
            goto fail_ref_map;
        }
        if ( memcmp(src_map, ref_map, size) )
        {
            ret |= 1;
        }
        munmap(ref_map, size);
fail_ref_map:
        munmap(src_map, size);
fail_src_map:
        ;
    }
    while(!opt_noxattr)
    {
        load_xattr_names(src_path, src_fd, 0);
        load_xattr_names(ref_path, ref_fd, 1);
        if (cmp_xattr_names())
        {
            ret |= 2;
            break;
        }
        if (cmp_xattr_values(src_path, src_fd, ref_fd))
        {
            ret |= 4;
            break;
        }
        break;
    }

    close(ref_fd);
fail_ref_fd:
    close(src_fd);
fail_src_fd:
    return ret;
}

void transfer_xattr(DIR * src_dir, DIR * dst_dir, const char *src_name, const char *dst_name)
{
    int src_fd = wrap_open(src_path, src_dir, src_name, O_RDONLY, 0);
    if ( src_fd < 0 )
    {
        goto fail_src_fd;
    }
    int dst_fd = wrap_open(dst_path, dst_dir, dst_name, O_RDONLY, 0);
    if ( dst_fd < 0 )
    {
        goto fail_dst_fd;
    }
    load_xattr_names(src_path, src_fd, 0);
    int n = xattr_n_names[0];
    int result;
    for ( int i = 0; i < n; ++i )
    {
        const char * key = xattr_pname_buf[0][i];
        result = fgetxattr(src_fd, key, xattr_value_buf[0], xattr_max);
        if (result == -1)
        {
            errhandle(src_path, "fgetxattr", src_name, FAIL_XATTR);
            continue;
        }
        result = fsetxattr(dst_fd, key, xattr_value_buf[0], result, 0);
        if (result == -1)
        {
            errhandle(dst_path, "fsetxattr", dst_name, FAIL_XATTR);
        }
    }
    close(dst_fd);
fail_dst_fd:
    close(src_fd);
fail_src_fd:
    ;
}

void copy_file(DIR * src_dir, DIR * dst_dir, const char *name, size_t size, mode_t mode)
{
    int dst_fd = wrap_creat(dst_path, dst_dir, name, mode);
    if ( dst_fd != -1 && size != 0 )
    {
        int src_fd = wrap_open(src_path, src_dir, name, O_RDONLY, FAIL_COPY);
        if ( src_fd == -1 )
        {
            goto fail_src_fd;
        }
        void * src_map = wrap_mmap(src_path, size, src_fd, 0, name);
        if ( src_map == MAP_FAILED )
        {
            goto fail_src_map;
        }
        size_t left = size;
        char * p = src_map;
        while ( left )
        {
            ssize_t readsize = write(dst_fd, p, left);
            if ( readsize <= 0 )
            {
                errhandle(dst_path, "write", name, FAIL_COPY);
                break;
            }
            left -= readsize;
            p += readsize;
        }
        munmap(src_map, size);
fail_src_map:
        close(src_fd);
fail_src_fd:
        ;
    }
    close(dst_fd);
}

void dive(DIR * src_dir, DIR * dst_dir, DIR * ref_dir)
{
    struct dirent *dent;
    while ( errno = 0, (dent = readdir(src_dir)) != NULL)
    {
        const char * name = dent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        {
            continue;
        }
        
        struct stat src_stat[1];
        struct stat ref_stat[1];
        int src_stat_res;
        int ref_stat_res;

        src_stat_res = wrap_stat(src_dir, name, src_stat);
        if ( src_stat_res )
        {
            continue;
        }
        if ( ref_dir )
        {
            ref_stat_res = wrap_stat(ref_dir, name, ref_stat);
        }
        else
        {
            ref_stat_res = ENOENT;
        }

        if ( opt_debug )
        {
            debug_stat(src_stat_res, src_stat);
            debug_stat(ref_stat_res, ref_stat);
            debug(" %-40s %-40s", compath, name);
        }

        int diff = 0;
        int hl = 0;
        int dc;

        if ( !S_ISREG(src_stat->st_mode) )
        {
            debug(" noreg\n");
            diff = 1;
        }
        else if ( ref_stat_res )
        {
            debug(" ref_stat_res\n");
            diff = 1;
        }
        else if ( src_stat->st_uid != ref_stat->st_uid )
        {
            debug(" st_uid\n");
            diff = 1;
        }
        else if ( src_stat->st_gid != ref_stat->st_gid )
        {
            debug(" st_gid\n");
            diff = 1;
        }
        else if ( src_stat->st_mode != ref_stat->st_mode )
        {
            debug(" st_mode\n");
            diff = 1;
        }
        else if ( src_stat->st_size != ref_stat->st_size )
        {
            debug(" st_size\n");
            diff = 1;
        }
        else if ( src_stat->st_dev == ref_stat->st_dev && src_stat->st_ino == ref_stat->st_ino )
        {
            debug(" ===\n");
            hl = 1;
        }
        else if (dc = diff_content(src_dir, ref_dir, name, src_stat->st_size))
        {
            char sep = ' ';
            if ( dc & 1 )
            {
                debug("%ccontent", sep);
                sep = ',';
            }
            if ( dc & 2 )
            {
                debug("%cxattr_names", sep);
                sep = ',';
            }
            if ( dc & 4 )
            {
                debug("%cxattr_values", sep);
                sep = ',';
            }
            if ( dc & 8 )
            {
                debug("%cerror", sep);
                sep = ',';
            }
            debug("\n");
            diff = 1;
        }
        else
        {
            debug(" ==\n");
        }

        if (diff)
        {
            if (dst_dir)
            {
                if ( S_ISREG(src_stat->st_mode) )
                {
                    if (opt_verbose)
                    {
                        fprintf(stderr, "COPY %s/%s\n", compath, name);
                    }
                    copy_file(src_dir, dst_dir, name, src_stat->st_size, src_stat->st_mode);
                }
                else if ( S_ISLNK(src_stat->st_mode) )
                {
                    char lnk[PATH_MAX];
                    if ( -1 != wrap_readlink(src_path, src_dir, name, lnk, sizeof(lnk)) )
                    {
                        wrap_symlink(dst_path, lnk, dst_dir, name);
                    }
                }
                else if ( S_ISDIR(src_stat->st_mode) )
                {
                    wrap_mkdir_p(dst_path, dst_dir, name, src_stat->st_mode & 07777);
                    DIR * nx_src_dir = wrap_opendir(src_path, src_dir, name);
                    DIR * nx_dst_dir = wrap_opendir(dst_path, dst_dir, name);
                    DIR * nx_ref_dir = ref_dir ? wrap_opendir(0, ref_dir, name) : NULL;
                    int frame = compath_push(name);
                    dive(nx_src_dir, nx_dst_dir, nx_ref_dir);
                    compath_pop(frame);
                    if (nx_ref_dir)
                    {
                        closedir(nx_ref_dir);
                    }
                    closedir(nx_dst_dir);
                    closedir(nx_src_dir);
                }
                else
                {
                    wrap_mknod(dst_path, dst_dir, name, src_stat->st_mode, src_stat->st_rdev);
                }
                transfer_mode(dst_path, src_stat, dst_dir, name);
                transfer_owner(dst_path, src_stat, dst_dir, name);
                if ( !opt_noxattr && (S_ISREG(src_stat->st_mode) || S_ISDIR(src_stat->st_mode) ) )
                {
                    transfer_xattr(src_dir, dst_dir, name, name);
                }
            }
            else
            {
                if (S_ISDIR(src_stat->st_mode))
                {
                    DIR * nx_src_dir = wrap_opendir(src_path, src_dir, name);
                    DIR * nx_ref_dir = ref_dir ? wrap_opendir(0, ref_dir, name) : NULL;
                    int frame = compath_push(name);
                    dive(nx_src_dir, NULL, nx_ref_dir);
                    compath_pop(frame);
                    if (nx_ref_dir)
                    {
                        closedir(nx_ref_dir);
                    }
                    closedir(nx_src_dir);
                }
                else if (opt_verbose && S_ISREG(src_stat->st_mode))
                {
                    printf("KEEP %s/%s\n", compath, name);
                }
            }
        }
        else
        {
            if ( dst_dir )
            {
                wrap_link(dst_path, ref_dir, dst_dir, name);
            }
            else
            {
                if ( ! hl )
                {
                    wrap_remove(src_path, src_dir, name);
                    wrap_link(src_path, ref_dir, src_dir, name);
                }
            }
        }
    }
    if (errno != 0)
    {
        fprintf(stderr, "ERROR: READDIR: {src}%s: %s\n", compath, strerror(errno));
        exit(1);
    }
}

int main(int argc, char *argv[])
{
    compath[0] = 0;
    compath[1] = 0;

    int n_posarg = 0;
    const int n_posarg_max = 3;
    char *posarg[n_posarg_max];

    const char *fail_str = "fail=";
    const int fail_len = strlen(fail_str);
    for ( int i = 1; i < argc; ++i )
    {
        char *arg = argv[i];
        if (!opt_off && arg[0] == '-')
        {
            ++arg;
            opt_off      |=! strcmp(arg, "-");
            opt_noxattr  |=! strcmp(arg, "noxattr");
            opt_static   |=! strcmp(arg, "static");
            opt_debug    |=! strcmp(arg, "debug");
            opt_verbose  |=! strcmp(arg, "verbose");
            opt_help     |=! strcmp(arg, "help");
            opt_help     |=! strcmp(arg, "-help");
            opt_help     |=! strcmp(arg, "h");
            if (!memcmp(arg, fail_str, fail_len))
            {
                opt_fail = 0;
                sscanf(arg + fail_len, "%i", &opt_fail);
            }
        }
        else if ( n_posarg < n_posarg_max )
        {
            posarg[n_posarg++] = arg;
        }
    }
    opt_fail |= FAIL_MUST;

    if (opt_help)
    {
        usage();
        exit(0);
    }

    if (!opt_noxattr)
    {
        xattr_name_buf[0] = malloc(xattr_max);
        xattr_name_buf[1] = malloc(xattr_max);
        xattr_pname_buf[0] = malloc(xattr_max);
        xattr_pname_buf[1] = malloc(xattr_max);
        xattr_value_buf[0] = malloc(xattr_max);
        xattr_value_buf[1] = malloc(xattr_max);
    }

    if (opt_static)
    {
        if ( n_posarg != 2 )
        {
            usage();
            exit(1);
        }
        src_path = posarg[0];
        ref_path = posarg[1];
        DIR * src_root = wrap_opendir_root(src_path);
        DIR * ref_root;
        if (access(ref_path, X_OK))
        {
            ref_root = NULL;
        }
        else
        {
            ref_root = wrap_opendir_root(ref_path);
        }

        dive(src_root, NULL, ref_root);
    }
    else
    {
        if ( n_posarg != 3 )
        {
            usage();
            exit(1);
        }
        src_path = posarg[0];
        dst_path = posarg[1];
        ref_path = posarg[2];

        if (!access(dst_path, X_OK))
        {
            fprintf(stderr, "%s already exists\n", dst_path);
            exit(3);
        }

        struct stat src_stat;
        if (wrap_stat(NULL, src_path, &src_stat))
        {
            fprintf(stderr, "%s does not exist\n", src_path);
            exit(3);
        }
        mkdir(dst_path, src_stat.st_mode);
        transfer_owner(dst_path, &src_stat, NULL, dst_path);
        transfer_mode(dst_path, &src_stat, NULL, dst_path);
        if (!opt_noxattr)
        {
            transfer_xattr(NULL, NULL, src_path, dst_path);
        }

        DIR * src_root = wrap_opendir_root(src_path);
        DIR * dst_root = wrap_opendir_root(dst_path);
        DIR * ref_root;

        if (access(ref_path, X_OK))
        {
            ref_root = NULL;
        }
        else
        {
            ref_root = wrap_opendir_root(ref_path);
        }

        dive(src_root, dst_root, ref_root);
    }


    return 0;
}
