/* SPDX-License-Identifier: GPL-3.0-or-later
 * Capture one 12.70 hypervisor page after a successful Byepervisor run.
 *
 * This is an acquisition diagnostic, not a loader. The prior 12.70 AP probe
 * read the qword at PA 0x62882a60. We first repeat that 8-byte read, then
 * capture only its containing 4 KiB page. An unmapped HV read can still crash
 * the console; there is no reliable recovery from a fault in kernel_copyout.
 *
 * Output: /mnt/usb0/PS5-12.70-hv-page-<pid>-<time>.{txt,bin}
 */
#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#define FW_1270       0x12700000U
#define SOC_PHAT      0x840f60
#define HV_PAGE_PA    UINT64_C(0x62882000)
#define HV_PROBE_PA   UINT64_C(0x62882a60)
#define PAGE_BYTES    0x1000U

/* The SDK exports this value but does not declare it in every kernel.h. */
extern unsigned long KERNEL_ADDRESS_DMAP_BASE;

static uint8_t page[PAGE_BYTES];

static int write_all(int fd, const void *data, size_t size)
{
  const uint8_t *bytes = data;
  while (size) {
    ssize_t n = write(fd, bytes, size);
    if (n <= 0)
      return -1;
    bytes += (size_t)n;
    size -= (size_t)n;
  }
  return 0;
}

static int save_line(int fd, const char *line)
{
  return write_all(fd, line, strlen(line)) == 0 ? fsync(fd) : -1;
}

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  int soc = sceKernelGetMainSocId();
  uint64_t dmap = (uint64_t)KERNEL_ADDRESS_DMAP_BASE;
  if ((fw & 0xffff0000U) != FW_1270 || soc != SOC_PHAT ||
      (dmap >> 48) != 0xffffU || (dmap & 0x3fffffffU) != 0 ||
      dmap > UINT64_MAX - HV_PAGE_PA - PAGE_BYTES)
    return 1;

  struct timespec stamp = {0};
  if (clock_gettime(CLOCK_REALTIME, &stamp) != 0)
    return 2;
  char log_path[192], bin_path[192], line[256];
  int pid = (int)getpid();
  snprintf(log_path, sizeof(log_path),
           "/mnt/usb0/PS5-12.70-hv-page-%d-%ld-%ld.txt",
           pid, (long)stamp.tv_sec, (long)stamp.tv_nsec);
  snprintf(bin_path, sizeof(bin_path),
           "/mnt/usb0/PS5-12.70-hv-page-%d-%ld-%ld.bin",
           pid, (long)stamp.tv_sec, (long)stamp.tv_nsec);

  int log_fd = open(log_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (log_fd < 0)
    return 3;
  snprintf(line, sizeof(line),
           "start,fw=0x%08x,soc=0x%x,dmap=0x%016" PRIx64
           ",pa=0x%016" PRIx64 ",bytes=0x%x\n",
           fw, soc, dmap, (uint64_t)HV_PAGE_PA, PAGE_BYTES);
  if (save_line(log_fd, line) != 0) {
    close(log_fd);
    return 4;
  }

  int bin_fd = open(bin_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (bin_fd < 0) {
    save_line(log_fd, "stop,stage=bin-open\n");
    close(log_fd);
    return 5;
  }

  uint64_t probe = 0;
  if (save_line(log_fd, "before,stage=probe\n") != 0)
    goto io_error;
  int rc = kernel_copyout((intptr_t)(dmap + HV_PROBE_PA), &probe,
                          sizeof(probe));
  snprintf(line, sizeof(line),
           "after,stage=probe,result=%d,value=0x%016" PRIx64 "\n",
           rc, probe);
  if (save_line(log_fd, line) != 0)
    goto io_error;
  if (rc != 0 || probe == 0) {
    save_line(log_fd, "stop,stage=probe-validation\n");
    close(bin_fd);
    close(log_fd);
    return 6;
  }

  if (save_line(log_fd, "before,stage=page\n") != 0)
    goto io_error;
  rc = kernel_copyout((intptr_t)(dmap + HV_PAGE_PA), page, sizeof(page));
  snprintf(line, sizeof(line), "after,stage=page,result=%d\n", rc);
  if (save_line(log_fd, line) != 0)
    goto io_error;
  if (rc != 0 ||
      memcmp(page + (HV_PROBE_PA - HV_PAGE_PA), &probe, sizeof(probe)) != 0) {
    save_line(log_fd, "stop,stage=page-validation\n");
    close(bin_fd);
    close(log_fd);
    return 7;
  }

  if (write_all(bin_fd, page, sizeof(page)) != 0 || fsync(bin_fd) != 0)
    goto io_error;
  if (save_line(log_fd, "end,complete=1,bytes=0x1000\n") != 0)
    goto io_error;
  int bin_close = close(bin_fd);
  int log_close = close(log_fd);
  if (bin_close != 0 || log_close != 0)
    return 8;
  return 0;

io_error:
  save_line(log_fd, "stop,stage=io\n");
  close(bin_fd);
  close(log_fd);
  return 8;
}
