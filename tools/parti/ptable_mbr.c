#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>

#include "disk.h"
#include "filesystem.h"
#include "util.h"
#include "json.h"

#include "ptable_mbr.h"

typedef struct {
  unsigned type;
  unsigned boot:1;
  unsigned valid:1;
  unsigned empty:1;
  struct {
    unsigned c, h, s, lin;
  } start;
  struct {
    unsigned c, h, s, lin;
  } end;
  unsigned real_base;
  unsigned base;
  unsigned idx;
} ptable_t;


unsigned cs2s(unsigned cs);
unsigned cs2c(unsigned cs);
char *mbr_partition_type(unsigned id);
void parse_ptable(void *buf, unsigned addr, ptable_t *ptable, unsigned base, unsigned ext_base, int entries);
int guess_geo(ptable_t *ptable, int entries, unsigned *s, unsigned *h);
void print_ptable_entry(json_object *json_table, disk_t *disk, int nr, ptable_t *ptable, int index);
int is_ext_ptable(ptable_t *ptable);
ptable_t *find_ext_ptable(ptable_t *ptable, int entries);


unsigned cs2s(unsigned cs)
{
  return cs & 0x3f;
}


unsigned cs2c(unsigned cs)
{
  return ((cs >> 8) & 0xff) + ((cs & 0xc0) << 2);
}


void parse_ptable(void *buf, unsigned addr, ptable_t *ptable, unsigned base, unsigned ext_base, int entries)
{
  unsigned u;

  memset(ptable, 0, entries * sizeof *ptable);

  for(; entries; entries--, addr += 0x10, ptable++) {
    ptable->idx = 4 - entries + 1;
    if(read_qword_le(buf + addr) == 0 && read_qword_le(buf + addr + 0x8) == 0) {
      ptable->empty = 1;
    }
    u = read_byte(buf + addr);
    if(u & 0x7f) continue;
    ptable->boot = u >> 7;
    ptable->type = read_byte(buf + addr + 4);
    u = read_word_le(buf + addr + 2);
    ptable->start.c = cs2c(u);
    ptable->start.s = cs2s(u);
    ptable->start.h = read_byte(buf + addr + 1);
    ptable->start.lin = read_dword_le(buf + addr + 8);
    u = read_word_le(buf + addr + 6);
    ptable->end.c = cs2c(u);
    ptable->end.s = cs2s(u);
    ptable->end.h = read_byte(buf + addr + 5);
    ptable->end.lin = ptable->start.lin + read_dword_le(buf + addr + 0xc);

    ptable->real_base = base;
    ptable->base = is_ext_ptable(ptable) ? ext_base : base;

    if(ptable->end.lin != ptable->start.lin && ptable->start.s && ptable->end.s) {
      ptable->valid = 1;
      ptable->end.lin--;
    }
  }
}


int guess_geo(ptable_t *ptable, int entries, unsigned *s, unsigned *h)
{
  unsigned sectors, heads, u, c;
  int i, ok, cnt;

  for(sectors = 63; sectors; sectors--) {
    for(heads = 255; heads; heads--) {
      ok = 1;
      for(cnt = i = 0; i < entries; i++) {
        if(!ptable[i].valid) continue;

        if(ptable[i].start.h >= heads) { ok = 0; break; }
        if(ptable[i].start.s > sectors) { ok = 0; break; }
        if(ptable[i].start.c >= 1023) {
          c = ((ptable[i].start.lin + 1 - ptable[i].start.s)/sectors - ptable[i].start.h)/heads;
          if(c < 1023) { ok = 0; break; }
        }
        else {
          c = ptable[i].start.c;
        }
        u = (c * heads + ptable[i].start.h) * sectors + ptable[i].start.s - 1;
        if(u != ptable[i].start.lin) {
          ok = 0;
          break;
        }
        cnt++;
        if(ptable[i].end.h >= heads) { ok = 0; break; }
        if(ptable[i].end.s > sectors) { ok = 0; break; }
        if(ptable[i].end.c >= 1023) {
          c = ((ptable[i].end.lin + 1 - ptable[i].end.s)/sectors - ptable[i].end.h)/heads;
          if(c < 1023) { ok = 0; break; }
        }
        else {
          c = ptable[i].end.c;
        }
        u = (c * heads + ptable[i].end.h) * sectors + ptable[i].end.s - 1;
        if(u != ptable[i].end.lin) {
          ok = 0;
          break;
        }
        cnt++;
      }
      if(!cnt) ok = 0;
      if(ok) break;
    }
    if(ok) break;
  }

  if(ok) {
    *h = heads;
    *s = sectors;
  }

  return ok;
}


