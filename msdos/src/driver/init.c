#include "commands.h"
#include "fujinet.h"
#include "nio.h"
#include "port_config.h"
#include "portio.h"
#include "id8250.h"
#include "print.h"
#include "dispatch.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <dos.h>

#ifndef VERSION
#define VERSION "0.8"
#endif

#include <stdio.h>

#if defined(__WATCOMC__)

#define CC_VERSION_MINOR (__WATCOMC__ % 100)
#if __WATCOMC__ > 1100
#define CC_VERSION_MAJOR (__WATCOMC__ / 100 - 11)
#define CC_VERSION_NAME "Open Watcom"
#else /* __WATCOMC__ > 1100 */
#define CC_VERSION_MAJOR (__WATCOMC__ / 100)
#define CC_VERSION_NAME "Watcom"
#endif /* __WATCOMC__ > 1100 */

#elif defined(__TURBOC__)

#define CC_VERSION_MAJOR (__TURBOC__ / 0x100)
#define CC_VERSION_MINOR (__TURBOC__ % 0x100)
#define CC_VERSION_NAME "Turbo C"

#endif /* __WATCOMC__ */

union REGS regs;
extern void *config_env, *driver_end;

extern void setf5(void);

#pragma data_seg("_CODE")

uint8_t probe_fujinet_nio();
void nio_driver_config_init(void);
void check_uart();
uint16_t parse_config(const uint8_t far *config_sys);
void find_drive_letter(uint8_t num_units);

uint16_t Init_cmd(SYSREQ far *req)
{
  uint8_t err;
  uint16_t unused;

  regs.h.ah = 0x30;
  intdos(&regs, &regs);
  consolef("\nFujiNet driver " VERSION " " CC_VERSION_NAME " %i.%i"
           " on MS-DOS %i.%i\n",
           CC_VERSION_MAJOR,
           CC_VERSION_MINOR,
           regs.h.al,
           regs.h.ah);
  unused = parse_config(req->init.bpb_ptr);
  environ = (char **) &config_env;

  req->init.end_ptr = MK_FP(getCS(), (uint8_t *) &driver_end - unused);

  port_config_init();
  check_uart();

  nio_driver_config_init();
  err = probe_fujinet_nio();

  // If get_ returned error, FujiNet is probably not connected
  if (err) {
    req->init.num_units = 0;
    req->init.end_ptr = 0;
    port_config_done();
    return ERROR_BIT;
  }

  /* Construct BPB table and pointers */
  {
    int idx;

    req->init.num_units = FN_MAX_DEV;

    for (idx = 0; idx < req->init.num_units; idx++) {
      /* 5.25" 360k BPB */
      fn_bpb_table[idx].bps = 512;
      fn_bpb_table[idx].spau = 2;
      fn_bpb_table[idx].rs = 1;
      fn_bpb_table[idx].num_FATs = 2;
      fn_bpb_table[idx].root_entries = 0x0070;
      fn_bpb_table[idx].num_sectors = 0x02d0;
      fn_bpb_table[idx].media_descriptor = 0xfd;
      fn_bpb_table[idx].spfat = 2;
      fn_bpb_table[idx].spt = 9;
      fn_bpb_table[idx].heads = 2;
      fn_bpb_table[idx].hidden = 0;
      fn_bpb_table[idx].num_sectors_32 = 0;

      fn_bpb_pointers[idx] = &fn_bpb_table[idx];
    }

    fn_bpb_pointers[idx] = NULL;
    req->bpb.table = MK_FP(getCS(), fn_bpb_pointers);
  }

  find_drive_letter(req->init.num_units);

  setf5();
  consolef("INT F5 detection installed.\n");

  return OP_COMPLETE;
}

