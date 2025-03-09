# hardlinker
Copy directory tree with hard links, to save space.

There are two modes, default and static.

# default mode

In default mode, the utility takes three directory trees:
- source
- destination
- reference

Destination is expected to not exist yet. It is created by the utility.

Source is scanned recursively.

For every non-regular-file object in source (directory, symlink, etc.) identical object, with identical hierarchy, is created in destination.

For every regular file in source:
- if there exist an identical file in reference directory tree with identical hierarchy, a hardlink in destination is created
- if there is no such file in reference directory, or it is different, i.e. it has different permission, owner, group, size, content, or extended attributes (xattr), it is copied from source

# static mode
 
In static mode (with argument -static), the utility takes two directory trees:
- new directory
- old directory

Both directories are expected to exist.

New directory is scanned recursively. For every regular file in new directory tree:
- if there exist an identical file in old directory tree with identical hierarchy, it is deleted and a hardlink is created instead
- if there is no such file in reference directory, or it is different, i.e. it has different permission, owner, group, size, content, or extended attributes (xattr), it is left intact


