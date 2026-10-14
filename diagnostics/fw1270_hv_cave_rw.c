/* SPDX-License-Identifier: GPL-3.0-or-later
 * Probe one guarded eight-byte write and restoration in the 12.70 HV cave.
 * Run only after NPT clear on the same launch-model boot. No code is executed.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#define CAVE_PA UINT64_C(0x62806f00)
#define EXPECTED UINT64_C(0xcccccccccccccccc)
#define PROBE UINT64_C(0xcccccccccccccc90)

extern unsigned long KERNEL_ADDRESS_DMAP_BASE;
extern int sceKernelGetMainSocId(void);

static int write_all(int fd, const char *bytes, size_t size)
{
  while (size) {
    ssize_t n = write(fd, bytes, size);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return -1;
    bytes += (size_t)n;
    size -= (size_t)n;
  }
  return 0;
}

static int log_line(int fd, const char *line)
{
  return write_all(fd, line, strlen(line)) == 0 ? fsync(fd) : -1;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  int soc = sceKernelGetMainSocId();
  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  uint64_t dmap = (uint64_t)KERNEL_ADDRESS_DMAP_BASE;
  if ((fw & 0xffff0000U) != 0x12700000U || soc != 0x840f60 ||
      !text || data <= text || data - text != 0xd50000U ||
      (dmap >> 48) != 0xffffU || (dmap & 0x3fffffffU) != 0 ||
      dmap > UINT64_MAX - CAVE_PA)
    return 1;

  struct timespec stamp = {0};
  if (clock_gettime(CLOCK_REALTIME, &stamp) != 0)
    return 2;
  char path[192], line[256];
  snprintf(path, sizeof(path),
           "/mnt/usb0/PS5-12.70-hv-cave-rw-%d-%ld-%ld.txt",
           (int)getpid(), (long)stamp.tv_sec, (long)stamp.tv_nsec);
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    return 3;
  snprintf(line, sizeof(line),
           "start,fw=0x%08x,soc=0x%x,dmap=0x%016" PRIx64
           ",pa=0x%016" PRIx64 "\n", fw, soc, dmap, CAVE_PA);
  if (log_line(fd, line) != 0)
    goto io_error;

  uint64_t region[32] = {0};
  if (log_line(fd, "before,stage=guard-read\n") != 0)
    goto io_error;
  int rc = kernel_copyout((intptr_t)(dmap + CAVE_PA), region, sizeof(region));
  if (rc != 0) {
    snprintf(line, sizeof(line), "stop,stage=guard-read,result=%d\n", rc);
    log_line(fd, line);
    close(fd);
    return 4;
  }
  for (size_t i = 0; i < sizeof(region) / sizeof(region[0]); ++i) {
    if (region[i] != EXPECTED) {
      snprintf(line, sizeof(line),
               "stop,stage=guard-match,qword=%zu,value=0x%016" PRIx64 "\n",
               i, region[i]);
      log_line(fd, line);
      close(fd);
      return 5;
    }
  }
  if (log_line(fd, "after,stage=guard-match,bytes=0x100,all_cc=1\n") != 0)
    goto io_error;

  uint64_t probe = PROBE;
  if (log_line(fd, "before,stage=write-probe\n") != 0)
    goto io_error;
  int write_rc = kernel_copyin(&probe, (intptr_t)(dmap + CAVE_PA), sizeof(probe));
  snprintf(line, sizeof(line), "after,stage=write-probe,result=%d\n", write_rc);
  log_line(fd, line);

  uint64_t readback = 0;
  int read_rc = kernel_copyout((intptr_t)(dmap + CAVE_PA),
                               &readback, sizeof(readback));
  snprintf(line, sizeof(line),
           "after,stage=read-probe,result=%d,value=0x%016" PRIx64 "\n",
           read_rc, readback);
  log_line(fd, line);

  /* Restore even if either operation reported an error: a partial write may
   * have happened before the SDK returned. */
  uint64_t original = EXPECTED;
  log_line(fd, "before,stage=restore\n");
  int restore_rc = kernel_copyin(&original, (intptr_t)(dmap + CAVE_PA),
                                 sizeof(original));
  uint64_t restored = 0;
  int verify_rc = kernel_copyout((intptr_t)(dmap + CAVE_PA),
                                 &restored, sizeof(restored));
  snprintf(line, sizeof(line),
           "after,stage=restore,write=%d,read=%d,value=0x%016" PRIx64 "\n",
           restore_rc, verify_rc, restored);
  log_line(fd, line);

  int pass = write_rc == 0 && read_rc == 0 && readback == PROBE &&
             restore_rc == 0 && verify_rc == 0 && restored == EXPECTED;
  snprintf(line, sizeof(line), "end,complete=%d,restored=%d\n",
           pass ? 1 : 0, verify_rc == 0 && restored == EXPECTED);
  log_line(fd, line);
  close(fd);
  return pass ? 0 : 6;

io_error:
  log_line(fd, "stop,stage=io-before-write\n");
  close(fd);
  return 7;
}