void print_ptable_entry(json_object *json_table, disk_t *disk, int nr, ptable_t *ptable, int index)
{
  unsigned u;

  if(ptable->valid) {
    json_object *json_entry = json_object_new_object();
    json_object_array_add(json_table, json_entry);

    json_object_object_add(json_entry, "index", json_object_new_int64(index));
    json_object_object_add(json_entry, "number", json_object_new_int64(nr));

    json_object *json_attributes = json_object_new_object();
    json_object_object_add(json_entry, "attributes", json_attributes);

    json_object_object_add(json_attributes, "boot", json_object_new_boolean(ptable->boot));
    json_object_object_add(json_attributes, "valid", json_object_new_boolean(1));

    if(nr > 4 && is_ext_ptable(ptable)) {
      if(!opt.verbose) return;
      log_info("    >");
    }
    else {
      log_info("  %-3d", nr);
    }

    u = opt.show.raw ? 0 : ptable->base;

    log_info("%c %u - %u (size %u), chs %u/%u/%u - %u/%u/%u",
      ptable->boot ? '*' : ' ',
      ptable->start.lin + u,
      ptable->end.lin + u,
      ptable->end.lin - ptable->start.lin + 1,
      ptable->start.c, ptable->start.h, ptable->start.s,
      ptable->end.c, ptable->end.h, ptable->end.s
    );

    if(opt.verbose) {
      log_info(", [ref %d.%d]", ptable->real_base, ptable->idx - 1);
    }

    json_object_object_add(json_entry, "first_lba", json_object_new_int64((uint64_t) ptable->start.lin + ptable->base));
    json_object_object_add(json_entry, "last_lba", json_object_new_int64((uint64_t) ptable->end.lin + ptable->base));
    json_object_object_add(json_entry, "size", json_object_new_int64((uint64_t) ptable->end.lin - ptable->start.lin + 1));

    json_object *json_geo = json_object_new_object();
    json_object_object_add(json_entry, "first_geo", json_geo);
    json_object_object_add(json_geo, "cylinders", json_object_new_int64(ptable->start.c));
    json_object_object_add(json_geo, "heads", json_object_new_int64(ptable->start.h));
    json_object_object_add(json_geo, "sectors", json_object_new_int64(ptable->start.s));

    json_geo = json_object_new_object();
    json_object_object_add(json_entry, "last_geo", json_geo);
    json_object_object_add(json_geo, "cylinders", json_object_new_int64(ptable->end.c));
    json_object_object_add(json_geo, "heads", json_object_new_int64(ptable->end.h));
    json_object_object_add(json_geo, "sectors", json_object_new_int64(ptable->end.s));

    json_object_object_add(json_entry, "base_lba", json_object_new_int64(ptable->base));
    json_object_object_add(json_entry, "table_lba", json_object_new_int64(ptable->real_base));
    json_object_object_add(json_entry, "table_index", json_object_new_int64(ptable->idx - 1));

    if(opt.show.raw && ptable->base) log_info(", ext base %+d", ptable->base);
    log_info("\n");

    json_object_object_add(json_entry, "type_id", json_object_new_int64(ptable->type));

    log_info("       type 0x%02x", ptable->type);
    char *s = mbr_partition_type(ptable->type);
    if(s) {
      json_object_object_add(json_entry, "type_name", json_object_new_string(s));
      log_info(" (%s)", s);
    }
    log_info("\n");

    disk->json_current = json_entry;
    dump_fs(disk, 7, (uint64_t) ptable->start.lin + ptable->base);
    disk->json_current = disk->json_disk;
  }
  else if(!ptable->empty) {
    log_info("  %-3d  invalid data\n", nr);

    json_object *json_entry = json_object_new_object();
    json_object_array_add(json_table, json_entry);

    json_object_object_add(json_entry, "index", json_object_new_int64(index));
    json_object_object_add(json_entry, "number", json_object_new_int64(nr));

    json_object *json_attributes = json_object_new_object();
    json_object_object_add(json_entry, "attributes", json_attributes);

    json_object_object_add(json_attributes, "valid", json_object_new_boolean(0));

    json_object_object_add(json_entry, "base_lba", json_object_new_int64(ptable->base));
    json_object_object_add(json_entry, "table_lba", json_object_new_int64(ptable->real_base));
    json_object_object_add(json_entry, "table_index", json_object_new_int64(ptable->idx - 1));
  }
}


