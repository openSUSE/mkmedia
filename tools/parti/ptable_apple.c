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

#include "ptable_apple.h"

void dump_apple_ptables(disk_t *disk)
{
  for(disk->block_size = 0x200; disk->block_size <= 0x1000; disk->block_size <<= 1) {
    if(dump_apple_ptable(disk)) return;
  }
}


int dump_apple_ptable(disk_t *disk)
{
  int i, parts;
  unsigned u1, u2;
  unsigned char buf[disk->block_size];
  apple_entry_t *apple;
  char *s;

  i = disk_read(disk, buf, 1, 1);

  apple = (apple_entry_t *) buf;

  if(i || be16toh(apple->signature) != APPLE_MAGIC) return 0;

  parts = be32toh(apple->partitions);

  json_object *json_apple = json_object_new_object();
  json_object_object_add(disk->json_disk, "apple", json_apple);

  json_object_object_add(json_apple, "block_size", json_object_new_int(disk->block_size));
  json_object_object_add(json_apple, "disk_size", json_object_new_int(disk->size_in_bytes / disk->block_size));
  json_object_object_add(json_apple, "entries", json_object_new_int(parts));

  log_info(SEP "\napple partition table: %d entries\n", parts);
  log_info("  sector size: %d\n", disk->block_size);
  log_info("  disk size: %"PRIu64"\n", disk->size_in_bytes / disk->block_size);

  json_object *json_table = json_object_new_array();
  json_object_object_add(json_apple, "partitions", json_table);

  for(i = 1; i <= parts; i++) {
    if(disk_read(disk, buf, i, 1)) break;
    apple = (apple_entry_t *) buf;

    json_object *json_entry = json_object_new_object();
    json_object_array_add(json_table, json_entry);

    json_object_object_add(json_entry, "index", json_object_new_int64(i));
    json_object_object_add(json_entry, "number", json_object_new_int64(i));

    u1 = be32toh(apple->start);
    u2 = be32toh(apple->size);
    log_info("%3d  %u - %llu (size %u)", i, u1, (unsigned long long) u1 + u2 - 1, u2);

    json_object_object_add(json_entry, "first_lba", json_object_new_int64(u1));
    json_object_object_add(json_entry, "last_lba", json_object_new_int64(u1 + u2 - 1));
    json_object_object_add(json_entry, "size", json_object_new_int64(u2));

    u1 = be32toh(apple->data_start);
    u2 = be32toh(apple->data_size);
    log_info(", rel. %u - %llu (size %u)\n", u1, (unsigned long long) u1 + u2, u2);

    json_object_object_add(json_entry, "data_start", json_object_new_int64(u1));
    json_object_object_add(json_entry, "data_size", json_object_new_int64(u2));

    json_object_object_add(json_entry, "status", json_object_new_int64(be32toh(apple->status)));

    s = cname(apple->type, sizeof apple->type);
    log_info("     type[%d] \"%s\"", (int) strlen(s), s);

    json_object_object_add(json_entry, "type", json_object_new_string(s));

    log_info(", status 0x%x\n", be32toh(apple->status));

    s = cname(apple->name, sizeof apple->name);
    log_info("     name[%d] \"%s\"\n", (int) strlen(s), s);

    json_object_object_add(json_entry, "name", json_object_new_string(s));

    disk->json_current = json_entry;
    dump_fs(disk, 5, be32toh(apple->start));
    disk->json_current = disk->json_disk;
  }

  return 1;
}
