/* SPDX-License-Identifier: GPL-3.0-or-later
 * Checkpointed capture of a bounded 12.70 hypervisor code window.
 *
 * Run only after the tested NPT clear in the same boot. A read of an unmapped
 * page can still panic the console. The synced USB log and binary retain all
 * completed pages if a later read fails or the console resets.
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

#define FW_1270       0x12700000U
#define SOC_PHAT      0x840f60
#define HV_START_PA   UINT64_C(0x62820000)
#define HV_END_PA     UINT64_C(0x62850000)
#define PAGE_BYTES    0x1000U
#define PAGE_COUNT    ((HV_END_PA - HV_START_PA) / PAGE_BYTES)

_Static_assert((HV_START_PA & (PAGE_BYTES - 1)) == 0 &&
               (HV_END_PA & (PAGE_BYTES - 1)) == 0 &&
               HV_END_PA > HV_START_PA &&
               HV_END_PA - HV_START_PA == 0x30000U,
               "HV code window must be exactly 192 KiB");

extern unsigned long KERNEL_ADDRESS_DMAP_BASE;
extern int sceKernelGetMainSocId(void);

static uint8_t page[PAGE_BYTES];

static int write_all(int fd, const void *data, size_t size)
{
  const uint8_t *bytes = data;
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

static int save_line(int fd, const char *line)
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
  if ((fw & 0xffff0000U) != FW_1270 || soc != SOC_PHAT ||
      !text || data <= text || data - text != 0xd50000U ||
      (dmap >> 48) != 0xffffU || (dmap & 0x3fffffffU) != 0 ||
      dmap > UINT64_MAX - HV_END_PA)
    return 1;

  struct timespec stamp = {0};
  if (clock_gettime(CLOCK_REALTIME, &stamp) != 0)
    return 2;
  char log_path[192], bin_path[192], line[256];
  int pid = (int)getpid();
  snprintf(log_path, sizeof(log_path),
           "/mnt/usb0/PS5-12.70-hv-code-%d-%ld-%ld.txt",
           pid, (long)stamp.tv_sec, (long)stamp.tv_nsec);
  snprintf(bin_path, sizeof(bin_path),
           "/mnt/usb0/PS5-12.70-hv-code-%d-%ld-%ld.bin",
           pid, (long)stamp.tv_sec, (long)stamp.tv_nsec);

  int log_fd = open(log_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (log_fd < 0)
    return 3;
  snprintf(line, sizeof(line),
           "start,fw=0x%08x,soc=0x%x,dmap=0x%016" PRIx64
           ",start=0x%016" PRIx64 ",end=0x%016" PRIx64
           ",page_bytes=0x%x\n",
           fw, soc, dmap, (uint64_t)HV_START_PA,
           (uint64_t)HV_END_PA, PAGE_BYTES);
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

  for (uint64_t off = 0; off < HV_END_PA - HV_START_PA; off += PAGE_BYTES) {
    snprintf(line, sizeof(line), "before,offset=0x%05" PRIx64 "\n", off);
    if (save_line(log_fd, line) != 0)
      goto io_error;

    int rc = kernel_copyout((intptr_t)(dmap + HV_START_PA + off),
                            page, sizeof(page));
    if (rc != 0) {
      snprintf(line, sizeof(line),
               "stop,stage=copyout,offset=0x%05" PRIx64 ",result=%d\n",
               off, rc);
      save_line(log_fd, line);
      close(bin_fd);
      close(log_fd);
      return 6;
    }
    if (write_all(bin_fd, page, sizeof(page)) != 0 || fsync(bin_fd) != 0)
      goto io_error;

    snprintf(line, sizeof(line), "after,offset=0x%05" PRIx64 "\n", off);
    if (save_line(log_fd, line) != 0)
      goto io_error;
  }

  snprintf(line, sizeof(line),
           "end,complete=1,pages=%" PRIu64 ",bytes=0x%05x\n",
           (uint64_t)PAGE_COUNT, (unsigned)(HV_END_PA - HV_START_PA));
  if (save_line(log_fd, line) != 0)
    goto io_error;
  int bin_close = close(bin_fd);
  int log_close = close(log_fd);
  return bin_close == 0 && log_close == 0 ? 0 : 7;

io_error:
  save_line(log_fd, "stop,stage=io\n");
  close(bin_fd);
  close(log_fd);
  return 7;
}