/* Returns non-zero on error */
uint8_t probe_fujinet_nio()
{
  nio_disk_info_t info;

  if (!nio_disk_begin_host_session(1)) {
    consolef("Unable to reset FujiNet NIO host session; continuing.\n");
  }

  if (!nio_disk_info(1, &info)) {
    consolef("Unable to contact FujiNet NIO DiskService.\nAborted.\n");
    return 1;
  }

  consolef("FujiNet NIO DiskService detected.\n");

  return 0;
}

void check_uart()
{
  int uart;

  uart = port_identify_uart();
  switch (uart) {
  case UART_16550A:
    consolef("Serial port is 16550A w/FIFO\n");
    break;

  case UART_16550:
    consolef("Serial port is 16550\n");
    break;

  case UART_16450:
    consolef("Serial port is 16450\n");
    break;

  case UART_8250:
    consolef("Serial port is 8250\n");
    break;

  default:
    consolef("Unknown serial port\n");
    break;
  }

  return;
}

/* Parse CONFIG.SYS command line, returns number of bytes remaining in config_env */
#define IS_CONFIG_EOL(c) (c == '\r' || c == '\n')
uint16_t parse_config(const uint8_t far *config_sys)
{
  int idx, count;
  const uint8_t far *cfg, far *bcfg;
  char **cfg_env = (char **) &config_env;
  char *buf, *buf_max;
  uint8_t eq_flag;

#ifdef CONFIG_SYS_DEBUG
  consolef("CONFIG.SYS: ");
  for (cfg = config_sys; cfg && !IS_CONFIG_EOL(*cfg); cfg++)
    printChar(*cfg);
  consolef("\n");
  dumpHex(config_sys, 64);
#endif /* CONFIG_SYS_DEBUG */
  *cfg_env = NULL;
  buf = (char *) &cfg_env[1];
  buf_max = (char *) cfg_env + ((uint16_t) &driver_end - (uint16_t) &config_env);

  // Driver filename is everything before the first space
  for (cfg = config_sys; cfg && *cfg > ' ' && !IS_CONFIG_EOL(*cfg); cfg++)
    ;

  if (*cfg && *cfg != ' ')
    goto done;

  cfg++;
  // Skip any extra spaces
  while (*cfg == ' ')
    cfg++;
  if (IS_CONFIG_EOL(*cfg))
    goto done;

  bcfg = cfg;

  // Count how many options we have
  count = 0;
  while (1) {
    // Find end of this config option
    for (; cfg && *cfg != ' ' && !IS_CONFIG_EOL(*cfg); cfg++)
      ;
    count++;
    if (*cfg != ' ')
      break;

    // Skip any trailing spaces
    while (*cfg == ' ')
      cfg++;
  }

  // Start strings after pointer table + NULL
  buf = ((char *) cfg_env) + sizeof(char *) * (count + 1);

  // Convert options to null terminated environment variables
  for (idx = 0, cfg = bcfg; idx < count && buf < buf_max; idx++) {
    cfg_env[idx] = buf;

    // Find end of this config option
    for (eq_flag = 0; cfg && *cfg != ' ' && !IS_CONFIG_EOL(*cfg); cfg++) {
      if (*cfg == '=')
        eq_flag = 1;
      *buf = *cfg;
      buf++;
      if (buf == buf_max)
        break;
    }
    if (buf == buf_max)
      break;

    if (!eq_flag) {
      *buf = '=';
      buf++;
      if (buf == buf_max)
        break;
    }
    *buf = 0;
    buf++;

    // Skip any trailing spaces
    while (*cfg == ' ')
      cfg++;
  }

  cfg_env[idx] = 0;

done:
  return buf - (char *) &config_env;
}

void find_drive_letter(uint8_t num_units)
{
  uint8_t far *lol;
  char first;

  _asm {
    mov ah, 52h
      int 21h
      mov word ptr lol, bx
      mov word ptr lol+2, es
  }

  // Undocumented but reliable field:
  // The number of current block devices in the List of Lists at 0x20
  first = lol[0x20] + 'A';
  consolef("FujiNet NIO active units exposed as drives %c:-%c:\n", first, first + num_units - 1);
}