int is_ext_ptable(ptable_t *ptable)
{
  return ptable->type == 5 || ptable->type == 0xf;
}


ptable_t *find_ext_ptable(ptable_t *ptable, int entries)
{
  for(; entries; entries--, ptable++) {
    if(ptable->valid && is_ext_ptable(ptable)) return ptable;
  }
  return NULL;
}


void dump_mbr_ptable(disk_t *disk)
{
  int i, j, pcnt, link_count;
  ptable_t ptable[4], *ptable_ext;
  unsigned s, h, ext_base, id;
  unsigned char buf[disk->block_size];

  i = disk_read(disk, buf, 0, 1);

  if(i || read_word_le(buf + 0x1fe) != 0xaa55) {
    return;
  }

  id = read_dword_le(buf + 0x1b8);

  for(i = j = 0; i < 4 * 16; i++) {
    j |= read_byte(buf + 0x1be + i);
  }

  // empty partition table
  if(!j && !id) return;

  json_object *json_mbr = json_object_new_object();
  json_object_object_add(disk->json_disk, "mbr", json_mbr);

  parse_ptable(buf, 0x1be, ptable, 0, 0, 4);
  i = guess_geo(ptable, 4, &s, &h);
  if(!i) {
    s = 63;
    h = 255;
  }
  disk->sectors = s;
  disk->heads = h;
  disk->cylinders = disk->size_in_bytes / ((uint64_t) disk->block_size * disk->sectors * disk->heads);

  log_info(SEP "\nmbr id: 0x%08x\n", id);

  json_object_object_add(json_mbr, "block_size", json_object_new_int(disk->block_size));
  json_object_object_add(json_mbr, "disk_size", json_object_new_int(disk->size_in_bytes / disk->block_size));
  log_info("  sector size: %u\n", disk->block_size);
  log_info("  disk size: %"PRIu64"\n", disk->size_in_bytes / disk->block_size);

  json_object_object_add(json_mbr, "id", json_object_new_format("0x%08x", id));

  // 32 or 64 bit?
  //
  // Tools that write the value claim 64 bit.
  // isolinux actually uses 32 bit when loading.
  // And no Legacy BIOS handles 64 bit correctly anyway.
  uint64_t bi_start = le64toh(*(uint64_t *) (buf + 0x1b0));

  if(bi_start) {
    char *s;
    char *bi_type = "bootinfo";
    if(memmem(buf, disk->block_size, "isolinux.bin", sizeof "isolinux.bin" - 1)) {
      bi_type = "isolinux";
      disk->isolinux_used = 1;
    }
    else if(memmem(buf, disk->block_size, "GRUB", sizeof "GRUB" - 1)) {
      bi_type = "grub";
      disk->grub_used = 1;
      // grub stores offset to image + 4 blocks
      bi_start -= 4;
    }

    log_info("  %s: %"PRIu64, bi_type, bi_start);
    if((s = iso_block_to_name(disk, bi_start, NULL))) {
      log_info(", \"%s\"", s);
    }
    log_info("\n");

    json_object *json_isolinux = json_object_new_object();
    json_object_object_add(json_mbr, bi_type, json_isolinux);

    json_object_object_add(json_isolinux, "first_lba", json_object_new_int64(bi_start));
    if(s) json_object_object_add(json_isolinux, "file_name", json_object_new_string(s));
  }

  log_info(
    "  mbr partition table (chs %u/%u/%u%s):\n",
    disk->cylinders,
    disk->heads,
    disk->sectors,
    i ? "" : ", inconsistent geo"
  );

  json_object *json_geo = json_object_new_object();
  json_object_object_add(json_mbr, "geometry", json_geo);
  json_object_object_add(json_geo, "consistent", json_object_new_boolean(i));
  json_object_object_add(json_geo, "cylinders", json_object_new_int64(disk->cylinders));
  json_object_object_add(json_geo, "heads", json_object_new_int64(disk->heads));
  json_object_object_add(json_geo, "sectors", json_object_new_int64(disk->sectors));

  json_object *json_table = json_object_new_array();
  json_object_object_add(json_mbr, "partitions", json_table);

  for(i = 0; i < 4; i++) {
    print_ptable_entry(json_table, disk, i + 1, ptable + i, i);
  }

  pcnt = 5;

  link_count = 0;
  ext_base = 0;

  while((ptable_ext = find_ext_ptable(ptable, 4))) {
    if(!link_count++) {
      ext_base = ptable_ext->start.lin;
    }
    // arbitrary, but we don't want to loop forever
    if(link_count > 10000) {
      log_info("too many partitions\n");
      break;
    }
    j = disk_read(disk, buf, ptable_ext->start.lin + ptable_ext->base, 1);
    if(j || read_word_le(buf + 0x1fe) != 0xaa55) {
      if(j) log_info("disk read error - ");
      log_info("not a valid extended partition\n");
      break;
    }
    parse_ptable(buf, 0x1be, ptable, ptable_ext->start.lin + ptable_ext->base, ext_base, 4);
    for(i = 0; i < 4; i++) {
      print_ptable_entry(json_table, disk, pcnt, ptable + i, i);
      if(ptable[i].valid && !is_ext_ptable(ptable + i)) pcnt++;
    }
  }
}


