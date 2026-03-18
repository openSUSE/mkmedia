#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <uuid/uuid.h>
#include <json-c/json.h>

#include "disk.h"
#include "util.h"
#include "filesystem.h"
#include "json.h"

#include "ptable_gpt.h"

#define GPT_SIGNATURE	0x5452415020494645ll

#define ADJUST_BYTEORDER_16(a) a = le16toh(a)
#define ADJUST_BYTEORDER_32(a) a = le32toh(a)
#define ADJUST_BYTEORDER_64(a) a = le64toh(a)

typedef struct {
  uint64_t signature;
  uint32_t revision;
  uint32_t header_size;
  uint32_t header_crc;
  uint32_t reserved;
  uint64_t current_lba;
  uint64_t backup_lba;
  uint64_t first_lba;
  uint64_t last_lba;
  uuid_t disk_guid;
  uint64_t partition_lba;
  uint32_t partition_entries;
  uint32_t partition_entry_size;
  uint32_t partition_crc;
} gpt_header_t;

typedef struct {
  uuid_t type_guid;
  uuid_t partition_guid;
  uint64_t first_lba;
  uint64_t last_lba;
  uint64_t attributes;
  uint16_t name[36];
} gpt_entry_t;

uint64_t dump_gpt_ptable(disk_t *disk, uint64_t addr);
uint32_t chksum_crc32(void *buf, unsigned len);
char *guid_decode(uuid_t guid);
char *efi_partition_type(char *guid);
char *utf8_encode(unsigned uc);


uint32_t chksum_crc32(void *buf, unsigned len)
{
  uint8_t *bytes = buf;
  uint32_t crc = -1u;

  while(len--) {
    crc ^= *bytes++;
    for(int i = 0; i < 8; i++) {
      crc = (crc >> 1) ^ 0xedb88320 * (crc & 1);
    }
  }

  return ~crc;
}


char *guid_decode(uuid_t guid)
{
  uuid_t uuid;
  static char buf[37];
  static unsigned char idx[sizeof (uuid_t)] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  int i;
  unsigned char *s, *d;

  s = (unsigned char *) guid;
  d = (unsigned char *) uuid;

  for(i = 0; i < sizeof uuid; i++) {
    d[i] = s[idx[i]];
  }

  uuid_unparse_lower(uuid, buf);

  return buf;
}


char *efi_partition_type(char *guid)
{
  static struct {
    char *guid;
    char *name;
  } types[] = {
    { "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", "efi system" },
    { "0fc63daf-8483-4772-8e79-3d69d8477de4", "linux data" },
    { "e6d6d379-f507-44c2-a23c-238f2a3df928", "linux lvm" },
    { "a19d880f-05fc-4d3b-a006-743f0f84911e", "linux raid" },
    { "933ac7e1-2eb4-4f13-b844-0e14e2aef915", "linux home" },
    { "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f", "linux swap" },
    { "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7", "windows data" },
    { "48465300-0000-11aa-aa11-00306543ecac", "hfs+" },
    { "21686148-6449-6e6f-744e-656564454649", "bios boot" },
    { "9e1a2d38-c612-4316-aa26-8b49521e5a8b", "prep" },
  };

  int i;

  for(i = 0; i < sizeof types / sizeof *types; i++) {
    if(!strcmp(types[i].guid, guid)) return types[i].name;

  }

  return NULL;
}


