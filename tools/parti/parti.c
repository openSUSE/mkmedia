#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>
#include <getopt.h>

#include "util.h"
#include "json.h"
#include "disk.h"

#include "eltorito.h"
#include "filesystem.h"
#include "ptable_apple.h"
#include "ptable_gpt.h"
#include "ptable_mbr.h"
#include "zipl.h"

#ifndef VERSION
#define VERSION "0.0"
#endif

void help(void);

struct option options[] = {
  { "help",        0, NULL, 'h'  },
  { "verbose",     0, NULL, 'v'  },
  { "mkisofs",     0, NULL, 1006 },
  { "xorriso",     0, NULL, 1007 },
  { "raw",         0, NULL, 1001 },
  { "version",     0, NULL, 1002 },
  { "export-disk", 1, NULL, 1003 },
  { "import-disk", 1, NULL, 1004 },
  { "json",        0, NULL, 1005 },
  { "mkisofs",     0, NULL, 1006 },
  { "xorriso",     0, NULL, 1007 },
  { }
};


int main(int argc, char **argv)
{
  int i;
  extern int optind;
  extern int opterr;

  json_init();

  opterr = 0;

  while((i = getopt_long(argc, argv, "hv", options, NULL)) != -1) {
    switch(i) {
      case 'v':
        opt.verbose++;
        break;

      case 1001:
        opt.show.raw = 1;
        break;

      case 1002:
        printf(VERSION "\n");
        return 0;
        break;

      case 1003:
        opt.export_file = optarg;
        break;

      case 1004:
        disk_import(optarg);
        break;

      case 1005:
        opt.json = 1;
        break;

      case 1006:
        opt.mkisofs = 1;
        break;

      case 1007:
        opt.xorriso = 1;
        break;

      default:
        help();
        return i == 'h' ? 0 : 1;
    }
  }

  argc -= optind;
  argv += optind;

  if(!opt.xorriso && !opt.mkisofs) {
    if(access("/usr/bin/isoinfo", X_OK)) {
      opt.mkisofs = 1;
    }
    else if(access("/usr/bin/xorriso", X_OK)) {
      opt.xorriso = 1;
    }
  }

  while(*argv) disk_init(*argv++);

  if(!disk_list_size) {
    help();

    return 1;
  }

  for(unsigned u = 0; u < disk_list_size; u++) {
    dump_fs(disk_list + u, 0, 0);
    dump_mbr_ptable(disk_list + u);
    dump_gpt_ptables(disk_list + u);
    dump_apple_ptables(disk_list + u);
    dump_eltorito(disk_list + u);
    dump_zipl(disk_list + u);
  }

  if(opt.export_file) {
    unlink(opt.export_file);
    for(unsigned u = 0; u < disk_list_size; u++) {
      disk_export(disk_list + u, opt.export_file);
    }
  }

  json_print();

  json_done();

  return 0;
}


void help()
{
  fprintf(stderr,
    "Partition Info " VERSION "\n"
    "Usage: parti [OPTIONS] DISK_DEVICES\n"
    "\n"
    "Print information about disk devices.\n"
    "\n"
    "Options:\n"
    "\n"
    "  --json              Use JSON format for output.\n"
    "  --export-disk FILE  Export all relevant disk data to FILE. FILE can then be used\n"
    "                      with --import-disk to reproduce the results.\n"
    "  --import-disk FILE  Import relevant disk data from FILE.\n"
    "  --mkisofs           Use isoinfo to read ISO9660 fs info (default).\n"
    "  --xorriso           Use xorriso to read ISO9660 fs info.\n"
    "  --verbose           Report more details.\n"
    "  --version           Show version.\n"
    "  --help              Print this help text.\n"
  );
}