char *mbr_partition_type(unsigned id)
{
  static struct {
    unsigned id;
    char *name;
  } types[] = {
    { 0x00, "empty" },
    { 0x01, "fat12" },
    { 0x04, "fat16 <32mb" },
    { 0x05, "extended" },
    { 0x06, "fat16" },
    { 0x07, "ntfs" },
    { 0x0b, "fat32" },
    { 0x0c, "fat32 lba" },
    { 0x0e, "fat16 lba" },
    { 0x0f, "extended lba" },
    { 0x11, "fat12 hidden" },
    { 0x14, "fat16 <32mb hidden" },
    { 0x16, "fat16 hidden" },
    { 0x17, "ntfs hidden" },
    { 0x1b, "fat32 hidden" },
    { 0x1c, "fat32 lba hidden" },
    { 0x1e, "fat16 lba hidden" },
    { 0x41, "prep" },
    { 0x82, "swap" },
    { 0x83, "linux" },
    { 0x8e, "lvm" },
    { 0x96, "chrp iso9660" },
    { 0xde, "dell utility" },
    { 0xee, "gpt" },
    { 0xef, "efi" },
    { 0xfd, "linux raid" },
  };

  int i;

  for(i = 0; i < sizeof types / sizeof *types; i++) {
    if(types[i].id == id) return types[i].name;

  }

  return NULL;
}
