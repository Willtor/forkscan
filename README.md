# The Forkscan Memory Reclamation System

## Introduction

Forkscan is a library for performing automated memory reclamation on concurrent data structures in C, C++, and DEF.

When one thread removes a node from a data structure, it isn't safe to call ***free*** if another thread may be accessing that node at the same time.  In place of ***free***, Forkscan provides the ***forkscan_retire*** function which reclaims the memory on the node when no remaining threads hold references to it.

## Compilation

At this time, the Forkscan is only supported on Linux.  Forkscan uses SuperMalloc by default (https://github.com/kuszmaul/SuperMalloc), but it requires a special non-C++ build.

```
% git clone https://github.com/kuszmaul/SuperMalloc.git
% cd SuperMalloc/release
% make PREFIX=__super_ NOCPPRUNTIME=true
```

Copy the resulting lib/supermalloc.a archive to the Forkscan directory and use ***make*** to build Forkscan.

```
% cp lib/supermalloc.a /path/to/forkscan/repo/
% cd /path/to/forkscan/repo
% make
```

The library will appear as ***libforkscan.so*** in the same directory as the source code.  If you want to install it on your system, use:

```
% sudo make install
```

The library will be installed at ***/usr/local/lib/libforkscan.so*** and the header file will be installed at ***/usr/local/include/forkscan.h***.

## Usage

Forkscan can be used in another code base by calling the collection function from that code, and building the other package with the library on the command line.  Forkscan is built into DEF and calling ***new***, ***retire***, and ***delete*** will call the appropriate Forkscan functions.

In C and C++, to access the library routines from your code, include the Forkscan header.

```
#include <forkscan.h>
```

Allocate memory using ***forkscan_malloc*** instead of ***malloc*** and ***forkscan_free*** instead of ***free***.  If the thread that wants to free memory is uncertain whether another thread may be using that memory, use ***forkscan_retire*** instead of ***free***.

To include the library in your build, install it as above and add the library to the link line given to GCC.

```
-lforkscan
```

## Semantics

Retiring a pointer causes it to be tracked by the Forkscan runtime library, and it will be freed for reuse when Forkscan can prove that no thread has (or can acquire) a reference to it.

For example, a lock-free linked list swings the previous node's next pointer, and the node is no longer reachable from the root.  The thread may call ***forkscan_retire*** after it performs the CAS that physically removes the node and then drop the reference.

Calling ***forkscan_retire*** on the same node multiple times will have the same consequences as calling ***free*** multiple times in single-threaded code.

To replace the underlying allocator (SuperMalloc), use the ***forkscan_set_allocator*** routine.  The function requires a ***malloc***, ***free***, and ***malloc_usable_size*** replacement functions.  ***malloc_usable_size*** is implemented by most allocators and returns the size (in bytes) of the given allocated block.  E.g.,

```
forkscan_set_allocator(malloc, free, malloc_usable_size);
```

## Recommendations

+ Use the default SuperMalloc, or install and use JE Malloc, TC-Malloc, or Hoard, which are known to be fast allocators in multi-threaded code.  Mixing ***malloc*** and ***free*** calls from different libraries can cause the program to crash.
+ Do **not** try to use ***retire*** as a stand-in for a general garbage collector.  It's optimized for concurrent data structures where ***retire*** is called on nodes for which public references have been eliminated.

## Bugs/Questions/Contributions

You can contact the maintainer, William M. Leiserson, at willtor@mit.edu.

We appreciate contributions of bug fixes, features, etc.  If you would like to contribute, please read the MIT License (LICENSE) carefully to be sure you agree to the terms under which this library is released.  If your name doesn't appear in the AUTHORS file, you can append your name to the list of authors along with your changes.
