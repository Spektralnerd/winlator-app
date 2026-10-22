/**************************************************************************
 *
 * Copyright (C) 2018 Chromium.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "virgl_server_shm.h"

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>

#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/types.h>
#include <linux/ashmem.h>

static int memfd_create(const char *name, unsigned int flags)
{
#ifdef __NR_memfd_create
    return syscall(__NR_memfd_create, name, flags);
#else
    return -1;
#endif
}

/* memfd gibt es erst ab Linux 3.17; auf älteren Kerneln (z. B. Android 7 auf 3.4) ashmem */
static int ashmem_create(const char *name, size_t size)
{
   int fd = open("/dev/ashmem", O_RDWR);
   if (fd < 0)
      return -1;

   char buf[ASHMEM_NAME_LEN] = {0};
   strncpy(buf, name, sizeof(buf) - 1);
   if (ioctl(fd, ASHMEM_SET_NAME, buf) < 0 || ioctl(fd, ASHMEM_SET_SIZE, size) < 0) {
      close(fd);
      return -1;
   }
   return fd;
}

int virgl_server_new_shm(uint32_t handle, size_t size)
{
   int fd, ret;
   int length = snprintf(NULL, 0, "virgl-res-%u", handle);
   char *str = malloc(length + 1);
   snprintf(str, length + 1, "virgl-res-%u", handle);

   fd = memfd_create(str, MFD_ALLOW_SEALING);
   if (fd < 0) {
      fd = ashmem_create(str, size);
      free(str);
      return fd < 0 ? -errno : fd;
   }
   free(str);

   ret = ftruncate(fd, size);
   if (ret < 0) {
      close(fd);
      return -errno;
   }

   return fd;
}