char *utf8_encode(unsigned uc)
{
  static char buf[7];
  char *s = buf;

  uc &= 0x7fffffff;

  if(uc < 0x80) {			// 7 bits
    *s++ = uc;
  }
  else {
    if(uc < (1 << 11)) {		// 11 (5 + 6) bits
      *s++ = 0xc0 + (uc >> 6);
      goto utf8_encode_2;
    }
    else if(uc < (1 << 16)) {		// 16 (4 + 6 + 6) bits
      *s++ = 0xe0 + (uc >> 12);
      goto utf8_encode_3;
    }
    else if(uc < (1 << 21)) {		// 21 (3 + 6 + 6 + 6) bits
      *s++ = 0xf0 + (uc >> 18);
      goto utf8_encode_4;
    }
    else if(uc < (1 << 26)) {		// 26 (2 + 6 + 6 + 6 + 6) bits
      *s++ = 0xf8 + (uc >> 24);
      goto utf8_encode_5;
    }
    else {				// 31 (1 + 6 + 6 + 6 + 6 + 6) bits
      *s++ = 0xfc + (uc >> 30);
    }

    *s++ = 0x80 + ((uc >> 24) & ((1 << 6) - 1));

    utf8_encode_5:
      *s++ = 0x80 + ((uc >> 18) & ((1 << 6) - 1));

    utf8_encode_4:
      *s++ = 0x80 + ((uc >> 12) & ((1 << 6) - 1));

    utf8_encode_3:
      *s++ = 0x80 + ((uc >> 6) & ((1 << 6) - 1));

    utf8_encode_2:
      *s++ = 0x80 + (uc & ((1 << 6) - 1));
  }

  *s = 0;

  return buf;
}


uint64_t dump_gpt_ptable(disk_t *disk, uint64_t addr)
{
  int i, j, name_len;
  unsigned char buf[disk->block_size];
  gpt_header_t *gpt;
  unsigned u, part_blocks;
  uint16_t *n;
  uint32_t orig_crc;
  uint64_t next_table = 0;

  if(!addr) return next_table;

  i = disk_read(disk, buf, addr, 1);

  gpt = (gpt_header_t *) buf;

  ADJUST_BYTEORDER_64(gpt->signature);
  ADJUST_BYTEORDER_32(gpt->revision);
  ADJUST_BYTEORDER_32(gpt->header_size);
  ADJUST_BYTEORDER_32(gpt->header_crc);
  ADJUST_BYTEORDER_32(gpt->reserved);
  ADJUST_BYTEORDER_64(gpt->current_lba);
  ADJUST_BYTEORDER_64(gpt->backup_lba);
  ADJUST_BYTEORDER_64(gpt->first_lba);
  ADJUST_BYTEORDER_64(gpt->last_lba);
  ADJUST_BYTEORDER_64(gpt->partition_lba);
  ADJUST_BYTEORDER_32(gpt->partition_entries);
  ADJUST_BYTEORDER_32(gpt->partition_entry_size);
  ADJUST_BYTEORDER_32(gpt->partition_crc);

  if(i || gpt->signature != GPT_SIGNATURE) {
    if(addr != 1) log_info(SEP "\nno backup gpt\n");

    return next_table;
  }

  next_table = gpt->backup_lba;

  orig_crc = gpt->header_crc;
  gpt->header_crc = 0;
  u = chksum_crc32(gpt, gpt->header_size);
  gpt->header_crc = orig_crc;

  json_object *json_gpt = json_object_new_object();
  json_object_object_add(disk->json_disk, addr == 1 ? "gpt_primary" : "gpt_backup", json_gpt);

  char *guid = guid_decode(gpt->disk_guid);

  json_object_object_add(json_gpt, "revision", json_object_new_format("%u.%u", gpt->revision >> 16, gpt->revision & 0xffff));
  json_object_object_add(json_gpt, "block_size", json_object_new_int(disk->block_size));
  json_object_object_add(json_gpt, "disk_size", json_object_new_int(disk->size_in_bytes / disk->block_size));
  json_object_object_add(json_gpt, "guid", json_object_new_string(guid));

  log_info(SEP "\ngpt (%s) guid: %s\n", addr == 1 ? "primary" : "backup", guid);
  log_info("  sector size: %u\n", disk->block_size);
  log_info("  disk size: %"PRIu64"\n", disk->size_in_bytes / disk->block_size);
  log_info("  revision: %u.%u\n", gpt->revision >> 16, gpt->revision & 0xffff);

  json_object *json_header = json_object_new_object();
  json_object_object_add(json_gpt, "header", json_header);

  json_object_object_add(json_header, "size", json_object_new_int(gpt->header_size));

  json_object *json_crc = json_object_new_object();
  json_object_object_add(json_header, "crc", json_crc);
  json_object_object_add(json_crc, "stored", json_object_new_format("0x%08x", gpt->header_crc));
  json_object_object_add(json_crc, "calculated", json_object_new_format("0x%08x", u));
  json_object_object_add(json_crc, "ok", json_object_new_boolean(gpt->header_crc == u));

  log_info("  header: size %u, crc 0x%08x - %s\n",
    gpt->header_size, gpt->header_crc, gpt->header_crc == u ? "ok" : "wrong"
  );

  json_object_object_add(json_gpt, "reserved", json_object_new_int64(gpt->reserved));
  json_object_object_add(json_gpt, "my_lba", json_object_new_int64(gpt->current_lba));
  json_object_object_add(json_gpt, "alternate_lba", json_object_new_int64(gpt->backup_lba));

  log_info(
    "  position: current %"PRIu64", backup %"PRIu64"\n",
    gpt->current_lba,
    gpt->backup_lba
  );

  json_object *json_area = json_object_new_object();
  json_object_object_add(json_gpt, "usable_area", json_area);

  json_object_object_add(json_area, "first_lba", json_object_new_int64(gpt->first_lba));
  json_object_object_add(json_area, "last_lba", json_object_new_int64(gpt->last_lba));
  json_object_object_add(json_area, "size", json_object_new_int64(gpt->last_lba - gpt->first_lba + 1));

  log_info("  usable area: %"PRIu64" - %"PRIu64" (size %"PRIu64")\n",
    gpt->first_lba, gpt->last_lba, gpt->last_lba - gpt->first_lba + 1
  );

  part_blocks = ((gpt->partition_entries * gpt->partition_entry_size) + disk->block_size - 1) / disk->block_size;

  gpt_entry_t *part = malloc(part_blocks * disk->block_size);

  if(!part_blocks || !part) return next_table;

  i = disk_read(disk, part, gpt->partition_lba, part_blocks);

  if(i) {
    log_info("error reading gpt\n");
    free(part);

    return next_table;
  }

  u = chksum_crc32(part, gpt->partition_entries * gpt->partition_entry_size);

  json_object *json_table_info = json_object_new_object();
  json_object_object_add(json_gpt, "partition_table", json_table_info);

  json_object_object_add(json_table_info, "first_lba", json_object_new_int64(gpt->partition_lba));
  json_object_object_add(json_table_info, "last_lba", json_object_new_int64(gpt->partition_lba + part_blocks - 1));
  json_object_object_add(json_table_info, "size", json_object_new_int64(part_blocks));
  json_object_object_add(json_table_info, "entries", json_object_new_int64(gpt->partition_entries));
  json_object_object_add(json_table_info, "entry_size", json_object_new_int64(gpt->partition_entry_size));

  json_crc = json_object_new_object();
  json_object_object_add(json_table_info, "crc", json_crc);
  json_object_object_add(json_crc, "stored", json_object_new_format("0x%08x", gpt->partition_crc));
  json_object_object_add(json_crc, "calculated", json_object_new_format("0x%08x", u));
  json_object_object_add(json_crc, "ok", json_object_new_boolean(gpt->partition_crc == u));

  log_info("  partition table: %"PRIu64" - %"PRIu64" (size %u, crc 0x%08x - %s), entries %u, entry_size %u\n",
    gpt->partition_lba,
    gpt->partition_lba + part_blocks - 1,
    part_blocks,
    gpt->partition_crc,
    gpt->partition_crc == u ? "ok" : "wrong",
    gpt->partition_entries,
    gpt->partition_entry_size
  );

  gpt_entry_t *p, *part0 = calloc(1, sizeof *part0);

  json_object *json_table = json_object_new_array();
  json_object_object_add(json_gpt, "partitions", json_table);

  for(i = 0, p = part; i < gpt->partition_entries; i++, p++) {
    if(!memcmp(p, part0, sizeof *part0)) continue;

    json_object *json_entry = json_object_new_object();
    json_object_array_add(json_table, json_entry);

    ADJUST_BYTEORDER_64(p->first_lba);
    ADJUST_BYTEORDER_64(p->last_lba);
    ADJUST_BYTEORDER_64(p->attributes);

    json_object_object_add(json_entry, "index", json_object_new_int64(i));
    json_object_object_add(json_entry, "number", json_object_new_int64(i + 1));
    json_object_object_add(json_entry, "first_lba", json_object_new_int64(p->first_lba));
    json_object_object_add(json_entry, "last_lba", json_object_new_int64(p->last_lba));
    json_object_object_add(json_entry, "size", json_object_new_int64(p->last_lba - p->first_lba + 1));

    uint64_t attr = p->attributes;

    log_info("  %-3d%c %"PRIu64" - %"PRIu64" (size %"PRIu64")\n",
      i + 1,
      (attr & 4) ? '*' : ' ',
      p->first_lba,
      p->last_lba,
      p->last_lba - p->first_lba + 1
    );

    guid = guid_decode(p->type_guid);
    char *type_name = efi_partition_type(guid);

    json_object_object_add(json_entry, "type_guid", json_object_new_string(guid));
    if(type_name) json_object_object_add(json_entry, "type_name", json_object_new_string(type_name));

    json_object *json_attributes = json_object_new_object();
    json_object_object_add(json_entry, "attributes", json_attributes);

    json_object_object_add(json_attributes, "value", json_object_new_int64(attr));
    json_object_object_add(json_attributes, "system", json_object_new_boolean(attr & 1));
    json_object_object_add(json_attributes, "hidden", json_object_new_boolean(attr & 2));
    json_object_object_add(json_attributes, "boot", json_object_new_boolean(attr & 4));

    log_info("       type %s", guid);
    char *s = efi_partition_type(guid_decode(p->type_guid));
    if(s) log_info(" (%s)", s);
    log_info(", attributes 0x%"PRIx64"", attr);
    if((attr & 7)) {
      char *pref = " (";
      if((attr & 1)) { log_info("%ssystem", pref), pref = ", "; }
      if((attr & 2)) { log_info("%shidden", pref), pref = ", "; }
      if((attr & 4)) { log_info("%sboot", pref), pref = ", "; }
      log_info(")");
    }
    log_info("\n");

    guid = guid_decode(p->partition_guid);

    json_object_object_add(json_entry, "guid", json_object_new_string(guid));

    log_info("       guid %s\n", guid);

    name_len = (gpt->partition_entry_size - 56) / 2;

    n = p->name;
    for(j = name_len - 1; j > 0 && !n[j]; j--);
    name_len = n[j] ? j + 1 : j;
    
    char name[name_len * 7 + 1];

    *name = 0;
    n = p->name;
    for(unsigned u = 0; u < name_len; u++, n++) {
      // actually it's utf16le, but really...
      strcat(name, utf8_encode(le16toh(*n)));
    }

    json_object_object_add(json_entry, "name", json_object_new_string(name));
    log_info("       name[%d] \"%s\"\n", name_len, name);

    *name = 0;
    n = p->name;
    for(unsigned u = 0; u < name_len; u++, n++) {
      sprintf(name + 6 * u, "\\u%04x", le16toh(*n));
    }

    json_object_object_add(json_entry, "name_hex", json_object_new_string(name));

    disk->json_current = json_entry;
    dump_fs(disk, 7, p->first_lba);
    disk->json_current = disk->json_disk;
  }

  free(part0);
  free(part);

  return next_table;
}


void dump_gpt_ptables(disk_t *disk)
{
  uint64_t u;

  for(disk->block_size = 0x200; disk->block_size <= 0x1000; disk->block_size <<= 1) {
    u = dump_gpt_ptable(disk, 1);
    if(!u) continue;
    dump_gpt_ptable(disk, u);

    // json format can hold only one gpt
    if(opt.json) return;
  }
}
